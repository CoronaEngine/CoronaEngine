// Call this with the prepared direct.json pipeline from the ReSTIR estimator
// fixture: one diffuse (rho=0.8, sigma=0) triangle at z=-1 and white environment.
// This tests secondary outgoing radiance, not the full scene's indirect image.
#include "base/integral/integrator.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/sampler.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

void check_restir_gi_depth_one_continuation(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    pipeline.upload_data();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto* integrator = dynamic_cast<IlluminationIntegrator*>(pipeline.renderer().integrator().get());
    if (!integrator || pipeline.scene().instances().size() != 1u ||
        !pipeline.scene().light_manager().env_light()) {
        throw std::runtime_error("GI continuation regression needs the prepared diffuse triangle / white environment fixture");
    }
    auto& sampler = pipeline.renderer().sampler();
    auto& spectrum = pipeline.renderer().spectrum();
    const uint pixel_count = pipeline.pixel_num();
    // Each Li invocation needs its own kernel-local RenderEnv and sampler state.
    // Compiling both paths into one kernel can retain DSL variables from the
    // first Li in outlined functions used by the second invocation.
    auto build_shader = [&](bool suppress_initial_emission) {
        Kernel kernel = [&, suppress_initial_emission](Uint frame_index, BufferVar<float4> output) {
            Env::instance().clear_global_vars();
            sampler->load_data();
            integrator->load_data();
            RenderEnv render_env;
            render_env.initial(sampler, frame_index, spectrum);
            // Both independent dispatches use the same pixel/frame seed, so
            // they receive identical NEE and BSDF samples.
            sampler->set_seed(dispatch_idx().xy(), frame_index, Dimension::PathTracing);
            RayVar ray = make_ray(Float3{make_float3(0.f, 0.f, 0.f)},
                                  Float3{make_float3(0.f, 0.f, -1.f)});
            Interaction secondary{false};
            secondary.pos = make_float3(0.f);
            secondary.ng = make_float3(0.f);
            HitContext context{secondary};
            context.suppress_initial_emission = suppress_initial_emission;
            Float3 radiance = integrator->Li(RayState::create(ray), Float{1e16f}, Uint{1u},
                                             spectrum->one(), true, context, render_env);
            Bool valid_receiver = secondary.has_material() && !secondary.has_emission() &&
                                  secondary.prim_id != InvalidUI32 && secondary.ng.z > 0.99f;
            output.write(dispatch_id(), make_float4(radiance, ocarina::select(valid_receiver, 1.f, 0.f)));
        };
        return pipeline.device().compile(kernel, suppress_initial_emission
            ? "restir_gi_depth_one_continuation_suppressed"
            : "restir_gi_depth_one_continuation_ordinary");
    };
    auto ordinary_shader = build_shader(false);
    auto suppressed_shader = build_shader(true);
    auto ordinary_output = pipeline.device().create_buffer<float4>(pixel_count, "restir_gi_continuation_ordinary_samples");
    auto suppressed_output = pipeline.device().create_buffer<float4>(pixel_count, "restir_gi_continuation_suppressed_samples");
    std::vector<float4> ordinary_samples(pixel_count);
    std::vector<float4> suppressed_samples(pixel_count);
    double ordinary_sum = 0.0;
    double suppressed_sum = 0.0;
    float max_pair_difference = 0.f;
    constexpr uint frame_count = 64u;
    for (uint frame = 0; frame < frame_count; ++frame) {
        pipeline.stream() << ordinary_shader(frame, ordinary_output).dispatch(pipeline.resolution())
                          << suppressed_shader(frame, suppressed_output).dispatch(pipeline.resolution())
                          << ordinary_output.download(ordinary_samples.data())
                          << suppressed_output.download(suppressed_samples.data())
                          << synchronize() << commit();
        for (uint pixel = 0; pixel < pixel_count; ++pixel) {
            const auto& ordinary = ordinary_samples[pixel];
            const auto& suppressed = suppressed_samples[pixel];
            if (ordinary.w != 1.f || suppressed.w != 1.f) {
                throw std::runtime_error("GI continuation rays must hit the non-emitting +Z diffuse receiver");
            }
            if (!std::isfinite(ordinary.x) || !std::isfinite(ordinary.y) || !std::isfinite(ordinary.z) ||
                !std::isfinite(suppressed.x) || !std::isfinite(suppressed.y) || !std::isfinite(suppressed.z)) {
                throw std::runtime_error("depth-one outgoing radiance must remain finite");
            }
            ordinary_sum += (double(ordinary.x) + ordinary.y + ordinary.z) / 3.0;
            suppressed_sum += (double(suppressed.x) + suppressed.y + suppressed.z) / 3.0;
            max_pair_difference = std::max({max_pair_difference,
                std::abs(ordinary.x - suppressed.x), std::abs(ordinary.y - suppressed.y),
                std::abs(ordinary.z - suppressed.z)});
        }
    }
    const double count = double(pixel_count) * frame_count;
    const double ordinary_mean = ordinary_sum / count;
    const double suppressed_mean = suppressed_sum / count;
    std::cout << "depth_one_outgoing_ordinary_mean=" << ordinary_mean
              << " depth_one_outgoing_suppressed_mean=" << suppressed_mean
              << " max_same_seed_difference=" << max_pair_difference << '\n';
    // Lambertian rho=0.8 under unit radiance integrates analytically to 0.8.
    // A nonzero-only check would miss the bug: the broken path retains NEE but
    // drops its complementary BSDF MIS term in the only_direct supplement loop.
    if (std::abs(ordinary_mean - 0.8) > 0.04) {
        throw std::runtime_error("ordinary depth-one NEE+BSDF MIS must recover outgoing radiance 0.8");
    }
    if (std::abs(suppressed_mean - 0.8) > 0.04 || max_pair_difference > 1e-5f) {
        throw std::runtime_error("suppress_initial_emission must preserve the post-scattering BSDF MIS supplement at max_depth=1");
    }
    std::cout << "PASS: GI depth-one continuation preserves reflected environment radiance\n";
}
