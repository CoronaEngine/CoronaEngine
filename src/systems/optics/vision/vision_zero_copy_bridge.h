#pragma once

// Vision runs its display passes on the linear framebuffer and writes float4
// pixels into an exportable buffer. The engine imports the same allocation and
// writes those pixels to its RGBA16F storage image on the GPU.
//
// NOTE: no external Vulkan timeline semaphore yet. Optics currently waits
// for the preceding Vulkan submit receipt before Vision reuses or releases
// the allocation.

#include <horizon.h>

#include <cstdint>

namespace vision {
class Pipeline;
}  // namespace vision

namespace Corona {
struct VisionDisplayProfile;
}

namespace Corona::Systems::Vision {

class VisionZeroCopyBridge {
public:
    VisionZeroCopyBridge() = default;
    ~VisionZeroCopyBridge();

    VisionZeroCopyBridge(const VisionZeroCopyBridge&) = delete;
    VisionZeroCopyBridge& operator=(const VisionZeroCopyBridge&) = delete;

    // (Re)allocates the shared exportable buffer and re-imports it into Vulkan
    // when the resolution changes. Safe to call every frame; a no-op when the
    // dimensions are unchanged. Failure is latched until the bridge is recreated:
    // callers must skip rendering/publishing this view while failed() is true.
    bool ensure(::vision::Pipeline& pipeline, uint32_t width, uint32_t height);

    // Resolves the linear display source into shared float4 pixels on the Vision
    // stream, then synchronizes before handing ownership to the engine.
    bool render_display(::vision::Pipeline& pipeline, float exposure,
                        const VisionDisplayProfile* profile, bool diagnostic);

    // Source for the engine's image-store pass after render_display().
    [[nodiscard]] Corona::Horizon::HardwareBuffer& imported() noexcept { return imported_; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }

private:
    // Releases the imported buffer before its exportable allocation.
    void release();
    bool fail(const char* reason);

    struct Shared;
    Shared* shared_ = nullptr;
    Corona::Horizon::HardwareBuffer imported_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool valid_ = false;
    bool failed_ = false;
};

}  // namespace Corona::Systems::Vision
