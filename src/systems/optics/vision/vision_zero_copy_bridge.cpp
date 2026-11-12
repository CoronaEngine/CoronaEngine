// Vision zero-copy bridge implementation. See vision_zero_copy_bridge.h.

#include "vision_zero_copy_bridge.h"

#include <stdexcept>
#include <corona/shared_data_hub.h>
#include <horizon/core/logging.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "base/mgr/pipeline.h"
#include "base/sensor/frame_buffer.h"
#include "vision_auto_exposure.h"
#include "vision_display_sampling.h"

namespace Corona::Systems::Vision {
namespace {

using namespace ocarina;

constexpr uint kLegacy = 0u, kHableManual = 1u, kHableAuto = 2u, kDiagnostic = 3u;

Float3 aces_filmic(const Float3& x) {
    return clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.f, 1.f);
}

Float3 hable_uc2(const Float3& x) {
    constexpr float a = 0.22f, b = 0.30f, c = 0.10f, d = 0.20f, e = 0.01f, f = 0.30f;
    return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
}

Float3 linear_to_srgb(const Float3& color) {
    const Float3 c = clamp(color, 0.f, 1.f);
    return select(c >= 0.0031308f, 1.055f * pow(c, make_float3(1.f / 2.4f)) - 0.055f, c * 12.92f);
}

auto make_display_resolve_kernel() {
    return ocarina::Kernel{[](BufferVar<float4> hdr, BufferVar<uint2> bloom, BufferVar<float> meter,
                              BufferVar<float4> output, Uint2 extent, Uint2 bloom_extent, Float intensity,
                              Float exposure, Uint mode) {
        configure_block(8u, 8u);
        const Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= extent)) { $return(); };
        Float3 color = display_detail::load_hdr(hdr, extent, pixel);
        $if(mode == kLegacy) {
            color = linear_to_srgb(aces_filmic(1.f - exp(-color * exposure)));
        }
        $elif(mode != kDiagnostic) {
            $if(intensity > 0.f) {
                color = display_detail::composite_bloom(color, pixel, extent, bloom, bloom_extent, intensity);
            };
            Float gain = exposure;
            $if(mode == kHableAuto) { gain *= meter.read(0u); };
            color = linear_to_srgb(hable_uc2(color * gain * 2.f) / hable_uc2(make_float3(11.2f)));
        };
        output.write(pixel.y * extent.x + pixel.x, make_float4(color, 1.f));
    }};
}

}  // namespace

struct VisionZeroCopyBridge::Shared {
    ocarina::Buffer<ocarina::float4> output;
    ocarina::Buffer<ocarina::uint2> bloom_half, bloom_quarter, bloom_blur;
    ocarina::Buffer<float> exposure_gain;
    ocarina::Shader<void(Buffer<float4>, Buffer<uint2>, Buffer<uint2>, uint2, uint2, uint)> bloom_shader;
    ocarina::Shader<void(Buffer<float4>, Buffer<uint2>, Buffer<float>, uint2, uint2, float, float, float)> exposure_shader;
    ocarina::Shader<void(Buffer<float4>, Buffer<uint2>, Buffer<float>, Buffer<float4>, uint2, uint2, float, float, uint)> resolve_shader;
};

VisionZeroCopyBridge::~VisionZeroCopyBridge() {
    release();
    delete shared_;
}

void VisionZeroCopyBridge::release() {
    // Drop the imported consumer before the backing Ocarina allocation.
    imported_ = Corona::Horizon::HardwareBuffer{};
    if (shared_) shared_->output = {};
    valid_ = false;
}

bool VisionZeroCopyBridge::fail(const char* reason) {
    valid_ = false;
    failed_ = true;
    CFW_LOG_ERROR(
        "Vision GPU display unavailable: {}. No new frames will be published; "
        "recreate the view to retry.",
        reason);
    return false;
}

bool VisionZeroCopyBridge::ensure(::vision::Pipeline& pipeline, uint32_t width, uint32_t height) {
    if (failed_) {
        return false;
    }
    if (valid_ && width == width_ && height == height_) {
        return true;
    }

    release();

    if (width == 0 || height == 0) {
        return fail("shared buffer dimensions must be nonzero");
    }

    try {
        ocarina::Device& device = pipeline.device();
        if (!device.supports_buffer_export()) {
            throw std::runtime_error("Vulkan external buffer export is required");
        }
        const uint64_t pixel_count = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);

        // Allocate the shared color buffer as an OS-exportable allocation
        // (dedicated Vulkan memory). A plain create_buffer is NOT
        // exportable and export_handle() would throw "Invalid handle".
        if (!shared_) shared_ = new Shared;
        shared_->output = device.create_exported_buffer<ocarina::float4>(
            pixel_count, "VisionZeroCopyBridge::display");

        const uint64_t shareable_handle = device.export_handle(shared_->output.handle());
        // Importing an NT handle does not transfer handle ownership to Vulkan.
        const std::unique_ptr<void, decltype(&CloseHandle)> exported_handle(
            reinterpret_cast<HANDLE>(shareable_handle), &CloseHandle);
        const uint64_t alloc_size = device.get_aligned_memory_size(shared_->output.handle());
        if (shareable_handle == 0 || alloc_size == 0) {
            throw std::runtime_error("Vulkan buffer export returned an invalid handle or size");
        }

        auto handle = Corona::Horizon::ExternalMemoryHandle::win32(
            reinterpret_cast<HANDLE>(shareable_handle),
            alloc_size);
        const auto metadata = device.buffer_export_metadata(shared_->output.handle());
        if (metadata.memory_type_index != ~0u) {
            handle.memory_type_index = metadata.memory_type_index;
            handle.vulkan_buffer_usage = metadata.usage;
            handle.vulkan_queue_families.assign(metadata.queue_families.begin(), metadata.queue_families.end());
            handle.device_uuid = metadata.device_uuid;
            handle.driver_uuid = metadata.driver_uuid;
            handle.memory_range = {0, pixel_count * sizeof(ocarina::float4)};
        }

        auto desc = Corona::Horizon::HardwareBufferDesc::typed<ocarina::float4>(
            pixel_count,
            Corona::Horizon::BufferUsage_Storage,
            "VisionZeroCopyBridge::imported");
        desc.dedicated = true;
        desc.cpu_access = Corona::Horizon::CpuAccessMode::None;

        imported_ = Corona::Horizon::HardwareBuffer::import_external(handle, desc);
        if (!imported_) {
            throw std::runtime_error("Vulkan external buffer import returned an invalid buffer");
        }

        width_ = width;
        height_ = height;
        valid_ = true;
        CFW_LOG_INFO("Vision output: external-memory buffer {}x{} (Vulkan)", width, height);
        return true;
    } catch (const std::exception& error) {
        release();
        return fail(error.what());
    }
}

