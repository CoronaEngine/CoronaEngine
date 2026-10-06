#pragma once

#include <filesystem>

namespace Corona::Startup {

struct StartupConfig {
    std::filesystem::path path;
    bool file_loaded{false};
    bool switch_profile{false};
    bool from_environment{false};
};

// Resolve the application configuration from the process's current working directory.
std::filesystem::path default_startup_config_path();
StartupConfig read_startup_config(const std::filesystem::path& path,
                                  const char* switch_profile_environment);
// Call before engine initialization and the profiler's first enabled() query.
void apply_startup_config(const StartupConfig& config);

}  // namespace Corona::Startup
