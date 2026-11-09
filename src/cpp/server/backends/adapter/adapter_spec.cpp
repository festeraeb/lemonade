#include "adapter_spec.h"

#include <cctype>
#include <map>
#include <regex>
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
        std::vector<std::string> tokens = split_args(tmpl);
        for (const auto& token : tokens) {
            argv.push_back(replace_placeholders(token));
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
