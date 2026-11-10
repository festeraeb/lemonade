#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <lemon/runtime_config.h>

#include "test_config_helpers.h"

using json = nlohmann::json;
using lemon::RuntimeConfig;
using test_helpers::check;
using test_helpers::report_results;

static bool set_throws(const json& cfg, const json& changes) {
    try {
        RuntimeConfig(cfg).set(changes);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

int main() {
    std::puts("=== RUNNING BACKEND PORT BASE CONFIG TESTS ===");

    unsetenv("LEMONADE_BACKEND_PORT_BASE");

    // Default behaviour: backend children are scanned for from 8001.
    check(RuntimeConfig(json::object()).backend_port_base() == 8001, "missing key -> 8001");
    check(RuntimeConfig(json::object({{"backend_port_base", 8001}})).backend_port_base() == 8001,
          "explicit 8001 -> 8001");
    check(RuntimeConfig::kDefaultBackendPortBase == 8001, "default constant is 8001");
    check(RuntimeConfig::backend_port_base_or_default() == 8001,
          "no global config -> default");

    // Config key.
    check(RuntimeConfig(json::object({{"backend_port_base", 18000}})).backend_port_base() == 18000,
          "config key 18000 -> 18000");

    // Environment variable wins over the config key; an unparsable one is ignored.
    setenv("LEMONADE_BACKEND_PORT_BASE", "19000", 1);
    check(RuntimeConfig(json::object({{"backend_port_base", 18000}})).backend_port_base() == 19000,
          "env var overrides config key");
    setenv("LEMONADE_BACKEND_PORT_BASE", "not-a-port", 1);
    check(RuntimeConfig(json::object({{"backend_port_base", 18000}})).backend_port_base() == 18000,
          "unparsable env var falls back to config key");
    unsetenv("LEMONADE_BACKEND_PORT_BASE");

    // Validation through set().
    const json base = json::object({{"backend_port_base", 8001}});
    check(!set_throws(base, json::object({{"backend_port_base", 18000}})), "set accepts 18000");
    check(set_throws(base, json::object({{"backend_port_base", "18000"}})), "rejects string");
    check(set_throws(base, json::object({{"backend_port_base", 80}})), "rejects privileged port");
    check(set_throws(base, json::object({{"backend_port_base", 70000}})), "rejects port above 65535");

    {
        RuntimeConfig rc(base);
        rc.set(json::object({{"backend_port_base", 18000}}));
        check(rc.backend_port_base() == 18000, "set is applied");
    }

    return report_results("backend port base");
}
