#include "lemon/backends/adapter/adapter_server.h"
#include "adapter_spec.h"

#include "lemon/backends/adapter/adapter.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/error_types.h"
#include "lemon/server_process.h"
#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <chrono>
#include <map>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/types.h>
#include <signal.h>
#endif

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

namespace adapter {

std::pair<std::string, std::string> split_checkpoint(const std::string& checkpoint) {
    const auto at = checkpoint.find("::");
    if (at == std::string::npos) {
        return {"", checkpoint};
    }
    return {checkpoint.substr(0, at), checkpoint.substr(at + 2)};
}

std::string adapters_dir() {
    if (const char* dir = std::getenv("LEMONADE_ADAPTERS_DIR"); dir != nullptr && *dir != '\0') {
        return dir;
    }
    return (fs::path(utils::get_config_dir()) / "adapters").string();
}

}  // namespace adapter

namespace {

// The pure spec helpers live in adapter_spec.{h,cpp}, tested without lemond (test/cpp/adapter_spec/).
using adapter::specs::contains;
using adapter::specs::fill;
using adapter::specs::split_args;
using adapter::specs::valid_spec_name;

json read_spec(const std::string& name, fs::path& spec_dir) {
    if (!valid_spec_name(name)) {
        throw std::runtime_error("adapter: '" + name + "' is not a valid spec name (letters, digits, - _ .)");
    }
    const fs::path path = fs::path(adapter::adapters_dir()) / (name + ".json");
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("adapter: no spec " + path.string() +
                                 " (install the engine's adapter, or fall back to running the engine without control)");
    }
    json spec;
    try {
        in >> spec;
    } catch (const std::exception& e) {
        throw std::runtime_error("adapter: " + path.string() + " is not valid JSON: " + e.what());
    }
    spec_dir = path.parent_path();
    return spec;
}

// The engine program: an absolute path, a path relative to the spec's folder, or a name on PATH.
std::string resolve_binary(const std::string& binary, const fs::path& spec_dir) {
    if (binary.empty()) {
        throw std::runtime_error("adapter: the spec names no binary");
    }
    const fs::path p(binary);
    std::error_code ec;
    if (p.is_absolute()) {
        if (fs::exists(p, ec)) return p.string();
        throw std::runtime_error("adapter: engine binary not found: " + binary);
    }
    if (binary.find('/') != std::string::npos || binary.find('\\') != std::string::npos) {
        const fs::path rel = spec_dir / p;
        if (fs::exists(rel, ec)) return rel.string();
        throw std::runtime_error("adapter: engine binary not found: " + rel.string());
    }
    if (!utils::find_executable_in_path(binary).empty()) return binary;
    throw std::runtime_error("adapter: engine binary '" + binary + "' is not on PATH");
}

// A recipe option as text: "" when unset (an unset option is JSON null).
std::string option_text(const RecipeOptions& options, const std::string& key) {
    const json v = options.get_option(key);
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return v.dump();
    return "";
}

// "size:mtime" of a prepare input, or "missing".
std::string input_stamp(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return fs::exists(path, ec) ? "dir:" + std::to_string(fs::last_write_time(path, ec).time_since_epoch().count())
                                        : "missing";
    return std::to_string(size) + ":" + std::to_string(fs::last_write_time(path, ec).time_since_epoch().count());
}

