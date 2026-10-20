// Vision zero-copy bridge implementation. See vision_zero_copy_bridge.h.

#ifdef CORONA_ENABLE_VISION

#include "vision_zero_copy_bridge.h"

#include <stdexcept>
#include <horizon/core/logging.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "base/mgr/pipeline.h"
#include "base/sensor/frame_buffer.h"

namespace Corona::Systems::Vision {

// Holds the Ocarina-side exportable buffer. Kept in the .cpp so ocarina::Buffer and
// ocarina::float4 never leak into the engine-facing header.
struct VisionZeroCopyBridge::Shared {
    ocarina::Buffer<ocarina::float4> buffer;
};

VisionZeroCopyBridge::~VisionZeroCopyBridge() {
    release();
}

void VisionZeroCopyBridge::release() {
    // Drop the imported consumer before the backing Ocarina allocation.
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
        // (dedicated Vulkan memory). A plain create_buffer is NOT
        // exportable and export_handle() would throw "Invalid handle".
        shared_ = new Shared{device.create_exported_buffer<ocarina::float4>(
            pixel_count, "VisionZeroCopyBridge::shared")};

        const uint64_t shareable_handle = device.export_handle(shared_->buffer.handle());
        // Importing an NT handle does not transfer handle ownership to Vulkan.
        const std::unique_ptr<void, decltype(&CloseHandle)> exported_handle(
            reinterpret_cast<HANDLE>(shareable_handle), &CloseHandle);
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
        const auto metadata = device.buffer_export_metadata(shared_->buffer.handle());
        if (metadata.memory_type_index != ~0u) {
            handle.memory_type_index = metadata.memory_type_index;
            handle.vulkan_buffer_usage = metadata.usage;
            handle.vulkan_queue_families.assign(metadata.queue_families.begin(), metadata.queue_families.end());
            handle.device_uuid = metadata.device_uuid;
            handle.driver_uuid = metadata.driver_uuid;
            handle.memory_range = {0, pixel_count * sizeof(ocarina::float4)};
        }

        auto desc = Corona::Horizon::HardwareBufferDesc::typed<float>(
            pixel_count * 4u,
            Corona::Horizon::BufferUsage_Storage,
            "VisionZeroCopyBridge::imported");
        desc.dedicated = true;
        desc.cpu_access = Corona::Horizon::CpuAccessMode::None;

        imported_ = Corona::Horizon::HardwareBuffer::import_external(handle, desc);

        width_ = width;
        height_ = height;
        valid_ = true;
        CFW_LOG_INFO("Vision output: external-memory buffer {}x{} (Vulkan)", width, height);
        return true;
    } catch (const std::exception& error) {
        CFW_LOG_WARNING("Vision shared buffer unavailable; using readback: {}", error.what());
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
        // Optics has waited for the previous resolve receipt before reusing us.
        // Vulkan instances require explicit external ownership even on one GPU.
        imported_.set_external_ownership(true);
        pipeline.device().set_buffer_external_ownership(shared_->buffer.handle(), false);
        // Device-to-device copy on the Vision stream, then synchronize+commit so
        // the bytes are present before Vulkan's resolve pass reads them. This
        // synchronize is the stand-in for real cross-API ordering.
        pipeline.stream() << shared_->buffer.view(0, pixel_count).copy_from(src)
                          << ocarina::synchronize()
                          << ocarina::commit();
        pipeline.device().set_buffer_external_ownership(shared_->buffer.handle(), true);
        imported_.set_external_ownership(false);
        return true;
    } catch (const std::exception& error) {
        CFW_LOG_WARNING("Vision shared buffer transfer failed: {}", error.what());
        return false;
    }
}

}  // namespace Corona::Systems::Vision

#endif  // CORONA_ENABLE_VISION
