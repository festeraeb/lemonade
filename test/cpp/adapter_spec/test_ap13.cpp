// AP13: models declared by the spec. Run: bash test/cpp/adapter_spec/run.sh ap13
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static bool skipped_has(const DeclaredModels& d, const std::string& needle) {
    for (const auto& s : d.skipped) {
        if (s.find(needle) != std::string::npos) return true;
    }
    return false;
}

static int body() {
    CHECK(declared_models(json{{"binary", "x"}}, "s", "/specs").models.empty());
    CHECK(declared_models(json{{"binary", "x"}}, "s", "/specs").skipped.empty());
    CHECK(declared_models(json::array(), "s", "/specs").models.empty());

    auto bad_type = declared_models(json{{"models", "qwen"}}, "eng", "/specs");
    CHECK(bad_type.models.empty() && bad_type.skipped.size() == 1);
    CHECK(skipped_has(bad_type, "adapter spec 'eng': models: not an array"));

    const json spec = {{"models", json::array({
        {{"name", "qwen3-0.6b"}, {"path", "/models/q.gguf"}, {"labels", {"chat", "custom"}}, {"ctx", 4096},
         {"note", "unknown fields are ignored"}},
        {{"name", "rel"}, {"path", "models/r.gguf"}},
        "not an object",
        {{"path", "/m/no-name.gguf"}},
        {{"name", "user.spoof"}, {"path", "/m/a.gguf"}},
        {{"name", "builtin.spoof"}, {"path", "/m/a.gguf"}},
        {{"name", "extra.spoof"}, {"path", "/m/a.gguf"}},
        {{"name", "-dash-first"}, {"path", "/m/a.gguf"}},
        {{"name", "has space"}, {"path", "/m/a.gguf"}},
        {{"name", "a/b"}, {"path", "/m/a.gguf"}},
        {{"name", std::string(129, 'a')}, {"path", "/m/a.gguf"}},
        {{"name", "nopath"}},
        {{"name", "emptypath"}, {"path", ""}},
        {{"name", "badlabels"}, {"path", "/m/a.gguf"}, {"labels", {"chat", 3}}},
        {{"name", "labelstring"}, {"path", "/m/a.gguf"}, {"labels", "chat"}},
        {{"name", "zeroctx"}, {"path", "/m/a.gguf"}, {"ctx", 0}},
        {{"name", "floatctx"}, {"path", "/m/a.gguf"}, {"ctx", 2.5}},
        {{"name", "qwen3-0.6b"}, {"path", "/m/dup.gguf"}},
        {{"name", "win"}, {"path", "C:\\models\\w.gguf"}},
    })}};
    auto d = declared_models(spec, "eng", "/specs/dir");
    CHECK(d.models.size() == 3);
    CHECK(d.models.size() == 3 && d.models[0].name == "qwen3-0.6b" && d.models[0].path == "/models/q.gguf");
    CHECK(d.models.size() == 3 && d.models[0].labels == (std::vector<std::string>{"chat", "custom"}));
    CHECK(d.models.size() == 3 && d.models[0].ctx == 4096);
    CHECK(d.models.size() == 3 && d.models[1].path == "/specs/dir/models/r.gguf" && d.models[1].ctx == 0);
    CHECK(d.models.size() == 3 && d.models[1].labels.empty());
    CHECK(d.models.size() == 3 && d.models[2].path == "C:\\models\\w.gguf");
    CHECK(d.skipped.size() == 16);
    CHECK(skipped_has(d, "adapter spec 'eng': models[2]: not an object"));
    CHECK(skipped_has(d, "models[3].name"));
    CHECK(skipped_has(d, "models[4].name"));
    CHECK(skipped_has(d, "models[5].name"));
    CHECK(skipped_has(d, "models[6].name"));
    CHECK(skipped_has(d, "models[7].name"));
    CHECK(skipped_has(d, "models[8].name"));
    CHECK(skipped_has(d, "models[9].name"));
    CHECK(skipped_has(d, "models[10].name"));
    CHECK(skipped_has(d, "models[11].path"));
    CHECK(skipped_has(d, "models[12].path"));
    CHECK(skipped_has(d, "models[13].labels"));
    CHECK(skipped_has(d, "models[14].labels"));
    CHECK(skipped_has(d, "models[15].ctx"));
    CHECK(skipped_has(d, "models[16].ctx"));
    CHECK(skipped_has(d, "models[17].name: 'qwen3-0.6b' repeats an earlier entry"));
    return report("ap13");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap13: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap13");
        return 2;
    }
}
