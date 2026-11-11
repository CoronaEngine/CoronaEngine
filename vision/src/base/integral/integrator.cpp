//
// Created by Zero on 2023/6/19.
//

#include "integrator.h"
#include "base/mgr/switch_profile.h"
#include "base/mgr/pipeline.h"
#include "math/warp.h"
#include "base/color/spectrum.h"
#include "base/sampler.h"
#include <cstdlib>

namespace vision {
using namespace ocarina;

namespace {
Float rtxpt_rr_probability(const Float3 &camera_beta, const Uint &surface_depth, const Uint &total_depth) {
    Float terminate = saturate(0.85f - sqrt(max(luminance(camera_beta), 0.f)));
    return saturate(terminate * terminate +
        max((cast<float>(surface_depth) / cast<float>(total_depth) - 0.5f) * 1.2f, 0.f));
}
}// namespace

double Integrator::cur_denoise_time() const noexcept { return denoise_timing_->elapsed_ms; }

void Integrator::reset_denoise_timing() const noexcept {
    // Queued callbacks own their sample even if this integrator is invalidated.
    denoise_timing_ = make_shared<DenoiseTiming>();
}

void Integrator::begin_denoise_timing(Stream &stream) const noexcept {
    stream << [timing = denoise_timing_] {
        timing->started_at = std::chrono::steady_clock::now();
    };
}

void Integrator::end_denoise_timing(Stream &stream) const noexcept {
    stream << [timing = denoise_timing_] {
        timing->elapsed_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - timing->started_at).count();
    };
}

void Integrator::invalidation() const noexcept {
    reset_denoise_timing();
    cur_stage_profile_ = {};
    frame_index_ = 0u;
    render_time_ = 0;
}

template<typename SF, typename SS>
SampledSpectrum IlluminationIntegrator::direct_lighting(const Interaction &it, const SF &sf, LightSample ls,
                                                        Bool occluded, TSampler &sampler,
                                                        const SampledWavelengths &swl, SS &ss, bool mis) {
    Float3 wi = normalize(ls.p_light - it.pos);
    ScatterEval scatter_eval = sf.evaluate(it.wo, wi, MaterialEvalMode::All,
                                           BxDFFlag::All, TransportMode::Radiance);
    ss = sf.sample(it.wo, sampler);
    Bool is_delta_light = ls.eval.pdf < 0;
    Float weight = mis ? select(is_delta_light, 1.f, vision::MIS_weight<D>(ls.eval.pdf, scatter_eval.pdf())) : 1.f;
    ls.eval.pdf = select(is_delta_light, -ls.eval.pdf, ls.eval.pdf);
    SampledSpectrum Ld = {swl.dimension(), 0.f};
    $if(!occluded && scatter_eval.valid() && ls.valid()) {
        Ld = ls.eval.L * scatter_eval.f * weight / ls.eval.pdf;
        frame_buffer().visualizer()->condition_add_line_segment(it.pos, ls.p_light);
    };
    return Ld;
}

#define VS_MAKE_DIRECT_LIGHTING_INSTANCE(T, U)                                                                                 \
    template SampledSpectrum IlluminationIntegrator::direct_lighting<T, U>(const Interaction &it, const T &sf, LightSample ls, \
                                                                           Bool occluded, TSampler &sampler,                   \
                                                                           const SampledWavelengths &swl, U &ss, bool mis);
VS_MAKE_DIRECT_LIGHTING_INSTANCE(PhaseFunction, PhaseSample)
VS_MAKE_DIRECT_LIGHTING_INSTANCE(MaterialEvaluator, BSDFSample)

#undef VS_MAKE_DIRECT_LIGHTING_INSTANCE

void RenderEnv::initial(TSampler &sampler, const Uint &frame_index, const TSpectrum &spectrum) noexcept {
    Uint2 pixel = dispatch_idx().xy();
    frame_index_.emplace(frame_index);
    SampledWavelengths wavelengths{spectrum->dimension()};
    sampler->temporary([&](Sampler *) {
        sampler->set_seed(pixel, frame_index, -1);
        wavelengths = spectrum->sample_wavelength(sampler);
    });
    swl_.emplace(wavelengths);
}

