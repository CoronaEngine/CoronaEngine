// Vision zero-copy bridge implementation. See vision_zero_copy_bridge.h.

#ifdef CORONA_ENABLE_VISION

#include "vision_zero_copy_bridge.h"

#include <stdexcept>

#include "base/mgr/pipeline.h"
#include "base/sensor/frame_buffer.h"

namespace Corona::Systems::Vision {

// Holds the CUDA-side exportable buffer. Kept in the .cpp so ocarina::Buffer and
// ocarina::float4 never leak into the engine-facing header.
struct VisionZeroCopyBridge::Shared {
    ocarina::Buffer<ocarina::float4> buffer;
};

VisionZeroCopyBridge::~VisionZeroCopyBridge() {
    release();
}

void VisionZeroCopyBridge::release() {
    // Vulkan-before-CUDA: drop the imported consumer first so it never outlives
    // the backing CUDA allocation.
    imported_ = Corona::Horizon::HardwareBuffer{};
    delete shared_;
    shared_ = nullptr;
    valid_ = false;
}

bool VisionZeroCopyBridge::ensure(::vision::Pipeline& pipeline, uint32_t width, uint32_t height) {
    if (valid_ && width == width_ && height == height_) {
        return true;
    }

    release();

    if (width == 0 || height == 0) {
        return false;
    }

    try {
        ocarina::Device& device = pipeline.device();
        const uint64_t pixel_count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);

        // Allocate the shared color buffer as an OS-exportable allocation
        // (cuMemCreate + CU_MEM_HANDLE_TYPE_WIN32). A plain create_buffer is NOT
        // exportable and export_handle() would throw "Invalid handle".
        shared_ = new Shared{device.create_exported_buffer<ocarina::float4>(
            pixel_count, "VisionZeroCopyBridge::shared")};

        const uint64_t shareable_handle = device.export_handle(shared_->buffer.handle());
        // allocSize is the granularity-aligned physical size, NOT w*h*16. The
        // Vulkan import must use this or it under-maps the allocation.
        const uint64_t alloc_size = device.get_aligned_memory_size(shared_->buffer.handle());
        if (shareable_handle == 0 || alloc_size == 0) {
            release();
            return false;
        }

        auto handle = Corona::Horizon::ExternalMemoryHandle::win32(
            reinterpret_cast<HANDLE>(shareable_handle),
            alloc_size);

        auto desc = Corona::Horizon::HardwareBufferDesc::typed<float>(
            pixel_count * 4u,
            Corona::Horizon::BufferUsageFlags::Storage,
            "VisionZeroCopyBridge::imported");
        desc.dedicated = true;

        imported_ = Corona::Horizon::HardwareBuffer::import_external(handle, desc);

        width_ = width;
        height_ = height;
        valid_ = true;
        return true;
    } catch (const std::exception&) {
        release();
        return false;
    }
}

bool VisionZeroCopyBridge::copy_from_framebuffer(::vision::Pipeline& pipeline) {
    if (!valid_) {
        return false;
    }

    auto* fb = pipeline.frame_buffer();
    if (fb == nullptr) {
        return false;
    }

    // Use the framebuffer-selected display source. Normal framebuffers return
    // accumulation/rt; lightfield framebuffers return pixel-sized encoded data.
    const auto src = fb->display_source_buffer();

    const uint64_t pixel_count = static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_);

    try {
        // Device-to-device copy on the Vision stream, then synchronize+commit so
        // the bytes are present before Vulkan's resolve pass reads them. This
        // synchronize is the stand-in for real cross-API ordering.
        pipeline.stream() << shared_->buffer.view(0, pixel_count).copy_from(src)
                          << ocarina::synchronize()
                          << ocarina::commit();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace Corona::Systems::Vision

#endif  // CORONA_ENABLE_VISION
