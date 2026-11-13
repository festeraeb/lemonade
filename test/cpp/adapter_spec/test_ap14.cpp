// AP14 (R10): what to ask a shrinkable engine for, and when its answer replaces an unload.
// Run: bash test/cpp/adapter_spec/run.sh ap14
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static int body() {
    const uint64_t MiB = 1024ULL * 1024ULL;
    // Device size known: give up (used - target) of the device.
    CHECK(budget_target_mib(8192 * MiB, 16384 * MiB, 0.95, 0.90) == 7372);  // 8192 - 819.2, rounded down
    CHECK(budget_target_mib(1024 * MiB, 16384 * MiB, 0.99, 0.90) == -1);  // would free everything
    // Device size unknown: scale the engine's own bytes by target / used.
    CHECK(budget_target_mib(1000 * MiB, 0, 1.0, 0.5) == 500);
    CHECK(budget_target_mib(1024 * MiB, 0, 0.95, 0.90) == 970);
    // Nothing to ask.
    CHECK(budget_target_mib(1024 * MiB, 0, 0.80, 0.90) == -1);
    CHECK(budget_target_mib(1024 * MiB, 0, 0.90, 0.90) == -1);
    CHECK(budget_target_mib(0, 16384 * MiB, 0.95, 0.90) == -1);
    CHECK(budget_target_mib(1024 * MiB, 0, -1.0, 0.90) == -1);
    CHECK(budget_target_mib(1024 * MiB, 0, 1.5, 0.90) == -1);
    CHECK(budget_target_mib(1024 * MiB, 0, 0.95, 0.0) == -1);

    const BudgetResult yes{true, 500 * MiB, ""};
    CHECK(budget_met(yes, 500, -1));
    CHECK(budget_met(yes, 500, static_cast<int64_t>(400 * MiB)));
    CHECK(!budget_met(yes, 499, -1));                                   // freed less than asked
    CHECK(!budget_met(yes, 500, static_cast<int64_t>(900 * MiB)));      // the measurement disagrees
    CHECK(!budget_met(BudgetResult{false, 0, "no"}, 500, -1));
    CHECK(!budget_met(yes, 0, -1));
    CHECK(!budget_met(yes, -1, -1));
    return report("ap14");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap14: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap14");
        return 2;
    }
}
