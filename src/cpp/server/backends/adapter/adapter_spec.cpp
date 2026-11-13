#include "adapter_spec.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace lemon {
namespace backends {
namespace adapter {
namespace specs {

bool valid_spec_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    }
    return name.find("..") == std::string::npos;
}

std::string fill(std::string text, const std::map<std::string, std::string>& subs) {
    for (const auto& [key, value] : subs) {
        for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + value.size())) {
            text.replace(at, key.size(), value);
        }
    }
    return text;
}

std::vector<std::string> split_args(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string word; in >> word;) {
        out.push_back(word);
    }
    return out;
}

bool contains(const json& values, const std::string& v) {
    if (!values.is_array()) return false;
    for (const auto& x : values) {
        if (x.is_string() && x.get<std::string>() == v) return true;
    }
    return false;
}

// ---- not built yet: each is one forge job (~/agentnotes/adapter-nexus-forge-plan-2026-11-09.md) ----

std::vector<std::string> build_argv(const json& spec, const std::map<std::string, std::string>& values) {
    std::vector<std::string> argv;

    // Helper to replace placeholders in a string
    auto replace_placeholders = [&](const std::string& s) -> std::string {
        std::string result;
        std::regex placeholder(R"(\{[A-Za-z_][A-Za-z0-9_]*\})");
        std::sregex_iterator it(s.begin(), s.end(), placeholder);
        std::sregex_iterator end;

        size_t last = 0;
        for (; it != end; ++it) {
            result.append(s.substr(last, it->position() - last));
            std::string token = it->str();
            // Check if token is a known placeholder
            if (std::find(kPlaceholders.begin(), kPlaceholders.end(), token) == kPlaceholders.end()) {
                throw SpecError("unexpected placeholder '" + token + "'");
            }
            // Check if value is supplied
            auto val_it = values.find(token);
            if (val_it == values.end()) {
                throw SpecError("placeholder '" + token + "' not supplied");
            }
            result += val_it->second;
            last = it->position() + it->length();
        }
        result.append(s.substr(last));
        return result;
    };

    if (spec.contains("args")) {
        if (!spec["args"].is_array()) {
            throw SpecError("args");
        }
        for (const auto& arg : spec["args"]) {
            if (!arg.is_string()) {
                throw SpecError("args");
            }
            argv.push_back(replace_placeholders(arg.get<std::string>()));
        }
    } else if (spec.contains("args_template")) {
        if (!spec["args_template"].is_string()) {
            throw SpecError("args_template");
        }
        if (spec.contains("spec_version") && !spec["spec_version"].is_number_integer()) {
            throw SpecError("spec_version");
        }
        int version = spec.contains("spec_version") ? spec["spec_version"].get<int>() : 1;
        if (version > 1) {
            throw SpecError("args_template");
        }
        std::string tmpl = spec["args_template"].get<std::string>();
        // Fill placeholders first, then split on whitespace
        std::string filled = replace_placeholders(tmpl);
        std::vector<std::string> tokens = split_args(filled);
        for (const auto& token : tokens) {
            argv.push_back(token);
        }
    }

    return argv;
}

