#include "startup_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void write_config(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) throw std::runtime_error("Cannot write test configuration");
}
}

int main(int argc, char* argv[]) {
    namespace fs = std::filesystem;
    using namespace Corona::Startup;
    try {
        expect(argc == 2, "A fixture directory is required");
        const fs::path directory = fs::absolute(argv[1]);
        fs::create_directories(directory);
        const fs::path path = directory / fs::path(L"启动配置.json");
        fs::remove(path);
        const auto missing = read_startup_config(path, nullptr);
        expect(!missing.file_loaded && !missing.switch_profile, "Missing file must default to off");

        write_config(path, R"({"profiling":{"switch_profile":true}})");
        auto config = read_startup_config(path, nullptr);
        expect(config.file_loaded && config.switch_profile, "File must enable profiling");
        expect(!read_startup_config(path, "0").switch_profile, "Environment 0 must override file true");
        expect(!read_startup_config(path, "").switch_profile, "Empty environment keeps existing off semantics");
        expect(read_startup_config(path, "1").from_environment, "Environment source must be reported");
        apply_startup_config(config);
        expect(std::string(std::getenv("CORONA_SWITCH_PROFILE")) == "1", "Configuration must reach C++ getenv");

        write_config(path, R"({"profiling":{"switch_profile":false}})");
        config = read_startup_config(path, nullptr);
        expect(!config.switch_profile, "File must disable profiling");
        expect(read_startup_config(path, "1").switch_profile, "Environment 1 must override file false");
        apply_startup_config(config);
        expect(std::string(std::getenv("CORONA_SWITCH_PROFILE")) == "0", "Disabled configuration must reach getenv");

        for (const auto* valid : {"{}", R"({"profiling":{}})"}) {
            write_config(path, valid);
            expect(!read_startup_config(path, nullptr).switch_profile, "Omitted setting must default to off");
        }
        for (const auto* invalid : {"broken json", "[]", R"({"profiling":true})",
                                   R"({"profiling":{"switch_profile":"true"}})",
                                   R"({"profiling":{"switch_profiler":true}})"}) {
            write_config(path, invalid);
            bool rejected = false;
            try { (void)read_startup_config(path, nullptr); }
            catch (const std::runtime_error&) { rejected = true; }
            expect(rejected, "Invalid configuration must be diagnosed");
        }
        const auto original = fs::current_path();
        const auto first_directory = directory / "first-working-directory";
        const auto second_directory = directory / "second-working-directory";
        fs::create_directories(first_directory);
        fs::create_directories(second_directory);
        const auto first_config = first_directory / "CoronaEngine.json";
        const auto second_config = second_directory / "CoronaEngine.json";
        write_config(first_config, R"({"profiling":{"switch_profile":true}})");
        write_config(second_config, R"({"profiling":{"switch_profile":false}})");
        fs::current_path(first_directory);
        const auto first = read_startup_config(default_startup_config_path(), nullptr);
        fs::current_path(second_directory);
        const auto second = read_startup_config(default_startup_config_path(), nullptr);
        fs::remove(second_config);
        const auto absent = read_startup_config(default_startup_config_path(), nullptr);
        fs::current_path(original);
        expect(first.path == first_config && first.file_loaded && first.switch_profile,
               "First working directory must supply its enabled configuration");
        expect(second.path == second_config && second.file_loaded && !second.switch_profile,
               "Changing working directory must select its disabled configuration");
        expect(absent.path == second_config && !absent.file_loaded && !absent.switch_profile,
               "Missing working-directory configuration must use defaults");
        fs::remove(first_config);
        fs::remove(path);
        std::cout << "Startup defaults, file, environment priority, validation and path checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
