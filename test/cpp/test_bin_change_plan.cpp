// Standalone unit tests for lemon::plan_bin_change(), the pure policy that
// decides what happens to loaded models when a backend's *_bin value changes
// (lemonade-sdk/lemonade#2161): a running model keeps its binary, and only
// platforms that cannot replace a running executable unload it first.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release build, where -DNDEBUG compiles assert() away.
//
// Compile with:
//   g++ -std=c++17 -I src/cpp/include test/cpp/test_bin_change_plan.cpp -o bin_change_plan_test

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <lemon/backend_version_policy.h>

using lemon::LoadedBackendUse;
using lemon::plan_bin_change;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const std::string& name) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", name.c_str());
    cond ? ++g_passed : ++g_failed;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

int main() {
    const std::vector<LoadedBackendUse> loaded = {
        {"on-vulkan", "llamacpp", "vulkan"},
        {"on-default", "llamacpp", ""},
        {"on-rocm", "llamacpp", "rocm"},
        {"whisper", "whispercpp", "vulkan"},
        {"", "llamacpp", "vulkan"},
    };

    auto keep = plan_bin_change(loaded, "llamacpp", "vulkan", true);
    check(keep.unload_and_reload.empty(), "replaceable platform: nothing is evicted");
    check(has(keep.keep_running, "on-vulkan") && has(keep.keep_running, "on-default"),
          "replaceable platform: affected models keep running");
    check(!has(keep.keep_running, "on-rocm"), "a model pinned to another backend is not affected");
    check(!has(keep.keep_running, "whisper"), "a model of another recipe is not affected");
    check(keep.keep_running.size() == 2, "an unnamed entry is ignored");

    auto evict = plan_bin_change(loaded, "llamacpp", "vulkan", false);
    check(evict.keep_running.empty(), "non-replaceable platform: nothing can keep running");
    check(has(evict.unload_and_reload, "on-vulkan") && has(evict.unload_and_reload, "on-default") &&
              evict.unload_and_reload.size() == 2,
          "non-replaceable platform: exactly the affected models are unloaded for reload");

    auto none = plan_bin_change({}, "llamacpp", "vulkan", true);
    check(none.keep_running.empty() && none.unload_and_reload.empty(), "nothing loaded, nothing planned");

    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
