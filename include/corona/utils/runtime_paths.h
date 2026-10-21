#pragma once

#include <corona/utils/path_utils.h>
#include <vector>

namespace Corona::Utils {

inline std::filesystem::path executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    while (buffer.size() <= 32768) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        if (buffer.size() == 32768) return {};
        buffer.resize(buffer.size() * 2 > 32768 ? 32768 : buffer.size() * 2);
    }
#elif defined(__linux__)
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
    return {};
}

inline std::filesystem::path executable_directory() {
    const auto executable = executable_path();
    return executable.empty() ? std::filesystem::current_path() : executable.parent_path();
}

inline std::filesystem::path editor_directory() {
    const auto installed = executable_directory() / "CabbageEditor";
    if (std::filesystem::is_directory(installed)) return installed;
    const auto source = std::filesystem::current_path() / "CoronaEditor" / "editor";
    return std::filesystem::is_directory(source) ? source : installed;
}

}  // namespace Corona::Utils