IlluminationIntegrator::IlluminationIntegrator(const vision::IntegratorDesc &desc)
    : Integrator(desc),
      max_depth_(desc["max_depth"].as_uint(16)),
      min_depth_(desc["min_depth"].as_uint(5)),
      rr_threshold_(desc["rr_threshold"].as_float(1.f)),
      mis_mode_(MISMode(desc["mis_mode"].as_int(0))),
      separate_(desc["separate"].as_bool(false)),
      denoiser_(Node::create_shared<Denoiser>(desc.denoiser_desc)) {
    const char *backend = std::getenv("VISION_RESTIR_GI_BACKEND");
    if ((backend && string_view(backend) == "rtxpt") ||
        desc.value("indirect")["backend"].as_string("legacy") == "rtxpt") separate_ = true;
}

DataWrap IlluminationIntegrator::illumination_settings() const {
    return {{"max_depth", max_depth_.hv()}, {"min_depth", min_depth_.hv()},
            {"rr_threshold", rr_threshold_.hv()}};
}

bool IlluminationIntegrator::merge_illumination_setting(DataWrap &candidate, const std::string &key,
                                                       const DataWrap &value) noexcept {
    const bool depth = key == "max_depth" || key == "min_depth";
    if ((!depth && key != "rr_threshold") || !value.is_number()) return false;
    const double maximum = key == "max_depth" ? 128 : key == "min_depth" ? 20 : 1;
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < 0 || number > maximum ||
        (depth && number != std::floor(number))) return false;
    if (depth) candidate[key] = static_cast<uint>(number);
    else candidate[key] = static_cast<float>(number);
    return true;
}

void IlluminationIntegrator::apply_illumination_settings(const DataWrap &candidate) noexcept {
    // The complete request must be validated before mutating live GPU state.
    max_depth_ = candidate["max_depth"].get<uint>();
    min_depth_ = candidate["min_depth"].get<uint>();
    rr_threshold_ = candidate["rr_threshold"].get<float>();
    update_data();
    upload_immediately();
}

void IlluminationIntegrator::prepare() noexcept {
    switch_profile::Scope profile{"IlluminationIntegrator::prepare", "buffers"};
    encode_data();
    datas().reset_device_buffer_immediately(device(), "IlluminationIntegrator::encoded_data");
    datas().register_self();
    datas().upload_immediately();
}

void IlluminationIntegrator::update_device_data() noexcept {
    if (has_changed()) {
        update_data();
        upload_immediately();
    }
}

