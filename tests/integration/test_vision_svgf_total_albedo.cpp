// Reuse the normal-mapped substrate fixture installed by
// check_substrate_sample_classification in the embedded mode-switch test.
// This executes the actual SVGF guide helpers on the GPU, including the
// DirectIndirect query_albedo path and its already-corrected specular control.
#define VISION_PLUGIN_NAME "SVGF"
#define VISION_CATEGORY "denoiser"
#include "render_core/denoiser/SVGF/svgf.h"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

void check_svgf_total_albedo(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    bool& individual = MaterialRegistry::instance().individual_ns();
    struct RestoreMode {
        bool& mode;
        bool previous;
        ~RestoreMode() { mode = previous; }
    } restore{individual, individual};

    // eta=1.5 has a spurious negative-cosine Fresnel pole near -0.5547.
    // Bracket it, and cover grazing and normal incidence on both sides.
    constexpr std::array<float, 7> magnitudes{0.5546f, 0.5548f, 0.001f, 0.2f, 0.55f, 0.9f, 1.f};
    std::vector<float> cosines;
    for (float cosine : magnitudes) {
        cosines.push_back(cosine);
        cosines.push_back(-cosine);
    }
    const uint count = static_cast<uint>(cosines.size());
    auto input = pipeline.device().create_buffer<float>(count, "svgf_albedo_test_cosines");
    auto total_output = pipeline.device().create_buffer<float4>(count, "svgf_total_albedo_test");
    auto specular_output = pipeline.device().create_buffer<float4>(count, "svgf_specular_albedo_control");
    std::vector<float4> total(count), specular(count);
    pipeline.stream() << input.upload(cosines.data()) << synchronize() << commit();

    for (bool enabled : {true, false}) {
        individual = enabled;
        auto build_shader = [&](bool specular_only) {
            Kernel kernel = [&, specular_only](BufferVar<float> angles, BufferVar<float4> output) {
                Env::instance().clear_global_vars();
                Uint index = dispatch_id();
                Float3 eye = make_float3(0.f, 0.f, 3.f);
                RayVar ray = make_ray(eye, Float3{make_float3(0.f, 0.f, -1.f)});
                auto hit = pipeline.geometry().trace_closest(ray);
                output.write(index, make_float4(-1.f));
                $if(hit->is_hit()) {
                    auto it = pipeline.geometry().compute_surface_interaction(hit, ray);
                    Float3 normal = svgf::PixelStateUtils::query_shading_normal(&pipeline, hit, eye);
                    Float3 axis = ocarina::select(abs(normal.y) < 0.9f,
                        Float3{make_float3(0.f, 1.f, 0.f)}, Float3{make_float3(1.f, 0.f, 0.f)});
                    Float3 tangent = normalize(cross(normal, axis));
                    Float cosine = angles.read(index);
                    Float3 direction = normal * cosine + tangent * sqrt(max(1.f - cosine * cosine, 0.f));
                    Float3 query_eye = it.pos + direction;
                    Float3 guide;
                    if (specular_only) {
                        guide = svgf::PixelStateUtils::query_specular_albedo(&pipeline, hit, query_eye);
                    } else {
                        guide = svgf::PixelStateUtils::query_albedo(&pipeline, hit, query_eye);
                    }
                    output.write(index, make_float4(guide, dot(normal, normalize(direction))));
                };
            };
            return pipeline.device().compile(kernel, specular_only
                ? (enabled ? "svgf_mapped_specular_albedo_control" : "svgf_shared_specular_albedo_control")
                : (enabled ? "svgf_mapped_total_albedo_regression" : "svgf_shared_total_albedo_regression"));
        };
        auto total_shader = build_shader(false);
        auto specular_shader = build_shader(true);
        pipeline.stream() << total_shader(input, total_output).dispatch(count)
                          << specular_shader(input, specular_output).dispatch(count)
                          << total_output.download(total.data())
                          << specular_output.download(specular.data()) << synchronize() << commit();

        for (uint index = 0; index < count; ++index) {
            const auto& value = total[index];
            const auto& control = specular[index];
            std::cout << "SVGF total guide individual_ns=" << enabled << " cos=" << cosines[index]
                      << " total=(" << value.x << ',' << value.y << ',' << value.z << ")"
                      << " specular=(" << control.x << ',' << control.y << ',' << control.z << ")\n";
            if (std::abs(value.w - cosines[index]) > 1e-4f ||
                std::abs(control.w - cosines[index]) > 1e-4f) {
                throw std::runtime_error("albedo regression must hit the fixture at the requested signed incidence angle");
            }
            const std::array<float, 3> rgb{value.x, value.y, value.z};
            const std::array<float, 3> reference{control.x, control.y, control.z};
            for (unsigned channel = 0; channel < 3; ++channel) {
                if (!std::isfinite(rgb[channel]) || rgb[channel] < 0.f || rgb[channel] > 1.0001f ||
                    !std::isfinite(reference[channel]) || reference[channel] < 0.f || reference[channel] > 1.0001f) {
                    throw std::runtime_error("SVGF substrate reflectance guides must stay finite and within [0,1] on both sides");
                }
                // Normal mapping can rebuild the effective frame for each
                // viewing direction. Compare both helpers at the same pose,
                // rather than assuming mirrored poses have identical guides.
                if (std::abs(rgb[channel] - reference[channel]) > 1e-4f) {
                    throw std::runtime_error("DirectIndirect total guide must match the substrate specular control at the same pose");
                }
            }
            if (value.x + value.y + value.z <= 1e-6f) {
                throw std::runtime_error("substrate guide regression must measure a nonzero material reflectance");
            }
        }
    }
    std::cout << "PASS: SVGF total albedo stays bounded and matches the substrate specular guide for both viewing sides\n";
}
