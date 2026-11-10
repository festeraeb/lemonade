// llama-server flags derived from recipe options: RoPE / YaRN scaling.

#include <lemon/backends/llamacpp/llamacpp_tuning.h>
#include <lemon/recipe_options.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using lemon::RecipeOptions;
using lemon::backends::llamacpp::build_rope_args;

using Args = std::vector<std::string>;

static std::string join(const Args& v) {
    std::string out;
    for (const auto& s : v) out += (out.empty() ? "" : " ") + s;
    return out;
}

static int expect_args(const char* name, const Args& got, const Args& want) {
    const bool ok = got == want;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) std::printf("  got:  %s\n  want: %s\n", join(got).c_str(), join(want).c_str());
    return ok ? 0 : 1;
}

int main() {
    int failures = 0;

    // Default behaviour: nothing set means no extra flags.
    failures += expect_args("unset options add no flags",
                            build_rope_args(RecipeOptions("llamacpp", json::object()), {}), {});
    failures += expect_args("sentinel values (-1, empty) add no flags",
                            build_rope_args(RecipeOptions("llamacpp", {
                                {"llamacpp_rope_scaling", ""}, {"llamacpp_rope_scale", -1},
                                {"llamacpp_yarn_orig_ctx", -1}}), {}), {});

    // The issue's case: YaRN with a 4x scale.
    failures += expect_args("yarn with scale 4",
                            build_rope_args(RecipeOptions("llamacpp", {
                                {"llamacpp_rope_scaling", "yarn"}, {"llamacpp_rope_scale", 4},
                                {"llamacpp_yarn_orig_ctx", 262144}}), {}),
                            {"--rope-scaling", "yarn", "--rope-scale", "4",
                             "--yarn-orig-ctx", "262144"});

    failures += expect_args("fractional values keep their precision",
                            build_rope_args(RecipeOptions("llamacpp", {
                                {"llamacpp_rope_freq_base", 1000000.0},
                                {"llamacpp_rope_freq_scale", 0.25},
                                {"llamacpp_yarn_ext_factor", 0.5},
                                {"llamacpp_yarn_attn_factor", 1.1},
                                {"llamacpp_yarn_beta_slow", 1.0},
                                {"llamacpp_yarn_beta_fast", 32.0}}), {}),
                            {"--rope-freq-base", "1000000.0", "--rope-freq-scale", "0.25",
                             "--yarn-ext-factor", "0.5", "--yarn-attn-factor", "1.1",
                             "--yarn-beta-slow", "1.0", "--yarn-beta-fast", "32.0"});

    // A flag the user spelled in llamacpp_args wins and is not duplicated.
    failures += expect_args("user --rope-scale wins over the option",
                            build_rope_args(RecipeOptions("llamacpp", {
                                {"llamacpp_rope_scaling", "yarn"}, {"llamacpp_rope_scale", 4}}),
                                {"--rope-scale", "8"}),
                            {"--rope-scaling", "yarn"});
    failures += expect_args("user --flag=value form also wins",
                            build_rope_args(RecipeOptions("llamacpp", {
                                {"llamacpp_rope_scale", 4}}), {"--rope-scale=8"}), {});

    bool threw = false;
    try {
        build_rope_args(RecipeOptions("llamacpp", {{"llamacpp_rope_scaling", "bogus"}}), {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    std::printf("[%s] unknown rope scaling type is rejected\n", threw ? "PASS" : "FAIL");
    failures += threw ? 0 : 1;

    return failures == 0 ? 0 : 1;
}
