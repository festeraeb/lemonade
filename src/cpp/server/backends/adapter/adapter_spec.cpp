#include "adapter_spec.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
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

std::vector<std::string> build_argv(const json&, const std::map<std::string, std::string>&) {
    throw std::logic_error("AP2 build_argv not built yet");
}

std::vector<std::pair<std::string, std::string>> build_env(
    const json&, const std::map<std::string, std::string>&, const std::vector<std::string>&) {
    throw std::logic_error("AP3 build_env not built yet");
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
    bool is_digit = std::all_of(req.begin(), req.end(), ::isdigit);
    if (is_digit) {
        int idx = std::stoi(req);
        if (idx < 0 || static_cast<size_t>(idx) >= inventory.size()) {
            throw SpecError("Invalid device index: " + req);
        }
        return DeviceChoice{idx, inventory[idx].uuid};
    }

    // Check if input is a UUID (case-insensitive match)
    std::string lower_req = req;
    std::transform(lower_req.begin(), lower_req.end(), lower_req.begin(), ::tolower);

    for (const auto& g : inventory) {
        std::string lower_guuid = g.uuid;
        std::transform(lower_guuid.begin(), lower_guuid.end(), lower_guuid.begin(), ::tolower);
        if (lower_req == lower_guuid) {
            return DeviceChoice{g.index, g.uuid};
        }
    }

    throw SpecError("Invalid device uuid: " + req);
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
