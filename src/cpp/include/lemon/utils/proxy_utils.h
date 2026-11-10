#pragma once

#include <cctype>
#include <cstdlib>
#include <functional>
#include <string>

#include <lemon/utils/http_security_policy.h>

namespace lemon {
namespace utils {

// Proxy settings for one outgoing request, taken from the conventional
// environment variables: HTTPS_PROXY / HTTP_PROXY / ALL_PROXY and NO_PROXY,
// each also accepted in lowercase. When both spellings are set the lowercase
// one wins, matching curl and Go.
struct ProxySettings {
    // Empty means connect directly.
    std::string proxy;
    // Comma-separated host list handed to CURLOPT_NOPROXY. Loopback is always
    // present so managed backends are never sent through a proxy.
    std::string no_proxy;
};

using EnvLookup = std::function<std::string(const char* name)>;

inline std::string process_env(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

namespace proxy_detail {

inline std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

// Lowercase spelling first, then uppercase. An empty value counts as unset.
inline std::string first_set(const EnvLookup& env, const char* lower, const char* upper) {
    for (const char* name : {lower, upper}) {
        const std::string value = trim(env(name));
        if (!value.empty()) return value;
    }
    return "";
}

inline bool starts_with_ignore_case(const std::string& text, const char* prefix) {
    for (size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= text.size() ||
            std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace proxy_detail

// Resolves the proxy for `url`. TrustedLoopback requests never use a proxy.
inline ProxySettings resolve_proxy_settings(const std::string& url,
                                            HttpSecurityPolicy policy,
                                            const EnvLookup& env = process_env) {
    ProxySettings settings;
    settings.no_proxy = "localhost,127.0.0.1,::1";
    if (policy == HttpSecurityPolicy::TrustedLoopback) {
        return settings;
    }

    std::string proxy;
    if (proxy_detail::starts_with_ignore_case(url, "https:")) {
        proxy = proxy_detail::first_set(env, "https_proxy", "HTTPS_PROXY");
    } else if (proxy_detail::starts_with_ignore_case(url, "http:")) {
        proxy = proxy_detail::first_set(env, "http_proxy", "HTTP_PROXY");
    }
    if (proxy.empty()) {
        proxy = proxy_detail::first_set(env, "all_proxy", "ALL_PROXY");
    }
    settings.proxy = proxy;

    const std::string user_no_proxy = proxy_detail::first_set(env, "no_proxy", "NO_PROXY");
    if (!user_no_proxy.empty()) {
        settings.no_proxy += "," + user_no_proxy;
    }
    return settings;
}

}  // namespace utils
}  // namespace lemon
