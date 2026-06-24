#pragma once

// Vision zero-copy bridge — shares Vision's pre-tonemap linear float4 color
// buffer with the Vulkan engine via CUDA<->Vulkan external memory, eliminating
// the per-frame GPU->CPU->GPU readback used by the [MANUAL-READBACK] path.
//
// Direction: Vision (CUDA) renders into the framebuffer-selected display source
// (linear HDR, pre-tonemap). We copy that on-device into a CUDA *exportable*
// buffer, export its Win32 handle, and import it as a Vulkan HardwareBuffer. A
// Vulkan compute pass (vision_resolve.comp) then applies exposure + ACES and
// writes the engine's RGBA16F finalOutputImage. The final-color view_texture_ is
// a cuArray and is intentionally NOT used (cuArray memory is not exportable).
//
// NOTE: no cross-API synchronization yet (no timeline semaphore). CUDA writes and
// Vulkan reads are not ordered, so tearing/flicker is possible. This is the
// "make it work first" path; a shared timeline semaphore is the follow-up.
//
// Only compiled when CORONA_ENABLE_VISION is defined.

#ifdef CORONA_ENABLE_VISION

#include <horizon.h>

#include <cstdint>

namespace vision {
class Pipeline;
}  // namespace vision

namespace Corona::Systems::Vision {

class VisionZeroCopyBridge {
public:
    VisionZeroCopyBridge() = default;
    ~VisionZeroCopyBridge();

    VisionZeroCopyBridge(const VisionZeroCopyBridge&) = delete;
    VisionZeroCopyBridge& operator=(const VisionZeroCopyBridge&) = delete;

    // (Re)allocates the shared exportable buffer and re-imports it into Vulkan
    // when the resolution changes. Safe to call every frame; a no-op when the
    // dimensions are unchanged. Returns false if allocation/import failed, in
    // which case the caller should fall back (e.g. skip this frame).
    bool ensure(::vision::Pipeline& pipeline, uint32_t width, uint32_t height);

    // Copies the current pre-tonemap linear display source into the shared
    // exportable buffer on the Vision stream, then synchronizes + commits so the
    // bytes are visible to Vulkan before the resolve dispatch reads them. This
    // synchronize is the only thing standing in for real cross-API sync.
    bool copy_from_framebuffer(::vision::Pipeline& pipeline);

    // The Vulkan-side view of the shared memory. Bind its storeDescriptor() as
    // the resolve pass's source SSBO. Valid only after a successful ensure().
    [[nodiscard]] Corona::Horizon::HardwareBuffer& imported() noexcept { return imported_; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }

private:
    // Releases the imported Vulkan buffer first, then the CUDA-side exportable
    // buffer (Vulkan-before-CUDA ordering avoids the consumer outliving the
    // backing allocation). pimpl holds the ocarina Buffer<float4> to keep
    // ocarina types out of this engine-facing header.
    void release();

    struct Shared;       // owns the exported ocarina Buffer<float4>
    Shared* shared_ = nullptr;
    Corona::Horizon::HardwareBuffer imported_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool valid_ = false;
};

}  // namespace Corona::Systems::Vision

#endif  // CORONA_ENABLE_VISION
