#include "adapter_spec.h"

#include <algorithm>
#include <cctype>
#include <sstream>

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

std::vector<std::string> build_argv(const json&, const std::map<std::string, std::string>&) {
    throw std::logic_error("AP2 build_argv not built yet");
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
        return result;
    }

    // Required: args (array) OR args_template (string)
    bool has_args = spec.contains("args") && spec["args"].is_array();
    bool has_args_template = spec.contains("args_template") && spec["args_template"].is_string();

    if (!has_args && !has_args_template) {
        result.level_cap = 0;
        result.problems.emplace_back("args or args_template missing");
        return result;
    }

    // Validate args array elements are strings
    if (has_args) {
        for (const auto& e : spec["args"]) {
            if (!e.is_string()) {
                result.level_cap = 0;
                result.problems.emplace_back("args must be array of strings");
                return result;
            }
        }
    }

    // Optional fields validation - collect all problems, don't return early
    if (spec.contains("spec_version")) {
        if (!spec["spec_version"].is_number_integer() || spec["spec_version"].get<int>() < 1) {
            result.level_cap = 0;
            result.problems.emplace_back("spec_version must be integer >= 1");
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

MemoryReport parse_memory_report(const json&) {
    throw std::logic_error("AP6 parse_memory_report not built yet");
}

uint64_t local_bytes(const MemoryReport&) {
    throw std::logic_error("AP6 local_bytes not built yet");
}

uint64_t device_bytes(const MemoryReport&, const std::string&) {
    throw std::logic_error("AP6 device_bytes not built yet");
}

json budget_request(const std::string&, int64_t) {
    throw std::logic_error("AP7 budget_request not built yet");
}

BudgetResult parse_budget_reply(const json&) {
    throw std::logic_error("AP7 parse_budget_reply not built yet");
}

Readiness ready_state(const json&, const json&) {
    throw std::logic_error("AP8 ready_state not built yet");
}

}  // namespace specs
}  // namespace adapter
}  // namespace backends
}  // namespace lemon
