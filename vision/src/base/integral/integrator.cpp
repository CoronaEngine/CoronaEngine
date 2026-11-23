//
// Created by Zero on 2023/6/19.
//

#include "integrator.h"
#include "restir_shadow.h"
#include "base/mgr/switch_profile.h"
#include "base/mgr/pipeline.h"
#include "math/warp.h"
#include "base/color/spectrum.h"
#include "base/scattering/prepared_bsdf.h"
#include "base/scattering/gi_principled_parameters.h"
#include "base/scattering/gi_ideal_glass.h"
#include "base/sampler.h"
#include <cstdlib>
#include "path_work_capture.h"

namespace vision {
using namespace ocarina;

namespace {
Float pt_signal_accumulate_hit_distance(const Float &current, const Float &segment,
                                       const Uint &bounces_from_plane, const Bool &delta_only) {
    return ocarina::select(bounces_from_plane == 1u, segment,
        ocarina::select(bounces_from_plane > 1u && bounces_from_plane <= 3u && delta_only, current + segment, current));
}

Float3 pt_signal_specular_fraction(const Float3 &direct_fraction, const Bool &base_diffuse,
                                  const Uint &bounce, const Uint &base_bounce, bool emission) {
    Float3 suffix_fraction = ocarina::select(base_diffuse, Float3{make_float3(0.f)}, Float3{make_float3(1.f)});
    return emission ? suffix_fraction : ocarina::select(bounce == base_bounce, direct_fraction, suffix_fraction);
}

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
    if (desc.value("indirect")["backend"].as_string("legacy") == "rtxpt") separate_ = true;
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

void IlluminationIntegrator::compile_path_tracing(bool lobe_signals) noexcept {
    lobe_signals = lobe_signals && denoiser_ && denoiser_->requires_lobe_signals();
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
    const bool count_work = path_cost::enabled();
    medium_diagnostics_compiled_ = participating_media_enabled() && scene().process_mediums() && medium_diagnostics_enabled();
    if (medium_diagnostics_compiled_) {
        medium_diagnostic_counters_ = device().create_buffer<uint>(2u, "PT medium event diagnostics");
        OC_INFO("PT medium event diagnostics enabled: atomic crossing/scattering counters and per-frame readback; diagnostic timings are instrumented");
    }
    if (count_work) {
        path_work_counters_ = device().create_buffer<path_cost::PathWorkCounters>(pipeline()->pixel_num(), "Ordinary PT work counters");
        OC_INFO("Path work counts enabled: mode=pt, stride=64, API-level calls");
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
        if (participating_media_enabled() && scene().process_mediums()) {
            rs.medium = pipeline()->geometry().initial_medium(rs.origin(), rs.medium);
        }
        Float scatter_pdf = 1e16f;
        RadType3Var Ld;
        RadType3Var Ld_specular;
        const char *mb = std::getenv("VISION_RESTIR_GI_MULTIBOUNCE_BENCH");
        if (mb && mb[0] == '1') Ld = make_RadType3(make_float3(0.f));
        Float3 bypass = make_float3(0.f);
        Float2 hit_distance = make_float2(0.f);
        optional<path_cost::PathWorkCountersVar> work_counters;
        if (count_work) work_counters.emplace();
        HitContext context{Ld, Ld_specular};
        optional<Uint2> medium_events;
        if (medium_diagnostics_compiled_) {
            medium_events.emplace(make_uint2(0u));
            context.medium_events = &*medium_events;
        }
        context.work_counters = work_counters ? &*work_counters : nullptr;
        context.aligned_lambert = aligned_lambert;
        if (lobe_signals) { context.bypass = &bypass; context.lobe_hitdist = &hit_distance; }
        Float3 L = Integrator::Li(rs, scatter_pdf, context, render_env);
        if (medium_events) {
            (void)medium_diagnostic_counters_.atomic(Uint{0u}).fetch_add(medium_events->x);
            (void)medium_diagnostic_counters_.atomic(Uint{1u}).fetch_add(medium_events->y);
        }
        if (work_counters) path_work_counters_.write(dispatch_id(), *work_counters);
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
    SP<array<uint, 2>> medium_counts;
    if (medium_diagnostics_compiled_) {
        if (param.frame_index == 0u) medium_diagnostics_totals_ = make_shared<MediumDiagnosticTotals>();
        medium_counts = make_shared<array<uint, 2>>(array<uint, 2>{0u, 0u});
        ret << medium_diagnostic_counters_.upload(medium_counts->data());
    }
    ret << path_tracing_(param).dispatch(res);
    if (medium_counts) {
        ret << medium_diagnostic_counters_.download(medium_counts->data()) << synchronize();
        ret << [medium_counts, totals = medium_diagnostics_totals_] {
            totals->boundary_crossings += (*medium_counts)[0];
            totals->scattering_events += (*medium_counts)[1];
            ++totals->sampled_frames;
        };
    }
    if (path_cost::enabled()) ret << path_cost::capture(path_work_counters_.view(), param.frame_index, res, "pt");
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
                                                      const SampledWavelengths &swl, uint light_samples,
                                                      const Bool *delta_scatter) const noexcept {
    TLightSampler &light_sampler = renderer().light_sampler();
    SampledSpectrum ret = spectrum()->zero();
    if (light_sampler->env_light()) {
        LightSampleContext p_ref;
        p_ref.pos = rs.origin();
        p_ref.ng = normal;
        SampledSpectrum tr = spectrum()->one();
        // The continuation segment was already weighted by medium sampling.
        // NEE uses an independent shadow Tr; emitter/miss hits must not apply
        // Beer attenuation a second time to this sampled continuation.
        LightEval eval = light_sampler->evaluate_miss_wi(p_ref, rs.direction(), swl);
        Float weight = MIS_weight_n<D>(1, scatter_pdf, int(light_samples), eval.pdf);
        if (light_samples != 1u) weight = select(ocarina::isinf(scatter_pdf), 1.f, weight);
        if (delta_scatter) weight = select(*delta_scatter, 1.f, weight);
        weight = correct_bsdf_weight(weight, bounces);
        ret = eval.L * tr * weight;
    }
    return ret;
}

Float3 IlluminationIntegrator::Li(RayState rs, Float scatter_pdf, const Uint &max_depth, SampledSpectrum throughput,
                                  bool only_direct, const HitContext &hc, const RenderEnv &render_env) const noexcept {
    const bool use_media = participating_media_enabled() && scene().process_mediums();
    const char *nee_cost_mode = std::getenv("VISION_PT_COST_NEE");
    const bool cost_skip_nee = nee_cost_mode && string_view(nee_cost_mode) == "off";
    if (cost_skip_nee) {
        OC_ASSERT(!scene().process_mediums());
        OC_INFO("Ordinary PT NEE cost diagnostic: proposals retained, visibility and direct F/PDF skipped, no media");
    }
    const char *nee_diagnostic = hc.rtxpt_sampling ? std::getenv("VISION_GI_NEE_DIAGNOSTIC") : nullptr;
    const bool nee_one_by_one = nee_diagnostic && string_view(nee_diagnostic) == "1x1";
    OC_ERROR_IF(nee_diagnostic && !nee_one_by_one, "GI NEE diagnostic supports only 1x1.");
    const uint nee_candidates = nee_one_by_one ? 1u : 6u;
    const uint nee_full_samples = nee_one_by_one ? 1u : 2u;
    if (nee_one_by_one) {
        OC_INFO("GI NEE diagnostic: 1x1, candidates=1, full_samples=1, visibility=true");
    }
    Pipeline *rp = pipeline();
    TSampler &sampler = renderer().sampler();
    TLightSampler &light_sampler = renderer().light_sampler();

    const SampledWavelengths &swl = render_env.sampled_wavelengths();
    const Geometry &geometry = rp->geometry();

    optional<Bool> gi_previous_delta;
    if (gi_principled::reading() || gi_principled::writing()) gi_previous_delta.emplace(false);
    const bool reflection_only_direct = hc.path_capture && hc.path_capture->reflection_only_direct;

    TriangleHitVar hit;
    Interaction it{use_media};
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
    Float3 signal_direct_fraction = make_float3(0.f);
    auto route_spec = [&](const Float3 &lin, const Uint &b, const Float3 &frac_b0, const Float &distance, bool emission) {
        if (hc.path_capture && hc.path_capture->on_signal) {
            auto &capture = *hc.path_capture;
            Float3 fraction = pt_signal_specular_fraction(signal_direct_fraction, capture.signal_base_diffuse,
                b, capture.signal_bounce, emission);
            Float hit_t = emission ? capture.signal_hit_distance
                : pt_signal_accumulate_hit_distance(capture.signal_hit_distance, distance,
                    b - capture.signal_bounce + 1u, Bool{false});
            capture.on_signal(lin, lin * fraction, hit_t, capture.signal_hit_distance, b, emission);
        }
        if (!hc.split()) { return; }
        // bounce 0: proportional (emission/miss pass frac_b0=0 -> diffuse; direct passes the
        // BRDF specular fraction). bounce >=1: whole contribution follows the primary lobe.
        Float3 spec = lin * ocarina::select(b == 0u, frac_b0, spec_route);
        ret_specular += spec;
        if (hc.direct_specular) {
            $if(b == 0u || (Bool(hc.restir_direct_split && emission) && b == 1u)) { ret_direct_specular += spec; };
        }
        if (hc.lobe_hitdist && (!hc.path_capture || hc.path_capture->record_hit_distance)) {
            Float2 weight = make_float2(max(luminance(lin - spec), 0.f), max(luminance(spec), 0.f));
            // Keep primary direct specular radiance. Its shadow-ray distance
            // does not describe a hit along the sampled reflection path.
            weight.y = ocarina::select(b == 0u, 0.f, weight.y);
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
        // Host-gated out of ordinary PT. Transmission has no competing DI
        // technique; genuinely discrete reflection is PT-owned as well.
        optional<Bool> gi_unique_emission;
        const Bool *emission_delta = gi_previous_delta ? &*gi_previous_delta : nullptr;
        if (reflection_only_direct) {
            const auto &capture = *hc.path_capture;
            gi_unique_emission.emplace((gi_previous_delta ? *gi_previous_delta : Bool{false}) ||
                (bounces == 1u && !gi_camera_emission_owned_by_di(capture.first_scatter_transmission,
                    capture.first_reflection_covered_by_di, bounces)));
            emission_delta = &*gi_unique_emission;
        }
        if (hc.work_counters) hc.work_counters->trace.x += 1u;
        if (use_media) {
            hit = geometry.trace_transport(scene(), sampler, swl, rs, it, throughput, hc.medium_events);
        } else {
            hit = geometry.trace_closest(rs.ray);
        }
        if (hc.path_capture && hc.path_capture->on_signal && hc.path_capture->record_hit_distance) {
            auto &capture = *hc.path_capture;
            Uint relative_bounce = bounces - capture.signal_bounce;
            $if(relative_bounce == 1u || (relative_bounce > 1u && relative_bounce <= 3u && capture.signal_delta_only)) {
                Float segment = capture.native_signal_miss ? Float{50000.f} : Float{scene().world_diameter()};
                if (capture.reuse_signal_distance) {
                    // Hit distance is recorded after the ordinary interaction
                    // reconstruction below, before any hit emission observer.
                    $if(hit->is_miss()) {
                        capture.signal_hit_distance = pt_signal_accumulate_hit_distance(
                            capture.signal_hit_distance, segment, relative_bounce, capture.signal_delta_only);
                    };
                } else {
                    $if(hit->is_hit()) {
                        if (hc.work_counters) hc.work_counters->trace.z += 1u;
                        auto endpoint = geometry.compute_surface_interaction(hit, rs.ray);
                        segment = length(endpoint.pos - rs.origin());
                    };
                    capture.signal_hit_distance = pt_signal_accumulate_hit_distance(
                        capture.signal_hit_distance, segment, relative_bounce, capture.signal_delta_only);
                }
            };
        }
        if (hc.lobe_hitdist && (!hc.path_capture || hc.path_capture->record_hit_distance)) {
            // RTXPT StablePlaneAccumulateSampleHitT: start after the primary
            // scatter, then allow a delta-only entry/exit pair (at most 3 segments).
            $if(bounces == 1u || (bounces > 1u && bounces <= 3u && delta_only)) {
                Float segment_distance = scene().world_diameter();
                $if(hit->is_hit()) {
                    if (hc.work_counters) hc.work_counters->trace.z += 1u;
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

        Bool missed = hit->is_miss();
        if (use_media) missed &= !it.has_phase();
        $if(missed) {
            $if(include_emission) {
                SampledSpectrum d = [&] {
                    if (!hc.path_capture || !hc.path_capture->record_radiance) return evaluate_miss(rs, prev_surface_ng, scatter_pdf, bounces, swl,
                                                              hc.rtxpt_sampling ? nee_full_samples : 1u,
                                                              emission_delta) * throughput;
                    auto incoming = evaluate_miss(rs, prev_surface_ng, scatter_pdf, bounces, swl, 1u,
                                                  emission_delta);
                    $if(hc.path_capture->active && bounces > 1u) {
                        hc.path_capture->radiance += spectrum()->linear_srgb(incoming * SampledSpectrum{hc.path_capture->beta}, swl);
                    };
                    return incoming * throughput;
                }();
                Float3 lin = spectrum()->linear_srgb(d, swl);
                if (hc.path_capture && hc.path_capture->on_emission) {
                    hc.path_capture->on_emission(hit, rs.direction(), bounces, lin,
                        lin * select(bounces == 0u, make_float3(0.f), spec_route));
                }
                ret += lin;
                if (hc.restir_direct_split && hc.Ld) {
                    if (reflection_only_direct) {
                        $if(gi_camera_emission_owned_by_di(hc.path_capture->first_scatter_transmission,
                            hc.path_capture->first_reflection_covered_by_di, bounces)) { *hc.Ld += make_RadType3(lin); };
                    } else {
                        $if(bounces <= 1u) { *hc.Ld += make_RadType3(lin); };
                    }
                }
                route_spec(lin, bounces, make_float3(0.f), 0.f, true);
                if (hc.bypass) { $if(bounces == 0u) { *hc.bypass += lin; }; }
            };
            $super_break;
        };

        if (hc.work_counters) hc.work_counters->trace.z += 1u;
        if (!use_media) it = geometry.compute_surface_interaction(hit, rs.ray);
        if (hc.path_capture && hc.path_capture->on_signal && hc.path_capture->record_hit_distance &&
            hc.path_capture->reuse_signal_distance) {
            auto &capture = *hc.path_capture;
            Uint relative_bounce = bounces - capture.signal_bounce;
            $if(relative_bounce == 1u || (relative_bounce > 1u && relative_bounce <= 3u && capture.signal_delta_only)) {
                capture.signal_hit_distance = pt_signal_accumulate_hit_distance(
                    capture.signal_hit_distance, length(it.pos - rs.origin()), relative_bounce, capture.signal_delta_only);
            };
        }
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

        $if(!it.has_material() && !it.has_phase()) {
            //todo remove no material mesh in non volumetric scene
            comment("process no material interaction for volumetric rendering");
            rs = it.spawn_ray_state(rs.direction());
            bounces -= 1;
            $super_continue;
        };

        if (hc.work_counters) {
            hc.work_counters->path.y += 1u;
            hc.work_counters->path.z = max(hc.work_counters->path.z, bounces + 1u);
        }

        if (hc.it) {
            $if(bounces == 0) { *hc.it = it; };
        }
        if (hc.path_capture && hc.path_capture->on_endpoint) {
            $if(hc.path_capture->active && bounces == hc.path_capture->receiver_bounce + 1u) {
                hc.path_capture->on_endpoint(it);
            };
        }

        comment("hit light");
        $if(it.has_emission() && include_emission) {
            LightSampleContext p_ref;
            p_ref.pos = rs.origin();
            p_ref.ng = prev_surface_ng;
            LightEval eval = light_sampler->evaluate_hit_wi(p_ref, it, swl);
            SampledSpectrum tr = spectrum()->one();
            Float weight = hc.rtxpt_sampling
                ? select(ocarina::isinf(scatter_pdf), 1.f, MIS_weight_n<D>(1, scatter_pdf, int(nee_full_samples), eval.pdf))
                : MIS_weight<D>(scatter_pdf, eval.pdf);
            if (gi_previous_delta) weight = select(*gi_previous_delta, 1.f, weight);
            if (reflection_only_direct) weight = gi_camera_emission_bsdf_weight(weight,
                hc.path_capture->first_scatter_transmission, hc.path_capture->first_reflection_covered_by_di, bounces);
            weight = correct_bsdf_weight(weight, bounces);
            SampledSpectrum d = eval.L * throughput * weight * tr;
            if (hc.path_capture && hc.path_capture->record_radiance) {
                $if(hc.path_capture->active && bounces > 1u) {
                    hc.path_capture->radiance += spectrum()->linear_srgb(
                        eval.L * SampledSpectrum{hc.path_capture->beta} * weight * tr, swl);
                };
            }
            Float3 lin = spectrum()->linear_srgb(d, swl);
            if (hc.path_capture && hc.path_capture->on_emission) {
                hc.path_capture->on_emission(hit, rs.direction(), bounces, lin,
                    lin * select(bounces == 0u, make_float3(0.f), spec_route));
            }
            ret += lin;
            if (hc.restir_direct_split && hc.Ld) {
                if (reflection_only_direct) {
                    $if(gi_camera_emission_owned_by_di(hc.path_capture->first_scatter_transmission,
                        hc.path_capture->first_reflection_covered_by_di, bounces)) { *hc.Ld += make_RadType3(lin); };
                } else {
                    $if(bounces <= 1u) { *hc.Ld += make_RadType3(lin); };
                }
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
        optional<Bool> light_sample_infinite;
        if (hc.restir_shadow_connections) light_sample_infinite.emplace(false);
        Bool occluded = false;
        SampledSpectrum tr = spectrum()->one();
        if (!hc.rtxpt_sampling) {
            if (hc.work_counters) hc.work_counters->bsdf.w += 1u;
            if (hc.restir_shadow_connections) {
                light_sample = restir_shadow::sample_light(*light_sampler, it, sampler, swl, *light_sample_infinite);
            } else {
                light_sample = light_sampler->sample_wi(it, sampler, swl);
            }
            if (!cost_skip_nee) {
                RayState shadow_ray;
                if (hc.work_counters) { hc.work_counters->trace.y += 1u; hc.work_counters->path.x += 1u; }
                if (hc.restir_shadow_connections) {
                    shadow_ray = it.spawn_ray_state(normalize(light_sample.p_light - it.pos));
                    shadow_ray.ray = restir_shadow::visibility_ray(it, light_sample.p_light, *light_sample_infinite);
                    occluded = geometry.trace_occlusion(shadow_ray.ray);
                } else {
                    occluded = geometry.occluded(it, light_sample.p_light, &shadow_ray);
                }
                if (use_media) tr = geometry.Tr(scene(), renderer().sampler(), swl, shadow_ray);
            }
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
            if (!hc.rtxpt_sampling) {
                if (cost_skip_nee) {
                    if (hc.work_counters) hc.work_counters->bsdf.x += 1u;
                    bsdf_sample = evaluator.sample(it.wo, sampler);
                    if (hc.path_capture) hc.path_capture->on_sample(it, hit, evaluator, bsdf_sample, bounces, throughput);
                    return spectrum()->zero();
                }
                if (hc.work_counters) { hc.work_counters->bsdf.x += 1u; hc.work_counters->bsdf.y += 1u; hc.work_counters->bsdf.z += 1u; }
                if (hc.path_capture) {
                    auto direct = direct_light_mis(it, evaluator, light_sample, occluded, sampler, swl, bsdf_sample);
                    hc.path_capture->on_sample(it, hit, evaluator, bsdf_sample, bounces, throughput);
                    if (hc.path_capture->on_signal && hc.path_capture->evaluate_signal_direct_fraction) {
                        $if(bounces == hc.path_capture->signal_bounce) {
                            // Fill has one ordinary PT NEE sample. Split that
                            // exact evaluated direction only at a plane base.
                            signal_direct_fraction = evaluator.specular_fraction_rgb(it.wo,
                                normalize(light_sample.p_light - it.pos));
                        };
                    }
                    return direct;
                }
                return direct_light_mis(it, evaluator, light_sample, occluded, sampler, swl, bsdf_sample);
            }
            if (hc.work_counters) hc.work_counters->bsdf.x += 1u;
            bsdf_sample = evaluator.sample(it.wo, sampler);
            if (mis_mode_ == MISMode::EBSDF) return spectrum()->zero();
            SampledSpectrum lighting = spectrum()->zero();
            evaluator.with_prepared_evaluation(it.wo, [&](const PreparedMaterialEvaluator &prepared) {
                $for(shadow_sample, nee_full_samples) {
                    LightSample selected{swl.dimension()};
                    optional<Bool> selected_infinite;
                    if (hc.restir_shadow_connections) selected_infinite.emplace(false);
                    Float total_weight = 0.f, selected_weight = 0.f;
                    $for(candidate_index, nee_candidates) {
                        if (hc.work_counters) hc.work_counters->bsdf.w += 1u;
                        LightSample candidate{swl.dimension()};
                        optional<Bool> candidate_infinite;
                        if (hc.restir_shadow_connections) {
                            candidate_infinite.emplace(false);
                            candidate = restir_shadow::sample_light(*light_sampler, it, sampler, swl, *candidate_infinite);
                        } else {
                            candidate = light_sampler->sample_wi(it, sampler, swl);
                        }
                        Float proposal_pdf = abs(candidate.eval.pdf);
                        Float target = 0.f;
                        $if(candidate.valid() && proposal_pdf > 0.f) {
                            Float3 direction = normalize(candidate.p_light - it.pos);
                            if (hc.work_counters) hc.work_counters->bsdf.z += 1u;
                            auto eval = prepared.evaluate(direction, MaterialEvalMode::PDF);
                            Float3 incoming = spectrum()->linear_srgb(candidate.eval.L, swl) / proposal_pdf;
                            target = max(max(incoming.x, incoming.y), incoming.z) * eval.pdf();
                            target = max(ocarina::zero_if_nan_inf(target), 0.f);
                        };
                        total_weight += target;
                        $if(sampler->next_1d() * total_weight < target) {
                            selected = candidate;
                            selected_weight = target;
                            if (hc.restir_shadow_connections) *selected_infinite = *candidate_infinite;
                        };
                    };
                    $if(selected_weight > 0.f) {
                        Float3 direction = normalize(selected.p_light - it.pos);
                        if (hc.work_counters) { hc.work_counters->bsdf.y += 1u; hc.work_counters->bsdf.z += 1u; }
                        auto eval = prepared.evaluate(direction);
                        $if(eval.valid()) {
                            RayState selected_shadow;
                            if (hc.work_counters) { hc.work_counters->trace.y += 1u; hc.work_counters->path.x += 1u; }
                            Bool blocked = false;
                            if (hc.restir_shadow_connections) {
                                selected_shadow = it.spawn_ray_state(normalize(selected.p_light - it.pos));
                                selected_shadow.ray = restir_shadow::visibility_ray(it, selected.p_light, *selected_infinite);
                                blocked = geometry.trace_occlusion(selected_shadow.ray);
                            } else {
                                blocked = geometry.occluded(it, selected.p_light, &selected_shadow);
                            }
                            SampledSpectrum transmission = geometry.Tr(scene(), sampler, swl, selected_shadow);
                            Float pdf = abs(selected.eval.pdf);
                            Float mis = mis_mode_ == MISMode::ELight ? Float{1.f}
                                : select(selected.eval.pdf < 0.f, 1.f, MIS_weight_n<D>(int(nee_full_samples), pdf, 1, eval.pdf()));
                            Float ris_weight = total_weight / (float(nee_candidates) * selected_weight);
                            $if(!blocked && pdf > 0.f) {
                                lighting += selected.eval.L * eval.f * transmission * (ris_weight * mis / (float(nee_full_samples) * pdf));
                            };
                        };
                        light_sample = selected;
                        if (hc.restir_shadow_connections) *light_sample_infinite = *selected_infinite;
                    };
                };
            }, MaterialEvalMode::All);
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
                if (hc.work_counters) hc.work_counters->trace.w += 1u;
                MaterialEvaluator evaluator(lambert_it, swl);
                evaluator.update_frame(shading);
                evaluator.link(make_unique<DiffuseLobe>(SampledSpectrum{swl, .7f}, swl, shading));
                if (cost_skip_nee && !hc.rtxpt_sampling) {
                    if (hc.work_counters) hc.work_counters->bsdf.x += 1u;
                    bsdf_sample = evaluator.sample(it.wo, sampler);
                    Ld = spectrum()->zero();
                } else {
                    if (hc.work_counters) { hc.work_counters->bsdf.x += 1u; hc.work_counters->bsdf.y += 1u; hc.work_counters->bsdf.z += 1u; }
                    Ld = direct_light_mis(it, evaluator, light_sample, occluded,
                                          sampler, swl, bsdf_sample);
                }
                albedo = make_float3(.7f);
                delta_scatter = false;
                spec_frac_b0 = make_float3(0.f);
                spec_route = make_float3(0.f);
                return;
            }
            if (separate_) {
                MaterialEvaluator evaluator(it, swl);
                scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                    if (hc.work_counters) hc.work_counters->trace.w += 1u;
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
                if (hc.work_counters) hc.work_counters->trace.w += 1u;
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

        if (use_media) {
            $if(it.has_phase()) {
                PhaseSample ps{swl.dimension()};
                if (hc.work_counters) { hc.work_counters->bsdf.x += 1u; hc.work_counters->bsdf.y += 1u; hc.work_counters->bsdf.z += 1u; }
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
        if (hc.path_capture && hc.path_capture->on_signal) {
            $if(bounces == hc.path_capture->signal_bounce) {
                hc.path_capture->signal_base_diffuse = BxDFFlag::is_diffuse(bsdf_sample.eval.flags);
            };
        }
        SampledSpectrum d = throughput * Ld * tr;
        if (hc.path_capture && hc.path_capture->record_radiance) {
            $if(hc.path_capture->active && bounces > hc.path_capture->receiver_bounce) {
                hc.path_capture->radiance += spectrum()->linear_srgb(
                    SampledSpectrum{hc.path_capture->beta} * Ld * tr, swl);
            };
        }
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
        if (hc.work_counters) {
            $if(bsdf_sample.valid() && BxDFFlag::is_specular(bsdf_sample.eval.flags)) { hc.work_counters->path.w += 1u; };
        }
        eta_scale *= sqr(bsdf_sample.eta);
        if (hc.lobe_hitdist) delta_only &= delta_scatter;
        if (hc.path_capture && hc.path_capture->on_signal) {
            // RTXPT's delta-only flag is global; switching planes does not
            // restore it after a rough prefix or receiver scatter.
            hc.path_capture->signal_delta_only &= BxDFFlag::is_specular(bsdf_sample.eval.flags);
        }
        Float lum = throughput.max();
        if (hc.path_capture && hc.path_capture->record_radiance) {
            $if(hc.path_capture->active && lum == 0.f && any(hc.path_capture->beta != make_float3(0.f))) {
                // Camera absorption can stop channels still present in the
                // receiver-free suffix. Keep this path as raw PT, not a
                // partially traced radiance candidate for another receiver.
                hc.path_capture->active = false;
                hc.path_capture->receiver_bounce = InvalidUI32;
            };
        }
        $if(!bsdf_sample.valid() || lum == 0.f) {
            $break;
        };
        throughput *= bsdf_sample.eval.throughput();
        if (hc.path_capture && hc.path_capture->record_radiance) {
            $if(hc.path_capture->active && bounces > hc.path_capture->receiver_bounce) {
                hc.path_capture->beta *= bsdf_sample.eval.throughput().vec3();
            };
        }
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
                if (hc.work_counters) hc.work_counters->rr.x += 1u;
                Float terminate = rtxpt_rr_probability(camera_beta, surface_depth, total_depth);
                pending_rr_death = sampler->next_1d() < terminate;
                if (hc.work_counters) { $if(pending_rr_death) { hc.work_counters->rr.y += 1u; }; }
                $if(!pending_rr_death) { pending_rr_boost = 1.f / (1.f - terminate); };
            };
        } else {
            $if(eta_scale * rr_lum < *rr_threshold_ && rr_depth >= *min_depth_) {
                if (hc.work_counters) hc.work_counters->rr.x += 1u;
                Float q = min(0.95f, rr_lum);
                Float rr = sampler->next_1d();
                $if(q < rr) {
                    if (hc.work_counters) hc.work_counters->rr.y += 1u;
                    $break;
                };
                throughput /= q;
                if (hc.path_capture && hc.path_capture->record_radiance) {
                    $if(hc.path_capture->active) { hc.path_capture->beta /= q; };
                }
            };
        }
        scatter_pdf = bsdf_sample.eval.pdf();
        if (gi_previous_delta) *gi_previous_delta = BxDFFlag::is_specular(bsdf_sample.eval.flags);
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
    if (hc.path_capture) hc.path_capture->camera_specular_fraction = spec_route;
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
