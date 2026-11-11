#pragma once

// Small shared string helpers for the editor-API units.
//
// to_lower_ascii was an anonymous-namespace local of the handler unit, used by argument
// parsing there and by the Win32 adapter-name matching in the network unit. Inline so
// both see one definition without a fourth translation unit for six lines.

#include <algorithm>
#include <cctype>
#include <string>

namespace Corona::Systems::UI {

inline std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

}  // namespace Corona::Systems::UI
