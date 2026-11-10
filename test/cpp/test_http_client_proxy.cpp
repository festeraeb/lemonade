// Proxy handling for lemon::utils::HttpClient (issue #2108).
//
// Two layers are covered: the pure resolver that maps the environment to a
// proxy for one URL, and the real libcurl path, where a local listener stands
// in for the proxy and must see the request. No external network is used; the
// proxied origin is a name that cannot resolve, so only a proxy can answer it.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release CI build, where -DNDEBUG no-ops assert().

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>

#include <lemon/utils/http_client.h>
#include <lemon/utils/proxy_utils.h>

using lemon::utils::HttpClient;
using lemon::utils::HttpSecurityPolicy;
using lemon::utils::ProxySettings;
using lemon::utils::resolve_proxy_settings;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const std::string& name) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", name.c_str());
    cond ? ++g_passed : ++g_failed;
}

lemon::utils::EnvLookup fake_env(std::map<std::string, std::string> vars) {
    return [vars = std::move(vars)](const char* name) {
        auto it = vars.find(name);
        return it == vars.end() ? std::string() : it->second;
    };
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

void clear_proxy_env() {
    for (const char* name : {"HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
                             "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy"}) {
        set_env(name, nullptr);
    }
}

void test_resolver() {
    const HttpSecurityPolicy ext = HttpSecurityPolicy::AllowInsecureHttp;

    check(resolve_proxy_settings("https://huggingface.co/x", ext, fake_env({})).proxy.empty(),
          "no environment: direct connection");
    check(resolve_proxy_settings("https://huggingface.co/x", ext,
                                 fake_env({{"HTTPS_PROXY", "http://p:3128"}})).proxy ==
              "http://p:3128",
          "HTTPS_PROXY applies to https URLs");
    check(resolve_proxy_settings("https://huggingface.co/x", ext,
                                 fake_env({{"https_proxy", "http://lower:1"},
                                           {"HTTPS_PROXY", "http://upper:2"}})).proxy ==
              "http://lower:1",
          "lowercase spelling wins over uppercase");
    check(resolve_proxy_settings("http://example.com/x", ext,
                                 fake_env({{"HTTP_PROXY", "http://p:3128"}})).proxy ==
              "http://p:3128",
          "uppercase HTTP_PROXY applies to http URLs");
    check(resolve_proxy_settings("http://example.com/x", ext,
                                 fake_env({{"HTTPS_PROXY", "http://p:3128"}})).proxy.empty(),
          "HTTPS_PROXY is not used for http URLs");
    check(resolve_proxy_settings("HTTPS://example.com/x", ext,
                                 fake_env({{"ALL_PROXY", "socks5://s:1080"}})).proxy ==
              "socks5://s:1080",
          "ALL_PROXY is the fallback, scheme match is case-insensitive");
    check(resolve_proxy_settings("https://example.com/x", ext,
                                 fake_env({{"HTTPS_PROXY", "  "}})).proxy.empty(),
          "blank value counts as unset");

    const ProxySettings with_no = resolve_proxy_settings(
        "https://example.com/x", ext, fake_env({{"NO_PROXY", ".corp.example,10.0.0.0/8"}}));
    check(with_no.no_proxy.find(".corp.example,10.0.0.0/8") != std::string::npos,
          "NO_PROXY entries are forwarded");
    check(with_no.no_proxy.find("127.0.0.1") != std::string::npos &&
              with_no.no_proxy.find("localhost") != std::string::npos,
          "loopback is always exempt");

    check(resolve_proxy_settings("http://127.0.0.1:8000/x", HttpSecurityPolicy::TrustedLoopback,
                                 fake_env({{"HTTP_PROXY", "http://p:3128"},
                                           {"ALL_PROXY", "http://p:3128"}})).proxy.empty(),
          "managed loopback backends never use a proxy");
}

// Records the request target of anything sent to it, answers 200 "via-proxy".
struct FakeProxy {
    httplib::Server server;
    std::mutex mu;
    std::string target;
    std::string host_header;
    int hits = 0;
    int port = 0;
    std::thread thread;

    FakeProxy() {
        server.Get(".*", [this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard<std::mutex> lock(mu);
            ++hits;
            target = req.target;
            host_header = req.get_header_value("Host");
            res.set_content("via-proxy", "text/plain");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~FakeProxy() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
};

void test_through_libcurl() {
    FakeProxy proxy;
    const std::string proxy_url = "http://127.0.0.1:" + std::to_string(proxy.port);
    const std::string origin = "http://model-origin.invalid:8081/models/a.bin";

    clear_proxy_env();
    set_env("HTTP_PROXY", proxy_url.c_str());
    auto response = HttpClient::get(origin, {}, 10, HttpSecurityPolicy::AllowInsecureHttp);
    check(response.status_code == 200 && response.body == "via-proxy",
          "HttpClient::get is answered by the proxy");
    {
        std::lock_guard<std::mutex> lock(proxy.mu);
        check(proxy.hits == 1 && proxy.target == origin,
              "proxy received the absolute-form request target");
        check(proxy.host_header == "model-origin.invalid:8081", "proxy received the origin Host");
    }

    // download_file goes through the same policy, including per-hop setup.
    const std::string out = "test_http_client_proxy_download.bin";
    lemon::utils::DownloadOptions options;
    options.max_retries = 0;
    auto download = HttpClient::download_file(origin, out, nullptr, {}, options,
                                              HttpSecurityPolicy::AllowInsecureHttp);
    check(download.success, "download_file succeeds through the proxy");
    {
        std::lock_guard<std::mutex> lock(proxy.mu);
        check(proxy.hits == 2, "download_file request reached the proxy");
    }
    std::remove(out.c_str());

    // A request to loopback must reach its origin, not the proxy.
    httplib::Server origin_server;
    origin_server.Get("/hello", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("direct", "text/plain");
    });
    const int origin_port = origin_server.bind_to_any_port("127.0.0.1");
    std::thread origin_thread([&] { origin_server.listen_after_bind(); });
    origin_server.wait_until_ready();
    const int hits_before = [&] { std::lock_guard<std::mutex> l(proxy.mu); return proxy.hits; }();
    auto direct = HttpClient::get("http://127.0.0.1:" + std::to_string(origin_port) + "/hello",
                                  {}, 10, HttpSecurityPolicy::AllowInsecureHttp);
    check(direct.body == "direct", "loopback origin is reached directly");
    {
        std::lock_guard<std::mutex> lock(proxy.mu);
        check(proxy.hits == hits_before, "loopback request did not touch the proxy");
    }

    // NO_PROXY: the same unresolvable origin is now dialled directly and fails.
    set_env("NO_PROXY", "model-origin.invalid");
    bool failed_directly = false;
    try {
        HttpClient::get(origin, {}, 10, HttpSecurityPolicy::AllowInsecureHttp);
    } catch (const std::exception&) {
        failed_directly = true;
    }
    check(failed_directly, "NO_PROXY host bypasses the proxy");
    {
        std::lock_guard<std::mutex> lock(proxy.mu);
        check(proxy.hits == hits_before, "NO_PROXY request did not reach the proxy");
    }

    origin_server.stop();
    origin_thread.join();
    clear_proxy_env();
}

}  // namespace

int main() {
    printf("=== HttpClient proxy Unit Tests ===\n\n");
    clear_proxy_env();
    test_resolver();
    test_through_libcurl();
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
