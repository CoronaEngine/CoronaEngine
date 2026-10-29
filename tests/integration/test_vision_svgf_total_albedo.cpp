// Reuse the normal-mapped Principled fixture installed by
// check_principled_sample_classification in the embedded mode-switch test.
// This executes the actual SVGF guide helpers on the GPU, including the
// DirectIndirect query_albedo path and both component guides.
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
    auto diffuse_output = pipeline.device().create_buffer<float4>(count, "svgf_diffuse_albedo_control");
    std::vector<float4> total(count), specular(count), diffuse(count);
    pipeline.stream() << input.upload(cosines.data()) << synchronize() << commit();

    for (bool enabled : {true, false}) {
        individual = enabled;
        auto build_shader = [&](unsigned component, const char* name) {
            Kernel kernel = [&, component](BufferVar<float> angles, BufferVar<float4> output) {
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
                    if (component == 1u) {
                        guide = svgf::PixelStateUtils::query_specular_albedo(&pipeline, hit, query_eye);
                    } else if (component == 2u) {
                        guide = svgf::PixelStateUtils::query_diffuse_albedo(&pipeline, hit, query_eye);
                    } else {
                        guide = svgf::PixelStateUtils::query_albedo(&pipeline, hit, query_eye);
                    }
                    output.write(index, make_float4(guide, dot(normal, normalize(direction))));
                };
            };
            return pipeline.device().compile(kernel, name);
        };
        auto total_shader = build_shader(0u, enabled ? "svgf_mapped_total_albedo_regression" : "svgf_shared_total_albedo_regression");
        auto specular_shader = build_shader(1u, enabled ? "svgf_mapped_specular_albedo_control" : "svgf_shared_specular_albedo_control");
        auto diffuse_shader = build_shader(2u, enabled ? "svgf_mapped_diffuse_albedo_control" : "svgf_shared_diffuse_albedo_control");
        pipeline.stream() << total_shader(input, total_output).dispatch(count)
                          << specular_shader(input, specular_output).dispatch(count)
                          << diffuse_shader(input, diffuse_output).dispatch(count)
                          << total_output.download(total.data())
                          << specular_output.download(specular.data())
                          << diffuse_output.download(diffuse.data()) << synchronize() << commit();

        bool saw_diffuse = false;
        for (uint index = 0; index < count; ++index) {
            const auto& value = total[index];
            const auto& control = specular[index];
            const auto& diffuse_control = diffuse[index];
            std::cout << "SVGF total guide individual_ns=" << enabled << " cos=" << cosines[index]
                      << " total=(" << value.x << ',' << value.y << ',' << value.z << ")"
                      << " specular=(" << control.x << ',' << control.y << ',' << control.z << ")"
                      << " diffuse=(" << diffuse_control.x << ',' << diffuse_control.y << ',' << diffuse_control.z << ")\n";
            if (std::abs(value.w - cosines[index]) > 1e-4f ||
                std::abs(control.w - cosines[index]) > 1e-4f ||
                std::abs(diffuse_control.w - cosines[index]) > 1e-4f) {
                throw std::runtime_error("albedo regression must hit the fixture at the requested signed incidence angle");
            }
            const std::array<float, 3> rgb{value.x, value.y, value.z};
            const std::array<float, 3> reference{control.x, control.y, control.z};
            const std::array<float, 3> diffuse_rgb{diffuse_control.x, diffuse_control.y, diffuse_control.z};
            for (unsigned channel = 0; channel < 3; ++channel) {
                if (!std::isfinite(rgb[channel]) || rgb[channel] < 0.f || rgb[channel] > 1.0001f ||
                    !std::isfinite(reference[channel]) || reference[channel] < 0.f || reference[channel] > 1.0001f ||
                    !std::isfinite(diffuse_rgb[channel]) || diffuse_rgb[channel] < 0.f || diffuse_rgb[channel] > 1.0001f) {
                    throw std::runtime_error("SVGF Principled reflectance guides must stay finite and within [0,1] on both sides");
                }
                // Normal mapping can rebuild the effective frame for each
                // viewing direction. Compare both helpers at the same pose,
                // rather than assuming mirrored poses have identical guides.
                if (std::abs(rgb[channel] - reference[channel] - diffuse_rgb[channel]) > 1e-4f) {
                    throw std::runtime_error("DirectIndirect total guide must equal the Principled diffuse plus specular guides at the same pose");
                }
                saw_diffuse |= diffuse_rgb[channel] > 1e-4f;
            }
            if (value.x + value.y + value.z <= 1e-6f) {
                throw std::runtime_error("Principled guide regression must measure a nonzero material reflectance");
            }
        }
        if (!saw_diffuse) throw std::runtime_error("Principled total guide regression must exercise its diffuse component");
    }
    std::cout << "PASS: SVGF total albedo stays bounded and equals the Principled component sum for both viewing sides\n";
}