void IlluminationIntegrator::compile_path_tracing() noexcept {
    TSensor &camera = scene().sensor();
    TSampler &sampler = renderer().sampler();
    const char *aligned_value = std::getenv("VISION_PT_ALIGNED_LAMBERT");
    const bool aligned_lambert = aligned_value && aligned_value[0] != '\0' && aligned_value[0] != '0';
    if (aligned_lambert) {
        OC_INFO("PT aligned Lambert benchmark active: albedo=0.7, opaque two-sided interpolated shading normal; native PT sampling/NEE/depth/RR, original geometric ray offsets");
    }
    const char *mb_log = std::getenv("VISION_RESTIR_GI_MULTIBOUNCE_BENCH");
    if (mb_log && mb_log[0] == '1') {
        OC_INFO("PT multibounce benchmark active: min_depth=5 max_depth=16 rr_threshold=1; first_RR_after_surface=6; faceforward_geometric_normals; raw_total=pure_indirect_without_primary_Ld");
    }
    Kernel kernel = [&](PTParamVar param) {
        Env::instance().clear_global_vars();
        Uint2 pixel = dispatch_idx().xy();
        Bool sample_pixel = true;
        $if(param.enable_sparse_sampling != 0u) {
            sample_pixel = param.sampling_mask.read(dispatch_id()) != 0u;
        };
        $if(!sample_pixel) {
            param.colors.write(dispatch_id(), make_float4(0.f));
            param.indirect.write(dispatch_id(), make_RadType4(make_float4(0.f)));
            param.direct.write(dispatch_id(), make_RadType4(make_float4(0.f)));
            $return();
        };
        RenderEnv render_env;
        sampler->load_data();
        load_data();
        const Uint &frame_index = param.frame_index;
        const char *mb_offset_value = std::getenv("VISION_BENCH_SAMPLE_OFFSET");
        const uint mb_offset = std::getenv("VISION_RESTIR_GI_MULTIBOUNCE_BENCH") && mb_offset_value
            ? uint(std::strtoul(mb_offset_value, nullptr, 10)) : 0u;
        Uint sample_index = frame_index + mb_offset;
        render_env.initial(sampler, sample_index, spectrum());
        sampler->set_seed(pixel, sample_index, param.dimension_index);
        RayDataVar ray_data = param.rays.read(dispatch_id());
        RayState rs = ray_data->to_ray_state();
        Float scatter_pdf = 1e16f;
        RadType3Var Ld;
        RadType3Var Ld_specular;
        const char *mb = std::getenv("VISION_RESTIR_GI_MULTIBOUNCE_BENCH");
        if (mb && mb[0] == '1') Ld = make_RadType3(make_float3(0.f));
        Float3 bypass = make_float3(0.f);
        Float2 hit_distance = make_float2(0.f);
        HitContext context{Ld, Ld_specular};
        context.aligned_lambert = aligned_lambert;
        const bool lobe_signals = denoiser_ && denoiser_->requires_lobe_signals();
        if (lobe_signals) { context.bypass = &bypass; context.lobe_hitdist = &hit_distance; }
        Float3 L = Integrator::Li(rs, scatter_pdf, context, render_env);
        if (mb && mb[0] == '1') {
            // Ld is the primary-vertex contribution, not the lobe-labelled
            // `indirect` framebuffer. The benchmark exports true indirect here.
            L = max(L - make_float3(Ld), make_float3(0.f));
        }
        // Repurposed channels for the SVGF denoiser: param.direct = DIFFUSE, param.indirect = SPECULAR.
        // diffuse = total - specular keeps diffuse + specular == L exactly (energy conserved);
        // `colors` stays the untouched total (ground truth).
        Float3 L_diffuse = L - Ld_specular;
        param.colors.write(dispatch_id(), make_float4(L, 1.f));
        param.indirect.write(dispatch_id(), make_RadType4(Ld_specular, RadType(1.f)));
        param.direct.write(dispatch_id(), make_RadType4(make_float4(L_diffuse, RadType(1.f))));
        if (lobe_signals) {
            frame_buffer().denoiser_diffuse().write(dispatch_id(), make_float4(L_diffuse - bypass, hit_distance.x));
            frame_buffer().denoiser_specular().write(dispatch_id(), make_float4(Ld_specular, hit_distance.y));
            frame_buffer().denoiser_bypass().write(dispatch_id(), make_float4(bypass, 0.f));
        }
    };
    path_tracing_ = device().compile(kernel, "path_tracing");
}

CommandBatch IlluminationIntegrator::path_tracing(const PTParam &param, uint2 res) const noexcept {
    CommandBatch ret;
    ret << path_tracing_(param).dispatch(res);
    return ret;
}

bool IlluminationIntegrator::render_UI(Widgets *widgets) noexcept {
    bool open = widgets->use_folding_header(
        ocarina::format("{} integrator", impl_type().data()),
        [&] {
            changed_ |= widgets->button_click("recompile", [&] {
                //                MaterialRegistry::instance().remedy();
                //                scene().fill_instances();
                //                pipeline()->update_geometry();
                compile();
            });
            changed_ |= widgets->input_uint_limit("max depth", &max_depth_.hv(), 0, 128, 1, 1);
            changed_ |= widgets->input_uint_limit("min depth", &min_depth_.hv(), 0, 20, 1, 1);
            changed_ |= widgets->drag_float("rr threshold", &rr_threshold_.hv(), 0.01, 0, 1);
            render_sub_UI(widgets);
        });
    denoiser_->render_UI(widgets);
    return open;
}

CommandBatch IlluminationIntegrator::denoise(RealTimeDenoiseInput &input) const noexcept {
    CommandBatch ret;
    if (!denoiser_) {
        return ret;
    }
    ret << denoiser_->dispatch(input);
    return ret;
}

