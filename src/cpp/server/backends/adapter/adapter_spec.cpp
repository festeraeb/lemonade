#include "adapter_spec.h"

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

SpecCheck check_spec(const json&) {
    throw std::logic_error("AP5 check_spec not built yet");
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
            throw SpecError("progress_field missing from ready_spec");
        }
        std::string progress_field = ready_spec["progress_field"].get<std::string>();

        if (!status.contains(progress_field)) {
            throw SpecError("progress_field '" + progress_field + "' missing from status");
        }

        const json& progress_val = status[progress_field];

        if (!progress_val.is_number()) {
            throw SpecError("progress_field '" + progress_field + "' is not a number");
        }

        double progress = progress_val.get<double>();

        if (progress >= 0.0 && progress <= 1.0) {
            result.progress = progress;
        } else if (progress > 1.0 && progress <= 100.0) {
            result.progress = progress / 100.0;
        } else {
            throw SpecError("progress_field '" + progress_field + "' value " + std::to_string(progress) + " out of range");
        }
    }

    return result;
}

}  // namespace specs
}  // namespace adapter
}  // namespace backends
}  // namespace lemon
