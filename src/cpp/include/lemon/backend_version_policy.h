#pragma once

#include <string>
#include <vector>

// Pure decision logic for resolving a user's "latest" *_bin config pin into a
// concrete release tag. Kept free of I/O (no network, no filesystem, no global
// config) so the policy can be unit-tested in isolation; callers gather the
// inputs (live GitHub lookup result + installed version.txt) and apply the
// result. See resolve_user_version() in backend_manager.cpp.
namespace lemon {

struct LatestPinResolution {
    // Concrete release tag to use. Empty only when resolution failed, in which
    // case `error` explains why.
    std::string version;
    // Human-readable reason the pin could not be resolved; empty on success.
    std::string error;
    // True when `version` came from the installed binary rather than a live
    // GitHub lookup (so the caller can log a "falling back" warning).
    bool used_installed_fallback = false;
};

// Resolve a "latest" pin.
//
//   offline           — true when the caller is in offline mode and so did not
//                        attempt the live lookup; when true the GitHub result
//                        is ignored. Every other reason the live tag is absent
//                        (HTTP 504, network error, no_fetch_executables, etc.)
//                        is conveyed by an empty `fetched_latest`, not this flag.
//   fetched_latest     — tag returned by the live GitHub lookup; empty when the
//                        lookup failed (e.g. HTTP 504, network error) or was
//                        skipped.
//   installed_version  — contents of the installed version.txt for this
//                        (recipe, backend); empty when nothing is installed.
//
// Policy: prefer a freshly-fetched tag; otherwise fall back to the binary
// already installed so a transient GitHub failure (or offline mode) does not
// block loading a model that is otherwise ready. Only when neither is available
// does resolution fail.
inline LatestPinResolution resolve_latest_pin(const std::string& recipe,
                                              const std::string& backend,
                                              bool offline,
                                              const std::string& fetched_latest,
                                              const std::string& installed_version) {
    if (!offline && !fetched_latest.empty()) {
        return {fetched_latest, "", false};
    }
    if (!installed_version.empty()) {
        return {installed_version, "", true};
    }
    const std::string why = offline
        ? "offline mode and no installed version found"
        : "GitHub lookup failed and no installed version found";
    return {"", "Cannot resolve 'latest' for " + recipe + ":" + backend + ": " + why, false};
}

// A loaded model as seen by the *_bin change policy.
struct LoadedBackendUse {
    std::string model_name;
    std::string recipe;
    // The backend the model's recipe options pin; empty when it follows the
    // recipe default and so could resolve to the changed backend on next load.
    std::string pinned_backend;
};

struct BinChangePlan {
    // Models to unload before the new binary is installed, and to reload on it.
    std::vector<std::string> unload_and_reload;
    // Models left running on the binary they started with. They keep serving
    // requests (and their KV cache) until they are unloaded; only later loads
    // use the new binary.
    std::vector<std::string> keep_running;
};

// What happens to loaded models when a (recipe, backend) *_bin value changes
// (lemonade-sdk/lemonade#2161). Models of other recipes, and models pinned to a
// different backend, are never affected.
//
// binary_replaceable_while_running is true where the installer can replace a
// backend directory under a running process (Linux and macOS unlink the old
// files). Windows cannot delete a running executable, so there the affected
// models must be unloaded first, as they always were.
inline BinChangePlan plan_bin_change(const std::vector<LoadedBackendUse>& loaded,
                                     const std::string& recipe,
                                     const std::string& backend,
                                     bool binary_replaceable_while_running) {
    BinChangePlan plan;
    for (const auto& use : loaded) {
        if (use.model_name.empty() || use.recipe != recipe) continue;
        if (!use.pinned_backend.empty() && use.pinned_backend != backend) continue;
        (binary_replaceable_while_running ? plan.keep_running : plan.unload_and_reload)
            .push_back(use.model_name);
    }
    return plan;
}

} // namespace lemon
