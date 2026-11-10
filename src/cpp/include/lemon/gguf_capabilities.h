#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "lemon/model_types.h"

namespace lemon {

struct GgufCapabilities {
    bool vision = false;
    bool tool_calling = false;
    bool mtp = false;
};

namespace gguf_capabilities_detail {

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace gguf_capabilities_detail

inline bool is_stable_vision_key(const std::string& key) {
    using namespace gguf_capabilities_detail;
    const std::string k = to_lower(key);
    return k == "general.architecture" || k == "general.basename" ||
           k == "general.name" || k == "general.finetune" ||
           contains(k, "clip.vision") || contains(k, "mmproj");
}

inline bool is_chat_template_key(const std::string& key) {
    using namespace gguf_capabilities_detail;
    const std::string k = to_lower(key);
    return k == "tokenizer.chat_template" || contains(k, ".chat_template");
}

/// Inspect a single GGUF metadata (key, value) pair for capability hints.
/// The key is checked against stable key patterns; the value is checked for
/// vision/tool-calling indicators only when the key matches.
inline void inspect_gguf_string(const std::string& key, const std::string& value, GgufCapabilities& caps) {
    using namespace gguf_capabilities_detail;
    const std::string k = to_lower(key);
    const std::string v = to_lower(value);

    if (is_stable_vision_key(k) &&
        (contains(v, "vision") || contains(v, "image") || contains(v, "mmproj") ||
         contains(v, "multimodal") || contains(v, "multi-modal") ||
         contains(v, "qwen2vl") || contains(v, "qwen2_5_vl") || contains(v, "qwen3vl") ||
         contains(v, "mllama") || contains(v, "llava") || contains(v, "pixtral") ||
         contains(v, "paligemma"))) {
        caps.vision = true;
    }

    if (is_chat_template_key(k) &&
        (contains(v, "tool_call") || contains(v, "tool-call") ||
         contains(v, "function_call") || contains(v, "function-call") ||
         contains(v, "<tool") || contains(v, "</tool") ||
         contains(v, " tools") || contains(v, "\"tools\"") ||
         contains(v, "'tools'"))) {
        caps.tool_calling = true;
    }
}

/// True when a chat template reads a thinking control: the `enable_thinking`
/// and `reasoning_effort` template variables, or the `<think>` tags a reasoning
/// model's template opens or closes. This is read from the template itself, so a
/// model that thinks is reported as one without anyone having to label it.
inline bool chat_template_supports_thinking(const std::string& chat_template) {
    using namespace gguf_capabilities_detail;
    return contains(chat_template, "enable_thinking") ||
           contains(chat_template, "reasoning_effort") ||
           contains(chat_template, "<think>") ||
           contains(chat_template, "</think>");
}

inline bool apply_gguf_capability_labels(std::vector<std::string>& labels, const GgufCapabilities& caps) {
    bool changed = false;
    if (caps.vision) changed = add_label_once(labels, "vision") || changed;
    if (caps.tool_calling) changed = add_label_once(labels, "tool-calling") || changed;
    if (caps.mtp) changed = add_label_once(labels, "mtp") || changed;
    return changed;
}

}  // namespace lemon
