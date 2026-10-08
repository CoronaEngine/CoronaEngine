#include "base/integral/integrator.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/sampler.h"
#include "render_core/integrator/ReSTIR/direct.h"
#include "render_core/integrator/ReSTIR/indirect.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

// Independent NearSpec camera-prefix reference, with ordinary MIS transport
// after the first non-prefix vertex. It does not consult any layer metadata.
void check_glass_transport_energy(vision::Pipeline &pipeline, unsigned recursion, const char *label) {
    using namespace vision;
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto *integrator = dynamic_cast<IlluminationIntegrator *>(pipeline.renderer().integrator().get());
    auto &sampler = pipeline.renderer().sampler();
    auto &spectrum = pipeline.renderer().spectrum();
    auto &lights = pipeline.renderer().light_sampler();
    auto &fb = *pipeline.frame_buffer();
    const auto &geometry = pipeline.geometry();
    uint total_depth = integrator->suffix_depth() + 1u;
    Kernel kernel = [&](Uint frame, BufferVar<float4> output) {
        Env::instance().clear_global_vars();
        sampler->load_data(); integrator->load_data();
        RenderEnv env; env.initial(sampler, frame, spectrum);
        sampler->set_seed(dispatch_idx().xy(), frame + 50000u, Dimension::PathTracing);
        RayState ray = fb.rays().read(dispatch_id())->to_ray_state();
        SampledSpectrum weight = spectrum->one();
        Float3 radiance = make_float3(0.f);
        Uint depth = 0u;
        $loop {
            auto hit = geometry.trace_closest(ray.ray);
            LightSampleContext ref; ref.pos = ray.origin(); ref.ng = ray.direction();
            $if(hit->is_miss()) {
                if (lights->env_light()) {
                    auto eval = lights->evaluate_miss_wi(ref, ray.direction(), env.sampled_wavelengths(), LightEvalMode::L);
                    radiance += spectrum->linear_srgb(eval.L * weight, env.sampled_wavelengths());
                }
                $break;
            };
            Interaction it = geometry.compute_surface_interaction(hit, ray.ray);
            Bool near_specular = false;
            BSDFSample bs{env.sampled_wavelengths()};
            $if(depth < total_depth) {
                pipeline.scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                    auto evaluator = material->create_evaluator(it, env.sampled_wavelengths());
                    near_specular = Bool(material->enable_delta()) && evaluator.flag() == SurfaceData::NearSpec && depth + 1u < recursion;
                    $if(near_specular) { bs = evaluator.sample_delta(it.wo, sampler); };
                });
            };
            $if(near_specular || depth >= total_depth) {
                $if(it.has_emission()) {
                    auto eval = lights->evaluate_hit_wi(ref, it, env.sampled_wavelengths());
                    radiance += spectrum->linear_srgb(eval.L * weight, env.sampled_wavelengths());
                };
                $if(depth >= total_depth || !bs.valid()) { $break; };
                weight *= bs.eval.throughput(); ray = it.spawn_ray_state(bs.wi); depth += 1u;
            }
            $else {
                HitContext context;
                context.complete_terminal_direct = true;
                ray.ray.dir_max.w = ray_t_max;
                radiance += integrator->Li(ray, Float{1e16f}, total_depth - depth, weight,
                    total_depth - 1u < 2u, context, env);
                $break;
            };
        };
        output.write(dispatch_id(), make_float4(radiance, 1.f));
    };
    auto reference = pipeline.device().compile(kernel, "glass_independent_nearspec_reference");
    auto reference_buffer = pipeline.device().create_buffer<float4>(pipeline.pixel_num(), "glass_reference_samples");
    std::vector<float4> actual(pipeline.pixel_num()), expected(actual.size()), layer(actual.size());
    constexpr uint batches = 16u, frames_per_batch = 4u;
    double means[2][batches]{};
    double layer_energy[StablePlaneCount]{};
    double layer_rgb[StablePlaneCount][3]{};
    auto luminance = [](const auto &v) { return .2126 * v.x + .7152 * v.y + .0722 * v.z; };
    for (uint batch = 0; batch < batches; ++batch) {
        for (uint sample = 0; sample < frames_per_batch; ++sample) {
            uint frame = batch * frames_per_batch + sample;
            pipeline.display(1.0 / 60.0);
            pipeline.stream() << reference(frame, reference_buffer).dispatch(pipeline.resolution())
                << reference_buffer.download(expected.data()) << fb.rt_buffer().view().download(actual.data())
                << synchronize() << commit();
            for (uint i = 0; i < pipeline.pixel_num(); ++i) {
                for (int c = 0; c < 3; ++c) {
                    if (!std::isfinite(actual[i][c]) || !std::isfinite(expected[i][c]))
                        throw std::runtime_error("glass transport must remain finite, including zero-prefix channels");
                }
                means[0][batch] += luminance(actual[i]); means[1][batch] += luminance(expected[i]);
            }
            for (uint l = 0; l < StablePlaneCount; ++l) {
                pipeline.stream() << fb.stable_direct_view(l).download(layer.data()) << synchronize() << commit();
                for (const auto &value : layer) {
                    layer_energy[l] += luminance(value);
                    for (int channel = 0; channel < 3; ++channel) { layer_rgb[l][channel] += value[channel]; }
                }
            }
        }
        for (auto &series : means) { series[batch] /= pipeline.pixel_num() * frames_per_batch; }
    }
    double mean[2]{}, variance[2]{};
    for (int i = 0; i < 2; ++i) {
        for (double v : means[i]) { mean[i] += v / batches; }
        for (double v : means[i]) { variance[i] += (v - mean[i]) * (v - mean[i]) / (batches - 1u); }
    }
    double se = std::sqrt((variance[0] + variance[1]) / batches);
    std::cout << label << " layered_mean=" << mean[0] << " reference_mean=" << mean[1]
        << " combined_SE=" << se << " layer_direct_energy=" << layer_energy[0] << ',' << layer_energy[1] << ',' << layer_energy[2]
        << " sizeof_plane=" << sizeof(StablePlaneData) << '\n';
    if (std::abs(mean[0] - mean[1]) > 3.0 * se + 1e-6)
        throw std::runtime_error("glass layered energy differs from independent NearSpec reference by over three combined standard errors");
    if (std::string(label) == "two-sided" && (layer_energy[1] <= 0 || layer_energy[2] <= 0))
        throw std::runtime_error("both glass child layers must own radiance, including the non-dominant reflection");
    if (std::string(label) == "two-sided" &&
        (layer_rgb[1][0] <= 2 * layer_rgb[1][2] || layer_rgb[2][2] <= 2 * layer_rgb[2][0]))
        throw std::runtime_error("glass transmission must retain its red target and residual reflection its blue target, never white missed-ray sky");
    if (std::string(label) == "occluded-reflection" && layer_energy[2] > 1e-7)
        throw std::runtime_error("black occluder must remove non-dominant target direct radiance");
}

