//
// Created by 25473 on 2025/11/19.
//
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Corona::Script::Python::PathCfg {

namespace {

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
    return normalize(engine_root_path() / "python-runtime");
}

auto python_stdlib_zip() -> std::string {
    return normalize(engine_root_path() / "python313.zip");
}

auto python_dll_dir() -> std::string {
    return normalize(engine_root_path() / "python-runtime" / "DLLs");
}

auto python_lib_dir() -> std::string {
    return normalize(engine_root_path() / "python-runtime" / "Lib");
}

auto site_packages_dir() -> std::string {
    return normalize(engine_root_path() / "python-runtime" / "Lib" / "site-packages");
}

}  // namespace Corona::Script::Python::PathCfg