std::vector<std::pair<std::string, std::string>> build_env(
    const json& env, const std::map<std::string, std::string>& values, const std::vector<std::string>& allowed_dirs) {
    // Contract: "env is not an object (null or absent is fine: empty result)"
    if (env.is_null() || env.is_discarded()) return {};
    if (!env.is_object()) {
        throw SpecError("env is not an object");
    }

    std::vector<std::pair<std::string, std::string>> result;
    // Iterate over env.items() in key order (JSON object preserves insertion order)
    for (const auto& [name, value] : env.items()) {
        // Contract: "a value is not a string"
        if (!value.is_string()) {
            throw SpecError("environment value for '" + name + "' is not a string");
        }
        std::string val_str = value.get<std::string>();

        // Contract: "a name is not allowed (named in the message)"
        bool allowed = false;
        for (const auto& p : kEnvPrefixes) {
            if (name.size() >= p.size() && name.substr(0, p.size()) == p) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            for (const auto& n : kEnvNames) {
                if (name == n) {
                    allowed = true;
                    break;
                }
            }
        }
        if (!allowed) {
            throw SpecError("environment variable name '" + name + "' is not allowed");
        }

        // Fill placeholders
        std::string filled_val = fill(val_str, values);

        // Special handling for LD_LIBRARY_PATH
        if (name == "LD_LIBRARY_PATH") {
            std::vector<std::string> parts;
            std::istringstream iss(filled_val);
            std::string part;
            while (std::getline(iss, part, ':')) {
                if (part.empty()) continue;
                // "an entry containing ".." is refused"
                if (part.find("..") != std::string::npos) {
                    throw SpecError("LD_LIBRARY_PATH entry '" + part + "' contains '..'");
                }
                // "not absolute"
                if (part.empty() || part[0] != '/') {
                    throw SpecError("LD_LIBRARY_PATH entry '" + part + "' is not absolute");
                }
                // "not inside one of allowed_dirs"
                bool ok = false;
                for (const auto& dir : allowed_dirs) {
                    if (part == dir || (part.size() > dir.size() && part.compare(0, dir.size() + 1, dir + "/") == 0)) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    throw SpecError("LD_LIBRARY_PATH entry '" + part + "' not inside allowed directory");
                }
                parts.push_back(part);
            }
            // Keep filled_val unchanged - validation passed, return original string
        }

        result.push_back({name, filled_val});
    }
    return result;
}

std::string launch_env_value(const std::string& name, const std::string& value) {
    if (name != "LD_LIBRARY_PATH") return value;
    std::string out;
    std::istringstream iss(value);
    std::string part;
    while (std::getline(iss, part, ':')) {
        if (part.empty()) continue;
        if (!out.empty()) out += ':';
        out += part;
    }
    return out;
}