void check_glass_depth_guides(vision::Pipeline &pipeline) {
    using namespace vision;
    auto &camera = pipeline.scene().sensor();
    auto &fb = *pipeline.frame_buffer();
    DIParam dp{}; GIParam gp{};
    dp.t_depth = gp.t_depth = 0.1f;
    dp.t_dot = gp.t_dot = 0.8f;
    dp.diff_factor = gp.diff_factor = 0.3f;
    Kernel kernel = [&](BufferVar<StablePlaneData> current, BufferVar<StablePlaneData> previous,
                        BufferVar<float4> output, Var<DIParam> di, Var<GIParam> gi) {
        camera->load_data();
        auto c = current.read(136u), p = previous.read(136u);
        Float expected = camera->linear_depth(c.depth_position);
        Bool depth = c.surface.approximate != 0u && abs(expected - c.surface->depth()) < 1e-5f &&
            length(c.surface.depth_position - c.depth_position) < 1e-6f;
        Bool di_valid = ReSTIRDI::is_temporal_valid(c.surface, p.surface, di, nullptr);
        Bool gi_valid = ReSTIRGI::is_temporal_valid(c.surface, p.surface, gi, nullptr);
        Float2 expected_motion = camera->raster_coord(c.surface.virtual_position).xy() -
            camera->prev_raster_coord(c.surface.virtual_position).xy();
        Bool motion = length(c.motion - expected_motion) < 1e-5f &&
            length(fb.motion_vectors().read(136u) - c.motion) < 1e-5f;
        output.write(0u, make_float4(cast<float>(depth), cast<float>(di_valid), cast<float>(gi_valid), cast<float>(motion)));
    };
    auto shader = pipeline.device().compile(kernel, "glass_physical_path_depth_history");
    auto result = pipeline.device().create_buffer<float4>(1u, "glass_depth_history_result");
    for (unsigned motion_case : {0u, 1u, 2u}) {
        if (motion_case == 1u) { camera->set_position(make_float3(0.15f, 0.f, 0.f)); camera->update_device_data(); }
        if (motion_case == 2u) { camera->update_yaw(2.f); camera->update_device_data(); }
        pipeline.upload_data(); pipeline.display(1.0 / 60.0);
        uint frame = pipeline.frame_index() - 1u;
        float4 value;
        pipeline.stream() << shader(fb.cur_stable_planes_view(frame, 1u), fb.prev_stable_planes_view(frame, 1u), result, dp, gp).dispatch(1u)
            << result.download(&value) << synchronize() << commit();
        if (value.x != 1.f || value.y != 1.f || value.z != 1.f || value.w != 1.f)
            throw std::runtime_error("glass static and translated history must use path-depth position consistently in DI and GI");
        std::vector<StablePlaneData> planes(pipeline.pixel_num());
        pipeline.stream() << fb.cur_stable_planes_view(frame, 1u).download(planes.data()) << synchronize() << commit();
        const auto motion = planes[136].motion;
        if ((motion_case == 0u && std::hypot(motion.x, motion.y) > 1e-4f) ||
            (motion_case == 1u && motion.x >= -1e-4f) || (motion_case == 2u && std::abs(motion.x) < 1e-4f))
            throw std::runtime_error("glass static motion must vanish and rightward camera translation must move image left");
    }
}
