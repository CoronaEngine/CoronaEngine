#include "engine_hotfix.h"
#include "engine_hotfix_config.h"
#include "hotfix/hotfix.h"
#include "horizon/hotfix/cmake_build_backend.h"
#ifdef _WIN32
#include "horizon/hotfix/windows_process_runner.h"
#else
#include "horizon/hotfix/posix_process_runner.h"
#endif
#include <chrono>
#include <thread>

namespace Corona::Hotfix {
namespace {
class RevisionObserver final : public vision::Observer {
    std::uint64_t& revision_;
public:
    explicit RevisionObserver(std::uint64_t& revision) : revision_(revision) {}
    void update_runtime_object(const vision::IObjectConstructor*) noexcept override {}
    void notified(const std::vector<const vision::IObjectConstructor*>& constructors) noexcept override {
        if (!constructors.empty()) ++revision_;
    }
};

struct Runtime {
#ifdef _WIN32
    horizon::hotfix::WindowsProcessRunner runner;
#else
    horizon::hotfix::PosixProcessRunner runner;
#endif
    std::shared_ptr<horizon::hotfix::CMakeBuildBackend> backend;
    std::vector<std::pair<std::filesystem::path, std::string>> targets;
    std::uint64_t revision{0};
    std::unique_ptr<RevisionObserver> revision_observer;
    bool initialized{false};
};
Runtime& runtime() {
    static Runtime state;
    return state;
}
}

void register_target(const char8_t* source, const char* target) {
    auto& state = runtime();
    for (const auto& entry : state.targets) {
        if (entry.second == target) return;
    }
    const std::filesystem::path source_path{source};
    state.targets.emplace_back(source_path, target);
    if (state.initialized) {
        vision::HotfixSystem::instance().register_target(source_path, target);
    }
}

bool initialize() {
    auto& state = runtime();
    if (state.initialized) return true;
    state.backend = std::make_shared<horizon::hotfix::CMakeBuildBackend>(
        state.runner, std::filesystem::path{Configuration::cmake}, Configuration::environment());
    auto& system = vision::HotfixSystem::instance();
    if (!system.set_build_backend(state.backend, std::filesystem::path{Configuration::build_directory}, Configuration::configuration)) {
        state.backend.reset();
        return false;
    }
    for (const auto& [source, target] : state.targets) {
        system.register_target(source, target);
    }
    state.revision_observer = std::make_unique<RevisionObserver>(state.revision);
    state.initialized = true;
    return true;
}

void pump() {
    if (runtime().initialized) vision::HotfixSystem::instance().execute_callback();
}

std::uint64_t revision() noexcept {
    return runtime().revision;
}

void shutdown() {
    auto& state = runtime();
    if (!state.initialized) return;
    auto& system = vision::HotfixSystem::instance();
    // Finish callback delivery before the caller releases the rendering device.
    // No module is unloaded: old versions can still own constructor/deleter code.
    while (system.is_working()) {
        system.execute_callback();
        if (system.is_working()) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    system.execute_callback();
    // Observer destruction deregisters through HotfixSystem::instance(). Do it
    // while that instance still exists, rather than resurrecting it at shutdown.
    state.revision_observer.reset();
    vision::HotfixSystem::destroy_instance();
    state.backend.reset();
    state.initialized = false;
}
}
