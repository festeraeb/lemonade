#include "adapter_spec.h"

#include <cctype>
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

MemoryReport parse_memory_report(const json& body) {
    if (!body.is_object()) throw SpecError("body must be object");

    const json& devices_json = body["devices"];
    std::vector<DeviceMemory> devices;

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

            const auto& uuid_val = dev["uuid"];
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

            const auto& bytes_val = dev["bytes"];
            if (!bytes_val.is_number_integer() || bytes_val.get<int64_t>() < 0) {
                throw SpecError("device bytes must be a non-negative integer");
            }
            uint64_t bytes = static_cast<uint64_t>(bytes_val.get<int64_t>());

            bool evictable = false;
            const auto& evictable_val = dev["evictable"];
            if (evictable_val.is_boolean()) {
                evictable = evictable_val.get<bool>();
            }

            devices.push_back({uuid_lower, bytes, evictable});
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
        if (dev.uuid == uuid_lower) {
            return dev.bytes;
        }
    }
    return 0;
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
