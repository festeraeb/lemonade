// AP16 (R6): health probe with a latency budget. Run: bash test/cpp/adapter_spec/run.sh ap16
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static int body() {
    CHECK(!parse_health(json{{"binary", "x"}}).present);
    auto d = parse_health(json{{"health", {{"path", "/health"}}}});
    CHECK(d.present && d.path == "/health" && d.interval_s == 10 && d.timeout_ms == 2000 && d.fail_after == 3);
    auto p = parse_health(json{{"health", {{"path", "/v1/models"}, {"interval_s", 1}, {"timeout_ms", 500}, {"fail_after", 2}}}});
    CHECK(p.interval_s == 1 && p.timeout_ms == 500 && p.fail_after == 2);

    THROWS(parse_health(json{{"health", "/health"}}), "health: not an object");
    THROWS(parse_health(json{{"health", json::object()}}), "health.path");
    THROWS(parse_health(json{{"health", {{"path", "health"}}}}), "health.path");
    THROWS(parse_health(json{{"health", {{"path", 7}}}}), "health.path");
    THROWS(parse_health(json{{"health", {{"path", "/h"}, {"interval_s", 0}}}}), "health.interval_s");
    THROWS(parse_health(json{{"health", {{"path", "/h"}, {"interval_s", "5"}}}}), "health.interval_s");
    THROWS(parse_health(json{{"health", {{"path", "/h"}, {"timeout_ms", 10}}}}), "health.timeout_ms");
    THROWS(parse_health(json{{"health", {{"path", "/h"}, {"timeout_ms", 1.5}}}}), "health.timeout_ms");
    THROWS(parse_health(json{{"health", {{"path", "/h"}, {"fail_after", 0}}}}), "health.fail_after");
    THROWS(parse_health(json{{"health", {{"path", "/h"}, {"fail_after", 101}}}}), "health.fail_after");

    HealthCount c{3};
    CHECK(!c.record(200, 10, 500));
    CHECK(!c.record(0, 500, 500));      // no answer
    CHECK(!c.record(200, 900, 500));    // answered, but over the budget
    CHECK(c.record(503, 5, 500));       // third miss in a row
    HealthCount r{2};
    CHECK(!r.record(0, 1, 500));
    CHECK(!r.record(200, 499, 500));    // an answer within the budget resets the count
    CHECK(!r.record(0, 1, 500));
    CHECK(r.record(0, 1, 500));
    HealthCount one{1};
    CHECK(one.record(200, 501, 500));
    return report("ap16");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap16: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap16");
        return 2;
    }
}
