#pragma once

#include <filesystem>
#include <string>

namespace Corona::Script::Python::PathCfg {

enum class PackagedRuntimeIssue {
    None,
    MissingLib,
    MissingDlls,
    MissingSiteModule,
    MissingEncodings,
};

struct PackagedRuntimeInspection {
    PackagedRuntimeIssue issue = PackagedRuntimeIssue::MissingLib;
    std::filesystem::path root;

    [[nodiscard]] bool available() const noexcept { return issue == PackagedRuntimeIssue::None; }
    [[nodiscard]] std::string describe() const;
};

PackagedRuntimeInspection inspect_packaged_runtime(const std::filesystem::path& root);

PackagedRuntimeInspection inspect_deployed_runtime();

std::string configured_python_home_dir();

bool packaged_runtime_required();

enum class PackagedRuntimeDecision {
    UsePackaged,
    UseFallback,
    RejectNoPackaged,
    RejectNoFallback,
};

PackagedRuntimeDecision decide_packaged_runtime_usage(const PackagedRuntimeInspection& inspection,
                                                      bool packaged_required,
                                                      bool fallback_available);

// All runtime paths are resolved relative to the running executable. This is
// intentional: the executable may be started from any working directory and
// the whole build output directory may be moved after it is packaged.
const std::filesystem::path& executable_dir();
const std::filesystem::path& engine_root_path();

const std::string& engine_root();
const std::string& editor_backend_rel();
const std::string& editor_backend_abs();
std::string runtime_backend_abs();
std::string python_home_dir();
std::string python_stdlib_zip();
std::string python_dll_dir();
std::string python_lib_dir();
std::string site_packages_dir();

}  // namespace Corona::Script::Python::PathCfg
