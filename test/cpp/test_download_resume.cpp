// Interrupted-download resume for lemon::utils::HttpClient (issue #4113).
//
// A loopback server sends half of a file and then drops the connection, once.
// The retry must keep the partial file, ask for the rest with a Range request,
// and finish with a file that matches its hash. The classifier test pins which
// libcurl failures keep the partial, including the HTTP/2 stream errors (curl
// 16 and 92) that cannot be produced from a plain-HTTP fixture.
//
// Checks use an explicit pass/fail counter (not assert()) so the test stays
// effective under the Release CI build, where -DNDEBUG no-ops assert().

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <httplib.h>

#include <lemon/utils/http_client.h>

namespace fs = std::filesystem;
using lemon::utils::DownloadOptions;
using lemon::utils::HttpClient;
using lemon::utils::HttpSecurityPolicy;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const std::string& name) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", name.c_str());
    cond ? ++g_passed : ++g_failed;
}

std::string make_payload(size_t size) {
    std::string data(size, '\0');
    unsigned state = 12345;
    for (auto& c : data) {
        state = state * 1664525u + 1013904223u;
        c = static_cast<char>(state >> 24);
    }
    return data;
}

// Plain FNV-1a is enough to compare two byte strings in a test.
uint64_t fnv(const std::string& data) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void test_classifier() {
    using lemon::utils::is_retryable_transfer_error;
    check(is_retryable_transfer_error(CURLE_HTTP2_STREAM), "curl 92 (HTTP/2 stream error) is retryable");
    check(is_retryable_transfer_error(CURLE_HTTP2), "curl 16 (HTTP/2 framing error) is retryable");
    check(is_retryable_transfer_error(CURLE_PARTIAL_FILE), "curl 18 (partial file) is retryable");
    check(is_retryable_transfer_error(CURLE_RECV_ERROR), "curl 56 (recv error) is retryable");
    check(!is_retryable_transfer_error(CURLE_URL_MALFORMAT), "malformed URL is not retryable");
    check(!is_retryable_transfer_error(CURLE_PEER_FAILED_VERIFICATION),
          "certificate failure is not retryable");
    check(!is_retryable_transfer_error(CURLE_OK), "success is not an error to retry");
}

void test_resume_after_drop() {
    const std::string payload = make_payload(256 * 1024);
    const size_t half = payload.size() / 2;

    std::mutex mu;
    std::vector<std::string> range_headers;
    std::atomic<int> requests{0};
    std::atomic<size_t> bytes_sent{0};
    std::atomic<bool> dropped{false};

    httplib::Server server;
    server.Get("/model.gguf", [&](const httplib::Request& req, httplib::Response& res) {
        ++requests;
        const std::string range = req.get_header_value("Range");
        {
            std::lock_guard<std::mutex> lock(mu);
            range_headers.push_back(range);
        }
        // httplib answers a Range request itself (206 and Content-Range) and
        // hands the provider absolute offsets into the full payload.
        const bool drop_this_one = !dropped.exchange(true);
        res.set_content_provider(
            payload.size(), "application/octet-stream",
            [&, drop_this_one](size_t offset, size_t length, httplib::DataSink& sink) {
                if (drop_this_one && offset >= half) {
                    return false;  // closes the connection mid-body
                }
                size_t n = length;
                if (drop_this_one) n = (std::min)(n, half - offset);
                sink.write(payload.data() + offset, n);
                bytes_sent += n;
                return true;
            });
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread thread([&] { server.listen_after_bind(); });
    server.wait_until_ready();

    const fs::path dir = fs::temp_directory_path() / "lemonade_download_resume_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string out = (dir / "model.gguf").string();

    DownloadOptions options;
    options.max_retries = 3;
    options.initial_retry_delay_ms = 50;
    options.max_retry_delay_ms = 100;

    auto result = HttpClient::download_file(
        "http://127.0.0.1:" + std::to_string(port) + "/model.gguf", out, nullptr, {}, options,
        HttpSecurityPolicy::AllowInsecureHttp);

    check(result.success, "download completes after the connection drops once");
    check(requests == 2, "exactly one retry was needed");
    {
        std::lock_guard<std::mutex> lock(mu);
        check(range_headers.size() == 2 && range_headers[0].empty() &&
                  range_headers[1].rfind("bytes=", 0) == 0 && range_headers[1] != "bytes=0-",
              "retry asked for the remainder with a Range header, not byte 0");
    }
    check(bytes_sent < payload.size() + payload.size() / 4,
          "bytes re-fetched are less than the whole file again");
    check(fs::exists(out) && !fs::exists(out + ".partial"), "partial file was promoted");
    check(fnv(read_file(out)) == fnv(payload) && fs::file_size(out) == payload.size(),
          "final file matches the served bytes");

    server.stop();
    thread.join();
    fs::remove_all(dir);
}

}  // namespace

int main() {
    printf("=== HttpClient download resume Unit Tests ===\n\n");
    test_classifier();
    test_resume_after_drop();
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
