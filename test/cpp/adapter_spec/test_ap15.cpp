// AP15: the prepare step. Run: bash test/cpp/adapter_spec/run.sh ap15
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static int body() {
    CHECK(!parse_prepare(json{{"binary", "x"}}).present);
    CHECK(!parse_prepare(json::array()).present);

    const json spec = {{"prepare", {{"args", {"{spec_dir}/prep.py", "--model", "{model_path}"}}}}};
    auto p = parse_prepare(spec);
    CHECK(p.present && p.timeout_s == 600 && p.inputs == (std::vector<std::string>{"{model_path}"}));
    const std::map<std::string, std::string> values = {
        {"{spec_dir}", "/specs"}, {"{model_path}", "/m/a b.gguf"}, {"{model}", "/m/a b.gguf"}};
    CHECK(prepare_argv(p, values) == (std::vector<std::string>{"/specs/prep.py", "--model", "/m/a b.gguf"}));

    auto q = parse_prepare(json{{"prepare", {{"args", {"prep"}}, {"timeout_s", 30}, {"inputs", {"/a", "/b"}}}}});
    CHECK(q.timeout_s == 30 && q.inputs == (std::vector<std::string>{"/a", "/b"}));
    CHECK(parse_prepare(json{{"prepare", {{"args", {"prep"}}, {"inputs", json::array()}}}}).inputs.empty());

    THROWS(parse_prepare(json{{"prepare", "prep.sh"}}), "prepare: not an object");
    THROWS(parse_prepare(json{{"prepare", json::object()}}), "prepare.args");
    THROWS(parse_prepare(json{{"prepare", {{"args", json::array()}}}}), "prepare.args");
    THROWS(parse_prepare(json{{"prepare", {{"args", "prep.sh --x"}}}}), "prepare.args");
    THROWS(parse_prepare(json{{"prepare", {{"args", {"prep", 3}}}}}), "prepare.args");
    THROWS(parse_prepare(json{{"prepare", {{"args", {"prep"}}, {"timeout_s", 0}}}}), "prepare.timeout_s");
    THROWS(parse_prepare(json{{"prepare", {{"args", {"prep"}}, {"timeout_s", "60"}}}}), "prepare.timeout_s");
    THROWS(parse_prepare(json{{"prepare", {{"args", {"prep"}}, {"timeout_s", 86401}}}}), "prepare.timeout_s");
    THROWS(parse_prepare(json{{"prepare", {{"args", {"prep"}}, {"inputs", "/a"}}}}), "prepare.inputs");
    THROWS(parse_prepare(json{{"prepare", {{"args", {"prep"}}, {"inputs", {1}}}}}), "prepare.inputs");

    // No port at prepare time; unknown placeholders are refused as in args. No shell: text stays one entry.
    THROWS(prepare_argv(parse_prepare(json{{"prepare", {{"args", {"prep", "{port}"}}}}}), values), "{port}");
    THROWS(prepare_argv(parse_prepare(json{{"prepare", {{"args", {"prep", "{shell}"}}}}}), values), "{shell}");
    CHECK(prepare_argv(parse_prepare(json{{"prepare", {{"args", {"prep", "$(rm -rf /); echo"}}}}}), values) ==
          (std::vector<std::string>{"prep", "$(rm -rf /); echo"}));

    const std::vector<std::string> argv = {"prep", "/m/a.gguf"};
    CHECK(prepare_key(argv, {"10:5"}) == prepare_key(argv, {"10:5"}));
    CHECK(prepare_key(argv, {"10:5"}).size() == 16);
    CHECK(prepare_key(argv, {"10:5"}) != prepare_key(argv, {"10:6"}));
    CHECK(prepare_key(argv, {"10:5"}) != prepare_key(argv, {"missing"}));
    CHECK(prepare_key({"prep", "/m/b.gguf"}, {"10:5"}) != prepare_key(argv, {"10:5"}));
    CHECK(prepare_key({"ab"}, {}) != prepare_key({"a", "b"}, {}));
    CHECK(prepare_key({"a"}, {"b"}) != prepare_key({"a", "b"}, {}));
    return report("ap15");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap15: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap15");
        return 2;
    }
}
