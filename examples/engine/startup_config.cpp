#include "startup_config.h"

#include <corona/utils/path_utils.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Corona::Startup {

std::filesystem::path default_startup_config_path() {
    return std::filesystem::current_path() / "CoronaEngine.json";
}

StartupConfig read_startup_config(const std::filesystem::path& path,
                                  const char* switch_profile_environment) {
    StartupConfig config;
    config.path = std::filesystem::absolute(path);
    try {
        if (std::filesystem::exists(config.path)) {
            std::ifstream input(config.path, std::ios::binary);
            if (!input) throw std::runtime_error("Cannot open configuration file");
            const auto data = nlohmann::json::parse(input);
            if (!data.is_object()) throw std::runtime_error("Root must be an object");
            for (const auto& [key, value] : data.items()) {
                if (key != "profiling") throw std::runtime_error("Unknown startup section: " + key);
            }
            if (data.contains("profiling")) {
                const auto& profiling = data.at("profiling");
                if (!profiling.is_object()) throw std::runtime_error("profiling must be an object");
                for (const auto& [key, value] : profiling.items()) {
                    if (key != "switch_profile") throw std::runtime_error("Unknown profiling option: " + key);
                    if (!value.is_boolean()) {
                        throw std::runtime_error("profiling.switch_profile must be a boolean (true/false)");
                    }
                    config.switch_profile = value.get<bool>();
                }
            }
            config.file_loaded = true;
        }
    } catch (const std::exception& error) {
        throw std::runtime_error("Invalid startup configuration " +
                                 Utils::path_to_utf8(config.path) + ": " + error.what());
    }
    if (switch_profile_environment != nullptr) {
        // Match the existing profiler: only the exact value "1" enables it.
        config.switch_profile = std::string_view(switch_profile_environment) == "1";
        config.from_environment = true;
    }
    return config;
}

void apply_startup_config(const StartupConfig& config) {
    const char* value = config.switch_profile ? "1" : "0";
#ifdef _WIN32
    const auto result = _putenv_s("CORONA_SWITCH_PROFILE", value);
#else
    const auto result = setenv("CORONA_SWITCH_PROFILE", value, 1);
#endif
    if (result != 0) throw std::runtime_error("Cannot apply CORONA_SWITCH_PROFILE startup setting");
}

}  // namespace Corona::Startup
