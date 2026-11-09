#include "adapter_spec.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>
#include <unordered_set>

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
    const json&, const std::map<std::string, std::string>&, const std::vector<std::string>&) {
    throw std::logic_error("AP3 build_env not built yet");
}

DeviceChoice choose_device(const std::string&, const std::string&, const std::vector<Gpu>&) {
    throw std::logic_error("AP4 choose_device not built yet");
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

}  // namespace specs
}  // namespace adapter
}  // namespace backends
}  // namespace lemon
