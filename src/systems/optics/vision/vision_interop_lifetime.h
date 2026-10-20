#pragma once

#include <functional>

namespace Corona::Systems::Vision {

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
