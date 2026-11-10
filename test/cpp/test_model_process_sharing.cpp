// Contract test for models that share one backend process.
//
// Two registered models over the same checkpoints and load options are one
// process with two names: the second is attached to the first instead of being
// loaded again. Cases drive Router::find_shareable_server_locked and the name
// lookup through a friend hook, with stub servers that never spawn a subprocess.
// The full load_model path needs a real ModelManager and backend, which is an
// integration concern (see test/server_endpoints.py).

#include "lemon/router.h"
#include "lemon/runtime_config.h"
#include "lemon/wrapped_server.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

using nlohmann::json;

namespace lemon {

class StubWrappedServer : public WrappedServer {
public:
    StubWrappedServer(const std::string& model_name, const std::string& recipe,
                      const std::map<std::string, std::string>& checkpoints,
                      const json& options, ResidencyClass residency)
        : WrappedServer("stub", "error", nullptr, nullptr) {
        set_model_metadata(model_name, checkpoints.count("main") ? checkpoints.at("main") : "",
                           ModelType::LLM, DEVICE_CPU, RecipeOptions(recipe, options));
        set_checkpoints(checkpoints);
        set_residency_class(residency);
        set_state(ModelState::READY);
    }

    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    void unload() override {}
    bool is_backend_alive() const override { return alive_.load(); }
    void set_alive(bool alive) { alive_.store(alive); }

private:
    std::atomic<bool> alive_{true};
};

struct RoutingHelperTestHook {
    static StubWrappedServer* add_server(Router& r, std::unique_ptr<StubWrappedServer> s) {
        StubWrappedServer* raw = s.get();
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        r.loaded_servers_.push_back(std::move(s));
        return raw;
    }

    static WrappedServer* shareable(Router& r, const std::string& name, const std::string& recipe,
                                    const std::map<std::string, std::string>& checkpoints,
                                    const json& options,
                                    ResidencyClass residency = ResidencyClass::Standard) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        return r.find_shareable_server_locked(name, recipe, checkpoints,
                                              RecipeOptions(recipe, options), residency);
    }

    static WrappedServer* find(Router& r, const std::string& name) {
        std::lock_guard<std::mutex> lock(r.load_mutex_);
        return r.find_server_by_model_name(name);
    }
};

}  // namespace lemon

using lemon::ResidencyClass;
using lemon::Router;
using lemon::RoutingHelperTestHook;
using lemon::RuntimeConfig;
using lemon::StubWrappedServer;

static int failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

static const std::map<std::string, std::string> kQwen{{"main", "org/Qwen3-0.6B-GGUF:Q4_0"}};
static const std::map<std::string, std::string> kOther{{"main", "org/Other-GGUF:Q4_0"}};
static const std::map<std::string, std::string> kQwenVision{
    {"main", "org/Qwen3-0.6B-GGUF:Q4_0"}, {"mmproj", "mmproj-f16.gguf"}};

int main() {
    json cfg = json::object();
    cfg["max_loaded_models"] = 2;
    cfg["log_level"] = "error";
    RuntimeConfig config(cfg);
    RuntimeConfig::set_global(&config);

    {
        Router router(&config, nullptr, nullptr);
        const json opts = {{"ctx_size", 4096}};
        StubWrappedServer* a = RoutingHelperTestHook::add_server(
            router, std::make_unique<StubWrappedServer>("A", "llamacpp", kQwen, opts,
                                                        ResidencyClass::Standard));

        check("same checkpoints and options share the loaded process",
              RoutingHelperTestHook::shareable(router, "B", "llamacpp", kQwen, opts) == a);
        check("a different checkpoint does not share",
              RoutingHelperTestHook::shareable(router, "B", "llamacpp", kOther, opts) == nullptr);
        check("an extra checkpoint role (mmproj) does not share",
              RoutingHelperTestHook::shareable(router, "B", "llamacpp", kQwenVision, opts) ==
                  nullptr);
        check("different load options do not share",
              RoutingHelperTestHook::shareable(router, "B", "llamacpp", kQwen,
                                               {{"ctx_size", 8192}}) == nullptr);
        check("a different recipe does not share",
              RoutingHelperTestHook::shareable(router, "B", "whispercpp", kQwen, opts) ==
                  nullptr);
        check("a different residency class does not share",
              RoutingHelperTestHook::shareable(router, "B", "llamacpp", kQwen, opts,
                                               ResidencyClass::RoutingHelper) == nullptr);
        check("a model with no checkpoints never shares",
              RoutingHelperTestHook::shareable(router, "B", "llamacpp", {}, opts) == nullptr);
        check("the pinned flag is not a load option",
              RoutingHelperTestHook::shareable(router, "B", "llamacpp", kQwen,
                                               {{"ctx_size", 4096}, {"pinned", true}}) == a);

        check("an unattached name finds no server", RoutingHelperTestHook::find(router, "B") == nullptr);
        a->add_model_alias("B");
        check("an attached name finds the shared process",
              RoutingHelperTestHook::find(router, "B") == a);
        check("the primary name still finds it", RoutingHelperTestHook::find(router, "A") == a);
        check("the process keeps the name that started it", a->get_model_name() == "A");
        check("a name already attached is not offered a second share",
              RoutingHelperTestHook::shareable(router, "B", "llamacpp", kQwen, opts) == nullptr);
        a->add_model_alias("B");
        check("attaching a name twice records it once", a->get_model_aliases().size() == 1);

        check("removing an alias leaves the process", a->remove_model_alias("B") &&
                                                          RoutingHelperTestHook::find(router, "B") == nullptr &&
                                                          RoutingHelperTestHook::find(router, "A") == a);
        check("removing an unknown alias reports false", !a->remove_model_alias("B"));

        a->set_alive(false);
        check("a dead process is not shared",
              RoutingHelperTestHook::shareable(router, "C", "llamacpp", kQwen, opts) == nullptr);
    }

    RuntimeConfig::set_global(nullptr);
    if (failures == 0) {
        std::printf("\nAll model process sharing tests passed.\n");
    } else {
        std::printf("\n%d model process sharing test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
