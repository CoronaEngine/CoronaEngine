#pragma once

#include <filesystem>
#include <string>

namespace Corona::Script::Python::PathCfg {

// One CPython installation layout: where the standard library and the extension
// modules live below the runtime root. CPython does not use the same names on
// every platform (Windows: Lib/ + DLLs/; POSIX: lib/pythonX.Y/ + lib-dynload/),
// so the layout is data instead of a chain of #ifdefs. platform_layout() selects
// the host's layout at compile time, and passing a layout explicitly lets the
// tests exercise either platform on any host.
struct PythonRuntimeLayout {
    std::filesystem::path stdlib_rel;
    std::filesystem::path ext_modules_rel;
    std::filesystem::path site_packages_rel;
    std::string stdlib_zip_name;
};

// Layout of the host the runtime was compiled for.
const PythonRuntimeLayout& platform_layout();

// The two concrete layouts. Exposed so tests can inspect a POSIX tree on Windows
// (and vice versa) instead of trusting a branch they cannot build here.
const PythonRuntimeLayout& windows_layout();
const PythonRuntimeLayout& posix_layout();

enum class PackagedRuntimeIssue {
    None,
    MissingStdlib,
    MissingExtensionModules,
    MissingSiteModule,
    MissingEncodings,
};

struct PackagedRuntimeInspection {
    PackagedRuntimeIssue issue = PackagedRuntimeIssue::MissingStdlib;
    std::filesystem::path root;
    // Relative paths that were inspected, so describe() names the directory the
    // layout really expects instead of a hard-coded Windows one.
    std::filesystem::path stdlib_rel;
    std::filesystem::path ext_modules_rel;

    [[nodiscard]] bool available() const noexcept { return issue == PackagedRuntimeIssue::None; }
    [[nodiscard]] std::string describe() const;
};

PackagedRuntimeInspection inspect_packaged_runtime(
    const std::filesystem::path& root,
    const PythonRuntimeLayout& layout = platform_layout());

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
std::string python_stdlib_dir();
std::string python_ext_modules_dir();
std::string site_packages_dir();

}  // namespace Corona::Script::Python::PathCfg
