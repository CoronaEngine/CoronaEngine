#pragma once

#include <corona/utils/path_utils.h>
#include "include/cef_base.h"

namespace Corona::Systems::UI {

inline void set_cef_path(cef_string_t& value, const std::filesystem::path& path) {
#ifdef _WIN32
    CefString(&value).FromWString(path.wstring());
#else
    CefString(&value).FromString(Utils::path_to_utf8(path));
#endif
}

inline std::string local_path_to_url(const std::string& value) {
    const auto path = Utils::utf8_to_path(value);
    if (!path.is_absolute()) return value;
    const auto bytes = path.generic_u8string();
    std::string normalized(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // Windows extended-length prefixes are filesystem syntax, not URL syntax.
    if (normalized.starts_with("//?/UNC/")) normalized = "//" + normalized.substr(8);
    else if (normalized.starts_with("//?/")) normalized.erase(0, 4);
    std::string url = normalized.starts_with("//") ? "file:" :
                      normalized.starts_with('/') ? "file://" : "file:///";
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : normalized) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == ':' ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            url += static_cast<char>(c);
        } else {
            url += '%';
            url += hex[c >> 4];
            url += hex[c & 15];
        }
    }
    return url;
}

}  // namespace Corona::Systems::UI
