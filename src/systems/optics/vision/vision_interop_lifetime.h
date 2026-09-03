#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>

namespace Corona::Systems::Vision {

[[nodiscard]] inline bool vision_zero_copy_disabled_from_value(const char* raw) {
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }

    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value != "0" && value != "false" && value != "off" && value != "no";
}

template <typename ReceiptMap, typename WaitFn, typename ReleaseFn>
void drain_vision_interop_submissions(ReceiptMap& receipts,
                                      WaitFn&& wait,
                                      ReleaseFn&& release_resources) {
    for (const auto& [camera_handle, receipt] : receipts) {
        if (receipt.serial != 0) {
            std::invoke(wait, camera_handle, receipt);
        }
    }
    receipts.clear();
    std::invoke(release_resources);
}

}  // namespace Corona::Systems::Vision
