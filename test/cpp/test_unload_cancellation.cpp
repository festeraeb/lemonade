// In-flight requests and unload (issue #3747).
//
// WrappedServer::begin_unload() is what Router::unload_model() calls before it
// waits for a model to go idle. A request running against the model must end
// promptly with a model_unloading error (HTTP 503) instead of running to
// completion, and a request that arrives afterwards must be refused the same way
// without reaching the backend. A loopback backend that streams slowly forever
// stands in for a long generation.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release CI build, where -DNDEBUG no-ops assert().

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <httplib.h>

#include <lemon/wrapped_server.h>

using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++g_failures;
}

class TestServer : public lemon::WrappedServer {
public:
    explicit TestServer(int port) : lemon::WrappedServer("test", "error", nullptr, nullptr) {
        port_ = port;
        set_state(lemon::ModelState::READY);
    }

    void load(const std::string&, const lemon::ModelInfo&, const lemon::RecipeOptions&, bool) override {}
    void unload() override {}
    bool is_backend_alive() const override { return true; }

    lemon::json test_forward(const std::string& endpoint, const lemon::json& request) {
        return forward_request(endpoint, request);
    }
};

// Streams one token every 50 ms until the client goes away or `stop` is set.
struct SlowBackend {
    httplib::Server server;
    std::thread thread;
    std::atomic<int> requests{0};
    std::atomic<int> chunks_sent{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> client_gone{false};
    int port = 0;

    SlowBackend() {
        server.Post("/v1/chat/completions", [this](const httplib::Request&, httplib::Response& res) {
            ++requests;
            res.set_chunked_content_provider(
                "text/event-stream", [this](size_t, httplib::DataSink& sink) {
                    if (stop.load()) return false;
                    const std::string chunk =
                        "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
                    if (!sink.write(chunk.data(), chunk.size())) {
                        client_gone = true;
                        return false;
                    }
                    ++chunks_sent;
                    std::this_thread::sleep_for(50ms);
                    return true;
                });
        });
        server.Post("/slow", [this](const httplib::Request&, httplib::Response& res) {
            ++requests;
            const auto deadline = std::chrono::steady_clock::now() + 30s;
            while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(20ms);
            }
            res.set_content("{}", "application/json");
        });
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    ~SlowBackend() {
        stop = true;
        server.stop();
        if (thread.joinable()) thread.join();
    }
};

void test_streaming_unload() {
    SlowBackend backend;
    TestServer server(backend.port);

    std::string received;
    bool done_called = false;
    httplib::DataSink sink;
    sink.write = [&](const char* data, size_t len) {
        received.append(data, len);
        return true;
    };
    sink.done = [&] { done_called = true; };
    sink.is_writable = [] { return true; };

    std::atomic<bool> finished{false};
    const auto start = std::chrono::steady_clock::now();
    std::thread request([&] {
        server.forward_streaming_request("/v1/chat/completions",
                                         R"({"model":"m","stream":true})", sink, true, 60);
        finished = true;
    });

    while (backend.chunks_sent < 3 && std::chrono::steady_clock::now() - start < 10s) {
        std::this_thread::sleep_for(10ms);
    }
    check(backend.chunks_sent >= 3 && !finished, "stream is running and has delivered tokens");

    server.begin_unload();
    const auto unload_at = std::chrono::steady_clock::now();
    while (!finished && std::chrono::steady_clock::now() - unload_at < 10s) {
        std::this_thread::sleep_for(10ms);
    }
    const auto ended_after = std::chrono::steady_clock::now() - unload_at;
    check(finished, "in-flight stream ends after begin_unload()");
    check(ended_after < 5s, "in-flight stream ends promptly, not at the 60 s timeout");
    request.join();

    check(done_called, "the stream is closed");
    check(received.find("\"type\":\"model_unloading\"") != std::string::npos,
          "client receives a model_unloading error event");
    check(received.find("\"status_code\":503") != std::string::npos,
          "the error event carries status 503");
    check(received.find("[DONE]") == std::string::npos,
          "no [DONE]: the truncated stream is not reported as complete");

    // A request arriving during teardown is refused without reaching the backend.
    const int requests_before = backend.requests;
    std::string refused;
    httplib::DataSink sink2;
    sink2.write = [&](const char* data, size_t len) {
        refused.append(data, len);
        return true;
    };
    sink2.done = [] {};
    sink2.is_writable = [] { return true; };
    server.forward_streaming_request("/v1/chat/completions", R"({"model":"m","stream":true})",
                                     sink2, true, 60);
    check(refused.find("model_unloading") != std::string::npos,
          "a new streaming request is refused with model_unloading");
    check(backend.requests == requests_before, "the refused request never reached the backend");
}

void test_non_streaming_unload() {
    SlowBackend backend;
    TestServer server(backend.port);

    lemon::json response;
    std::atomic<bool> finished{false};
    std::thread request([&] {
        response = server.test_forward("/slow", lemon::json::object());
        finished = true;
    });
    while (backend.requests < 1) std::this_thread::sleep_for(10ms);

    server.begin_unload();
    const auto unload_at = std::chrono::steady_clock::now();
    while (!finished && std::chrono::steady_clock::now() - unload_at < 10s) {
        std::this_thread::sleep_for(10ms);
    }
    check(finished && std::chrono::steady_clock::now() - unload_at < 5s,
          "in-flight non-streaming request is cancelled promptly");
    request.join();
    check(response.contains("error") && response["error"].value("type", "") == "model_unloading" &&
              response["error"].value("status_code", 0) == 503,
          "non-streaming request fails with model_unloading, status 503");

    const int requests_before = backend.requests;
    auto refused = server.test_forward("/slow", lemon::json::object());
    check(refused.contains("error") && refused["error"].value("type", "") == "model_unloading",
          "a new non-streaming request is refused with model_unloading");
    check(backend.requests == requests_before, "the refused request never reached the backend");
}

void test_unaffected_without_unload() {
    SlowBackend backend;
    TestServer server(backend.port);
    check(!server.is_unloading(), "a server that is not being unloaded reports so");

    std::string received;
    httplib::DataSink sink;
    sink.write = [&](const char* data, size_t len) {
        received.append(data, len);
        return true;
    };
    sink.done = [] {};
    sink.is_writable = [] { return true; };
    std::thread request([&] {
        try {
            server.forward_streaming_request("/v1/chat/completions",
                                             R"({"model":"m","stream":true})", sink, true, 60);
        } catch (const std::exception&) {
            // A backend that just ended the stream may ask for a retry; only
            // the absence of model_unloading matters here.
        }
    });
    while (backend.chunks_sent < 3) std::this_thread::sleep_for(10ms);
    backend.stop = true;  // backend ends the stream normally
    request.join();
    check(received.find("model_unloading") == std::string::npos,
          "without begin_unload() a stream is not interrupted");
}

}  // namespace

int main() {
    std::printf("=== Unload cancellation tests ===\n\n");
    test_streaming_unload();
    test_non_streaming_unload();
    test_unaffected_without_unload();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASSED" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
