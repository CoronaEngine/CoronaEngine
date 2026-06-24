//
// Created by 25473 on 2025/11/19.
//
#include <algorithm>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Corona::Script::Python::PathCfg {

static auto normalize(std::string s) -> std::string {
    std::ranges::replace(s, '\\', '/');
    return s;
}

static auto executable_dir() -> std::filesystem::path {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            break;
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
    return std::filesystem::current_path();
}

auto engine_root() -> const std::string& {
    static std::string root = [] {
        std::string resultPath;
        std::string runtimePath = std::filesystem::current_path().string();
        std::regex pattern(R"((.*)CoronaEngine\b)");
        std::smatch matches;
        if (std::regex_search(runtimePath, matches, pattern)) {
            if (matches.size() > 1) {
                resultPath = matches[1].str() + "CoronaEngine";
            } else {
                throw std::runtime_error("Failed to resolve source path.");
            }
        }
        return normalize(resultPath);
    }();
    return root;
}

auto editor_backend_rel() -> const std::string& {
    static const std::string rel = "editor";
    return rel;
}

auto editor_backend_abs() -> const std::string& {
    static const std::string abs = normalize(engine_root() + "/" + editor_backend_rel());
    return abs;
}

auto runtime_backend_abs() -> std::string {
    const auto cwd_runtime = std::filesystem::current_path() / "CabbageEditor";
    if (std::filesystem::exists(cwd_runtime / "main.py")) {
        return normalize(cwd_runtime.string());
    }

    const auto exe_runtime = executable_dir() / "CabbageEditor";
    if (std::filesystem::exists(exe_runtime / "main.py")) {
        return normalize(exe_runtime.string());
    }

    return normalize(exe_runtime.string());
}

auto site_packages_dir() -> std::string {
    return normalize(std::string(CORONA_PYTHON_MODULE_LIB_DIR) + "/site-packages");
}

}  // namespace Corona::Script::Python::PathCfg
