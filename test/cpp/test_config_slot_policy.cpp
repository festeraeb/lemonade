#include <cstdio>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <lemon/backends/backend_descriptor.h>
#include <lemon/runtime_config.h>

#include "test_config_helpers.h"

using json = nlohmann::json;
using lemon::RuntimeConfig;
using lemon::SlotPolicy;
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
    std::puts("=== RUNNING SLOT POLICY CONFIG TESTS ===");

    // Default behaviour: with no override each recipe keeps its descriptor policy.
    RuntimeConfig plain(json::object());
    check(plain.slot_policy_for_recipe("halogen") == SlotPolicy::ExclusiveHost,
          "halogen defaults to exclusive_host");
    check(plain.slot_policy_for_recipe("llamacpp") == SlotPolicy::Standard,
          "llamacpp defaults to standard");
    check(plain.slot_policy_for_recipe("flm") == SlotPolicy::CoexistByType,
          "flm defaults to coexist_by_type");
    check(plain.slot_policy_for_recipe("no-such-recipe") == SlotPolicy::Standard,
          "unknown recipe is standard");
    check(RuntimeConfig(json::object({{"halogen", {{"slot_policy", ""}}}}))
              .slot_policy_for_recipe("halogen") == SlotPolicy::ExclusiveHost,
          "empty slot_policy keeps the descriptor policy");

    // Override: the issue's case.
    RuntimeConfig overridden(json::object({{"halogen", {{"slot_policy", "standard"}}}}));
    check(overridden.slot_policy_for_recipe("halogen") == SlotPolicy::Standard,
          "halogen.slot_policy=standard overrides exclusive_host");
    check(overridden.slot_policy_for_recipe("llamacpp") == SlotPolicy::Standard &&
              overridden.slot_policy_for_recipe("flm") == SlotPolicy::CoexistByType,
          "other recipes are unaffected");

    // Every named policy round-trips.
    for (const char* name : {"standard", "exclusive_npu", "coexist_by_type", "exclusive_host", "unmetered"}) {
        const auto parsed = lemon::slot_policy_from_string(name);
        check(parsed && std::string(lemon::slot_policy_to_string(*parsed)) == name,
              (std::string("round trip ") + name).c_str());
    }
    check(!lemon::slot_policy_from_string("bogus"), "unknown name does not parse");

    // A bad value in the file falls back instead of changing behaviour.
    check(RuntimeConfig(json::object({{"halogen", {{"slot_policy", "bogus"}}}}))
              .slot_policy_for_recipe("halogen") == SlotPolicy::ExclusiveHost,
          "unrecognised stored value keeps the descriptor policy");

    // Validation through set().
    const json base = json::object();
    check(!set_throws(base, json::object({{"halogen", {{"slot_policy", "standard"}}}})),
          "set accepts a valid policy");
    check(!set_throws(base, json::object({{"halogen", {{"slot_policy", ""}}}})),
          "set accepts clearing the override");
    check(set_throws(base, json::object({{"halogen", {{"slot_policy", "bogus"}}}})),
          "set rejects an unknown policy");
    check(set_throws(base, json::object({{"halogen", {{"slot_policy", 3}}}})),
          "set rejects a non-string");

    {
        RuntimeConfig rc(json::object());
        rc.set(json::object({{"halogen", {{"slot_policy", "standard"}}}}));
        check(rc.slot_policy_for_recipe("halogen") == SlotPolicy::Standard, "set is applied");
        rc.set(json::object({{"halogen", {{"slot_policy", ""}}}}));
        check(rc.slot_policy_for_recipe("halogen") == SlotPolicy::ExclusiveHost, "clearing restores the default");
    }

    return report_results("slot policy");
}