bool VisionZeroCopyBridge::render_display(::vision::Pipeline& pipeline, float exposure,
                                         const VisionDisplayProfile* profile, bool diagnostic) {
    if (failed_) {
        return false;
    }
    if (!valid_) {
        return fail("shared buffer has not been initialized");
    }

    auto* fb = pipeline.frame_buffer();
    if (fb == nullptr) {
        return fail("Vision framebuffer is unavailable");
    }

    // Use the framebuffer-selected display source. Normal framebuffers return
    // accumulation/rt; lightfield framebuffers return pixel-sized encoded data.
    const auto src = fb->display_source_buffer();

    const uint64_t pixel_count = static_cast<uint64_t>(width_) * static_cast<uint64_t>(height_);
    if (fb->resolution().x != width_ || fb->resolution().y != height_ ||
        src.size() < pixel_count) {
        return fail("Vision display source does not match the shared buffer extent");
    }

    try {
        using namespace ocarina;
        auto& device = pipeline.device();
        auto& display = *shared_;
        const uint2 extent = make_uint2(width_, height_);
        const auto* settings = diagnostic ? nullptr : profile;
        const bool auto_exposure = settings && settings->auto_exposure;
        const float intensity = settings ? settings->bloom_intensity : 0.f;
        const bool bloom = intensity > 0.f;
        const uint mode = diagnostic ? kDiagnostic : auto_exposure ? kHableAuto :
                          settings ? kHableManual : kLegacy;
        if (settings) exposure = settings->exposure_gain;

        const uint2 half_extent = (extent + 1u) / 2u;
        const uint2 bloom_extent = (half_extent + 1u) / 2u;
        const auto ensure_bloom_buffer = [&](Buffer<uint2>& buffer, uint2 size, const char* name) {
            const uint64_t count = uint64_t(size.x) * size.y;
            if (buffer.size() < count) buffer = device.create_buffer<uint2>(count, name);
        };
        if (bloom) {
            ensure_bloom_buffer(display.bloom_half, half_extent, "Vision Bloom Half");
            ensure_bloom_buffer(display.bloom_quarter, bloom_extent, "Vision Bloom Quarter");
            ensure_bloom_buffer(display.bloom_blur, bloom_extent, "Vision Bloom Blur");
            if (!display.bloom_shader.has_function()) {
                display.bloom_shader = device.compile(display_detail::make_vision_bloom_kernel(), "Corona-Vision-Bloom");
            }
        } else {
            ensure_bloom_buffer(display.bloom_quarter, make_uint2(1u), "Vision Bloom Quarter");
        }
        if (display.exposure_gain.size() == 0u) {
            display.exposure_gain = device.create_buffer<float>(1u, "Vision Exposure Gain");
        }
        if (auto_exposure && !display.exposure_shader.has_function()) {
            display.exposure_shader = device.compile(make_vision_auto_exposure_kernel(), "Corona-Vision-Exposure");
        }
        if (!display.resolve_shader.has_function()) {
            display.resolve_shader = device.compile(make_display_resolve_kernel(), "Corona-Vision-Resolve");
        }

        // Optics has waited for the previous resolve receipt before reusing us.
        // Vulkan instances require explicit external ownership even on one GPU.
        imported_.set_external_ownership(true);
        device.set_buffer_external_ownership(display.output.handle(), false);
        auto& stream = pipeline.stream();
        if (bloom) {
            const auto& filter = display.bloom_shader;
            stream << filter(src, display.bloom_quarter, display.bloom_half, extent, half_extent, 0u).dispatch(half_extent)
                   << filter(src, display.bloom_half, display.bloom_quarter, half_extent, bloom_extent, 1u).dispatch(bloom_extent)
                   << filter(src, display.bloom_quarter, display.bloom_blur, bloom_extent, bloom_extent, 2u).dispatch(bloom_extent)
                   << filter(src, display.bloom_blur, display.bloom_quarter, bloom_extent, bloom_extent, 3u).dispatch(bloom_extent);
        }
        if (auto_exposure) {
            stream << display.exposure_shader(src, display.bloom_quarter, display.exposure_gain,
                                              extent, bloom_extent, intensity,
                                              settings->minimum_auto_gain, settings->maximum_auto_gain).dispatch(64u);
        }
        stream << display.resolve_shader(src, display.bloom_quarter, display.exposure_gain, display.output,
                                         extent, bloom_extent, intensity, exposure, mode).dispatch(extent)
               << synchronize() << commit();
        device.set_buffer_external_ownership(display.output.handle(), true);
        imported_.set_external_ownership(false);
        return true;
    } catch (const std::exception& error) {
        return fail(error.what());
    }
}

}  // namespace Corona::Systems::Vision