DeviceChoice choose_device(const std::string& requested, const std::string& spec_default, const std::vector<Gpu>& inventory) {
    // Helper to trim leading/trailing whitespace
    auto trim = [](std::string s) -> std::string {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    };

    std::string req = trim(requested);
    std::string def = trim(spec_default);

    // Fallback chain: requested -> spec_default -> "0"
    if (req.empty()) req = def;
    if (req.empty()) req = "0";

    // Check if input is a decimal index
    bool is_digit = std::all_of(req.begin(), req.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    if (is_digit) {
        // Use from_chars to safely parse and detect overflow
        int idx = 0;
        auto [ptr, ec] = std::from_chars(req.data(), req.data() + req.size(), idx);
        if (ec != std::errc() || ptr != req.data() + req.size()) {
            throw SpecError("Invalid device index: " + req);
        }

        // Loop over inventory entries and compare g.index (not use as vector position)
        for (const auto& g : inventory) {
            if (g.index == idx) {
                return DeviceChoice{idx, g.uuid};
            }
        }
        throw SpecError("Invalid device index: " + req);
    }

    // Check if input is a UUID (case-insensitive match)
    std::string lower_req = req;
    std::transform(lower_req.begin(), lower_req.end(), lower_req.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    for (const auto& g : inventory) {
        std::string lower_guuid = g.uuid;
        std::transform(lower_guuid.begin(), lower_guuid.end(), lower_guuid.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (lower_req == lower_guuid) {
            return DeviceChoice{g.index, g.uuid};
        }
    }

    throw SpecError("Invalid device uuid: " + req);
}

SpecCheck check_spec(const json& spec) {
    SpecCheck result;
    result.level_cap = 4;
    result.problems.clear();

    // Not a JSON object
    if (!spec.is_object()) {
        result.level_cap = 0;
        result.problems.emplace_back("spec is not a JSON object");
        return result;
    }

    // Required: binary (non-empty string)
    if (!spec.contains("binary") || !spec["binary"].is_string() || spec["binary"].get<std::string>().empty()) {
        result.level_cap = 0;
        result.problems.emplace_back("binary missing or empty");
    }

    // Required: args (array) OR args_template (string)
    bool has_args = spec.contains("args") && spec["args"].is_array();
    bool has_args_template = spec.contains("args_template") && spec["args_template"].is_string();

    if (!has_args && !has_args_template) {
        result.level_cap = 0;
        result.problems.emplace_back("args or args_template missing");
    }

    // Validate args array elements are strings
    if (has_args) {
        for (const auto& e : spec["args"]) {
            if (!e.is_string()) {
                result.level_cap = 0;
                result.problems.emplace_back("args must be array of strings");
                break;
            }
        }
    }

    // Optional fields validation - collect all problems, don't return early
    if (spec.contains("spec_version")) {
        if (!spec["spec_version"].is_number_integer() || spec["spec_version"].get<int>() < 1) {
            result.level_cap = 0;
            result.problems.emplace_back("spec_version must be integer >= 1");
        } else if (spec["spec_version"].get<int>() > kSpecVersion) {
            result.level_cap = 0;
            result.problems.emplace_back("spec_version must be <= " + std::to_string(kSpecVersion));
        }
    }

    if (spec.contains("env")) {
        if (!spec["env"].is_object()) {
            result.level_cap = 0;
            result.problems.emplace_back("env must be object");
        }
    }

    if (spec.contains("ready")) {
        if (!spec["ready"].is_object()) {
            result.level_cap = 0;
            result.problems.emplace_back("ready must be object");
        }
    }

    if (spec.contains("telemetry")) {
        if (!spec["telemetry"].is_object()) {
            result.level_cap = 0;
            result.problems.emplace_back("telemetry must be object");
        }
    }

    if (spec.contains("telemetry_path")) {
        if (!spec["telemetry_path"].is_string()) {
            result.level_cap = 0;
            result.problems.emplace_back("telemetry_path must be string");
        }
    }

    if (spec.contains("memory_path")) {
        if (!spec["memory_path"].is_string()) {
            result.level_cap = 0;
            result.problems.emplace_back("memory_path must be string");
        }
    }

    if (spec.contains("budget_path")) {
        if (!spec["budget_path"].is_string()) {
            result.level_cap = 0;
            result.problems.emplace_back("budget_path must be string");
        }
    }

    if (spec.contains("binary_sha256")) {
        if (!spec["binary_sha256"].is_string()) {
            result.level_cap = 0;
            result.problems.emplace_back("binary_sha256 must be string");
        } else {
            std::string h = spec["binary_sha256"].get<std::string>();
            if (h.size() != 64 || !std::all_of(h.begin(), h.end(), [](char c) {
                return std::isxdigit(static_cast<unsigned char>(c));
            })) {
                result.level_cap = 0;
                result.problems.emplace_back("binary_sha256 must be 64 hex chars");
            }
        }
    }

    if (spec.contains("default_device_index")) {
        if (!spec["default_device_index"].is_string()) {
            result.level_cap = 0;
            result.problems.emplace_back("default_device_index must be string");
        }
    }

    // Unknown fields are ignored
    return result;
}

MemoryReport parse_memory_report(const json& body) {
    if (!body.is_object()) throw SpecError("body must be object");

    std::vector<DeviceMemory> devices;

    if (body.contains("devices")) {
        const json& devices_json = body.value("devices", json());
        if (devices_json.is_null()) {
            // "devices" may be absent (empty)
        } else if (devices_json.is_object()) {
            throw SpecError("devices must be array");
        } else if (!devices_json.is_array()) {
            throw SpecError("devices must be array");
        } else {
            std::unordered_set<std::string> seen_uuids;
            for (const auto& dev : devices_json) {
                if (!dev.is_object()) throw SpecError("device must be object");

                const auto& uuid_val = dev.value("uuid", json());
                if (!uuid_val.is_string() || uuid_val.get<std::string>().empty()) {
                    throw SpecError("device uuid missing or empty");
                }
                std::string uuid = uuid_val.get<std::string>();
                std::string uuid_lower;
                for (char c : uuid) {
                    uuid_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (seen_uuids.count(uuid_lower)) {
                    throw SpecError("duplicate uuid (case-insensitive)");
                }
                seen_uuids.insert(uuid_lower);

                const auto& bytes_val = dev.value("bytes", json());
                if (!bytes_val.is_number_integer() || bytes_val.get<int64_t>() < 0) {
                    throw SpecError("device bytes must be a non-negative integer");
                }
                uint64_t bytes = static_cast<uint64_t>(bytes_val.get<int64_t>());

                bool evictable = false;
                const auto& evictable_val = dev.value("evictable", json());
                if (evictable_val.is_boolean()) {
                    evictable = evictable_val.get<bool>();
                }

                devices.push_back({uuid, bytes, evictable});
            }
        }
    }

    uint64_t pinned_host = 0;
    if (body.contains("pinned_host")) {
        const auto& val = body.value("pinned_host", json(0));
        if (!val.is_number_integer() || val.get<int64_t>() < 0) {
            throw SpecError("pinned_host must be a non-negative integer");
        }
        pinned_host = static_cast<uint64_t>(val.get<int64_t>());
    }

    uint64_t remote = 0;
    if (body.contains("remote")) {
        const auto& val = body.value("remote", json(0));
        if (!val.is_number_integer() || val.get<int64_t>() < 0) {
            throw SpecError("remote must be a non-negative integer");
        }
        remote = static_cast<uint64_t>(val.get<int64_t>());
    }

    uint64_t disk = 0;
    if (body.contains("disk")) {
        const auto& val = body.value("disk", json(0));
        if (!val.is_number_integer() || val.get<int64_t>() < 0) {
            throw SpecError("disk must be a non-negative integer");
        }
        disk = static_cast<uint64_t>(val.get<int64_t>());
    }

    return MemoryReport{devices, pinned_host, remote, disk};
}

uint64_t local_bytes(const MemoryReport& report) {
    uint64_t sum = report.pinned_host;
    for (const auto& dev : report.devices) {
        sum += dev.bytes;
    }
    return sum;
}

uint64_t device_bytes(const MemoryReport& report, const std::string& uuid) {
    std::string uuid_lower;
    for (char c : uuid) {
        uuid_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const auto& dev : report.devices) {
        std::string dev_uuid_lower;
        for (char c : dev.uuid) {
            dev_uuid_lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (dev_uuid_lower == uuid_lower) {
            return dev.bytes;
        }
    }
    return 0;
}

json budget_request(const std::string& uuid, int64_t mib) {
    if (uuid.empty()) throw SpecError("empty device");
    if (mib < 0) throw SpecError("negative mib");
    return json{{"device", uuid}, {"mib", mib}};
}

BudgetResult parse_budget_reply(const json& body) {
    if (!body.is_object()) throw SpecError("body not object");
    if (!body.contains("accepted")) throw SpecError("missing accepted");
    if (!body["accepted"].is_boolean()) throw SpecError("accepted not bool");
    bool accepted = body["accepted"].get<bool>();
    if (accepted) {
        if (!body.contains("now_bytes")) throw SpecError("now_bytes missing");
        const auto& nb = body["now_bytes"];
        if (!nb.is_number_integer() || nb.get<int64_t>() < 0) throw SpecError("now_bytes negative");
        return BudgetResult{accepted, static_cast<uint64_t>(nb.get<int64_t>()), ""};
    }
    return BudgetResult{accepted, 0, body.value("reason", "")};
}

int64_t budget_target_mib(uint64_t engine_bytes, uint64_t device_total, double used, double target) {
    constexpr double kMiB = 1024.0 * 1024.0;
    if (!(used > 0.0 && used <= 1.0 && target > 0.0 && target < used) || engine_bytes == 0) return -1;
    double keep;
    if (device_total > 0) {
        keep = static_cast<double>(engine_bytes) - (used - target) * static_cast<double>(device_total);
    } else {
        keep = static_cast<double>(engine_bytes) * target / used;
    }
    const auto mib = static_cast<int64_t>(keep / kMiB);
    return mib > 0 ? mib : -1;
}

bool budget_met(const BudgetResult& reply, int64_t target_mib, int64_t measured_bytes) {
    if (!reply.accepted || target_mib <= 0) return false;
    const uint64_t limit = static_cast<uint64_t>(target_mib) * 1024 * 1024;
    if (reply.now_bytes > limit) return false;
    return measured_bytes < 0 || static_cast<uint64_t>(measured_bytes) <= limit;
}

Readiness ready_state(const json& status, const json& ready_spec) {
    Readiness result{ReadyState::Starting, -1};

    // Get the json_field name from ready_spec
    std::string json_field;
    if (ready_spec.contains("json_field") && ready_spec["json_field"].is_string()) {
        json_field = ready_spec["json_field"].get<std::string>();
    }

    // If no json_field: any object answer is Ready
    if (json_field.empty()) {
        if (status.is_object()) {
            result.state = ReadyState::Ready;
        }
        return result;
    }

    // Non-object status is Starting
    if (!status.is_object()) {
        return result;
    }

    // Check if json_field exists in status
    if (!status.contains(json_field)) {
        return result;
    }

    const json& status_val = status[json_field];

    // Non-object status value is Starting
    if (!status_val.is_string()) {
        return result;
    }

    std::string status_str = status_val.get<std::string>();

    // Check error_values
    if (ready_spec.contains("error_values") && ready_spec["error_values"].is_array()) {
        for (const auto& v : ready_spec["error_values"]) {
            if (v.is_string() && v.get<std::string>() == status_str) {
                result.state = ReadyState::Error;
                goto check_progress;
            }
        }
    }

    // Check warm_values
    if (ready_spec.contains("warm_values") && ready_spec["warm_values"].is_array()) {
        for (const auto& v : ready_spec["warm_values"]) {
            if (v.is_string() && v.get<std::string>() == status_str) {
                result.state = ReadyState::Warm;
                goto check_progress;
            }
        }
    }

    // Check ready_values
    if (ready_spec.contains("ready_values") && ready_spec["ready_values"].is_array()) {
        for (const auto& v : ready_spec["ready_values"]) {
            if (v.is_string() && v.get<std::string>() == status_str) {
                result.state = ReadyState::Ready;
                goto check_progress;
            }
        }
    }

    // Check loading_values
    if (ready_spec.contains("loading_values") && ready_spec["loading_values"].is_array()) {
        for (const auto& v : ready_spec["loading_values"]) {
            if (v.is_string() && v.get<std::string>() == status_str) {
                result.state = ReadyState::Loading;
                goto check_progress;
            }
        }
    }

    // Not in any list -> Starting
    return result;

check_progress:
    // Progress is read only while Loading
    if (result.state == ReadyState::Loading) {
        if (!ready_spec.contains("progress_field") || !ready_spec["progress_field"].is_string()) {
            return result;  // progress_field missing from ready_spec -> -1
        }
        std::string progress_field = ready_spec["progress_field"].get<std::string>();

        if (!status.contains(progress_field)) {
            return result;  // progress_field missing from status -> -1
        }

        const json& progress_val = status[progress_field];

        if (!progress_val.is_number()) {
            return result;  // not a number -> -1
        }

        double progress = progress_val.get<double>();

        if (progress >= 0.0 && progress <= 1.0) {
            result.progress = progress;
        } else if (progress > 1.0 && progress <= 100.0) {
            result.progress = progress / 100.0;
        }
        // else: out of range -> -1 (default)
    }

    return result;
}

json memory_telemetry(const json& body) {
    try {
        MemoryReport report = parse_memory_report(body);
        uint64_t local = local_bytes(report);
        json result;
        result["devices"] = json::array();
        for (const auto& d : report.devices) {
            result["devices"].push_back({{"uuid", d.uuid}, {"bytes", d.bytes}, {"evictable", d.evictable}});
        }
        result["pinned_host"] = report.pinned_host;
        result["remote"] = report.remote;
        result["disk"] = report.disk;
        result["local_bytes"] = local;
        return result;
    } catch (const std::exception& e) {
        return json{{"error", e.what()}};
    }
}


bool selects_gpu(const json& spec, const std::string& requested) {
    if (!requested.empty()) return true;
    if (spec.is_object() && spec.contains("default_device_index")) {
        const json& d = spec["default_device_index"];
        if (d.is_number() || (d.is_string() && !d.get<std::string>().empty())) return true;
    }
    for (const char* key : {"args", "env"}) {
        if (!spec.is_object() || !spec.contains(key)) continue;
        const std::string text = spec[key].dump();
        if (text.find("{device}") != std::string::npos || text.find("{gpu_uuids}") != std::string::npos) {
            return true;
        }
    }
    return false;
}

DeviceValues device_values(const std::string& requested, const std::string& spec_default, const std::vector<Gpu>& inventory) {
    // Helper to trim leading/trailing whitespace
    auto trim = [](std::string s) -> std::string {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    };

    // Trim the inputs
    std::string req = trim(requested);
    std::string def = trim(spec_default);

    // No card list on this host: nothing to check against.
    if (inventory.empty()) {
        if (req.empty()) req = def;
        if (req.empty()) req = "0";
        return DeviceValues{req, req};
    }

    // Otherwise, choose_device decides and its SpecError propagates
    DeviceChoice choice = choose_device(req, def, inventory);

    // device = std::to_string(choice.index)
    // gpu_uuids = choice.uuid, or device when choice.uuid is empty
    std::string device = std::to_string(choice.index);
    std::string gpu_uuids = choice.uuid.empty() ? device : choice.uuid;

    return DeviceValues{device, gpu_uuids};
}

PrepareStep parse_prepare(const json& spec) {
    PrepareStep step;
    if (!spec.is_object() || !spec.contains("prepare")) return step;
    const json& p = spec["prepare"];
    if (!p.is_object()) throw SpecError("prepare: not an object");
    if (!p.contains("args") || !p["args"].is_array() || p["args"].empty()) {
        throw SpecError("prepare.args: must be a non-empty array of strings (the program first)");
    }
    for (const auto& a : p["args"]) {
        if (!a.is_string()) throw SpecError("prepare.args: must be a non-empty array of strings (the program first)");
    }
    step.args = p["args"];
    if (p.contains("timeout_s")) {
        if (!p["timeout_s"].is_number_integer() || p["timeout_s"].get<int64_t>() < 1 ||
            p["timeout_s"].get<int64_t>() > 86400) {
            throw SpecError("prepare.timeout_s: must be an integer from 1 to 86400");
        }
        step.timeout_s = p["timeout_s"].get<long>();
    }
    step.inputs = {"{model_path}"};
    if (p.contains("inputs")) {
        if (!p["inputs"].is_array()) throw SpecError("prepare.inputs: must be an array of strings");
        step.inputs.clear();
        for (const auto& i : p["inputs"]) {
            if (!i.is_string()) throw SpecError("prepare.inputs: must be an array of strings");
            step.inputs.push_back(i.get<std::string>());
        }
    }
    step.present = true;
    return step;
}

std::vector<std::string> prepare_argv(const PrepareStep& step, const std::map<std::string, std::string>& values) {
    try {
        return build_argv(json{{"args", step.args}}, values);
    } catch (const SpecError& e) {
        throw SpecError(std::string("prepare.args: ") + e.what());
    }
}

std::string prepare_key(const std::vector<std::string>& argv, const std::vector<std::string>& stamps) {
    uint64_t h = 1469598103934665603ULL;
    const auto add = [&h](const std::string& text) {
        for (unsigned char c : text) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        h ^= 0xff;  // field separator, so {"ab"} and {"a", "b"} differ
        h *= 1099511628211ULL;
    };
    for (const auto& a : argv) add(a);
    add("--inputs--");
    for (const auto& s : stamps) add(s);
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
    return out;
}

HealthProbe parse_health(const json& spec) {
    HealthProbe probe;
    if (!spec.is_object() || !spec.contains("health")) return probe;
    const json& h = spec["health"];
    if (!h.is_object()) throw SpecError("health: not an object");
    if (!h.contains("path") || !h["path"].is_string() || h["path"].get<std::string>().rfind("/", 0) != 0) {
        throw SpecError("health.path: must be a string starting with /");
    }
    probe.path = h["path"].get<std::string>();
    const auto read = [&h](const char* key, int64_t lo, int64_t hi, auto& out) {
        if (!h.contains(key)) return;
        if (!h[key].is_number_integer() || h[key].get<int64_t>() < lo || h[key].get<int64_t>() > hi) {
            throw SpecError(std::string("health.") + key + ": must be an integer from " + std::to_string(lo) +
                            " to " + std::to_string(hi));
        }
        out = static_cast<std::decay_t<decltype(out)>>(h[key].get<int64_t>());
    };
    read("interval_s", 1, 3600, probe.interval_s);
    read("timeout_ms", 50, 600000, probe.timeout_ms);
    read("fail_after", 1, 100, probe.fail_after);
    probe.present = true;
    return probe;
}

bool HealthCount::record(int status, long elapsed_ms, long timeout_ms) {
    if (status == 200 && elapsed_ms <= timeout_ms) {
        misses = 0;
        return false;
    }
    return ++misses >= fail_after;
}

namespace {

bool valid_model_name(const std::string& name) {
    if (name.empty() || name.size() > 128 || !std::isalnum(static_cast<unsigned char>(name[0]))) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    }
    for (const char* prefix : {"user.", "extra.", "builtin."}) {
        if (name.rfind(prefix, 0) == 0) return false;
    }
    return true;
}

bool absolute_path(const std::string& p) {
    if (!p.empty() && (p[0] == '/' || p[0] == '\\')) return true;
    return p.size() >= 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' &&
           (p[2] == '/' || p[2] == '\\');
}

}  // namespace

DeclaredModels declared_models(const json& spec, const std::string& spec_name, const std::string& spec_dir) {
    DeclaredModels out;
    if (!spec.is_object() || !spec.contains("models")) return out;
    const std::string where = "adapter spec '" + spec_name + "': ";
    const json& models = spec["models"];
    if (!models.is_array()) {
        out.skipped.push_back(where + "models: not an array");
        return out;
    }
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < models.size(); ++i) {
        const json& entry = models[i];
        const std::string field = "models[" + std::to_string(i) + "]";
        if (!entry.is_object()) {
            out.skipped.push_back(where + field + ": not an object");
            continue;
        }
        DeclaredModel m;
        if (!entry.contains("name") || !entry["name"].is_string() ||
            !valid_model_name(entry["name"].get<std::string>())) {
            out.skipped.push_back(where + field + ".name: missing or not a valid model name");
            continue;
        }
        m.name = entry["name"].get<std::string>();
        if (!entry.contains("path") || !entry["path"].is_string() || entry["path"].get<std::string>().empty()) {
            out.skipped.push_back(where + field + ".path: missing or not a string");
            continue;
        }
        m.path = entry["path"].get<std::string>();
        if (!absolute_path(m.path)) m.path = spec_dir + "/" + m.path;
        if (entry.contains("labels")) {
            const json& labels = entry["labels"];
            bool ok = labels.is_array();
            if (ok) {
                for (const auto& l : labels) {
                    if (!l.is_string()) ok = false;
                    else m.labels.push_back(l.get<std::string>());
                }
            }
            if (!ok) {
                out.skipped.push_back(where + field + ".labels: not an array of strings");
                continue;
            }
        }
        if (entry.contains("ctx")) {
            if (!entry["ctx"].is_number_integer() || entry["ctx"].get<int64_t>() <= 0) {
                out.skipped.push_back(where + field + ".ctx: not a positive integer");
                continue;
            }
            m.ctx = entry["ctx"].get<int64_t>();
        }
        if (!seen.insert(m.name).second) {
            out.skipped.push_back(where + field + ".name: '" + m.name + "' repeats an earlier entry");
            continue;
        }
        out.models.push_back(std::move(m));
    }
    return out;
}

}  // namespace specs
}  // namespace adapter
}  // namespace backends
}  // namespace lemon
