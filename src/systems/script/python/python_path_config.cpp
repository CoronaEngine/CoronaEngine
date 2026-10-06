//
// Created by 25473 on 2025/11/19.
//
#include <corona/systems/script/python/python_path_config.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Corona::Script::Python::PathCfg {

namespace {

std::filesystem::path resolve_executable_dir();

std::filesystem::path configured_python_home() {
#ifdef CORONA_PYTHON_HOME_DIR
    return std::filesystem::path(CORONA_PYTHON_HOME_DIR);
#else
    return {};
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
    return std::filesystem::exists(root / "Lib") &&
           std::filesystem::exists(root / "DLLs");
}

std::filesystem::path resolve_executable_dir() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            break;
        }
        // On Windows, an exact-size result means the buffer may have been too
        // small. Grow it and retry rather than returning a truncated path.
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
    return std::filesystem::current_path();
}

std::string normalize(const std::filesystem::path& path) {
    auto value = path.generic_string();
    std::ranges::replace(value, '\\', '/');
    return value;
}

}  // namespace

PackagedRuntimeInspection inspect_packaged_runtime(const std::filesystem::path& root) {
    PackagedRuntimeInspection result;
    result.root = root;

    std::error_code ec;
    if (!std::filesystem::is_directory(root / "Lib", ec)) {
        result.issue = PackagedRuntimeIssue::MissingLib;
        return result;
    }
    if (!std::filesystem::is_directory(root / "DLLs", ec)) {
        result.issue = PackagedRuntimeIssue::MissingDlls;
        return result;
    }
    if (!std::filesystem::is_regular_file(root / "Lib" / "site.py", ec)) {
        result.issue = PackagedRuntimeIssue::MissingSiteModule;
        return result;
    }
    if (!std::filesystem::is_directory(root / "Lib" / "encodings", ec)) {
        result.issue = PackagedRuntimeIssue::MissingEncodings;
        return result;
    }

    result.issue = PackagedRuntimeIssue::None;
    return result;
}

std::string PackagedRuntimeInspection::describe() const {
    const std::string root_text = root.string();
    switch (issue) {
        case PackagedRuntimeIssue::None:
            return "packaged python runtime is usable: " + root_text;
        case PackagedRuntimeIssue::MissingLib:
            return "packaged python runtime is unusable (missing Lib/): " + root_text;
        case PackagedRuntimeIssue::MissingDlls:
            return "packaged python runtime is unusable (missing DLLs/): " + root_text;
        case PackagedRuntimeIssue::MissingSiteModule:
            return "packaged python runtime is unusable (missing Lib/site.py): " + root_text;
        case PackagedRuntimeIssue::MissingEncodings:
            return "packaged python runtime is unusable (missing Lib/encodings/): " + root_text;
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
    static const std::string abs = normalize(engine_root_path() / editor_backend_rel());
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
    return normalize(packaged_or_configured(
        engine_root_path() / "python313.zip", configured_python_home() / "python313.zip"));
}

auto python_dll_dir() -> std::string {
    return normalize(packaged_python_runtime_available()
                         ? engine_root_path() / "python-runtime" / "DLLs"
#ifdef CORONA_PYTHON_MODULE_DLL_DIR
                         : std::filesystem::path(CORONA_PYTHON_MODULE_DLL_DIR)
#else
                         : configured_python_home() / "DLLs"
#endif
    );
}

auto python_lib_dir() -> std::string {
    return normalize(packaged_python_runtime_available()
                         ? engine_root_path() / "python-runtime" / "Lib"
#ifdef CORONA_PYTHON_MODULE_LIB_DIR
                         : std::filesystem::path(CORONA_PYTHON_MODULE_LIB_DIR)
#else
                         : configured_python_home() / "Lib"
#endif
    );
}

auto site_packages_dir() -> std::string {
    return normalize(engine_root_path() / "python-runtime" / "Lib" / "site-packages");
}

}  // namespace Corona::Script::Python::PathCfg
