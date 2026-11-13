#pragma once

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

// Runs an out-of-tree engine from its adapter spec (see adapter.h): start from the spec's
// arguments, wait for its ready probe, check its identity, proxy OpenAI requests, stop on unload.
class AdapterServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    AdapterServer(const std::string& log_level, ModelManager* model_manager,
                  BackendManager* backend_manager);
    ~AdapterServer() override;

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;

    // Stops the engine, then checks the GPUs rather than the engine's word: a process of the engine's
    // tree that still holds device memory is stopped, and the mismatch is logged (one adapter that does
    // not really unload must not break eviction for every other model).
    void unload() override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;

    std::map<std::string, nlohmann::json> get_additional_telemetry() override;
    std::string get_additional_telemetry_url() const override;

    // POSTs the spec's budget_path ({"device", "mib"}) and keeps the model only when the engine's answer and
    // what lemond can measure afterwards both fit the budget. Specs without budget_path, or on the CPU, are
    // never asked.
    bool can_shrink() const override;
    bool shrink_for_pressure(double used, double target) override;

private:
    std::string spec_name_;
    std::string telemetry_path_;
    std::string memory_path_;
    std::string budget_path_;
    std::string device_uuid_;

    // R6: the spec's health probe. After fail_after misses in a row the engine is reset through
    // request_backend_reset_from_watchdog(), the path lemond's own watchdog takes for a hung backend.
    void probe_loop(const std::string& path, long interval_s, long timeout_ms, int fail_after);
    void stop_probe();
    std::thread probe_thread_;
    std::mutex probe_mutex_;
    std::condition_variable probe_cv_;
    bool probe_stop_ = false;
    bool completion_ = true;
};

namespace adapter {
// "<spec>::<path>" -> {"<spec>", "<path>"}; {"", checkpoint} when there is no "::".
std::pair<std::string, std::string> split_checkpoint(const std::string& checkpoint);
// Where specs live: LEMONADE_ADAPTERS_DIR, else <config dir>/adapters.
std::string adapters_dir();

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<AdapterServer>(); }
}  // namespace adapter

}  // namespace backends
}  // namespace lemon
