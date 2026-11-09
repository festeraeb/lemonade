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
    const json& env, const std::map<std::string, std::string>& values, const std::vector<std::string>& allowed_dirs) {
    // Contract: "env is not an object (null or absent is fine: empty result)"
    if (!env.is_object()) return {};

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
            // Reconstruct the string from valid parts
            std::string new_val;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) new_val += ":";
                new_val += parts[i];
            }
            filled_val = new_val;
        }

        result.push_back({name, filled_val});
    }
    return result;
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

Readiness ready_state(const json&, const json&) {
    throw std::logic_error("AP8 ready_state not built yet");
}

}  // namespace specs
}  // namespace adapter
}  // namespace backends
}  // namespace lemon