// Runs the spec's prepare step before a load, once per set of inputs: a marker named by prepare_key() in
// <cache dir>/adapter-prepare/<spec>/ records a run that exited 0. No shell; output goes to the log. A failure
// or a timeout throws ConfigurationException with the last lines, so the router does not evict every other
// model and retry, which is what it does after an ordinary load failure.
void run_prepare(const std::string& name, const json& spec, const fs::path& spec_dir,
                 std::map<std::string, std::string> values, const std::function<bool()>& cancelled) {
    adapter::specs::PrepareStep step;
    std::vector<std::string> argv;
    try {
        step = adapter::specs::parse_prepare(spec);
        if (!step.present) return;
        values.erase("{port}");
        argv = adapter::specs::prepare_argv(step, values);
    } catch (const adapter::specs::SpecError& e) {
        throw ConfigurationException("adapter " + name + ": " + e.what());
    }
    try {
        argv[0] = resolve_binary(argv[0], spec_dir);
    } catch (const std::exception& e) {
        throw ConfigurationException("adapter " + name + ": prepare.args[0]: " + e.what());
    }
    std::vector<std::string> stamps;
    for (const auto& input : step.inputs) stamps.push_back(fill(input, values) + "=" + input_stamp(fill(input, values)));
    const fs::path marker = fs::path(utils::get_cache_dir()) / "adapter-prepare" / name /
                            (adapter::specs::prepare_key(argv, stamps) + ".done");

    static std::mutex prepare_mutex;  // two loads of one model must not prepare at the same time
    std::lock_guard<std::mutex> lock(prepare_mutex);
    std::error_code ec;
    if (fs::exists(marker, ec)) {
        LOG(INFO, "Adapter") << name << ": prepare already done for these inputs (" << marker.filename().string() << ")"
                             << std::endl;
        return;
    }
    LOG(INFO, "Adapter") << name << ": prepare: " << argv[0] << " (timeout " << step.timeout_s << " s)" << std::endl;
    std::deque<std::string> last;
    const auto started = std::chrono::steady_clock::now();
    const int code = utils::ProcessManager::run_process_with_output(
        argv[0], std::vector<std::string>(argv.begin() + 1, argv.end()),
        [&](const std::string& line) {
            LOG(INFO, "Adapter") << name << " prepare: " << line << std::endl;
            last.push_back(line);
            if (last.size() > 20) last.pop_front();
            return !cancelled();
        },
        spec_dir.string(), static_cast<int>(step.timeout_s));
    if (code != 0) {
        const auto took = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
        std::string why = took.count() >= step.timeout_s ? "timed out after " + std::to_string(step.timeout_s) + " s"
                                                          : "exit " + std::to_string(code);
        std::string tail;
        for (const auto& line : last) tail += "\n  " + line;
        throw ConfigurationException("adapter " + name + ": prepare failed (" + why + ")" +
                                     (tail.empty() ? std::string(", no output") : "; last lines:" + tail));
    }
    fs::create_directories(marker.parent_path(), ec);
    std::ofstream(marker) << json{{"argv", argv}, {"inputs", stamps}}.dump(1) << "\n";
    LOG(INFO, "Adapter") << name << ": prepare done" << std::endl;
}

}  // namespace

InstallParams AdapterServer::get_install_params(const std::string& /*backend*/, const std::string& /*version*/) {
    return {};  // engines are installed by their adapter (e.g. the Lemonade Stand), never by lemond
}

AdapterServer::AdapterServer(const std::string& log_level, ModelManager* model_manager,
                             BackendManager* backend_manager)
    : WrappedServer("adapter", log_level, model_manager, backend_manager) {}

