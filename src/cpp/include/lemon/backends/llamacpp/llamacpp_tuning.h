#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lemon/recipe_options.h"
#include "lemon/utils/custom_args.h"

namespace lemon {
namespace backends {
namespace llamacpp {

// Recipe option -> llama-server flag, for the RoPE / YaRN context-extension
// knobs. Each is passed to llama-server only when set; unset leaves
// llama-server's own default (which reads the value from the model).
inline const std::vector<std::pair<std::string, std::string>>& rope_option_flags() {
    static const std::vector<std::pair<std::string, std::string>> flags = {
        {"llamacpp_rope_scaling",      "--rope-scaling"},
        {"llamacpp_rope_scale",        "--rope-scale"},
        {"llamacpp_rope_freq_base",    "--rope-freq-base"},
        {"llamacpp_rope_freq_scale",   "--rope-freq-scale"},
        {"llamacpp_yarn_orig_ctx",     "--yarn-orig-ctx"},
        {"llamacpp_yarn_ext_factor",   "--yarn-ext-factor"},
        {"llamacpp_yarn_attn_factor",  "--yarn-attn-factor"},
        {"llamacpp_yarn_beta_slow",    "--yarn-beta-slow"},
        {"llamacpp_yarn_beta_fast",    "--yarn-beta-fast"},
    };
    return flags;
}

// llama-server flags for the RoPE / YaRN options set on `options`. A flag the
// user already spelled in llamacpp_args is skipped, so the explicit argument
// wins. Throws std::invalid_argument for an unknown --rope-scaling type.
inline std::vector<std::string> build_rope_args(const RecipeOptions& options,
                                                const std::vector<std::string>& user_tokens) {
    std::vector<std::string> args;
    for (const auto& [option, flag] : rope_option_flags()) {
        if (!options.has_option(option)) {
            continue;
        }
        if (utils::custom_args_has_flag(user_tokens, flag)) {
            continue;
        }
        const json value = options.get_option(option);
        if (option == "llamacpp_rope_scaling") {
            if (!value.is_string()) {
                continue;
            }
            const std::string type = value.get<std::string>();
            if (type != "none" && type != "linear" && type != "yarn") {
                throw std::invalid_argument(
                    "llamacpp_rope_scaling must be one of none, linear, yarn (got '" + type + "')");
            }
            args.push_back(flag);
            args.push_back(type);
        } else if (value.is_number()) {
            args.push_back(flag);
            args.push_back(value.dump());
        }
    }
    return args;
}

// Whether lemond adds --no-mmproj-offload itself. On a CPU backend the
// multimodal projector gains nothing from offload, so lemond turns it off, but
// the flag is not reserved: one the user spells in llamacpp_args (either
// --no-mmproj-offload or --mmproj-offload) is accepted and takes precedence.
// A GPU backend never gets the flag from lemond, so a user's passes through.
inline bool should_add_no_mmproj_offload(bool use_gpu, const std::vector<std::string>& user_tokens) {
    if (use_gpu) {
        return false;
    }
    return !utils::custom_args_has_flag(user_tokens, "--no-mmproj-offload") &&
           !utils::custom_args_has_flag(user_tokens, "--mmproj-offload");
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
