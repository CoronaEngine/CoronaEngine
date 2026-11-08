#include "fixture.h"
#include "hotfix_runtime/engine_hotfix.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>{input}, {}};
}
void write(const fs::path& path, const std::string& content) {
    const auto previous = fs::last_write_time(path);
    std::ofstream(path) << content;
    fs::last_write_time(path, (std::max)(fs::file_time_type::clock::now(), previous + std::chrono::seconds{2}));
}
struct RestoreSources {
    fs::path implementation{fs::path{FIXTURE_DIRECTORY} / "implementation.cpp"};
    fs::path base{FIXTURE_BASE_SOURCE};
    std::string implementation_text{read(implementation)};
    std::string base_text{read(base)};
    ~RestoreSources() {
        write(implementation, implementation_text);
        write(base, base_text);
    }
};
void await_build() {
    auto& system = vision::HotfixSystem::instance();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{120};
    while (system.is_working() && std::chrono::steady_clock::now() < deadline) {
        Corona::Hotfix::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    require(!system.is_working(), "background build timed out");
}
vision::ModuleInterface* staged_registry() {
    fs::path newest;
    for (const auto& entry : fs::recursive_directory_iterator(fs::path{FIXTURE_BUILD_DIRECTORY} / "hotfix-staged")) {
        if (!entry.is_regular_file() || entry.path().stem() != "fixture-normal-hotfix") continue;
#ifdef _WIN32
        if (entry.path().extension() != ".dll") continue;
#else
        if (entry.path().extension() != ".so" && entry.path().extension() != ".dylib") continue;
#endif
        if (newest.empty() || entry.last_write_time() > fs::last_write_time(newest)) newest = entry.path();
    }
    require(!newest.empty(), "no staged patch found");
#ifdef _WIN32
        auto module = LoadLibraryW(newest.c_str());
        auto entrypoint = reinterpret_cast<vision::ModuleInterface*(*)()>(GetProcAddress(module, "module_interface"));
        auto runtime = reinterpret_cast<vision::HotfixSystem*(*)()>(GetProcAddress(module, "fixture_patch_runtime"));
#else
        auto module = dlopen(newest.c_str(), RTLD_NOW | RTLD_LOCAL);
        auto entrypoint = reinterpret_cast<vision::ModuleInterface*(*)()>(dlsym(module, "module_interface"));
        auto runtime = reinterpret_cast<vision::HotfixSystem*(*)()>(dlsym(module, "fixture_patch_runtime"));
#endif
        require(entrypoint != nullptr, "patch has no private registry entrypoint");
        require(runtime != nullptr && runtime() == &vision::HotfixSystem::instance(), "patch does not share the runtime owner");
        return entrypoint();
}
}

