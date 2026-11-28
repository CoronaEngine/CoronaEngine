//
// Created by 25473 on 2025/11/19.
//
#include <corona/systems/script/python/python_path_config.h>
#include <corona/utils/runtime_paths.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

// corona_python.cmake pins one CPython minor version and passes it through the
// build definitions. The fallback keeps this translation unit self-contained for
// tests that compile it without the build's compile definitions.
#ifndef CORONA_PYTHON_VERSION
#define CORONA_PYTHON_VERSION "3.13"
#endif

namespace Corona::Script::Python::PathCfg {

namespace {

std::filesystem::path resolve_executable_dir();

// "3.13" -> "313": CPython names its stdlib zip after the bare digits
// (python313.zip), not after the dotted version.
std::string python_version_tag() {
    std::string tag;
    for (const char ch : std::string_view(CORONA_PYTHON_VERSION)) {
        if (ch >= '0' && ch <= '9') {
            tag.push_back(ch);
        }
    }
    return tag;
}

std::filesystem::path configured_python_home() {
#ifdef CORONA_PYTHON_HOME_DIR
    return Utils::utf8_to_path(std::string(CORONA_PYTHON_HOME_DIR));
#else
    return {};
#endif
}

// The build resolves these per platform (Lib vs lib/pythonX.Y), so the fallback
// layout below only applies when this unit is built outside the engine build.
std::filesystem::path configured_stdlib_dir() {
#ifdef CORONA_PYTHON_STDLIB_DIR
    return Utils::utf8_to_path(std::string(CORONA_PYTHON_STDLIB_DIR));
#else
    return configured_python_home() / platform_layout().stdlib_rel;
#endif
}

std::filesystem::path configured_ext_modules_dir() {
#ifdef CORONA_PYTHON_EXT_MODULES_DIR
    return Utils::utf8_to_path(std::string(CORONA_PYTHON_EXT_MODULES_DIR));
#else
    return configured_python_home() / platform_layout().ext_modules_rel;
#endif
}

std::filesystem::path packaged_or_configured(const std::filesystem::path& packaged,
                                             const std::filesystem::path& configured) {
    if (std::filesystem::exists(packaged)) {
        return packaged;
    }
    return configured;
}

bool packaged_python_runtime_available() {
    const auto root = resolve_executable_dir() / "python-runtime";
    const auto& layout = platform_layout();
    std::error_code ec;
    return std::filesystem::is_directory(root / layout.stdlib_rel, ec) &&
           std::filesystem::is_directory(root / layout.ext_modules_rel, ec);
}

std::filesystem::path resolve_executable_dir() {
    return Utils::executable_directory();
}

std::string normalize(const std::filesystem::path& path) {
    auto value = Utils::path_to_utf8(path);
    std::ranges::replace(value, '\\', '/');
    return value;
}

}  // namespace

const PythonRuntimeLayout& windows_layout() {
    static const PythonRuntimeLayout layout{
        "Lib",
        "DLLs",
        "Lib/site-packages",
        "python" + python_version_tag() + ".zip",
    };
    return layout;
}

const PythonRuntimeLayout& posix_layout() {
    static const PythonRuntimeLayout layout = [] {
        const std::filesystem::path stdlib{"lib/python" + std::string(CORONA_PYTHON_VERSION)};
        return PythonRuntimeLayout{
            stdlib,
            stdlib / "lib-dynload",
            stdlib / "site-packages",
            "python" + python_version_tag() + ".zip",
        };
    }();
    return layout;
}

const PythonRuntimeLayout& platform_layout() {
#ifdef _WIN32
    return windows_layout();
#else
    return posix_layout();
#endif
}

PackagedRuntimeInspection inspect_packaged_runtime(const std::filesystem::path& root,
                                                   const PythonRuntimeLayout& layout) {
    PackagedRuntimeInspection result;
    result.root = root;
    result.stdlib_rel = layout.stdlib_rel;
    result.ext_modules_rel = layout.ext_modules_rel;

    const auto stdlib = root / layout.stdlib_rel;
    const auto ext_modules = root / layout.ext_modules_rel;

    std::error_code ec;
    if (!std::filesystem::is_directory(stdlib, ec)) {
        result.issue = PackagedRuntimeIssue::MissingStdlib;
        return result;
    }
    if (!std::filesystem::is_directory(ext_modules, ec)) {
        result.issue = PackagedRuntimeIssue::MissingExtensionModules;
        return result;
    }
    if (!std::filesystem::is_regular_file(stdlib / "site.py", ec)) {
        result.issue = PackagedRuntimeIssue::MissingSiteModule;
        return result;
    }
    if (!std::filesystem::is_directory(stdlib / "encodings", ec)) {
        result.issue = PackagedRuntimeIssue::MissingEncodings;
        return result;
    }

    result.issue = PackagedRuntimeIssue::None;
    return result;
}

std::string PackagedRuntimeInspection::describe() const {
    const std::string root_text = Utils::path_to_utf8(root);
    switch (issue) {
        case PackagedRuntimeIssue::None:
            return "packaged python runtime is usable: " + root_text;
        case PackagedRuntimeIssue::MissingStdlib:
            return "packaged python runtime is unusable (missing " +
                   stdlib_rel.generic_string() + "/): " + root_text;
        case PackagedRuntimeIssue::MissingExtensionModules:
            return "packaged python runtime is unusable (missing " +
                   ext_modules_rel.generic_string() + "/): " + root_text;
        case PackagedRuntimeIssue::MissingSiteModule:
            return "packaged python runtime is unusable (missing " +
                   (stdlib_rel / "site.py").generic_string() + "): " + root_text;
        case PackagedRuntimeIssue::MissingEncodings:
            return "packaged python runtime is unusable (missing " +
                   (stdlib_rel / "encodings").generic_string() + "/): " + root_text;
    }
    return "packaged python runtime state unknown: " + root_text;
}

PackagedRuntimeInspection inspect_deployed_runtime() {
    return inspect_packaged_runtime(engine_root_path() / "python-runtime");
}

std::string configured_python_home_dir() {
    const auto home = configured_python_home();
    return home.empty() ? std::string{} : normalize(home);
}

bool packaged_runtime_required() {
    const char* value = std::getenv("CORONA_REQUIRE_PACKAGED_PYTHON");
    return value != nullptr && std::string_view(value) == "1";
}

PackagedRuntimeDecision decide_packaged_runtime_usage(const PackagedRuntimeInspection& inspection,
                                                      bool packaged_required,
                                                      bool fallback_available) {
    if (inspection.available()) {
        return PackagedRuntimeDecision::UsePackaged;
    }
    if (packaged_required) {
        return PackagedRuntimeDecision::RejectNoPackaged;
    }
    if (!fallback_available) {
        return PackagedRuntimeDecision::RejectNoFallback;
    }
    return PackagedRuntimeDecision::UseFallback;
}

const std::filesystem::path& executable_dir() {
    static const auto path = resolve_executable_dir();
    return path;
}

const std::filesystem::path& engine_root_path() {
    // The packaged runtime root is the directory containing corona_engine.exe.
    return executable_dir();
}

auto engine_root() -> const std::string& {
    static const std::string root = normalize(engine_root_path());
    return root;
}

auto editor_backend_rel() -> const std::string& {
    static const std::string rel = "CabbageEditor";
    return rel;
}

auto editor_backend_abs() -> const std::string& {
    static const std::string abs = normalize(Utils::editor_directory());
    return abs;
}

auto runtime_backend_abs() -> std::string {
    return editor_backend_abs();
}

auto python_home_dir() -> std::string {
    return normalize(packaged_python_runtime_available()
                         ? engine_root_path() / "python-runtime"
                         : configured_python_home());
}

auto python_stdlib_zip() -> std::string {
    const auto& layout = platform_layout();
    return normalize(packaged_or_configured(
        engine_root_path() / layout.stdlib_zip_name,
        configured_python_home() / layout.stdlib_zip_name));
}

auto python_stdlib_dir() -> std::string {
    return normalize(packaged_python_runtime_available()
                         ? engine_root_path() / "python-runtime" / platform_layout().stdlib_rel
                         : configured_stdlib_dir());
}

auto python_ext_modules_dir() -> std::string {
    return normalize(packaged_python_runtime_available()
                         ? engine_root_path() / "python-runtime" / platform_layout().ext_modules_rel
                         : configured_ext_modules_dir());
}

auto site_packages_dir() -> std::string {
    return normalize(engine_root_path() / "python-runtime" / platform_layout().site_packages_rel);
}

}  // namespace Corona::Script::Python::PathCfg
