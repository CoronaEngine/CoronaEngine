#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "corona/systems/display/image_frame_metadata.h"
#include "corona/systems/display/surface_frame_coordinator.h"

namespace Corona::Systems::Detail {

/**
 * A copyable image publication, bound to one storage allocation's lifetime.
 *
 * Producers retire and wait BEFORE draining GPU receipts and freeing the slot.
 * Display snapshots may outlive retirement, but cannot acquire the old slot,
 * even if its numeric handle has already been reused by another producer.
 * This is independent of surface retirement: an idle optics layer can disappear
 * while the surface and its UI continue to present.
 */
class PublishedImage {
    struct State {
        explicit State(std::uintptr_t handle) : image_handle(handle) {
            (void)coordinator.activate(image_handle);
            snapshot = coordinator.capture(image_handle);
        }

        const std::uintptr_t image_handle;
        SurfaceFrameCoordinator coordinator;
        SurfaceFrameCoordinator::Snapshot snapshot;
    };

   public:
    PublishedImage() = default;

    explicit PublishedImage(std::uintptr_t image_handle)
        : state_(image_handle == 0 ? nullptr : std::make_shared<State>(image_handle)) {}

    template <typename Storage>
    [[nodiscard]] auto acquire_write(Storage& storage) const {
        using Access = SurfaceFrameCoordinator::FrameAccess<typename Storage::WriteHandle>;
        if (!state_) {
            return std::optional<Access>{};
        }
        return state_->coordinator.begin_frame(state_->snapshot, [&]() {
            return storage.acquire_write(state_->image_handle);
        });
    }

    template <typename Storage>
    [[nodiscard]] auto acquire_write(Storage& storage, ImageFrameMetadata& metadata) const {
        auto access = acquire_write(storage);
        // Event snapshots can lag behind a producer replacing the image in the
        // same slot. Resolve all sampling metadata while holding that slot's lock.
        metadata = access ? access->images()->metadata : ImageFrameMetadata{};
        return access;
    }

    // Nonblocking invalidation; the producer must wait without holding a
    // storage handle so an already-entered Display frame can finish writeback.
    [[nodiscard]] SurfaceFrameCoordinator::Retirement retire() const {
        return state_ ? state_->coordinator.retire(state_->image_handle)
                      : SurfaceFrameCoordinator::Retirement{};
    }

   private:
    std::shared_ptr<State> state_;
};

}  // namespace Corona::Systems::Detail