SampledSpectrum IlluminationIntegrator::evaluate_miss(RayState &rs, const Float3 &normal,
                                                      const Float &scatter_pdf, const Uint &bounces,
                                                      const SampledWavelengths &swl, uint light_samples) const noexcept {
    TLightSampler &light_sampler = renderer().light_sampler();
    SampledSpectrum ret = spectrum()->zero();
    const Geometry &geometry = pipeline()->geometry();
    if (light_sampler->env_light()) {
        LightSampleContext p_ref;
        p_ref.pos = rs.origin();
        p_ref.ng = normal;
        SampledSpectrum tr = spectrum()->one();
        if (scene().process_mediums()) {
            rs.ray.dir_max.w = scene().world_diameter();
            tr = geometry.Tr(scene(), renderer().sampler(), swl, rs);
        }
        LightEval eval = light_sampler->evaluate_miss_wi(p_ref, rs.direction(), swl);
        Float weight = MIS_weight_n<D>(1, scatter_pdf, int(light_samples), eval.pdf);
        if (light_samples != 1u) weight = select(ocarina::isinf(scatter_pdf), 1.f, weight);
        weight = correct_bsdf_weight(weight, bounces);
        ret = eval.L * tr * weight;
    }
    return ret;
}

Float3 IlluminationIntegrator::Li(RayState rs, Float scatter_pdf, const Uint &max_depth, SampledSpectrum throughput,
                                  bool only_direct, const HitContext &hc, const RenderEnv &render_env) const noexcept {
    Pipeline *rp = pipeline();
    TSampler &sampler = renderer().sampler();
    TLightSampler &light_sampler = renderer().light_sampler();

    const SampledWavelengths &swl = render_env.sampled_wavelengths();
    const Geometry &geometry = rp->geometry();

    TriangleHitVar hit;
    Interaction it{scene().process_mediums()};
    Float3 prev_surface_ng = rs.direction();

    Float3 ret = make_float3(0.f);

    // --- diffuse/specular split (active only when hc.split(), i.e. the PT denoiser path) ---
    // We accumulate ONLY the specular channel; the diffuse channel is derived downstream as
    // (total - specular), which keeps diffuse + specular == ret exactly (energy conserving).
    Float3 ret_specular = make_float3(0.f);
    Float3 spec_route = make_float3(0.f); // RGB ownership of the primary BSDF sample
    Float3 spec_frac_b0 = make_float3(0.f);
    Float3 ret_direct_specular = make_float3(0.f);
    Float2 distance_sum = make_float2(0.f), distance_weight = make_float2(0.f), sampled_distance = make_float2(0.f);
    Float first_distance = 0.f;
    auto route_spec = [&](const Float3 &lin, const Uint &b, const Float3 &frac_b0, const Float &distance, bool emission) {
        if (!hc.split()) { return; }
        // bounce 0: proportional (emission/miss pass frac_b0=0 -> diffuse; direct passes the
        // BRDF specular fraction). bounce >=1: whole contribution follows the primary lobe.
        Float3 spec = lin * ocarina::select(b == 0u, frac_b0, spec_route);
        ret_specular += spec;
        if (hc.direct_specular) {
            $if(b == 0u || (Bool(hc.restir_direct_split && emission) && b == 1u)) { ret_direct_specular += spec; };
        }
        if (hc.lobe_hitdist) {
            Float2 weight = make_float2(max(luminance(lin - spec), 0.f), max(luminance(spec), 0.f));
            Float hit_t = ocarina::select(b == 0u, distance, first_distance);
            weight *= (hit_t > 0.f).cast<float>();
            distance_sum += weight * hit_t; distance_weight += weight;
        }
    };

    Float3 primary_dir = rs.direction();
    Bool delta_only = true;
    Bool continuation_ready = false;
    Uint continuation_depth = 0u;
    Bool pending_rr_death = false;
    Float pending_rr_boost = 1.f;
    auto mis_bsdf = [&](auto &bounces, bool inner) {
        hit = geometry.trace_closest(rs.ray);
        if (hc.lobe_hitdist) {
            // RTXPT StablePlaneAccumulateSampleHitT: start after the primary
            // scatter, then allow a delta-only entry/exit pair (at most 3 segments).
            $if(bounces == 1u || (bounces > 1u && bounces <= 3u && delta_only)) {
                Float segment_distance = scene().world_diameter();
                $if(hit->is_hit()) {
                    auto endpoint = geometry.compute_surface_interaction(hit, rs.ray);
                    segment_distance = length(endpoint.pos - rs.origin());
                };
                first_distance = select(bounces == 1u, 0.f, first_distance) + max(segment_distance, 1e-5f);
                sampled_distance = make_float2(select(any(spec_route < 1.f), first_distance, 0.f),
                                               select(any(spec_route > 0.f), first_distance, 0.f));
            };
        }
        Bool include_emission = true;
        // The only-direct supplement is already a post-scattering segment,
        // even though its own loop counter starts at zero.
        if (hc.suppress_initial_emission && inner) {
            include_emission = bounces != 0u;
        }
        if (hc.suppress_initial_emission_if && inner) {
            include_emission = bounces != 0u || !*hc.suppress_initial_emission_if;
        }
        comment("miss");
        if (!inner) {
            Bool primary_miss = hc.complete_terminal_direct ? !continuation_ready : all(rs.direction() == primary_dir);
            $if(primary_miss) {
                $super_break;
            };
        }

        $if(hit->is_miss()) {
            $if(include_emission) {
                SampledSpectrum d = evaluate_miss(rs, prev_surface_ng, scatter_pdf, bounces, swl,
                                                 hc.rtxpt_sampling ? 2u : 1u) * throughput;
                Float3 lin = spectrum()->linear_srgb(d, swl);
                ret += lin;
                if (hc.restir_direct_split && hc.Ld) {
                    $if(bounces <= 1u) { *hc.Ld += make_RadType3(lin); };
                }
                route_spec(lin, bounces, make_float3(0.f), 0.f, true);
                if (hc.bypass) { $if(bounces == 0u) { *hc.bypass += lin; }; }
            };
            $super_break;
        };

        it = geometry.compute_surface_interaction(hit, rs.ray);
        const char *mb = std::getenv("VISION_RESTIR_GI_MULTIBOUNCE_BENCH");
        if (mb && mb[0] == '1' && hc.aligned_lambert) {
            Float3 n = normalize(it.ng);
            n = ocarina::select(dot(n, rs.direction()) > 0.f, -n, n);
            it.ng = n;
            it.shading.set_frame(Frame<Float3>{n});
        }
        $if(bounces == 0) {
            frame_buffer().visualizer()->condition_add_normal(it);
        };

        frame_buffer().visualizer()->condition_add_line_segment(rs.origin(), it.pos);

        if (scene().process_mediums()) {
            $if(rs.in_medium()) {
                scene().mediums().dispatch(rs.medium, [&](const Medium *medium) {
                    SampledSpectrum medium_throughput = medium->sample(rs, it, sampler, swl);
                    throughput *= medium_throughput;
                });
            };
        }

        $if(!it.has_material() && !it.has_phase()) {
            //todo remove no material mesh in non volumetric scene
            comment("process no material interaction for volumetric rendering");
            rs = it.spawn_ray_state(rs.direction());
            bounces -= 1;
            $super_continue;
        };

        if (hc.it) {
            $if(bounces == 0) { *hc.it = it; };
        }

        comment("hit light");
        $if(it.has_emission() && include_emission) {
            LightSampleContext p_ref;
            p_ref.pos = rs.origin();
            p_ref.ng = prev_surface_ng;
            LightEval eval = light_sampler->evaluate_hit_wi(p_ref, it, swl);
            SampledSpectrum tr = geometry.Tr(scene(), renderer().sampler(), swl, rs);
            Float weight = hc.rtxpt_sampling
                ? select(ocarina::isinf(scatter_pdf), 1.f, MIS_weight_n<D>(1, scatter_pdf, 2, eval.pdf))
                : MIS_weight<D>(scatter_pdf, eval.pdf);
            weight = correct_bsdf_weight(weight, bounces);
            SampledSpectrum d = eval.L * throughput * weight * tr;
            Float3 lin = spectrum()->linear_srgb(d, swl);
            ret += lin;
            if (hc.restir_direct_split && hc.Ld) {
                $if(bounces <= 1u) { *hc.Ld += make_RadType3(lin); };
            }
            route_spec(lin, bounces, make_float3(0.f), 0.f, true);
            if (hc.bypass) { $if(bounces == 0u) { *hc.bypass += lin; }; }
        };
        prev_surface_ng = it.ng;
    };

    Float eta_scale = 1.f;
    $for(&bounces, 0, max_depth) {
        continuation_ready = false;
        mis_bsdf(bounces, true);
        if (hc.rtxpt_sampling) {
            // Native RR always visits the next emission/miss endpoint first.
            $if(pending_rr_death) { $break; };
            throughput *= pending_rr_boost;
            pending_rr_boost = 1.f;
        }
        Env::instance().set("bounces", bounces);
        comment("estimate direct lighting");
        comment("sample light");
        LightSample light_sample{swl.dimension()};
        Bool occluded = false;
        SampledSpectrum tr = spectrum()->one();
        if (!hc.rtxpt_sampling) {
            light_sample = light_sampler->sample_wi(it, sampler, swl);
            RayState shadow_ray;
            occluded = geometry.occluded(it, light_sample.p_light, &shadow_ray);
            tr = geometry.Tr(scene(), renderer().sampler(), swl, shadow_ray);
        }
        comment("sample bsdf");
        BSDFSample bsdf_sample{swl};
        Bool delta_scatter = false;
        SampledSpectrum Ld = {swl.dimension(), 0.f};

        Float3 albedo = make_float3(0.f);

        // Preserve the PT framework's proposals, BSDF, visibility and Tr while
        // matching RTXPT's six candidates per each of two shadow samples.
        auto sample_lighting = [&](const MaterialEvaluator &evaluator) {
            if (hc.captured_secondary_normal) {
                $if(bounces == 0u) { *hc.captured_secondary_normal = evaluator.shading_frame().normal(); };
            }
            if (!hc.rtxpt_sampling)
                return direct_light_mis(it, evaluator, light_sample, occluded, sampler, swl, bsdf_sample);
            bsdf_sample = evaluator.sample(it.wo, sampler);
            if (mis_mode_ == MISMode::EBSDF) return spectrum()->zero();
            SampledSpectrum lighting = spectrum()->zero();
            $for(shadow_sample, 2u) {
                LightSample selected{swl.dimension()};
                Float total_weight = 0.f, selected_weight = 0.f;
                $for(candidate_index, 6u) {
                    LightSample candidate = light_sampler->sample_wi(it, sampler, swl);
                    Float proposal_pdf = abs(candidate.eval.pdf);
                    Float target = 0.f;
                    $if(candidate.valid() && proposal_pdf > 0.f) {
                        Float3 direction = normalize(candidate.p_light - it.pos);
                        auto eval = evaluator.evaluate(it.wo, direction, MaterialEvalMode::PDF,
                                                       BxDFFlag::All, TransportMode::Radiance);
                        Float3 incoming = spectrum()->linear_srgb(candidate.eval.L, swl) / proposal_pdf;
                        target = max(max(incoming.x, incoming.y), incoming.z) * eval.pdf();
                        target = max(ocarina::zero_if_nan_inf(target), 0.f);
                    };
                    total_weight += target;
                    $if(sampler->next_1d() * total_weight < target) {
                        selected = candidate;
                        selected_weight = target;
                    };
                };
                $if(selected_weight > 0.f) {
                    Float3 direction = normalize(selected.p_light - it.pos);
                    auto eval = evaluator.evaluate(it.wo, direction, MaterialEvalMode::All,
                                                   BxDFFlag::All, TransportMode::Radiance);
                    $if(eval.valid()) {
                        RayState selected_shadow;
                        Bool blocked = geometry.occluded(it, selected.p_light, &selected_shadow);
                        SampledSpectrum transmission = geometry.Tr(scene(), sampler, swl, selected_shadow);
                        Float pdf = abs(selected.eval.pdf);
                        Float mis = mis_mode_ == MISMode::ELight ? Float{1.f}
                            : select(selected.eval.pdf < 0.f, 1.f, MIS_weight_n<D>(2, pdf, 1, eval.pdf()));
                        Float ris_weight = total_weight / (6.f * selected_weight);
                        $if(!blocked && pdf > 0.f) {
                            lighting += selected.eval.L * eval.f * transmission * (ris_weight * mis / (2.f * pdf));
                        };
                    };
                    light_sample = selected;
                };
            };
            return lighting;
        };

        auto sample_surface = [&]() {
            if (hc.aligned_lambert) {
                // Match the controlled GI's Lambert hemisphere; retain it.ng for
                // visibility and ray offsets, changing only the BSDF evaluator.
                auto shading = it.shading;
                Float side = ocarina::select(dot(shading.normal(), it.wo) < 0.f, -1.f, 1.f);
                shading.y *= side;
                shading.z = normalize(shading.normal()) * side;
                shading.dn_du *= side;
                shading.dn_dv *= side;
                Interaction lambert_it = it;
                lambert_it.shading = shading;
                lambert_it.ng = shading.normal();
                MaterialEvaluator evaluator(lambert_it, swl);
                evaluator.update_frame(shading);
                evaluator.link(make_unique<DiffuseLobe>(SampledSpectrum{swl, .7f}, swl, shading));
                Ld = direct_light_mis(it, evaluator, light_sample, occluded,
                                      sampler, swl, bsdf_sample);
                albedo = make_float3(.7f);
                delta_scatter = false;
                spec_frac_b0 = make_float3(0.f);
                spec_route = make_float3(0.f);
                return;
            }
            if (separate_) {
                MaterialEvaluator evaluator(it, swl);
                scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                    outline("PT build material evaluator", [&] {
                        material->build_evaluator(evaluator, it, swl);
                        evaluator.update_frame(it.shading);
                        swl.check_dispersive(spectrum(), evaluator);
                    });
                });
                Ld = sample_lighting(evaluator);
                if (hc.lobe_hitdist) delta_scatter = evaluator.flag() == SurfaceData::NearSpec || BxDFFlag::is_specular(evaluator.flag());
                if (hc.split()) {
                    $if(bounces == 0u) {
                        spec_frac_b0 = hc.lobe_hitdist
                            ? evaluator.specular_fraction_rgb(it.wo, normalize(light_sample.p_light - it.pos))
                            : make_float3(evaluator.specular_fraction(it.wo, normalize(light_sample.p_light - it.pos)));
                        if (hc.lobe_hitdist) spec_route = evaluator.specular_fraction_rgb(it.wo, bsdf_sample.wi);
                    };
                }
                return;
            }
            scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                MaterialEvaluator evaluator = material->create_evaluator(it, swl);
                swl.check_dispersive(spectrum(), evaluator);
                Ld = sample_lighting(evaluator);
                if (hc.lobe_hitdist) delta_scatter = evaluator.flag() == SurfaceData::NearSpec || BxDFFlag::is_specular(evaluator.flag());
                albedo = spectrum()->linear_srgb(evaluator.albedo(it.wo), swl);
                if (hc.split()) {
                    $if(bounces == 0u) {
                        spec_frac_b0 = hc.lobe_hitdist
                            ? evaluator.specular_fraction_rgb(it.wo, normalize(light_sample.p_light - it.pos))
                            : make_float3(evaluator.specular_fraction(it.wo, normalize(light_sample.p_light - it.pos)));
                        if (hc.lobe_hitdist) spec_route = evaluator.specular_fraction_rgb(it.wo, bsdf_sample.wi);
                    };
                }
            });
        };

        if (scene().process_mediums()) {
            $if(it.has_phase()) {
                PhaseSample ps{swl.dimension()};
                Ld = direct_light_mis(it, it.phase(), light_sample, occluded, sampler, swl, ps);
                bsdf_sample.eval = ps.eval;
                bsdf_sample.wi = ps.wi;
            }
            $else {
                sample_surface();
            };
        } else {
            sample_surface();
        }
        SampledSpectrum d = throughput * Ld * tr;
        Float3 lin = spectrum()->linear_srgb(d, swl);
        ret += lin;
        route_spec(lin, bounces, spec_frac_b0, length(light_sample.p_light - it.pos), false);
        if (hc.Ld) {
            $if (bounces == 0u) {
                *hc.Ld = make_RadType3(ret);
            };
        }
        if (hc.split() && !hc.lobe_hitdist) {
            $if(bounces == 0u) {
                Uint sflags = bsdf_sample.eval.flags;
                spec_route = make_float3(ocarina::select(BxDFFlag::is_glossy(sflags) | BxDFFlag::is_specular(sflags), 1.f, 0.f));
            };
        }
        eta_scale *= sqr(bsdf_sample.eta);
        if (hc.lobe_hitdist) delta_only &= delta_scatter;
        Float lum = throughput.max();
        $if(!bsdf_sample.valid() || lum == 0.f) {
            $break;
        };
        throughput *= bsdf_sample.eval.throughput();
        Uint rr_depth = bounces;
        if (hc.rr_depth_offset) { rr_depth += *hc.rr_depth_offset; }
        Float rr_lum = lum;
        if (hc.rr_survival_scale) { rr_lum *= *hc.rr_survival_scale; }
        if (hc.rtxpt_sampling) {
            Uint surface_depth = rr_depth + 1u;
            Uint total_depth = max_depth;
            if (hc.rr_depth_offset) total_depth += *hc.rr_depth_offset;
            $if(surface_depth > *min_depth_ && surface_depth < total_depth) {
                Float3 camera_beta = spectrum()->linear_srgb(throughput, swl);
                if (hc.rr_camera_throughput) camera_beta *= *hc.rr_camera_throughput;
                Float terminate = rtxpt_rr_probability(camera_beta, surface_depth, total_depth);
                pending_rr_death = sampler->next_1d() < terminate;
                $if(!pending_rr_death) { pending_rr_boost = 1.f / (1.f - terminate); };
            };
        } else {
            $if(eta_scale * rr_lum < *rr_threshold_ && rr_depth >= *min_depth_) {
                Float q = min(0.95f, rr_lum);
                Float rr = sampler->next_1d();
                $if(q < rr) { $break; };
                throughput /= q;
            };
        }
        scatter_pdf = bsdf_sample.eval.pdf();
        rs = it.spawn_ray_state(bsdf_sample.wi);
        continuation_ready = true;
        continuation_depth = bounces + 1u;
    };

    if ((only_direct || hc.complete_terminal_direct) && mis_mode_ == MISMode::EBoth) {
        /// Supplement only direct light BSDF sampling
        if (hc.complete_terminal_direct) {
            $if(continuation_ready && continuation_depth == max_depth && max_depth > 0u) {
                $for(&bounce, 1u) {
                    mis_bsdf(continuation_depth, false);
                };
            };
        } else {
            $for(&bounce, 1u) {
                // This ray already scattered at the primary surface. The explicit denoiser input's
                // lobe ownership and hit distance must use that first segment.
                if (hc.lobe_hitdist) mis_bsdf(continuation_depth, false);
                else mis_bsdf(bounce, false);
            };
        }
    }
    if (hc.Ld_specular) {
        *hc.Ld_specular = make_RadType3(ret_specular);
    }
    if (hc.direct_specular) { *hc.direct_specular = ret_direct_specular; }
    if (hc.lobe_hitdist) {
        *hc.lobe_hitdist = ocarina::select(distance_weight > 1e-5f,
            distance_sum / max(distance_weight, make_float2(1e-5f)), sampled_distance);
    }
    return ret;
}

Float3 IlluminationIntegrator::Li(vision::RayState rs, Float scatter_pdf,
                                  SampledSpectrum throughput, const HitContext &hc,
                                  const RenderEnv &render_env) const noexcept {
    return Li(rs, scatter_pdf, *max_depth_, throughput,
              max_depth_.hv() < 2, hc, render_env);
}

}// namespace vision
