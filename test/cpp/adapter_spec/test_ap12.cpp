// AP12: selects_gpu (health device) and the {ctx} placeholder. Run: bash test/cpp/adapter_spec/run.sh ap12
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static int body() {
    // A CPU spec: no device requested, no default index, no device placeholder.
    const json cpu = {{"args", {"-m", "{model_path}", "--port", "{port}", "-c", "{ctx}"}},
                      {"env", {{"OMP_NUM_THREADS", "4"}}}};
    CHECK(!selects_gpu(cpu, ""));
    CHECK(selects_gpu(cpu, "0"));
    CHECK(selects_gpu(cpu, "GPU-abc"));

    json with_default = cpu;
    with_default["default_device_index"] = "1";
    CHECK(selects_gpu(with_default, ""));
    with_default["default_device_index"] = "";
    CHECK(!selects_gpu(with_default, ""));
    with_default["default_device_index"] = 0;
    CHECK(selects_gpu(with_default, ""));

    json with_arg = cpu;
    with_arg["args"] = {"--main-gpu", "{device}"};
    CHECK(selects_gpu(with_arg, ""));
    json with_env = cpu;
    with_env["env"] = {{"CUDA_VISIBLE_DEVICES", "{gpu_uuids}"}};
    CHECK(selects_gpu(with_env, ""));
    CHECK(!selects_gpu(json::array(), ""));

    // {ctx} is a known placeholder: filled when supplied, refused when lemond has no value.
    const std::map<std::string, std::string> with_ctx = {{"{model_path}", "/m"}, {"{port}", "9"}, {"{ctx}", "8192"}};
    const auto argv = build_argv(cpu, with_ctx);
    CHECK(argv.size() == 6 && argv[4] == "-c" && argv[5] == "8192");
    bool refused = false;
    try {
        build_argv(cpu, {{"{model_path}", "/m"}, {"{port}", "9"}});
    } catch (const SpecError& e) {
        refused = std::string(e.what()).find("{ctx}") != std::string::npos;
    }
    CHECK(refused);
    return report("ap12");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap12: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap12");
        return 2;
    }
}