int main() {
    try {
        RestoreSources restore;
        require(Corona::Hotfix::initialize(), "Engine backend initialization failed");
        auto& system = vision::HotfixSystem::instance();
        const auto initial_revision = Corona::Hotfix::revision();
        require(fixture_runtime() == &system, "host and normal DLL have different runtime singletons");
        require(fixture_registry() == &vision::ModuleInterface::instance(), "normal DLL registry is not shared");
        auto* original_constructor = vision::ModuleInterface::instance().find_constructor(vision::type_string<corona_fixture::Object>());
        require(original_constructor != nullptr, "normal DLL factory was not registered");
        const auto normal_time = fs::last_write_time(FIXTURE_NORMAL_DLL);
        const auto base_time = fs::last_write_time(FIXTURE_BASE_DLL);
        std::vector<std::pair<fs::path, fs::file_time_type>> snapshots;
        for (const auto& entry : fs::recursive_directory_iterator(fs::path{FIXTURE_BUILD_DIRECTORY} / "hotfix-link")) {
            if (entry.is_regular_file()) snapshots.emplace_back(entry.path(), entry.last_write_time());
        }
        require(!snapshots.empty(), "normal build did not prepare link snapshots");
        {
            vision::HotfixSlot<std::shared_ptr<corona_fixture::Object>> slot{
                original_constructor->construct_shared<corona_fixture::Object>()};
            slot->state = 41;
            require(slot->value() == 11, "initial implementation differs");
            // An invalid, stale dependency must never be rebuilt by a patch.
            write(restore.base, "this_original_dependency_must_not_be_recompiled\n");
            write(restore.implementation, "int fixture_implementation() { return 2; }\n");
            require(system.check_and_build(), "nonfirst source was not watched");
            await_build();
            require(Corona::Hotfix::revision() == initial_revision + 1, "successful batch did not advance revision once");
            require(slot->value() == 12 && slot->state == 41, "patch did not replace implementation and preserve state");
            require(fs::last_write_time(FIXTURE_NORMAL_DLL) == normal_time, "patch rebuilt the loaded original DLL");
            require(fs::last_write_time(FIXTURE_BASE_DLL) == base_time, "patch rebuilt its stale original dependency");
            for (const auto& [path, time] : snapshots) {
                require(fs::last_write_time(path) == time, "patch modified a stable link snapshot");
            }
            auto* patch_registry = staged_registry();
            require(patch_registry != fixture_registry(), "patch shares the initial registry");
            require(patch_registry->find_constructor(vision::type_string<corona_fixture::Object>()) != original_constructor,
                    "patch did not own a new factory");
            require(vision::ModuleInterface::instance().find_constructor(vision::type_string<corona_fixture::Object>()) == original_constructor,
                    "loading patch changed the initial factory registry");
            system.previous_version();
            require(Corona::Hotfix::revision() == initial_revision + 2, "rollback did not advance revision");
            require(slot->value() == 11 && slot->state == 41, "rollback did not preserve state");
            system.next_version();
            require(Corona::Hotfix::revision() == initial_revision + 3, "forward version did not advance revision");
            require(slot->value() == 12 && slot->state == 41, "forward version did not preserve state");
            auto* before_failure = slot.get();
            write(restore.implementation, "this_patch_must_fail_to_compile\n");
            require(system.check_and_build(), "invalid edit was not watched");
            await_build();
            require(Corona::Hotfix::revision() == initial_revision + 3, "failed build changed revision");
            require(slot.get() == before_failure && slot->value() == 12 && slot->state == 41,
                    "failed build changed the live object");
            system.previous_version();
            require(slot->value() == 11 && slot->state == 41, "failed build changed version history");
            system.next_version();
            require(slot->value() == 12 && slot->state == 41, "failed build changed forward history");
            write(restore.implementation, "int fixture_implementation() { return 3; }\n");
            require(system.check_and_build(), "failed build was acknowledged instead of remaining retryable");
            await_build();
            require(Corona::Hotfix::revision() == initial_revision + 6, "corrected batch did not advance revision once");
            require(slot->value() == 13 && slot->state == 41, "retry did not apply the corrected patch");
        }
        Corona::Hotfix::shutdown();
        const auto shutdown_revision = Corona::Hotfix::revision();
        require(Corona::Hotfix::initialize(), "runtime cannot initialize after shutdown");
        require(Corona::Hotfix::revision() == shutdown_revision, "initialization reset the monotonic revision");
        require(vision::HotfixSystem::instance().file_tool().has_target("fixture-normal-hotfix"), "watch registration was lost after shutdown");
        vision::HotfixSystem::instance().update_objects({});
        require(Corona::Hotfix::revision() == shutdown_revision, "empty notification changed revision");
        vision::HotfixSystem::instance().update_objects({original_constructor, original_constructor});
        require(Corona::Hotfix::revision() == shutdown_revision + 1, "reinitialized sentinel counted per object or was lost");
        Corona::Hotfix::shutdown();
        std::cout << "PASS: shared owner, private patch registry, reload, state, rollback, retry, source watches and stable dependencies\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