void AdapterServer::load(const std::string& model_name, const ModelInfo& model_info,
                         const RecipeOptions& options, bool /*do_not_upgrade*/) {
  try {
    const auto [name, model_path] = adapter::split_checkpoint(model_info.checkpoint());
    if (name.empty()) {
        throw std::runtime_error("adapter model '" + model_name + "' has no spec: write its checkpoint as "
                                 "\"<spec>::<model path>\"");
    }
    fs::path spec_dir;
    const json spec = read_spec(name, spec_dir);
    spec_name_ = name;
    const auto checked = adapter::specs::check_spec(spec);
    if (checked.level_cap == 0) {
        std::string problems;
        for (size_t i = 0; i < checked.problems.size(); ++i) {
            if (i > 0) problems += "; ";
            problems += checked.problems[i];
        }
        throw std::runtime_error("adapter " + name + ": the spec cannot be used (" + problems +
                                 "); run the engine without control instead");
    }

    const json ready = spec.value("ready", json::object());
    const std::string ready_path = ready.value("path", std::string("/v1/models"));
    const long ready_timeout = ready.value("timeout_s", 600L);
    const std::string ready_field = ready.value("json_field", std::string{});
    auto load_started = std::chrono::steady_clock::now();
    // identity.unreported: the engine's /v1/models carries no owned_by (Strata, PolyStrata, Nauti-Strata), so the
    // check cannot pass; the spec still names the owner for make_adapter, and the plane skips the check.
    const json identity = spec.value("identity", json::object());
    const std::string owned_by =
        identity.value("unreported", false) ? std::string{} : identity.value("owned_by", std::string{});
    completion_ = spec.value("completion", true);
    telemetry_path_ = spec.value("telemetry_path", std::string{});
    memory_path_ = spec.value("memory_path", std::string{});
    budget_path_ = spec.value("budget_path", std::string{});

    std::string device;
    const std::string requested = option_text(options, "adapter_device");
    const std::string spec_default = spec.value("default_device_index", std::string{});
    std::vector<adapter::specs::Gpu> inventory;
    try {
        auto sys = create_system_info();
        for (auto& g : sys->get_nvidia_gpu_devices())
            if (g.index >= 0) inventory.push_back({g.index, g.uuid});
        for (auto& g : sys->get_amd_dgpu_devices())
            if (g.index >= 0) inventory.push_back({g.index, g.uuid});
    } catch (...) { }
    std::string gpu_uuids;
    try {
        const auto values = adapter::specs::device_values(requested, spec_default, inventory);
        device = values.device;
        gpu_uuids = values.gpu_uuids;
        device_uuid_ = gpu_uuids;
    } catch (const adapter::specs::SpecError& e) {
        throw std::runtime_error("adapter " + name + ": " + std::string(e.what()));
    }

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("adapter: failed to allocate a port for " + name);
    }
    std::map<std::string, std::string> subs = {
        {"{model}", model_path}, {"{model_path}", model_path}, {"{port}", std::to_string(port_)}, {"{device}", device},
        {"{spec_dir}", spec_dir.string()}};
    subs["{gpu_uuids}"] = gpu_uuids;
    // {ctx}: the context size lemond knows for this load, from the recipe's ctx_size, else the model's
    // registered window. When neither is known the placeholder stays unsupplied and the spec is refused.
    int64_t ctx = 0;
    const json ctx_option = options.get_option("ctx_size");
    if (ctx_option.is_number() && ctx_option.get<int64_t>() > 0) ctx = ctx_option.get<int64_t>();
    else if (model_info.max_context_window > 0) ctx = model_info.max_context_window;
    if (ctx > 0) subs["{ctx}"] = std::to_string(ctx);
    // Health reports cpu unless the spec asked for a card.
    device_type_ = adapter::specs::selects_gpu(spec, requested) ? DEVICE_GPU : DEVICE_CPU;
    run_prepare(name, spec, spec_dir, subs, [this] { return load_cancelled(); });
    load_started = std::chrono::steady_clock::now();  // the ready timeout does not count the prepare step

    std::vector<std::string> args;
    const std::string override_args = option_text(options, "adapter_args");
    if (!override_args.empty()) {
        args = split_args(fill(override_args, subs));
    } else {
        try {
            args = adapter::specs::build_argv(spec, subs);
        } catch (const adapter::specs::SpecError& e) {
            throw std::runtime_error("adapter " + name + ": " + std::string(e.what()));
        }
    }

    ServerCommand command;
    command.program = resolve_binary(fill(spec.value("binary", std::string{}), subs), spec_dir);
    command.args = std::move(args);
    const json env = spec.value("env", json::object());
    std::vector<std::string> allowed_dirs = {spec_dir.string()};
    if (fs::path(command.program).is_absolute()) {
        allowed_dirs.push_back(fs::path(command.program).parent_path().string());
        // When the binary's directory is named "bin", also push the engine bundle's root (parent of <binary>/bin) to allow LD_LIBRARY_PATH to point to sibling lib directories.
        if (fs::path(command.program).parent_path().filename() == "bin") {
            allowed_dirs.push_back(fs::path(command.program).parent_path().parent_path().string());
        }
    }
    try {
        for (auto& [k, v] : adapter::specs::build_env(env, subs, allowed_dirs)) {
            command.env.emplace_back(k, adapter::specs::launch_env_value(k, v));
        }
    } catch (const adapter::specs::SpecError& e) {
        throw std::runtime_error("adapter " + name + ": " + std::string(e.what()));
    }
    // Opt-in telemetry is declared in the spec, never hidden in its arguments: its env applies only when the
    // spec enables it or the user sets adapter_telemetry=on for this model.
    const json telemetry = spec.value("telemetry", json::object());
    if (telemetry.value("enabled", false) || option_text(options, "adapter_telemetry") == "on") {
        const json tenv = telemetry.value("env", json::object());
        try {
            for (auto& [k, v] : adapter::specs::build_env(tenv, subs, allowed_dirs)) {
                command.env.emplace_back(k, adapter::specs::launch_env_value(k, v));
            }
        } catch (const adapter::specs::SpecError& e) {
            throw std::runtime_error("adapter " + name + ": " + std::string(e.what()));
        }
        LOG(INFO, "Adapter") << name << ": telemetry on (" << telemetry.value("what", std::string("declared in the spec")) << ")" << std::endl;
    }
    // A catalogue can pin exactly which program runs.
    const std::string want_sha = spec.value("binary_sha256", std::string{});
    if (!want_sha.empty()) {
        std::string full = command.program;
        if (!fs::path(full).is_absolute()) full = utils::find_executable_in_path(full);
        std::string digest_error;
        if (full.empty() || !utils::HttpClient::verify_file_digest(full, "sha256", want_sha, digest_error)) {
            throw std::runtime_error("adapter " + name + ": " + command.program +
                                     " does not match the spec's binary_sha256 (" + digest_error + ")");
        }
    }
    command.port = port_;
    command.ready_endpoint = ready_path;

    LOG(INFO, "Adapter") << "Starting " << name << " (" << command.program << ") for " << model_path
                         << " on port " << port_ << " (device " << device << ")" << std::endl;
    const bool inherit_output = (log_level_ == "info") || (log_level_ == "debug");
    start_server(std::make_unique<NativeProcess>(ProcessOutput{inherit_output, true}), command, ready_timeout);

    // A health route can answer before the engine is ready (e.g. while weights load).
    if (!ready_field.empty()) {
        // The spec's timeout covers the whole load, including the wait start_server() already did.
        const auto deadline = load_started + std::chrono::seconds(ready_timeout);
        double last_progress = -1;
        while (true) {
            std::string last;
            adapter::specs::Readiness state{adapter::specs::ReadyState::Starting, -1};
            try {
                const json status = forward_get_request(ready_path, 5);
                last = status.value(ready_field, std::string{});
                state = adapter::specs::ready_state(status, ready);
            } catch (...) {
            }
            using adapter::specs::ReadyState;
            if (state.state == ReadyState::Ready || state.state == ReadyState::Warm) break;
            if (state.state == ReadyState::Loading && state.progress >= 0 && state.progress != last_progress) {
                last_progress = state.progress;
                LOG(INFO, "Adapter") << name << " loading " << static_cast<int>(state.progress * 100) << "%"
                                     << std::endl;
            }
            if (state.state == ReadyState::Error || std::chrono::steady_clock::now() > deadline) {
                stop_server();
                throw std::runtime_error("adapter " + name + ": engine did not become ready (" + ready_field +
                                         "=\"" + last + "\")");
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    // Refuse a mislabelled engine when the spec names its owner.
    if (!owned_by.empty()) {
        bool identified = false;
        try {
            for (const auto& m : forward_get_request("/v1/models", 10).value("data", json::array())) {
                if (m.value("owned_by", std::string{}) == owned_by) identified = true;
            }
        } catch (...) {
        }
        if (!identified) {
            stop_server();
            throw std::runtime_error("adapter " + name + ": /v1/models does not report owned_by=" + owned_by +
                                     "; refusing a mislabelled engine");
        }
    }
    LOG(INFO, "Adapter") << name << " ready on port " << port_ << std::endl;
  } catch (const nlohmann::json::exception& e) {
    throw std::runtime_error(std::string("adapter spec for '") + model_name + "': a field has the wrong type (" +
                             e.what() + ")");
  }
}

namespace {

#ifdef __linux__
// The engine's process and every descendant, read from /proc (a launcher script often starts the real server).
std::vector<int> process_tree(int root) {
    std::vector<int> out;
    if (root <= 0) return out;
    std::vector<int> todo{root};
    while (!todo.empty()) {
        const int pid = todo.back();
        todo.pop_back();
        out.push_back(pid);
        std::error_code ec;
        for (const auto& task : fs::directory_iterator("/proc/" + std::to_string(pid) + "/task", ec)) {
            std::ifstream children(task.path() / "children");
            for (int child; children >> child;) todo.push_back(child);
        }
    }
    return out;
}

std::string run(const char* cmd) {
    std::string text;
    if (FILE* p = popen(cmd, "r")) {
        char buf[512];
        while (fgets(buf, sizeof(buf), p)) text += buf;
        pclose(p);
    }
    return text;
}

// pid -> MiB for every process holding GPU memory, from nvidia-smi and rocm-smi when they are installed.
std::map<int, long> gpu_holders() {
    std::map<int, long> out;
    std::istringstream nv(run("nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits 2>/dev/null"));
    for (std::string line; std::getline(nv, line);) {
        int pid = 0;
        long mib = 0;
        if (std::sscanf(line.c_str(), "%d, %ld", &pid, &mib) == 2) out[pid] += mib;
    }
    std::istringstream amd(run("rocm-smi --showpids 2>/dev/null"));
    for (std::string line; std::getline(amd, line);) {
        int pid = 0;
        if (std::sscanf(line.c_str(), "%d", &pid) == 1 && pid > 1) out[pid] += 0;   // rocm-smi lists holders, not sizes
    }
    return out;
}

// MiB that the processes in `tree` hold on the NVIDIA card with this uuid; -1 when nvidia-smi says nothing.
long tree_mib_on(const std::vector<int>& tree, const std::string& uuid) {
    const std::string text =
        run("nvidia-smi --query-compute-apps=pid,gpu_uuid,used_memory --format=csv,noheader,nounits 2>/dev/null");
    if (text.empty()) return -1;
    long total = 0;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        int pid = 0;
        char id[128] = {};
        long mib = 0;
        if (std::sscanf(line.c_str(), "%d, %127[^,], %ld", &pid, id, &mib) == 3 && id == uuid &&
            std::find(tree.begin(), tree.end(), pid) != tree.end()) {
            total += mib;
        }
    }
    return total;
}

// Size in bytes of the NVIDIA card with this uuid; 0 when unknown.
uint64_t device_total_bytes(const std::string& uuid) {
    std::istringstream in(run("nvidia-smi --query-gpu=uuid,memory.total --format=csv,noheader,nounits 2>/dev/null"));
    for (std::string line; std::getline(in, line);) {
        char id[128] = {};
        unsigned long long mib = 0;
        if (std::sscanf(line.c_str(), "%127[^,], %llu", id, &mib) == 2 && id == uuid) return mib * 1024 * 1024;
    }
    return 0;
}
#endif

constexpr long kBudgetTimeoutSeconds = 5;

json get_json(const std::string& url) {
    const auto r = utils::HttpClient::get(url, {}, kBudgetTimeoutSeconds, utils::HttpSecurityPolicy::TrustedLoopback);
    if (r.status_code != 200) throw std::runtime_error("HTTP " + std::to_string(r.status_code) + " from " + url);
    return json::parse(r.body);
}

}  // namespace

bool AdapterServer::can_shrink() const {
    return !budget_path_.empty() && !device_uuid_.empty() && (device_type_ & DEVICE_GPU);
}

// R10: ask the engine to fit a smaller budget on its card instead of being unloaded. Any failure, timeout or
// short answer returns false and eviction goes on as without this hook.
bool AdapterServer::shrink_for_pressure(double used, double target) {
    constexpr uint64_t kMiB = 1024 * 1024;
    const std::string base = get_base_url();
    try {
        uint64_t engine_bytes = 0;
        if (!memory_path_.empty()) {
            engine_bytes = adapter::specs::device_bytes(
                adapter::specs::parse_memory_report(get_json(base + memory_path_)), device_uuid_);
        }
        uint64_t total = 0;
#ifdef __linux__
        const std::vector<int> tree = process_tree(get_process_id());
        const long gpu_before = tree_mib_on(tree, device_uuid_);
        if (engine_bytes == 0 && gpu_before > 0) engine_bytes = static_cast<uint64_t>(gpu_before) * kMiB;
        total = device_total_bytes(device_uuid_);
#endif
        const int64_t mib = adapter::specs::budget_target_mib(engine_bytes, total, used, target);
        if (mib < 0) {
            LOG(INFO, "Adapter") << spec_name_ << ": no budget to ask for (" << engine_bytes / kMiB << " MiB on "
                                 << device_uuid_ << "); unloading" << std::endl;
            return false;
        }
        const auto r = utils::HttpClient::post(base + budget_path_,
                                               adapter::specs::budget_request(device_uuid_, mib).dump(),
                                               {{"Content-Type", "application/json"}}, kBudgetTimeoutSeconds,
                                               utils::HttpSecurityPolicy::TrustedLoopback);
        if (r.status_code != 200) throw std::runtime_error("HTTP " + std::to_string(r.status_code));
        const auto reply = adapter::specs::parse_budget_reply(json::parse(r.body));
        // The engine's word is checked against what lemond can measure: its memory report read again, and
        // nvidia-smi for the engine's processes on that card when the card held them before.
        int64_t measured = -1;
        std::string how = "not measured";
        if (reply.accepted && !memory_path_.empty()) {
            measured = static_cast<int64_t>(adapter::specs::device_bytes(
                adapter::specs::parse_memory_report(get_json(base + memory_path_)), device_uuid_));
            how = "engine memory report";
        }
#ifdef __linux__
        if (reply.accepted && gpu_before > 0) {
            const long gpu_after = tree_mib_on(tree, device_uuid_);
            if (gpu_after >= 0) {
                measured = std::max<int64_t>(measured, static_cast<int64_t>(gpu_after) * kMiB);
                how += ", checked on the GPU";
            }
        }
#endif
        const bool met = adapter::specs::budget_met(reply, mib, measured);
        LOG(INFO, "Adapter") << spec_name_ << ": budget " << mib << " MiB on " << device_uuid_ << " "
                             << (met ? "met" : "not met") << " (accepted=" << (reply.accepted ? "yes" : "no")
                             << ", engine says " << reply.now_bytes / kMiB << " MiB, measured "
                             << (measured < 0 ? std::string("-") : std::to_string(measured / kMiB) + " MiB") << ", "
                             << how << (reply.reason.empty() ? "" : ", reason: " + reply.reason) << ")"
                             << (met ? "" : "; unloading") << std::endl;
        return met;
    } catch (const std::exception& e) {
        LOG(WARNING, "Adapter") << spec_name_ << ": budget request failed (" << e.what() << "); unloading" << std::endl;
        return false;
    }
}

void AdapterServer::unload() {
#ifdef __linux__
    const std::vector<int> tree = process_tree(get_process_id());
#endif
    stop_server();
#ifdef __linux__
    if (tree.empty()) return;
    // A device frees its memory a moment after the process exits; give it time before calling it a leak.
    std::map<int, long> held;
    for (int i = 0; i < 15; ++i) {
        held.clear();
        const auto holders = gpu_holders();
        for (int pid : tree) {
            if (const auto it = holders.find(pid); it != holders.end()) held[pid] = it->second;
        }
        if (held.empty()) {
            LOG(INFO, "Adapter") << spec_name_ << ": unloaded, device memory released (checked on the GPU)" << std::endl;
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    for (const auto& [pid, mib] : held) {
        LOG(WARNING, "Adapter") << spec_name_ << ": engine reported stopped but pid " << pid << " still holds "
                                << mib << " MiB on a GPU; stopping it" << std::endl;
        ::kill(pid, SIGKILL);
    }
#endif
}

json AdapterServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json AdapterServer::completion(const json& request) {
    if (!completion_) {
        throw std::runtime_error("adapter " + spec_name_ + ": this engine has no /v1/completions");
    }
    return forward_request("/v1/completions", request);
}

std::map<std::string, nlohmann::json> AdapterServer::get_additional_telemetry() {
    if (get_backend_port() <= 0 || (telemetry_path_.empty() && memory_path_.empty())) return {};
    std::map<std::string, nlohmann::json> out;
    if (!telemetry_path_.empty()) {
        try {
            out["adapter_stats"] = forward_get_request(telemetry_path_, 2);
        } catch (...) {
            out["adapter_stats"] = nullptr;
        }
    }
    // R9: the engine's memory by device and tier, plus the one number lemond's evictor would use.
    if (!memory_path_.empty()) {
        try {
            out["adapter_memory"] = adapter::specs::memory_telemetry(forward_get_request(memory_path_, 2));
        } catch (const std::exception& e) {
            out["adapter_memory"] = nlohmann::json{{"error", e.what()}};
        }
    }
    return out;
}

std::string AdapterServer::get_additional_telemetry_url() const {
    if (get_backend_port() <= 0 || telemetry_path_.empty()) return "";
    return get_base_url() + telemetry_path_;
}

}  // namespace backends
}  // namespace lemon

namespace lemon {
namespace backends {
namespace adapter {

namespace {
// The model is a local path ("<spec>::<path>") the engine reads itself: "downloaded" means it exists
// here and its spec is installed. Nothing is fetched by lemond.
class AdapterOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& /*ctx*/) const override {
        return split_checkpoint(info.checkpoint()).second;
    }
    bool is_downloaded(const ModelInfo& info, const BackendOpsContext& /*ctx*/) const override {
        const auto [name, path] = split_checkpoint(info.checkpoint());
        std::error_code ec;
        return !name.empty() && fs::exists(fs::path(path), ec) &&
               fs::exists(fs::path(adapters_dir()) / (name + ".json"), ec);
    }
    void download_model(const ModelInfo& info, bool /*do_not_upgrade*/, DownloadProgressCallback /*progress*/,
                        const BackendOpsContext& ctx) const override {
        if (is_downloaded(info, ctx)) return;
        const auto [name, path] = split_checkpoint(info.checkpoint());
        if (name.empty()) {
            throw std::runtime_error("adapter checkpoints are \"<spec>::<model path>\"; got '" + info.checkpoint() + "'");
        }
        throw std::runtime_error("adapter " + name + ": '" + path + "' or the spec " +
                                 (fs::path(adapters_dir()) / (name + ".json")).string() + " is missing on this machine");
    }
    // The context window a spec's model entry (or a registration's "adapter_ctx") declares; lemond clears
    // max_context_window before asking, and reads no metadata from an engine's model file itself.
    void populate_metadata(ModelInfo& info, const BackendOpsContext& /*ctx*/) const override {
        if (const auto ctx = info.extra<int64_t>("adapter_ctx", 0); ctx > 0) info.max_context_window = ctx;
    }
    // Each installed spec's "models" list. A model whose file is missing is still offered, and is_downloaded()
    // reports it as not downloaded, as for a registration. A broken spec or entry costs only itself.
    std::vector<ModelInfo> declared_models() const override {
        std::vector<ModelInfo> out;
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(adapters_dir(), ec)) {
            if (entry.path().extension() == ".json" && specs::valid_spec_name(entry.path().stem().string())) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            const std::string name = file.stem().string();
            json spec;
            try {
                std::ifstream in(file);
                in >> spec;
            } catch (const std::exception& e) {
                LOG(WARNING, "Adapter") << "adapter spec '" << name << "': not valid JSON, its models are not offered ("
                                        << e.what() << ")" << std::endl;
                continue;
            }
            const auto declared = specs::declared_models(spec, name, file.parent_path().string());
            for (const auto& line : declared.skipped) {
                LOG(WARNING, "Adapter") << line << "; entry skipped" << std::endl;
            }
            for (const auto& m : declared.models) {
                ModelInfo info;
                info.model_name = m.name;
                info.checkpoints["main"] = name + "::" + m.path;
                info.resolved_paths["main"] = m.path;
                info.recipe = descriptor.recipe;
                info.labels = m.labels.empty() ? std::vector<std::string>{"chat"} : m.labels;
                info.type = get_model_type_from_labels(info.labels);
                info.device = descriptor.default_device;
                if (m.ctx > 0) info.extras["adapter_ctx"] = m.ctx;
                info.suggested = true;
                const auto bytes = fs::is_regular_file(m.path, ec) ? fs::file_size(m.path, ec) : 0;
                if (!ec) info.size = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
                out.push_back(std::move(info));
            }
        }
        return out;
    }
};
}  // namespace

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<AdapterServer>(ctx);
}

const BackendSpec* spec() {
    return make_spec<AdapterServer>(descriptor);
}

const BackendOps* ops() {
    static const AdapterOps kOps;
    return &kOps;
}

}  // namespace adapter
}  // namespace backends
}  // namespace lemon
