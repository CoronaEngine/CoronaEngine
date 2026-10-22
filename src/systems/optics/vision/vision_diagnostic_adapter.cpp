#include "vision_diagnostic_adapter.h"

#include <corona/shared_data_hub.h>
#include "base/mgr/pipeline.h"
#include "base/sensor/sensor.h"

namespace Corona::Systems::Vision {

struct VisionDiagnosticAdapter::Impl {
    ::vision::Sensor* sensor{};
    ::vision::GeometryGpuResource* geometry{};
    ocarina::Shader<void(ocarina::Buffer<ocarina::float4>, ocarina::uint)> shader;
};

VisionDiagnosticAdapter::VisionDiagnosticAdapter() : impl_(std::make_unique<Impl>()) {}
VisionDiagnosticAdapter::~VisionDiagnosticAdapter() = default;

bool VisionDiagnosticAdapter::supports(CameraOutputMode mode) noexcept {
    return mode == CameraOutputMode::Normal || mode == CameraOutputMode::ObjectID ||
        mode == CameraOutputMode::VisibilityBuffer || mode == CameraOutputMode::WorldPosition;
}

bool VisionDiagnosticAdapter::render(::vision::Pipeline& pipeline, CameraOutputMode mode) {
    if (!supports(mode) || !pipeline.geometry().has_gpu_resource()) return false;
    using namespace ocarina;
    pipeline.activate_global_context();
    auto* sensor = pipeline.scene().sensor().get();
    auto* geometry = pipeline.geometry().gpu_resource().get();
    if (impl_->sensor != sensor || impl_->geometry != geometry) {
        ocarina::Kernel kernel = [&](BufferVar<float4> output, Uint output_mode) {
            sensor->load_data();
            const Uint2 pixel = dispatch_idx().xy();
            ::vision::SensorSample sample{pixel};
            sample.p_lens = make_float2(.5f);
            sample.time = 0.f;
            auto ray = sensor->generate_ray(sample).ray;
            auto hit = pipeline.geometry().trace_closest(ray);
            Float3 color = make_float3(0.f);
            $if(hit->is_hit()) {
                // IDs are Vision instance IDs, independent of material validity.
                $if(output_mode == static_cast<uint>(CameraOutputMode::ObjectID) ||
                    output_mode == static_cast<uint>(CameraOutputMode::VisibilityBuffer)) {
                    Uint id = hit.inst_id + 1u;
                    $if(output_mode == static_cast<uint>(CameraOutputMode::VisibilityBuffer)) {
                        id = id * 1664525u + hit.prim_id;
                    };
                    color = fract(make_float3(.1031f, .11369f, .13787f) * cast<float>(id % 65521u));
                }
                $else {
                    auto interaction = pipeline.geometry().compute_surface_interaction(hit, false);
                    $if(output_mode == static_cast<uint>(CameraOutputMode::Normal)) {
                        // Show the triangle's geometric normal in Corona world
                        // coordinates; zero imported shading normals cannot hide it.
                        Float3 normal = interaction.ng * make_float3(1.f, 1.f, -1.f);
                        color = normal * .5f + .5f;
                    }
                    $else {
                        color = fract(abs(interaction.pos) * .1f);
                    };
                };
            };
            output.write(dispatch_id(), make_float4(color, 1.f));
        };
        impl_->shader = pipeline.device().compile(kernel, "Corona-Vision-GeometryDiagnostic");
        impl_->sensor = sensor;
        impl_->geometry = geometry;
    }
    // Reset before writing: invalidation clears the accumulation buffer when
    // enabled, and that buffer may also be our selected output destination.
    // Returning to PT starts at frame zero and overwrites diagnostic pixels.
    pipeline.invalidate();
    pipeline.stream() << impl_->shader(pipeline.frame_buffer()->display_source_buffer(),
                                      static_cast<uint>(mode)).dispatch(pipeline.resolution())
                      << synchronize() << commit();
    return true;
}

} // namespace Corona::Systems::Vision
