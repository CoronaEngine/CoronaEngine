//
// Created by Zero on 2023/10/29.
//

#include "indirect.h"
#include "base/scattering/gi_ideal_glass.h"
#include "base/integral/path_work_capture.h"
#include "rtxpt_gi.h"
#include "rtxpt_gi_candidate.h"
#include "rtxpt_gi_material.h"
#include "rtxpt_gi_material_inputs.h"
#include "rtxpt_gi_lights.h"
#include "rtxpt_gi_rays.h"
#include "rtxpt_gi_prefix.h"
#include "base/integral/restir_shadow.h"
#include "rtxpt_gi_pt.h"
#include "denoiser_signals.h"
#include "base/integral/integrator.h"

namespace vision {
using namespace ocarina;
ReSTIRGI::ReSTIRGI(IntegratorPtr integrator,
                   const vision::ParameterSet &desc)
    : ReSTIR(integrator, desc),
    sample_num_(desc["sample_num"].as_uint(1u)),
    ratio_(desc["ratio"].as_uint(2u)),
    debias_(desc["debias"].as_bool(true)) {
    const char *override_backend = std::getenv("VISION_RESTIR_GI_BACKEND");
    const string backend = override_backend && *override_backend ? override_backend : desc["backend"].as_string("legacy");
    OC_ERROR_IF(backend != "legacy" && backend != "rtxpt", "GI backend must be legacy or rtxpt.");
    rtxpt_backend_ = backend == "rtxpt";
    // Two GI neighbors balance reuse quality and the cost of support queries.
    spatial_.sample_num = spatial_.open ? desc["spatial"]["sample_num"].as_uint(2u) : 1u;
    spatial_.sampling_radius = desc["spatial"]["radius"].as_float(12.f);
}

void ReSTIRGI::compile() noexcept {
    switch_profile::Scope profile{"ReSTIR.GI.compile", "compile"};
    if (rtxpt_backend_) {
        if (native_reblur_enabled_) prepare_native_reblur_buffers();
        if (!rtxpt_materials_) rtxpt_materials_ = make_shared<rtxpt_gi::StandardMaterialInputs>();
        rtxpt_materials_->prepare();
        // Preserve the existing binding layout used by the independent DI
        // shaders. Shared PT does not dispatch these private light producers.
        if (!rtxpt_lights_) {
            rtxpt_lights_ = make_shared<rtxpt_gi::NativeLightSampler>(*this);
            rtxpt_lights_->prepare();
        }
        if (!rtxpt_) {
            rtxpt_ = make_shared<RtxptGI>(*this);
            rtxpt_->prepare();
        }
        compile_initial_samples();
        rtxpt_->compile();
        OC_INFO("RTXPT ReSTIR GI active: shared ordinary PT, complete BSDF, private proposals, packed32 reservoirs, temporal/spatial reuse");
    } else {
        compile_initial_samples();
        compile_temporal_reuse();
        compile_spatial_shading();
    }
}

namespace {
void gi_signal_switch_plane(PTPathCapture &capture, Uint &current_plane,
                            const Uint &plane, const Uint &bounce) {
    current_plane = plane;
    capture.signal_bounce = bounce;
    capture.signal_hit_distance = 0.f;
    // The global delta-only flag survives plane switches, as in RTXPT.
}

void gi_signal_commit(Float4 &diffuse, Float4 &specular, const Float3 &secondary,
                       const Float &hit_distance, const Bool &base_diffuse) {
    // Native CommitDenoiserRadiance merges the whole secondaryL once, using
    // the final sampleHitT even when early delta suffix vertices emitted.
    $if(base_diffuse) {
        diffuse = rtxpt_gi::native_reblur_commit(diffuse, secondary, hit_distance);
    } $else {
        specular = rtxpt_gi::native_reblur_commit(specular, secondary, hit_distance);
    };
}

void gi_signal_commit_visit(Float4 &committed_diffuse, Float4 &committed_specular,
                             Float4 &base_diffuse, Float4 &base_specular, Float3 &secondary,
                             const Float &hit_distance, const Bool &scatter_diffuse) {
    gi_signal_commit(base_diffuse, base_specular, secondary, hit_distance, scatter_diffuse);
    committed_diffuse = rtxpt_gi::native_reblur_commit(committed_diffuse, Float3{base_diffuse.xyz()}, base_diffuse.w);
    committed_specular = rtxpt_gi::native_reblur_commit(committed_specular, Float3{base_specular.xyz()}, base_specular.w);
    base_diffuse = make_float4(0.f); base_specular = make_float4(0.f); secondary = make_float3(0.f);
}

void gi_signal_commit_buffer_visit(const rtxpt_gi::NativeReblurStorage &storage,
                                   const Uint &pixel, const Uint &plane,
                                   Float4 &base_diffuse, Float4 &base_specular, Float3 &secondary,
                                   const Float &hit_distance, const Bool &scatter_diffuse) {
    Float4 diffuse = storage.read_diffuse(pixel, plane);
    Float4 specular = storage.read_specular(pixel, plane);
    gi_signal_commit_visit(diffuse, specular, base_diffuse, base_specular, secondary, hit_distance, scatter_diffuse);
    // Keep the existing float32 accumulation. Native packs FP16 here, but that
    // would change the signal values and confound this retained-state test.
    storage.write(pixel, plane, diffuse, specular);
}

// Branches occupy two bits: transmission 01, reflection 10. The root
// sentinel is also 01, so inspect only the depth encoded branch pairs.
Bool gi_prefix_has_transmission(const Uint &sequence, const Uint &depth) {
    Uint prefix_mask = (Uint{1u} << (min(depth, 15u) * 2u)) - 1u;
    Uint transmission_bits = sequence & ~(sequence >> 1u) & 0x55555555u;
    return (transmission_bits & prefix_mask) != 0u;
}

// The cached secondary Lo is direction dependent through transmission. Keep
// its fresh BSDF estimate, but do not reconnect its receiver to other streams.
Bool gi_receiver_has_transmission(const MaterialEvaluator &bsdf, uint dimension) {
    Bool transmissive = false;
    auto visit = [&](const Lobe *lobe, const Float &sample_weight, const Float &weight) {
        if (dynamic_cast<const DielectricLobe *>(lobe)) {
            SampledSpectrum transmission{dimension}, reflection{dimension};
            Float2 roughness;
            lobe->reuse_material(transmission, reflection, roughness);
            transmissive |= sample_weight > 0.f && weight > 0.f && transmission.max() > 0.f;
        }
    };
    bsdf.dispatch([&](const Lobe *root) {
        if (root->is_multi()) {
            static_cast<const LobeSet *>(root)->for_each([&](const WeightedLobe &lobe) {
                visit(lobe.get(), lobe.sample_weight(), lobe.weight());
            });
        } else {
            visit(root, Float{1.f}, Float{1.f});
        }
    });
    return transmissive;
}
}// namespace

Bool ReSTIRGI::receiver_reuse_allowed(const SurfaceDataVar &surface) const noexcept {
    auto it = pipeline()->geometry().compute_surface_interaction(surface.hit, cur_view_pos(surface.is_replaced));
    Bool allowed = true;
    scene().materials().dispatch(it.material_id(), [&](const Material *material) {
        auto bsdf = material->create_evaluator(it, sampled_wavelengths());
        allowed = !gi_receiver_has_transmission(bsdf, spectrum()->dimension());
    });
    return allowed;
}

Float ReSTIRGI::Jacobian_det(Float3 cur_pos, Float3 neighbor_pos,
                             Var<SurfacePoint> sample_point) const noexcept {
    Float ret = 0.f;
    Float3 cur_vec = cur_pos - sample_point->position();
    Float3 neighbor_vec = neighbor_pos - sample_point->position();
    Float cur_dist2 = length_squared(cur_vec);
    Float neighbor_dist2 = length_squared(neighbor_vec);
    $if(sample_point->valid() && cur_dist2 > 0.f && neighbor_dist2 > 0.f) {
        Float cos_phi_c = dot(normalize(cur_vec), sample_point->normal());
        Float cos_phi_n = dot(normalize(neighbor_vec), sample_point->normal());
        Float denominator = abs(cos_phi_n) * cur_dist2;
        $if(gi_reconnection_supported(cos_phi_c, cos_phi_n) && denominator > 0.f) {
            ret = (abs(cos_phi_c) * neighbor_dist2) / denominator;
            ret = ocarina::max(ocarina::zero_if_nan_inf(ret), 0.f);
        };
    };
    return ret;
}

bool ReSTIRGI::render_UI(Widgets *widgets) noexcept {
    return widgets->use_tree("ReSTIR GI", [&] {
        changed_ |= widgets->check_box("switch", &open_);
        if (open_) {
            render_sub_UI(widgets);
        }
    });
}

void ReSTIRGI::render_sub_UI(Widgets *widgets) noexcept {
    changed_ |= widgets->check_box("temporal", &temporal_.open);
    changed_ |= widgets->drag_uint("max age", &max_age_, 1, 0, 100);
    changed_ |= widgets->drag_float("diffuse factor threshold", &diff_factor_, 0, 1);
    widgets->slider_uint("resolution ratio", &ratio_, 1, 4);
    widgets->button_click("resize", [&] {

    });
    if (temporal_.open) {
        changed_ |= widgets->input_uint_limit("history", &temporal_.limit, 0, 50, 1, 3);
        changed_ |= widgets->input_float_limit("temporal theta",
                                               &temporal_.theta, 0, 90, 1, 1);
        changed_ |= widgets->input_float_limit("temporal depth", &temporal_.depth_threshold,
                                               0, 1, 0.02, 0.1);
        changed_ |= widgets->input_float_limit("temporal radius", &temporal_.sampling_radius,
                                               0, 50, 1, 5);
    }
    changed_ |= widgets->check_box("spatial", &spatial_.open);
    if (spatial_.open) {
        changed_ |= widgets->input_float_limit("spatial theta",
                                               &spatial_.theta, 0, 90, 1, 1);
        changed_ |= widgets->input_float_limit("spatial depth", &spatial_.depth_threshold,
                                               0, 1, 0.02, 0.1);
        changed_ |= widgets->input_float_limit("spatial radius", &spatial_.sampling_radius,
                                               0, 50, 1, 5);
    }
}

GISampleVar ReSTIRGI::init_sample(const Interaction &it, const SensorSample &ss,
                                  HitBSDFVar &hit_bsdf) noexcept {
    const char *transport_override = std::getenv("VISION_RESTIR_GI_NATIVE_TRANSPORT");
    const bool native_transport = rtxpt_backend_ && transport_override && string_view(transport_override) == "1";
    Uint2 pixel = dispatch_idx().xy();
    sampler()->set_seed(pixel, frame_index(), 3);
    GISampleVar sample;
    $if(hit_bsdf->valid() && !ocarina::isinf(hit_bsdf.pdf)) {
        Interaction sp_it{false};
        sp_it.pos = make_float3(0.f);
        sp_it.ng = make_float3(0.f);
        HitContext hit_context{sp_it};
        hit_context.restir_shadow_connections = true;
        const char *native = std::getenv("VISION_RESTIR_GI_NATIVE_ALIGNED_LAMBERT");
        const char *multibounce = std::getenv("VISION_RESTIR_GI_MULTIBOUNCE_BENCH");
        hit_context.aligned_lambert = native && native[0] == '1' && multibounce && multibounce[0] == '1';
        Float3 captured_secondary_normal = make_float3(0.f);
        if (rtxpt_backend_) hit_context.captured_secondary_normal = addressof(captured_secondary_normal);
        hit_context.suppress_initial_emission = true;
        // Native transport hard-stops after terminal-surface NEE. Ordinary
        // Corona transport retains its final BSDF emission/miss supplement.
        hit_context.complete_terminal_direct = !native_transport;
        RayVar ray = it.spawn_ray(hit_bsdf.wi.as_vec3());
        RayState ray_state = RayState::create(ray);
        // Store radiance independent of the source receiver's BSDF/prefix.
        // Dividing a weighted result cannot recover zero throughput channels.
        auto surface = cur_surfaces().read(dispatch_id());
        Uint prefix_depth = 0u;
        $if(surface.is_replaced) { prefix_depth = cur_surface_extends().read(dispatch_id()).prefix_depth; };
        Uint rr_depth_offset = prefix_depth + 1u;
        hit_context.rr_depth_offset = addressof(rr_depth_offset);
        // Keep reusable Lo independent of the receiver, including zero channels.
        Float3 rr_throughput = hit_bsdf->safe_throughput();
        if (native_transport) {
            hit_context.rtxpt_sampling = true;
            // Native GI defers the receiver F, including in roulette throughput.
            rr_throughput = make_float3(1.f) / hit_bsdf.pdf;
            $if(surface.is_replaced) {
                rr_throughput *= cur_surface_extends().read(dispatch_id()).throughput.as_vec3();
            };
            hit_context.rr_camera_throughput = addressof(rr_throughput);
        }
        Float rr_survival_scale = clamp(max(max(rr_throughput.x, rr_throughput.y), rr_throughput.z), 0.25f, 1.f);
        hit_context.rr_survival_scale = addressof(rr_survival_scale);
        Uint suffix_depth = integrator()->runtime_suffix_depth();
        Uint remaining = suffix_depth - min(prefix_depth, suffix_depth);
        // Runtime suffix depth already excludes the primary receiver.
        Float3 L = integrator()->Li(ray_state, hit_bsdf.pdf, remaining, spectrum()->one(),
            false, hit_context, *this);
        sample.sp->set(sp_it);
        if (rtxpt_backend_) sample.sp->set_normal(captured_secondary_normal);
        sample.Lo.set(ocarina::zero_if_nan_inf(L));
    };
    return sample;
}

void ReSTIRGI::compile_initial_samples() noexcept {
    switch_profile::Scope profile{"GI.initial.compile", "compile"};
    if (rtxpt_backend_) {
        const char *nee_diagnostic = std::getenv("VISION_GI_NEE_DIAGNOSTIC");
        OC_ERROR_IF(nee_diagnostic && string_view(nee_diagnostic) != "1x1",
                    "Standard RTXPT GI currently supports NEE 1x1 only.");
        OC_ERROR_IF(scene().process_mediums(), "Standard RTXPT GI does not support participating media.");
        OC_ERROR_IF(spectrum()->dimension() != 3u, "Standard RTXPT GI requires RGB rendering.");
        OC_INFO("GI NEE diagnostic: 1x1, candidates=1, full_samples=1, visibility=true");
        OC_INFO("GI candidate transport: shared ordinary PT, full MaterialEvaluator, camera_path=true, single_path=true, exports_only=true, NEE=1/1");
        OC_INFO_FORMAT("ReSTIR GI candidate transport: backend=rtxpt, native_transport=false, candidates_per_pixel=1, NEE_candidates_per_shadow=1, NEE_shadow_samples=1, RR=ordinary immediate, max_surface_depth={}, suffix_depth={}, terminal_endpoint=ordinary PT, BSDF=complete Corona",
                       integrator()->suffix_depth() + 1u, integrator()->suffix_depth());
        OC_INFO("RTXPT GI private stable planes: count=3, max_vertex_depth=5, primary_replacement=true, incident_Lo=unweighted, shared_Falcor_primary=true");
        OC_INFO("RTXPT GI glass policy: complete pure RGB dielectric, roughness^2<0.0064 ideal delta, exact Fresnel, thick sqrt tint per interface, occupied-slot transmission continuation; ordinary PT/Falcor DI unchanged");
        OC_INFO("RTXPT GI stable emission: continuous first reflection owned by DI; first transmission and uncovered delta reflection retained by PT/Build exactly once");
        // Both private passes consume the same primary flag from the active DI producer.
        auto read_primary_flag = [](const BufferVar<uint> &words, const Uint2 &layout) {
            return words.read(dispatch_id() * layout.x + layout.y);
        };
        Kernel build = [&](Uint frame_index, BufferVar<uint> flags, Uint2 flag_layout) {
            gi_principled::CompilationScope parameters{false};
            initial(sampler(), frame_index, spectrum());
            scene().sensor()->load_data();
            sampler()->load_data();
            integrator()->load_data();
            rtxpt_gi::build_pt_prefix(prefix_storage(), *this, read_primary_flag(flags, flag_layout));
        };
        build.function()->set_block_dim(8u, 8u, 1u);
        rtxpt_prefix_build_ = device().compile(build, "RTXPT GI stable plane build");
        const bool count_work = path_cost::enabled();
        const char *cost_override = std::getenv("VISION_GI_EXPORT_COST");
        const string_view cost_mode = cost_override && *cost_override ? string_view{cost_override} : string_view{"full"};
        const array<string_view, 9> cost_modes{"full", "no-distance", "no-aux", "no-guides", "no-surface",
                                             "no-endpoint", "no-radiance", "no-observer", "plain"};
        uint cost_rank = 0u;
        if (cost_mode == "zero-surface") cost_rank = 3u;
        else if (cost_mode == "zero-material") cost_rank = 0u;
        else if (cost_mode == "plain-output48" || cost_mode == "plain-no-bridge" ||
                 cost_mode == "plain-output48-no-bridge") cost_rank = 8u;
        else if (cost_mode != "no-distance-math") {
            while (cost_rank < cost_modes.size() && cost_modes[cost_rank] != cost_mode) ++cost_rank;
            OC_ERROR_IF(cost_rank == cost_modes.size(), "Unknown GI export cost diagnostic.");
        }
        const bool skip_distance = cost_rank >= 1u, skip_aux = cost_rank >= 2u, skip_guides = cost_rank >= 3u;
        const bool skip_surface = cost_rank >= 4u, skip_endpoint = cost_rank >= 5u;
        const bool skip_radiance = cost_rank >= 6u, skip_observer = cost_rank >= 7u;
        const bool no_parameter_bridge = cost_mode == "plain-no-bridge" || cost_mode == "plain-output48-no-bridge";
        OC_INFO_FORMAT("GI export cost mode={}, production={}", cost_mode, cost_mode == "full");
        const char *signal_cost_override = std::getenv("VISION_GI_SIGNAL_COST");
        const string_view signal_cost_mode = signal_cost_override && *signal_cost_override
            ? string_view{signal_cost_override} : string_view{"optimized-buffer"};
        const bool reuse_signal_distance = signal_cost_mode == "reuse-distance" || signal_cost_mode == "optimized" ||
                                           signal_cost_mode == "optimized-buffer" || signal_cost_mode == "optimized-native";
        const bool stop_leaf_match = signal_cost_mode == "stop-leaf-match" || signal_cost_mode == "optimized" ||
                                     signal_cost_mode == "optimized-buffer" || signal_cost_mode == "optimized-native";
        const bool skip_signal_direct_fraction = signal_cost_mode == "skip-direct-fraction";
        const bool skip_signal_commit = signal_cost_mode == "no-commit";
        const bool buffer_signal_commit = signal_cost_mode == "buffer-commit" || signal_cost_mode == "optimized-buffer" ||
                                          signal_cost_mode == "optimized-native";
        const bool skip_dominant_signal_suffix = signal_cost_mode == "skip-dominant-suffix" ||
                                                 signal_cost_mode == "optimized-native";
        const bool stop_zero_signal_branch = signal_cost_mode == "optimized-buffer" || signal_cost_mode == "optimized-native";
        OC_ERROR_IF(signal_cost_mode != "full" && !reuse_signal_distance && !stop_leaf_match &&
                    !skip_signal_direct_fraction && !skip_signal_commit && !buffer_signal_commit && !skip_dominant_signal_suffix,
                    "Unknown GI signal cost diagnostic.");
        const char *signal_override = std::getenv("VISION_GI_SIGNAL_DIAGNOSTIC");
        const string_view signal_mode = signal_override && *signal_override ? string_view{signal_override} : string_view{"full"};
        const bool native_signal_miss = signal_mode != "world-miss" && signal_mode != "half-mis-world-miss";
        OC_ERROR_IF(signal_mode != "full" && signal_mode != "half-mis" && signal_mode != "mis-distance-only" && native_signal_miss,
                    "Unknown GI signal diagnostic.");
        OC_INFO_FORMAT("GI signal cost mode={}, reuse_distance={}, stop_leaf_match={}, direct_fraction={}, visit_commit={}, buffer_commit={}, dominant_suffix={}, stop_zero_branch={}",
                       signal_cost_mode, reuse_signal_distance, stop_leaf_match,
                       !skip_signal_direct_fraction, !skip_signal_commit, buffer_signal_commit,
                       !skip_dominant_signal_suffix, stop_zero_signal_branch);
        OC_INFO_FORMAT("GI signal diagnostic mode={}, native_miss={}", signal_mode, native_signal_miss);
        const char *endpoint_normal_override = std::getenv("VISION_GI_ENDPOINT_NORMAL");
        const string_view endpoint_normal_mode = endpoint_normal_override && *endpoint_normal_override
            ? string_view{endpoint_normal_override} : string_view{"shading-facing"};
        OC_ERROR_IF(endpoint_normal_mode != "geometric" && endpoint_normal_mode != "geometric-facing" &&
                    endpoint_normal_mode != "shading-facing", "Unknown GI endpoint normal diagnostic.");
        OC_INFO_FORMAT("GI endpoint normal diagnostic: mode={}, material_normal={}, native_grazing_adjust={}",
                       endpoint_normal_mode, endpoint_normal_mode == "shading-facing", endpoint_normal_mode == "shading-facing");
        OC_INFO_FORMAT("GI Fill transport: ordinary camera PT with observers only, counters={}", count_work);
        Kernel kernel = [&](Uint frame_index, Var<RtxptGICameraParam> camera, BufferVar<uint> flags, Uint2 flag_layout) {
            gi_principled::CompilationScope parameters{false, !no_parameter_bridge};
            initial(sampler(), frame_index, spectrum());
            scene().sensor()->load_data(); sampler()->load_data(); integrator()->load_data();
            sampler()->set_seed(dispatch_idx().xy(), frame_index, PTParam{}.dimension_index);
            optional<path_cost::PathWorkCountersVar> work_counters;
            if (count_work) work_counters.emplace();
            if (cost_mode == "plain" || cost_mode == "plain-output48" ||
                cost_mode == "plain-no-bridge" || cost_mode == "plain-output48-no-bridge") {
                RadType3Var direct, specular;
                HitContext context{direct, specular};
                context.restir_shadow_connections = true;
                context.work_counters = work_counters ? &*work_counters : nullptr;
                RayState ray = frame_buffer().rays().read(dispatch_id())->to_ray_state();
                Float3 total = integrator()->Li(ray, Float{1e16f}, integrator()->runtime_suffix_depth() + 1u,
                    spectrum()->one(), false, context, *this);
                if (cost_mode == "plain-output48" || cost_mode == "plain-output48-no-bridge") {
                    rtxpt_bypass_.write(dispatch_id(), make_float4(total, 1.f));
                    auto denoiser = prefix_storage().denoiser;
                    OC_ASSERT(denoiser.enabled());
                    denoiser.write(dispatch_id(), Uint{0u},
                        make_float4(total - make_float3(specular), 1.f), make_float4(specular, 1.f));
                    if (work_counters) path_work_counters_.write(dispatch_id(), *work_counters);
                    return;
                }
                rtxpt_bypass_.write(dispatch_id(), make_float4(total, 1.f));
                rtxpt_gi::CandidateTraceResult empty;
                empty.sample.Lo.set(make_float3(specular));
                samples_.write(dispatch_id(), empty.sample);
                rtxpt_proposals_.write(dispatch_id(), empty.proposal);
                if (work_counters) path_work_counters_.write(dispatch_id(), *work_counters);
                return;
            }
            PTPathCapture capture;
            capture.reflection_only_direct = true;
            capture.first_reflection_covered_by_di = !BxDFFlag::is_specular(read_primary_flag(flags, flag_layout));
            capture.record_radiance = !skip_radiance;
            capture.record_hit_distance = cost_mode != "no-distance-math";
            capture.reuse_signal_distance = reuse_signal_distance;
            capture.evaluate_signal_direct_fraction = !skip_signal_direct_fraction;
            capture.native_signal_miss = native_signal_miss;
            Bool terminal_seen = false, receiver_written = false;
            Uint signal_plane = 0u;
            Bool sampled_stable_event = false;
            optional<array<Float4, 3>> signal_diffuse, signal_specular;
            if (!buffer_signal_commit) { signal_diffuse.emplace(); signal_specular.emplace(); }
            Float4 signal_base_diffuse = make_float4(0.f), signal_base_specular = make_float4(0.f);
            Float3 signal_secondary = make_float3(0.f);
            Bool signal_scatter_diffuse = false;
            if (!buffer_signal_commit) {
                for (uint p = 0u; p < 3u; ++p) {
                    (*signal_diffuse)[p] = make_float4(0.f); (*signal_specular)[p] = make_float4(0.f);
                }
            }
            Uint sampled_branch = 1u;
            Bool ideal_prefix = true;
            Float3 sampled_stable = make_float3(0.f);
            const auto storage = prefix_storage();
            if (buffer_signal_commit && storage.denoiser.enabled()) {
                // This invocation owns all three plane addresses for its
                // pixel. Initialize them before any visit reads them.
                for (uint p = 0u; p < 3u; ++p) {
                    storage.denoiser.write(dispatch_id(), Uint{p}, make_float4(0.f), make_float4(0.f));
                }
            }
            Uint4 header = storage.headers.read(dispatch_id());
            Uint dominant = header.w & 3u;
            capture.on_sample = [&](const Interaction &it, const TriangleHitVar &hit,
                                    const MaterialEvaluator &bsdf, const BSDFSample &sample,
                                    const Uint &bounce, const SampledSpectrum &throughput) {
                $if(bounce == 0u) {
                    const Float3 n = bsdf.shading_frame().normal();
                    capture.first_scatter_transmission = dot(n, it.wo) * dot(n, sample.wi) < 0.f;
                };
                auto match_planes = [&] {
                    $for(plane, 3u) {
                        $if(header[plane] == rtxpt_gi::PrefixInvalid || header[plane] == rtxpt_gi::PrefixQueued) { $continue; };
                        auto node = storage.read(plane);
                        $if(!gi_prefix_endpoint_matches(sampled_branch, bounce + 1u, -it.wo,
                            header[plane], rtxpt_gi::pt_prefix_depth(node), node.direction_vertex.xyz())) { $continue; };
                        Float2 bary = as<float2>(Uint2{node.hit_words.zw()});
                        $if(hit.inst_id != node.hit_words.x || hit.prim_id != node.hit_words.y ||
                            length_squared(Float2{hit.bary} - bary) > 1e-6f) { $continue; };
                        // Signals follow every visited plane, including a
                        // consumed primary and an ineligible GI receiver.
                        if (storage.denoiser.enabled() && !skip_signal_commit) {
                            for (uint p = 0u; p < 3u; ++p) {
                                $if(signal_plane == p) {
                                    if (buffer_signal_commit) {
                                        gi_signal_commit_buffer_visit(storage.denoiser, dispatch_id(), Uint{p},
                                            signal_base_diffuse, signal_base_specular, signal_secondary,
                                            skip_distance ? Float{0.f} : capture.signal_hit_distance, signal_scatter_diffuse);
                                    } else {
                                        gi_signal_commit_visit((*signal_diffuse)[p], (*signal_specular)[p], signal_base_diffuse,
                                            signal_base_specular, signal_secondary,
                                            skip_distance ? Float{0.f} : capture.signal_hit_distance, signal_scatter_diffuse);
                                    }
                                };
                            }
                        }
                        gi_signal_switch_plane(capture, signal_plane, plane, bounce);
                        $if(!terminal_seen && !rtxpt_gi::pt_prefix_consumed(node)) {
                            terminal_seen = true;
                            $if(plane == dominant) {
                                rtxpt_gi::PTSurfaceView receiver;
                                receiver.it = it; receiver.hit = hit;
                                receiver.depth = node.virtual_position.w;
                                receiver.plane_hash = (plane << 24u) | header[plane];
                                receiver.valid = gi_principled::captured(it.material_id());
                                for (uint i = 0u; i < 6u; ++i) receiver.parameters[i] = gi_principled::row(i);
                                if (cost_mode == "zero-material") {
                                    // Cost diagnostic only: preserve receiver eligibility,
                                    // configuration, geometry, guides and all96B stores.
                                    for (uint i = 0u; i < 5u; ++i) receiver.parameters[i] = make_float4(0.f);
                                    receiver.parameters[5].xyz() = make_float3(0.f);
                                }
                                if (!skip_guides) {
                                    receiver.normal = bsdf.shading_frame().normal();
                                    SampledSpectrum d{spectrum()->dimension()}, sp{spectrum()->dimension()}; Float2 roughness;
                                    bsdf.reuse_material(d, sp, roughness);
                                    receiver.diffuse_guide = spectrum()->linear_srgb(d, sampled_wavelengths());
                                    receiver.specular_guide = spectrum()->linear_srgb(sp, sampled_wavelengths());
                                    receiver.roughness = roughness;
                                }
                                if (cost_mode == "zero-surface") {
                                    // Keep the same receiver stores; let unused
                                    // interaction payload reconstruction disappear.
                                    rtxpt_gi::PTSurfaceView empty;
                                    rtxpt_->write_pt_surface(dispatch_id(), empty);
                                } else if (!skip_surface) rtxpt_->write_pt_surface(dispatch_id(), receiver);
                                receiver_written = receiver.valid;
                                RtxptGIPrefixAuxVar aux;
                                aux.prefix_throughput_depth = make_float4(spectrum()->linear_srgb(throughput, sampled_wavelengths()), receiver.depth);
                                if (!skip_aux) {
                                aux.current_virtual_position = make_float4(node.virtual_position.xyz(), node.origin_distance.w);
                                aux.previous_virtual_position = make_float4(node.virtual_position.xyz(), 0.f);
                                aux.metadata = make_uint4(receiver.plane_hash, plane, bounce + 1u, header[plane]);
                                Float4x4 world_to_camera = make_float4x4(camera.columns[0u], camera.columns[1u], camera.columns[2u], camera.columns[3u]);
                                Float4x4 camera_to_raster = make_float4x4(camera.columns[4u], camera.columns[5u], camera.columns[6u], camera.columns[7u]);
                                Float3 current_view = transform_point(world_to_camera, aux.current_virtual_position.xyz());
                                Float3 current_raster = transform_point(camera_to_raster, current_view / current_view.z);
                                Float2 motion = current_raster.xy() - scene().sensor()->prev_raster_coord(aux.previous_virtual_position.xyz()).xy();
                                Float previous_depth = scene().sensor()->prev_linear_depth(aux.previous_virtual_position.xyz());
                                aux.packed_motion = make_uint2(f32tof16(motion.x) | (f32tof16(motion.y) << 16u), f32tof16(previous_depth - current_view.z));
                                }
                                rtxpt_prefix_aux_.write(dispatch_id(), aux);
                                Bool delta = BxDFFlag::is_specular(sample.eval.flags) || ocarina::isinf(sample.eval.pdf());
                                Float3 physical = spectrum()->linear_srgb(throughput * sample.eval.throughput(), sampled_wavelengths());
                                capture.active = receiver.valid && sample.valid() && !delta && any(physical != make_float3(0.f));
                                capture.receiver_bounce = bounce;
                                $if(capture.active) {
                                    HitBSDFVar proposal;
                                    proposal.wi.set(sample.wi); proposal.bsdf.set(sample.eval.f.vec3()); proposal.pdf = sample.eval.pdf();
                                    rtxpt_proposals_.write(dispatch_id(), proposal);
                                    GISampleVar empty; empty.sp->set_position(make_float3(0.f)); empty.sp->set_normal(make_float3(0.f)); empty.Lo.set(Float3{make_float3(0.f)});
                                    samples_.write(dispatch_id(), empty);
                                };
                            };
                        };
                        $break;
                    };
                };
                if (stop_leaf_match) {
                    // The prefix producer stores no descendant of an unconsumed
                    // endpoint. Consumed-parent visits still run before the leaf.
                    if (stop_zero_signal_branch) {
                        // A zero sampled branch is absorbing and cannot match
                        // any stored endpoint at this or a later bounce.
                        $if(!terminal_seen && sampled_branch != 0u) { match_planes(); };
                    } else {
                        $if(!terminal_seen) { match_planes(); };
                    }
                } else {
                    match_planes();
                }
                sampled_branch = gi_prefix_next_branch(sampled_branch, sample.stable_lobe_index, sample.valid());
                ideal_prefix = ideal_prefix && BxDFFlag::is_specular(sample.eval.flags) && sample.valid();
            };
            capture.on_emission = [&](const TriangleHitVar &hit, const Float3 &direction, const Uint &bounce,
                                      const Float3 &radiance, const Float3 &) {
                Bool represented = false;
                $if(gi_prefix_stable_emission_allowed(ideal_prefix, bounce + 1u, sampled_branch,
                    capture.first_reflection_covered_by_di)) {
                    $for(plane, 3u) {
                        $if(header[plane] == rtxpt_gi::PrefixInvalid || header[plane] == rtxpt_gi::PrefixQueued) { $continue; };
                        auto node = storage.read(plane);
                        $if(!rtxpt_gi::pt_prefix_ideal_path(node)) { $continue; };
                        Bool identity = hit->is_miss() && node.hit_words.x == InvalidUI32 && node.hit_words.y == InvalidUI32;
                        $if(hit->is_hit()) {
                            Float2 bary = as<float2>(Uint2{node.hit_words.zw()});
                            identity = hit.inst_id == node.hit_words.x && hit.prim_id == node.hit_words.y &&
                                length_squared(Float2{hit.bary} - bary) <= 1e-6f;
                        };
                        represented |= gi_prefix_emission_represented(ideal_prefix, sampled_branch, bounce + 1u,
                            direction, header[plane], rtxpt_gi::pt_prefix_depth(node), node.direction_vertex.xyz(), identity,
                            capture.first_reflection_covered_by_di);
                        $if(represented) { $break; };
                    };
                };
                $if(represented) {
                    sampled_stable += radiance;
                };
                sampled_stable_event = represented;
            };
            if (storage.denoiser.enabled()) {
                capture.on_signal = [&](const Float3 &radiance, const Float3 &specular,
                                         const Float &hit_distance, const Float &,
                                         const Uint &bounce, bool emission) {
                    signal_scatter_diffuse = capture.signal_base_diffuse;
                    // First retained light stays in the camera-plane residual,
                    // not receiver Lo. Build replaces represented ideal events.
                    const Bool direct_owned = gi_camera_emission_owned_by_di(capture.first_scatter_transmission,
                        capture.first_reflection_covered_by_di, bounce);
                    $if(bounce > 0u && !(Bool{emission} && (direct_owned || sampled_stable_event))) {
                        $if(Bool{!emission} && bounce == capture.signal_bounce) {
                            Float distance = skip_distance ? Float{0.f} : hit_distance;
                            signal_base_diffuse = make_float4(radiance - specular, distance);
                            signal_base_specular = make_float4(specular, distance);
                        } $else {
                            if (skip_dominant_signal_suffix) {
                                // Native dominant GI writes this suffix to the
                                // candidate, rather than adding then cancelling it.
                                // Diagnostic: an invalidated candidate cannot
                                // recover suffix events already omitted here.
                                $if(!capture.active) { signal_secondary += radiance; };
                            } else {
                                signal_secondary += radiance;
                            }
                        };
                    };
                };
            }
            capture.on_endpoint = [&](const Interaction &it) {
                GISampleVar sample;
                sample.sp->set(it); sample.Lo.set(Float3{make_float3(0.f)}); sample.age = 0.f;
                if (endpoint_normal_mode != "geometric") {
                    Float side = select(dot(it.ng, it.wo) >= 0.f, 1.f, -1.f);
                    Float3 normal = it.ng * side;
                    if (endpoint_normal_mode == "shading-facing") {
                        // The endpoint callback precedes ordinary material
                        // evaluation. Read the deterministic unoriented normal;
                        // no BSDF sample, throughput, PDF or RNG is changed.
                        scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                            normal = material->gi_unoriented_shading_normal(it, sampled_wavelengths()) * side;
                        });
                        // Native Bridge::loadSurface first orients Ns by the
                        // geometric front-face test, then adjustShadingNormal
                        // blends it towards the oriented Ng below NdotV=.1.
                        Float3 geometric = it.ng * side;
                        Float sign_n = select(dot(normal, geometric) >= 0.f, 1.f, -1.f);
                        Float3 oriented = normal * sign_n;
                        Float cosine = dot(it.wo, oriented);
                        $if(cosine <= .1f) {
                            normal = sign_n * normalize(lerp(geometric, oriented, saturate(cosine * 10.f)));
                        };
                    }
                    sample.sp->set_normal(normal);
                }
                samples_.write(dispatch_id(), sample);
            };
            if (skip_endpoint) capture.on_endpoint = {};
            RadType3Var direct = make_RadType3(make_float3(0.f));
            HitContext context{direct};
            context.restir_shadow_connections = true;
            if (!skip_observer) context.path_capture = &capture;
            context.work_counters = work_counters ? &*work_counters : nullptr;
            context.restir_direct_split = true;
            // This is the ordinary camera PT: one Li call, original camera ray,
            // complete depth, original NEE, BSDF samples and immediate roulette.
            RayState ray = frame_buffer().rays().read(dispatch_id())->to_ray_state();
            Float3 total = integrator()->Li(ray, Float{1e16f}, integrator()->runtime_suffix_depth() + 1u,
                spectrum()->one(), false, context, *this);
            Float3 indirect = total - make_float3(direct);
            Float3 stable = make_float3(0.f);
            if (!skip_observer) {
                stable = storage.stable_radiance.read(dispatch_id()).xyz();
                indirect -= sampled_stable;
            }
            $if(capture.active) {
                auto sample = samples_.read(dispatch_id());
                sample.Lo.set(ocarina::zero_if_nan_inf(capture.radiance));
                samples_.write(dispatch_id(), sample);
                auto proposal = rtxpt_proposals_.read(dispatch_id());
                Float3 prefix = rtxpt_prefix_aux_.read(dispatch_id()).prefix_throughput_depth.xyz();
                Float3 fresh = prefix * proposal->safe_throughput() * sample.Lo.as_vec3();
                indirect -= fresh;
                if (storage.denoiser.enabled() && !skip_dominant_signal_suffix) {
                    // An eligible terminal receiver's candidate is the final
                    // visit; earlier visits have already been committed.
                    signal_secondary -= fresh;
                }
            } $else {
                rtxpt_gi::CandidateTraceResult empty;
                rtxpt_proposals_.write(dispatch_id(), empty.proposal);
                samples_.write(dispatch_id(), empty.sample);
            };
            $if(!receiver_written || capture.receiver_bounce == InvalidUI32) {
                rtxpt_gi::PTSurfaceView empty;
                if (!skip_surface) rtxpt_->write_pt_surface(dispatch_id(), empty);
                RtxptGIPrefixAuxVar aux; rtxpt_prefix_aux_.write(dispatch_id(), aux);
            };
            indirect = ocarina::zero_if_nan_inf(indirect);
            rtxpt_bypass_.write(dispatch_id(), make_float4(indirect + stable, 0.f));
            if (work_counters) path_work_counters_.write(dispatch_id(), *work_counters);
            if (storage.denoiser.enabled()) {
                if (buffer_signal_commit) {
                    for (uint p = 0u; p < 3u; ++p) {
                        $if(signal_plane == p) {
                            gi_signal_commit_buffer_visit(storage.denoiser, dispatch_id(), Uint{p},
                                signal_base_diffuse, signal_base_specular, signal_secondary,
                                skip_distance ? Float{0.f} : capture.signal_hit_distance, signal_scatter_diffuse);
                        };
                        Float4 diffuse = storage.denoiser.read_diffuse(dispatch_id(), Uint{p});
                        Float4 specular = storage.denoiser.read_specular(dispatch_id(), Uint{p});
                        storage.denoiser.write(dispatch_id(), Uint{p},
                            make_float4(ocarina::zero_if_nan_inf(Float3{diffuse.xyz()}), diffuse.w),
                            make_float4(ocarina::zero_if_nan_inf(Float3{specular.xyz()}), specular.w));
                    }
                } else {
                    for (uint p = 0u; p < 3u; ++p) {
                        if (!skip_signal_commit) {
                            $if(signal_plane == p) {
                                gi_signal_commit_visit((*signal_diffuse)[p], (*signal_specular)[p], signal_base_diffuse,
                                    signal_base_specular, signal_secondary,
                                    skip_distance ? Float{0.f} : capture.signal_hit_distance, signal_scatter_diffuse);
                            };
                        }
                        Float4 diffuse = (*signal_diffuse)[p], specular = (*signal_specular)[p];
                        storage.denoiser.write(dispatch_id(), Uint{p},
                            make_float4(ocarina::zero_if_nan_inf(Float3{diffuse.xyz()}), diffuse.w),
                            make_float4(ocarina::zero_if_nan_inf(Float3{specular.xyz()}), specular.w));
                    }
                }
                // Physical stable-prefix radiance is outside every noisy plane
                // and receives no second multiplication by prefix throughput.
                storage.denoiser.bypass->write(dispatch_id(), make_float4(stable, 0.f));
            }
        };
        kernel.function()->set_block_dim(64u, 1u, 1u);
        rtxpt_initial_samples_ = device().compile(kernel, "RTXPT GI shared PT initial candidates");
        return;
    }
    const char *transport_override = std::getenv("VISION_RESTIR_GI_NATIVE_TRANSPORT");
    const bool native_transport = rtxpt_backend_ && transport_override && string_view(transport_override) == "1";
    const char *nee_diagnostic = std::getenv("VISION_GI_NEE_DIAGNOSTIC");
    const bool nee_one_by_one = nee_diagnostic && string_view(nee_diagnostic) == "1x1";
    OC_INFO_FORMAT("ReSTIR GI candidate transport: backend={}, native_transport={}, candidates_per_pixel=1, NEE_candidates_per_shadow={}, NEE_shadow_samples={}, RR={}, max_surface_depth={}, suffix_depth={}, terminal_endpoint={}",
        rtxpt_backend_ ? "rtxpt" : "legacy", native_transport,
        native_transport && !nee_one_by_one ? 6u : 1u, native_transport && !nee_one_by_one ? 2u : 1u,
        native_transport ? "native deferred" : "Corona immediate",
        integrator()->suffix_depth() + 1u, integrator()->suffix_depth(),
        native_transport ? "hard stop" : "Corona BSDF supplement");
    const char *native = std::getenv("VISION_RESTIR_GI_NATIVE_ALIGNED_LAMBERT");
    const char *multibounce = std::getenv("VISION_RESTIR_GI_MULTIBOUNCE_BENCH");
    if (native && native[0] == '1' && multibounce && multibounce[0] == '1')
        OC_INFO("Native ReSTIR GI multibounce benchmark: geometric Lambert0.7; public5/16/RR1; native prefix-aware roulette; joint IndependentSampler offset; one candidate per pixel; original reuse/visibility/normalization retained");
    TSpectrum &spectrum = pipeline()->spectrum();
    TSensor &camera = scene().sensor();
    Kernel kernel = [&](Uint frame_index) {
        initial(sampler(), frame_index, spectrum);
        SurfaceDataVar surf = cur_surfaces().read(dispatch_id());
        $if(surf.hit->is_miss()) {
            write_radiance(dispatch_id(), make_float4(0.f));
            $return();
        };
        camera->load_data();
        sampler()->load_data();
        integrator()->load_data();
        Uint2 pixel = dispatch_idx().xy();
        sampler()->set_seed(pixel, frame_index, 0);
        SensorSample ss = sampler()->sensor_sample(pixel, camera->filter());
        Float3 view_pos = cur_view_pos(surf.is_replaced);
        Interaction it = pipeline()->geometry().compute_surface_interaction(surf.hit, view_pos);
        HitBSDFVar hit_bsdf = frame_buffer().hit_bsdfs().read(dispatch_id());
        GISampleVar sample = init_sample(it, ss, hit_bsdf);
        samples_.write(dispatch_id(), sample);
    };
    kernel.function()->set_block_dim(32u, 1u, 1u);
    initial_samples_ = device().compile(kernel, "ReSTIR indirect initial samples");
}

ScatterEval ReSTIRGI::eval_bsdf(const Interaction &it, const GISampleVar &sample,
                                MaterialEvalMode mode, MaterialEvaluator *bsdf) const noexcept {
    if (bsdf) {
        Float3 wi = normalize(sample.sp->position() - it.pos);
        return bsdf->evaluate(it.wo, wi, mode);
    }
    return outline("ReSTIRGI::eval_bsdf", [&] {
        ScatterEval ret{spectrum()->dimension(), 1};
        scene().materials().dispatch(it.material_id(), [&](const Material *material) {
            MaterialEvaluator bsdf = material->create_evaluator(it, sampled_wavelengths());
            Float3 wi = normalize(sample.sp->position() - it.pos);
            ret = bsdf.evaluate(it.wo, wi, mode);
        });
        return ret;
    });
}

Float ReSTIRGI::compute_p_hat(const vision::Interaction &it,
                              const vision::GISampleVar &sample, MaterialEvaluator *bsdf) const noexcept {
    Float ret = 0.f;
    $if(sample.sp->valid() && luminance(sample.Lo.as_vec3()) > 0.f &&
        length_squared(sample.sp->position() - it.pos) > 0.f) {
        // The target is a PDF proxy; final shading still evaluates exact F.
        // Retain the receiver proposal's geometric/material support.
        ScatterEval eval = eval_bsdf(it, sample, MaterialEvalMode::PDF, bsdf);
        $if(eval.valid() && !ocarina::isinf(eval.pdf())) {
            ret = ocarina::max(ocarina::zero_if_nan_inf(luminance(sample.Lo.as_vec3()) * eval.pdf()), 0.f);
        };
    };
    return ret;
}

Float ReSTIRGI::selected_sample_support(const Interaction &target_it,
                                         const Interaction &source_it,
                                         const GISampleVar &sample) const noexcept {
    // Receiver support under the cached-Lo approximation; this does not
    // re-evaluate the secondary vertex's view-dependent outgoing radiance.
    Float support = 0.f;
    Float jacobian = Jacobian_det(target_it.pos, source_it.pos, sample.sp);
    $if(jacobian > 0.f) {
        ScatterEval eval = eval_bsdf(source_it, sample, MaterialEvalMode::PDF);
        Float pdf = eval.pdf();
        // A positive proposal can support a proxy winner even when F*Lo is zero.
        Float p_hat = ocarina::zero_if_nan_inf(luminance(sample.Lo.as_vec3()) * pdf);
        $if(p_hat > 0.f && eval.valid() && !ocarina::isinf(pdf)) {
            support = cast<float>(!pipeline()->geometry().trace_occlusion(
                restir_shadow::visibility_ray(source_it, sample.sp->position(), Bool{false})));
        };
    };
    return support;
}

GIReservoirVar ReSTIRGI::combine_temporal(const GIReservoirVar &cur_rsv, SurfaceDataVar cur_surf,
                                          GIReservoirVar &other_rsv, SurfaceDataVar *neighbor_surf,
                                          Float3 view_pos, Float3 prev_view_pos) const noexcept {
    other_rsv.sample.age += 1;
    TSensor &camera = scene().sensor();
    Interaction it = pipeline()->geometry().compute_surface_interaction(cur_surf.hit, view_pos);
    GIReservoirVar ret;
    auto combine = [&](MaterialEvaluator *bsdf) {
        Float cur_p_hat = compute_p_hat(it, cur_rsv.sample, bsdf);
        Bool selected_cur = ret->update(sampler()->next_1d(), cur_rsv.sample,
                    GIReservoir::safe_weight(cur_rsv.C, cur_p_hat, cur_rsv.W), cur_rsv.C);
        Float winner_p_hat = ocarina::select(selected_cur, cur_p_hat, 0.f);
        Float other_p_hat = 0.f;
        Float other_weight = 0.f;
        Interaction neighbor_it{false};
        if (neighbor_surf) {
            neighbor_it = pipeline()->geometry().compute_surface_interaction(neighbor_surf->hit, prev_view_pos);
            $if(other_rsv.W > 0.f && other_rsv.sample.sp->valid()) {
                other_p_hat = compute_p_hat(it, other_rsv.sample, bsdf);
                Float jacobian = Jacobian_det(it.pos, neighbor_it.pos, other_rsv.sample.sp);
                $if(other_p_hat > 0.f && jacobian > 0.f) {
                    Float visibility = cast<float>(!pipeline()->geometry().trace_occlusion(
                        restir_shadow::visibility_ray(it, other_rsv.sample.sp->position(), Bool{false})));
                    other_weight = GIReservoir::safe_weight(other_rsv.C, other_p_hat * jacobian, other_rsv.W) * visibility;
                };
            };
        }
        // A null or occluded realization still represents its original stream.
        Bool selected_other = ret->update(sampler()->next_1d(), other_rsv.sample, other_weight, other_rsv.C);
        Float p_hat = ocarina::select(selected_other, other_p_hat, winner_p_hat);
        if (debias_) {
            Float normalization = 0.f;
            $if(ret.weight_sum > 0.f && p_hat > 0.f && ret.sample.sp->valid()) {
                // Every positive-weight winner is supported at the current receiver.
                normalization = ocarina::max(ocarina::zero_if_nan_inf(cur_rsv.C), 0.f);
                if (neighbor_surf) {
                    Float count = ocarina::max(ocarina::zero_if_nan_inf(other_rsv.C), 0.f);
                    $if(count > 0.f) {
                        // Test the final winner, even if the history's own sample is null.
                        normalization += count * selected_sample_support(it, neighbor_it, ret.sample);
                    };
                }
            };
            ret->update_W_with_support(p_hat, normalization);
        } else {
            ret->update_W(p_hat);
        }
    };
    $if(it.has_material()) {
        scene().materials().dispatch(it.material_id(), [&](const Material *material) {
            auto bsdf = material->create_evaluator(it, sampled_wavelengths());
            combine(addressof(bsdf));
        });
    }
    $else {
        // Null receivers still carry the original streams' confidence counts.
        combine(nullptr);
    };
    return ret;
}

GIReservoirVar ReSTIRGI::temporal_reuse(GIReservoirVar rsv, const SurfaceDataVar &cur_surf,
                                        const Float2 &motion_vec, const SensorSample &ss,
                                        const Var<GIParam> &param) const noexcept {
    Float2 prev_p_film = previous_reservoir_coord(ss.p_film, motion_vec);
    Int2 prev_p = reservoir_pixel(prev_p_film);
    Float limit = rsv.C * param.history_limit;
    Int2 res = make_int2(dispatch_dim().xy());
    TSensor &camera = scene().sensor();

    Float3 view_pos = cur_view_pos(cur_surf.is_replaced);
    Float3 prev_view_pos = camera->prev_device_position();

    auto get_prev_data = [this, &limit](const Uint2 &pos, Float3 &view_pos) {
        Uint index = dispatch_id(make_uint2(pos));
        GIReservoirVar prev_rsv = prev_reservoirs().read(index);
        prev_rsv->truncation(limit);
        SurfaceDataVar surf = prev_surfaces().read(index);
        view_pos = scene().sensor()->prev_device_position();
        $if(surf.is_replaced) {
            view_pos = prev_surface_extends().read(index).view_pos.as_vec3();
        };
        return make_pair(surf, prev_rsv);
    };

    $if(in_screen(prev_p, res) && param.temporal && receiver_reuse_allowed(cur_surf)) {
        auto data = get_prev_data(make_uint2(prev_p), prev_view_pos);
        auto prev_surf = data.first;
        auto prev_rsv = data.second;

        $if(is_temporal_valid(cur_surf, prev_surf,
                              param, addressof(prev_rsv.sample))) {
            rsv = combine_temporal(rsv, cur_surf, prev_rsv, addressof(prev_surf),
                                   view_pos, prev_view_pos);
        }
        $else {
            $if(param.t_radius == 0.f) {
                // The rejected pixel is unchanged; preserve all fallback RNG.
                $for(i, temporal_.N) { (void)sampler()->next_2d(); };
            }
            $else {
                $for(i, temporal_.N) {
                    Int2 p = reservoir_pixel(square_to_disk(sampler()->next_2d()) * param.t_radius + prev_p_film);
                    $if(!in_screen(p, res)) { $continue; };
                    auto data = get_prev_data(make_uint2(p), prev_view_pos);
                    auto another_surf = data.first;
                    auto another_rsv = data.second;
                    $if(is_temporal_valid(cur_surf, another_surf,
                                          param, addressof(another_rsv.sample))) {
                        rsv = combine_temporal(rsv, cur_surf, another_rsv, addressof(another_surf),
                                               view_pos, prev_view_pos);
                        $break;
                    };
                };
            };
        };
    };
    return rsv;
}

void ReSTIRGI::compile_temporal_reuse() noexcept {
    switch_profile::Scope profile{"GI.temporal.compile", "compile"};
    TSpectrum &spectrum = pipeline()->spectrum();
    TSensor &camera = scene().sensor();
    //todo remedy init samples and reservoir
    Kernel kernel = [&](Var<GIParam> param, Uint frame_index) {
        initial(sampler(), frame_index, spectrum);
        SurfaceDataVar surf = cur_surfaces().read(dispatch_id());
        $if(surf.hit->is_miss()) {
            $return();
        };
        sampler()->load_data();
        camera->load_data();
        Uint2 pixel = dispatch_idx().xy();
        SensorSample ss;
        sampler()->temporary([&](Sampler *sampler) {
            // Film coordinates must agree with the GBuffer motion vector used
            // below; the GI reservoir RNG is separately seeded afterwards.
            sampler->set_seed(pixel, frame_index, Dimension::Camera);
            ss = sampler->sensor_sample(pixel, camera->filter(), param.camera_jitter != 0u);
        });
        sampler()->set_seed(pixel, frame_index, 4);
        GISampleVar sample = samples_.read(dispatch_id());
        HitBSDFVar hit_bsdf = frame_buffer().hit_bsdfs().read(dispatch_id());
        GIReservoirVar rsv;
        Float p_hat = 0.f;
        $if(sample.sp->valid() && hit_bsdf.pdf > 0.f && !ocarina::isinf(hit_bsdf.pdf)) {
            p_hat = ocarina::max(ocarina::zero_if_nan_inf(luminance(sample.Lo.as_vec3()) * hit_bsdf.pdf), 0.f);
        };
        Float weight = GIReservoir::safe_weight(1, p_hat, 1.f / hit_bsdf.pdf);
        rsv->update(0.5f, sample, weight);
        rsv->update_W(p_hat);
        Float2 motion_vec = frame_buffer().motion_vectors().read(dispatch_id());
        Bool temporal_allowed = param.temporal != 0u;
        // Virtual receiver history does not preserve a refractive camera prefix.
        // Keep fresh GI and same-frame spatial reuse; only mask this temporal kernel.
        // denoiser_layered carries the runtime SP switch even for raw rendering.
        $if(param.denoiser_layered != 0u && temporal_allowed && surf.is_replaced) {
            Uint dominant = frame_buffer().stable_dominant().read(dispatch_id());
            $if(dominant < StablePlaneCount) {
                Uint address = dominant * (dispatch_dim().x * dispatch_dim().y) + dispatch_id();
                StablePlaneDataVar plane = frame_buffer().stable_planes().read(address);
                $if(plane->valid() != 0u && plane->reservoir_eligible() != 0u) {
                    temporal_allowed = !gi_prefix_has_transmission(plane.branch_sequence, plane.depth);
                };
            };
        };
        $if(temporal_allowed) {
            rsv = temporal_reuse(rsv, surf, motion_vec, ss, param);
        };
        passthrough_reservoirs().write(dispatch_id(), rsv);
    };
    temporal_pass_ = device().compile(kernel, "ReSTIR indirect temporal reuse");
}

GIReservoirVar ReSTIRGI::constant_combine_cached(const GIReservoirVar &canonical_rsv,
                                                 const Container<uint> &rsv_idx,
                                                 const Interaction &canonical_it,
                                                 MaterialEvaluator *cached_bsdf) const noexcept {
    GIReservoirVar ret = canonical_rsv;

    auto combine = [&](MaterialEvaluator *bsdf) {
        // Re-express the canonical stream under this receiver's current target.
        Float canonical_p_hat = compute_p_hat(canonical_it, ret.sample, bsdf);
        ret.weight_sum = GIReservoir::safe_weight(ret.C, canonical_p_hat, ret.W);
        Float winner_p_hat = canonical_p_hat;
        rsv_idx.for_each([&](const Uint &idx) {
            GIReservoirVar rsv = passthrough_reservoirs().read(idx);
            Float weight = 0.f;
            Float donor_p_hat = 0.f;
            $if(rsv.W > 0.f && rsv.sample.sp->valid()) {
                SurfaceDataVar neighbor_surf = cur_surfaces().read(idx);
                Float3 neighbor_pos = neighbor_surf->position();
                donor_p_hat = compute_p_hat(canonical_it, rsv.sample, bsdf);
                Float p_hat = donor_p_hat;
                p_hat = p_hat * Jacobian_det(canonical_it.pos, neighbor_pos, rsv.sample.sp);
                $if(p_hat > 0.f) {
                    Float v = cast<float>(!pipeline()->geometry().trace_occlusion(
                        restir_shadow::visibility_ray(canonical_it, rsv.sample.sp->position(), Bool{false})));
                    weight = GIReservoir::safe_weight(rsv.C, p_hat, rsv.W) * v;
                };
            };
            // Do not condition the denominator on this donor's sampled radiance
            // or visibility. Cross-domain support correction is a separate step.
            Bool selected = ret->update(sampler()->next_1d(), rsv.sample, weight, rsv.C);
            winner_p_hat = ocarina::select(selected, donor_p_hat, winner_p_hat);
        });
        Float p_hat = winner_p_hat;
        if (debias_) {
            Float normalization = 0.f;
            $if(ret.weight_sum > 0.f && p_hat > 0.f && ret.sample.sp->valid()) {
                normalization = ocarina::max(ocarina::zero_if_nan_inf(canonical_rsv.C), 0.f);
                rsv_idx.for_each([&](const Uint &idx) {
                    GIReservoirVar source_rsv = passthrough_reservoirs().read(idx);
                    Float count = ocarina::max(ocarina::zero_if_nan_inf(source_rsv.C), 0.f);
                    $if(count > 0.f) {
                        SurfaceDataVar source_surf = cur_surfaces().read(idx);
                        Interaction source_it = pipeline()->geometry().compute_surface_interaction(source_surf.hit, cur_view_pos(source_surf.is_replaced, idx));
                        // Zero Lo/W in this source's realization does not remove its support.
                        normalization += count * selected_sample_support(canonical_it, source_it, ret.sample);
                    };
                });
            };
            ret->update_W_with_support(p_hat, normalization);
        } else {
            ret->update_W(p_hat);
        }
    };
    if (cached_bsdf) {
        combine(cached_bsdf);
    } else {
        $if(canonical_it.has_material()) {
            scene().materials().dispatch(canonical_it.material_id(), [&](const Material *material) {
                auto bsdf = material->create_evaluator(canonical_it, sampled_wavelengths());
                combine(addressof(bsdf));
            });
        }
        $else {
            // Null receivers still carry the original streams' confidence counts.
            combine(nullptr);
        };
    }
    return ret;
}

GIReservoirVar ReSTIRGI::constant_combine(const GIReservoirVar &canonical_rsv,
                                          const Container<uint> &rsv_idx) const noexcept {
    TSensor &camera = scene().sensor();
    SurfaceDataVar cur_surf = cur_surfaces().read(dispatch_id());
    Float3 view_pos = cur_view_pos(cur_surf.is_replaced);
    Interaction canonical_it = pipeline()->geometry().compute_surface_interaction(cur_surf.hit, view_pos);

    return constant_combine_cached(canonical_rsv, rsv_idx, canonical_it, nullptr);
}

GIReservoirVar ReSTIRGI::combine_spatial(GIReservoirVar cur_rsv,
                                         const Container<uint> &rsv_idx) const noexcept {

    cur_rsv = constant_combine(cur_rsv, rsv_idx);

    return cur_rsv;
}

GIReservoirVar ReSTIRGI::spatial_reuse_cached(GIReservoirVar rsv, const SurfaceDataVar &cur_surf,
                                              const Int2 &pixel, const Var<GIParam> &param,
                                              const Interaction *cached_it,
                                              MaterialEvaluator *cached_bsdf) const noexcept {
    // A supplied evaluator already applied this receiver's normal map once.
    // Null evaluators in the cached path are the material-less kernel branch.
    Bool reuse_allowed = cached_it ? Bool{true} : receiver_reuse_allowed(cur_surf);
    if (cached_bsdf) {
        reuse_allowed = !gi_receiver_has_transmission(*cached_bsdf, spectrum()->dimension());
    }
    $if(param.spatial && reuse_allowed) {
        Int2 res = make_int2(dispatch_dim().xy());
        Container<uint> rsv_idx{spatial_.sample_num};
        $for(i, spatial_.sample_num) {
            Float2 offset = square_to_disk(sampler()->next_2d()) * param.s_radius;
            Int2 offset_i = make_int2(ocarina::round(offset));
            Int2 another_pixel = pixel + offset_i;
            another_pixel = ocarina::clamp(another_pixel, make_int2(0u), res - 1);
            Uint index = dispatch_id(another_pixel);
            SurfaceDataVar other_surf = cur_surfaces().read(index);
            $if(is_valid_neighbor(cur_surf, other_surf, param)) {
                rsv_idx.push_back(index);
            };
        };
        $if(cur_surf.hit->is_hit()) {
            if (cached_it) {
                rsv = constant_combine_cached(rsv, rsv_idx, *cached_it, cached_bsdf);
            } else {
                rsv = combine_spatial(rsv, rsv_idx);
            }
        };
    };
    return rsv;
}

GIReservoirVar ReSTIRGI::spatial_reuse(GIReservoirVar rsv, const SurfaceDataVar &cur_surf,
                                       const Int2 &pixel, const Var<GIParam> &param) const noexcept {
    return spatial_reuse_cached(rsv, cur_surf, pixel, param, nullptr, nullptr);
}

Float3 ReSTIRGI::shading(GIReservoirVar rsv,
                         const SurfaceDataVar &cur_surf, Float3 *specular, Float *hit_distance) const noexcept {
    TSensor &camera = scene().sensor();
    Float3 view_pos = cur_view_pos(cur_surf.is_replaced);
    Float3 throughput = make_float3(1.f);
    $if(cur_surf.is_replaced) {
        SurfaceExtendVar surf_ext = cur_surface_extends().read(dispatch_id());
        throughput = surf_ext.throughput.as_vec3();
        view_pos = surf_ext.view_pos.as_vec3();
    };
    Interaction it = pipeline()->geometry().compute_surface_interaction(cur_surf.hit, view_pos);
    Float3 ret = make_float3(0.f);
    $if(rsv.W > 0.f && rsv.sample.sp->valid() &&
        length_squared(rsv.sample.sp->position() - it.pos) > 0.f) {
        scene().materials().dispatch(it.material_id(), [&](const Material *material) {
            auto bsdf = material->create_evaluator(it, sampled_wavelengths());
            ScatterEval scatter_eval = eval_bsdf(it, rsv.sample, MaterialEvalMode::All, addressof(bsdf));
            $if(scatter_eval.valid() && !ocarina::isinf(scatter_eval.pdf())) {
                ret = ocarina::zero_if_nan_inf(throughput * rsv.sample.Lo.as_vec3() * scatter_eval.f.vec3() * rsv.W);
                if (specular) {
                    Float3 delta = rsv.sample.sp->position() - it.pos;
                    *specular = ret * bsdf.specular_fraction_rgb(it.wo, normalize(delta));
                    *hit_distance = max(length(delta), 1e-5f);
                }
            };
        });
    };
    return ret;
}

void ReSTIRGI::compile_spatial_shading() noexcept {
    switch_profile::Scope profile{"GI.spatial_shading.compile", "compile"};
    TSensor &camera = scene().sensor();
    TLightSampler &light_sampler = renderer().light_sampler();
    TSpectrum &spectrum = pipeline()->spectrum();
    const bool lobes = integrator()->denoiser() && integrator()->denoiser()->requires_lobe_signals();

    Kernel kernel = [&](Var<GIParam> param, Uint frame_index) {
        initial(sampler(), frame_index, spectrum);
        SurfaceDataVar surf = cur_surfaces().read(dispatch_id());
        // Invoke only after shading, avoiding live DI staging registers during
        // the expensive GI reservoir/material work. A miss still finalizes DI.
        auto finish_lobes = [&](const Float3 &total, const Float3 &specular, const Float &distance) {
            $if(param.precise_lobes != 0u) {
                Uint i = dispatch_id();
                Float4 direct = make_float4(0.f), di_specular = make_float4(0.f);
                Float3 bypass = make_float3(0.f);
                $if(param.denoiser_di_active != 0u) {
                    direct = frame_buffer().denoiser_di_specular().read(i);
                    di_specular = make_float4(frame_buffer().denoiser_gi_specular().read(i).xyz(), direct.w);
                    bypass = frame_buffer().denoiser_di_bypass().read(i).xyz();
                };
                Uint layer = 0u;
                $if(param.denoiser_layered != 0u) { layer = frame_buffer().stable_dominant().read(i); };
                $if(layer < StablePlaneCount) {
                    Uint address = layer * (dispatch_dim().x * dispatch_dim().y) + i;
                    Float4 old_d = make_float4(0.f), old_s = make_float4(0.f), old_b = make_float4(0.f);
                    $if(param.denoiser_layered != 0u) {
                        old_d = frame_buffer().denoiser_diffuse().read(address);
                        old_s = frame_buffer().denoiser_specular().read(address);
                        old_b = frame_buffer().denoiser_bypass().read(address);
                    };
                    auto merged = denoiser_signals::merge_restir_lobes(direct.xyz(), total, di_specular,
                        make_float4(specular, distance), bypass, param.denoiser_di_factor,
                        param.denoiser_gi_factor, old_d, old_s, old_b);
                    frame_buffer().denoiser_diffuse().write(address, merged.diffuse);
                    frame_buffer().denoiser_specular().write(address, merged.specular);
                    frame_buffer().denoiser_bypass().write(address, merged.bypass);
                };
                // Preserve the existing diagnostic specular-buffer contract.
                frame_buffer().denoiser_di_specular().write(i, di_specular);
                frame_buffer().denoiser_gi_specular().write(i, make_float4(specular, distance));
            };
        };
        if (lobes) {
            $if(param.precise_lobes == 0u) {
                frame_buffer().denoiser_gi_specular().write(dispatch_id(), make_float4(0.f));
            };
        }
        $if(surf.hit->is_miss()) {
            if (lobes) finish_lobes(make_float3(0.f), make_float3(0.f), Float{0.f});
            $return();
        };
        sampler()->load_data();
        sampler()->set_seed(dispatch_idx().xy(), frame_index, 5);
        camera->load_data();
        GIReservoirVar rsv = passthrough_reservoirs().read(dispatch_id());
        // Retain temporal history before spatial reuse spreads shared samples.
        // This dispatch only gathers scratch B, so writing history A is safe.
        cur_reservoirs().write(dispatch_id(), rsv);
        // Share the current receiver only for its transmission gate and PDF
        // resampling. End this material scope before exact-F shading below.
        Interaction it = pipeline()->geometry().compute_surface_interaction(
            surf.hit, cur_view_pos(surf.is_replaced));
        $if(it.has_material()) {
            scene().materials().dispatch(it.material_id(), [&](const Material *material) {
                auto bsdf = material->create_evaluator(it, sampled_wavelengths());
                rsv = spatial_reuse_cached(rsv, surf, make_int2(dispatch_idx().xy()), param,
                                          addressof(it), addressof(bsdf));
            });
        }
        $else {
            rsv = spatial_reuse_cached(rsv, surf, make_int2(dispatch_idx().xy()), param,
                                      addressof(it), nullptr);
        };
        Float3 specular = make_float3(0.f); Float distance = 0.f;
        Float3 L = shading(rsv, surf, lobes ? &specular : nullptr, lobes ? &distance : nullptr);
        write_radiance(dispatch_id(), make_float4(L, 1.f));
        if (lobes) {
            $if(param.precise_lobes != 0u) { finish_lobes(L, specular, distance); }
            $else { frame_buffer().denoiser_gi_specular().write(dispatch_id(), make_float4(specular, distance)); };
        }
    };
    kernel.function()->set_block_dim(8u, 8u, 1u);
    spatial_shading_ = device().compile(kernel, "ReSTIR indirect spatial reuse and shading");
}

GIParam ReSTIRGI::construct_param() const noexcept {
    GIParam param;
    param.camera_jitter = integrator()->jitter_primary_samples();
    param.max_age = max_age_;
    param.diff_factor = diff_factor_;

    param.spatial = static_cast<uint>(spatial_.open);
    param.N = spatial_.sample_num;
    param.s_dot = spatial_.dot_threshold();
    param.s_depth = spatial_.depth_threshold;
    param.s_radius = spatial_.sampling_radius;

    param.temporal = static_cast<uint>(temporal_.open);
    param.history_limit = temporal_.limit;
    param.t_dot = temporal_.dot_threshold();
    param.t_depth = temporal_.depth_threshold;
    param.t_radius = temporal_.sampling_radius;
    return param;
}

CommandBatch ReSTIRGI::dispatch(uint frame_index, bool precise_lobes, bool denoiser_layered,
                               float di_factor, float gi_factor, bool di_active, BufferView<uint> compact_primary_flags) const noexcept {
    CommandBatch ret;
    const Pipeline *rp = pipeline();
    GIParam param = construct_param();
    param.precise_lobes = uint(precise_lobes);
    param.denoiser_layered = uint(denoiser_layered);
    param.denoiser_di_active = uint(di_active);
    param.denoiser_di_factor = di_factor;
    param.denoiser_gi_factor = gi_factor;
    // The first frame after a mode/scene reset must not reuse old reservoirs.
    bool history_valid = history_.begin_frame(frame_index, param.camera_jitter);
    param.temporal = param.temporal && history_valid;
    const char *stage_profile = std::getenv("VISION_STAGE_PROFILE");
    const bool synchronize_native_stages = rtxpt_backend_ && stage_profile && string_view(stage_profile) == "1";
    if (synchronize_native_stages) ret << synchronize();
    if (rtxpt_backend_) {
        static_assert(offsetof(SurfaceData, flag) % sizeof(uint) == 0u && sizeof(SurfaceData) % sizeof(uint) == 0u);
        const bool compact = compact_primary_flags.size() != 0u;
        const auto flag_words = compact ? compact_primary_flags : frame_buffer().cur_surfaces_view(frame_index).view_as<uint>();
        const uint2 flag_layout = compact ? make_uint2(1u, 0u)
            : make_uint2(uint(sizeof(SurfaceData) / sizeof(uint)), uint(offsetof(SurfaceData, flag) / sizeof(uint)));
        ret << rtxpt_prefix_build_(frame_index, flag_words, flag_layout).dispatch(rp->resolution());
        if (synchronize_native_stages) ret << synchronize();
        RtxptGICameraParam camera;
        const float4x4 world_to_camera = inverse(scene().sensor()->host_c2w());
        const float4x4 camera_to_raster = inverse(scene().sensor()->host_raster_to_sensor());
        for (uint column = 0u; column < 4u; ++column) {
            camera.columns[column] = world_to_camera[column];
            camera.columns[4u + column] = camera_to_raster[column];
        }
        ret << rtxpt_initial_samples_(frame_index, camera, flag_words, flag_layout).dispatch(rp->resolution());
        if (path_cost::enabled()) ret << path_cost::capture(path_work_counters_.view(), frame_index, rp->resolution(), "gi");
    } else {
        ret << initial_samples_(frame_index).dispatch(rp->resolution());
    }
    if (synchronize_native_stages) ret << synchronize();
    if (rtxpt_backend_) ret << rtxpt_->dispatch(frame_index, param);
    else {
        ret << temporal_pass_(param, frame_index).dispatch(rp->resolution());
        ret << spatial_shading_(param, frame_index).dispatch(rp->resolution());
    }
    return ret;
}

void ReSTIRGI::update_resolution(ocarina::uint2 res) noexcept {
    Pipeline *rp = pipeline();
    if (compact_radiance_) {
        raw_radiance_.super() = device().create_buffer<uint2>(rp->pixel_num(), radiance_->name() + " rgb21");
        raw_radiance_.register_self();
        vector<uint2> zero(rp->pixel_num(), make_uint2(0u));
        raw_radiance_.upload_immediately(zero.data());
    }
    if (!rtxpt_backend_) {
    reservoirs_.super() = device().create_buffer<GIReservoirStorage>(rp->pixel_num() * 2,
                                                                     GIReservoirStorage::Name);
    reservoirs_.register_self(0, rp->pixel_num());
    reservoirs_.register_view_index(1, rp->pixel_num(), rp->pixel_num());
    vector<GIReservoirStorage> host{rp->pixel_num() * 2, GIReservoirStorage{}};
    reservoirs_.upload_immediately(host.data());
    }
    if (rtxpt_) rtxpt_->update_resolution(res);
    if (rtxpt_lights_) rtxpt_lights_->update_resolution(res);
    if (rtxpt_backend_) prepare_rtxpt_candidate_buffers();
    if (native_reblur_enabled_) {
        prepare_native_reblur_buffers();
        if (rtxpt_) {
            compile_initial_samples();
            rtxpt_->compile_native_reblur_final();
        }
    }


    samples_.super() = device().create_buffer<GISample>(rp->pixel_num(),
                                                        "ReSTIRGI::samples_");
    samples_.register_self();
    vector<GISample> vec{rp->pixel_num(), GISample{}};
    samples_.upload_immediately(vec.data());
}

void ReSTIRGI::download_storage(vector<GISample> &samples, vector<GIReservoir> &reservoirs) const noexcept {
    if (rtxpt_backend_) { rtxpt_->download_storage(samples, reservoirs); return; }
    samples.resize(pipeline()->pixel_num());
    reservoirs.resize(pipeline()->pixel_num() * 2u);
    vector<GIReservoirStorage> stored(reservoirs.size());
    pipeline()->stream() << samples_.view().download(samples.data())
                         << reservoirs_.view().download(stored.data()) << synchronize() << commit();
    for (size_t i = 0u; i < stored.size(); ++i) reservoirs[i] = unpack_gi_reservoir_host(stored[i]);
}

void ReSTIRGI::restore(RuntimeObject *old_obj) noexcept {
    const auto *previous = dynamic_cast<ReSTIRGI *>(old_obj);
    OC_ERROR_IF(!previous->radiance_->name().ends_with(" [gi-radiance-v2-rtxpt]"),
                "GI radiance storage ABI changed; restart the engine before loading the new shaders.");
    if (previous->reservoirs_.has_registered()) {
        const auto &buffer = previous->reservoirs_;
        const uint base = buffer.index().hv();
        const auto *bindings = buffer.bindless_array()->impl();
        OC_ERROR_IF(base + 1u >= bindings->buffer_num() ||
                        !gi_reservoir_storage_matches(buffer.name(), bindings->buffer_view(base),
                                                      bindings->buffer_view(base + 1u), buffer.length().hv()),
                    "GI reservoir storage or target changed; restart the engine before loading the new shaders.");
    }
    ReSTIR::restore(old_obj);
    VS_HOTFIX_MOVE_ATTRS(radiance_, reservoirs_, samples_, sample_num_, ratio_, debias_,
                       initial_samples_, temporal_pass_, spatial_shading_, raw_radiance_, compact_radiance_, radiance_prepared_,
                       rtxpt_backend_, rtxpt_, rtxpt_proposals_, rtxpt_bypass_, rtxpt_lights_, rtxpt_materials_,
                       rtxpt_prefix_planes_, rtxpt_prefix_headers_, rtxpt_prefix_stable_, rtxpt_prefix_aux_, rtxpt_prefix_build_, rtxpt_initial_samples_,
                       native_reblur_enabled_, native_reblur_diffuse_, native_reblur_specular_, native_reblur_bypass_, native_reblur_motion_y_)
    if (rtxpt_) rtxpt_->rebind_owner(*this);
    if (rtxpt_lights_) rtxpt_lights_->rebind_owner(*this);
}

void ReSTIRGI::set_compact_radiance(bool compact) noexcept {
    if (radiance_prepared_ && compact == compact_radiance_) return;
    const string suffix = " [gi-radiance-v2-rtxpt]";
    if (!radiance_->name().ends_with(suffix)) radiance_->set_name(radiance_->name() + suffix);
    ByteBufferView empty{handle_ty{0u}, 0u};
    if (radiance_prepared_) {
        // remove_buffer renumbers later slots; preserve their captured indices.
        if (compact_radiance_) {
            pipeline()->bindless_array().set_buffer(raw_radiance_.index().hv(), empty);
            raw_radiance_.super().destroy();
        } else {
            frame_buffer().unregister(radiance_);
            pipeline()->bindless_array().set_buffer(radiance_->index().hv(), empty);
            radiance_->clear_all();
        }
    }
    compact_radiance_ = compact;
    if (compact) {
        raw_radiance_.super() = device().create_buffer<uint2>(pipeline()->pixel_num(), radiance_->name() + " rgb21");
        raw_radiance_.register_self();
        vector<uint2> zero(pipeline()->pixel_num(), make_uint2(0u));
        raw_radiance_.upload_immediately(zero.data());
    } else {
        frame_buffer().prepare_screen_buffer(radiance_);
        vector<float4> zero(pipeline()->pixel_num(), make_float4(0.f));
        radiance_->device_buffer().upload_immediately(zero.data());
    }
    radiance_prepared_ = true;
}

vector<float4> ReSTIRGI::download_radiance() const noexcept {
    vector<float4> result(pipeline()->pixel_num());
    if (compact_radiance_) {
        vector<uint2> stored(result.size());
        pipeline()->stream() << raw_radiance_.view().download(stored.data()) << synchronize() << commit();
        for (size_t i = 0u; i < result.size(); ++i) result[i] = unpack_restir_radiance_host(stored[i]);
    } else {
        pipeline()->stream() << radiance_->view().download(result.data()) << synchronize() << commit();
    }
    return result;
}

void ReSTIRGI::prepare(bool compact_radiance) noexcept {
    switch_profile::Scope profile{"ReSTIRGI::prepare", "buffers"};
    Pipeline *rp = pipeline();

    set_compact_radiance(compact_radiance);

    if (!rtxpt_backend_) {
    reservoirs_.super() = device().create_buffer<GIReservoirStorage>(rp->pixel_num() * 2,
                                                                     GIReservoirStorage::Name);
    reservoirs_.register_self(0, rp->pixel_num());
    reservoirs_.register_view(rp->pixel_num(), rp->pixel_num());
    vector<GIReservoirStorage> host{rp->pixel_num() * 2, GIReservoirStorage{}};
    reservoirs_.upload_immediately(host.data());
    }


    samples_.super() = device().create_buffer<GISample>(rp->pixel_num(),
                                                        "ReSTIRGI::samples_");
    samples_.register_self();
    vector<GISample> vec{rp->pixel_num(), GISample{}};
    samples_.upload_immediately(vec.data());
    if (rtxpt_backend_) prepare_rtxpt_candidate_buffers();
}

void ReSTIRGI::prepare_rtxpt_candidate_buffers() noexcept {
    const uint pixels = pipeline()->pixel_num();
    if (path_cost::enabled()) path_work_counters_ = device().create_buffer<path_cost::PathWorkCounters>(pixels, "GI Fill work counters");
    rtxpt_proposals_.super() = device().create_buffer<HitBSDF>(pixels,
        "ReSTIRGI::rtxpt_proposals_ [native-standard-v1]");
    rtxpt_proposals_.register_self();
    vector<HitBSDF> proposals(pixels);
    rtxpt_proposals_.upload_immediately(proposals.data());
    rtxpt_bypass_.super() = device().create_buffer<float4>(pixels,
        "ReSTIRGI::rtxpt_bypass_ [native-standard-v1]");
    rtxpt_bypass_.register_self();
    vector<float4> bypass(pixels, make_float4(0.f));
    rtxpt_bypass_.upload_immediately(bypass.data());
    rtxpt_prefix_planes_.super() = device().create_buffer<RtxptGIPrefixPlane>(pixels * 3u,
        "RTXPT GI private native three stable planes");
    rtxpt_prefix_planes_.register_self();
    rtxpt_prefix_headers_.super() = device().create_buffer<uint4>(pixels,
        "RTXPT GI private stable plane headers");
    rtxpt_prefix_headers_.register_self();
    rtxpt_prefix_stable_.super() = device().create_buffer<float4>(pixels,
        "RTXPT GI private stable emission without shared direct");
    rtxpt_prefix_stable_.register_self();
    rtxpt_prefix_aux_.super() = device().create_buffer<RtxptGIPrefixAux>(pixels,
        "RTXPT GI private prefix aux and half motion [80B-v2]");
    rtxpt_prefix_aux_.register_self();
}

void ReSTIRGI::prepare_native_reblur_buffers() noexcept {
    const uint pixels = pipeline()->pixel_num();
    if (native_reblur_diffuse_.size() == size_t(pixels) * 3u &&
        native_reblur_motion_y_.size() == size_t(pixels) * 3u) return;
    native_reblur_diffuse_ = device().create_buffer<float4>(size_t(pixels) * 3u, "RTXPT GI native REBLUR diffuse x3");
    native_reblur_specular_ = device().create_buffer<float4>(size_t(pixels) * 3u, "RTXPT GI native REBLUR specular x3");
    native_reblur_bypass_ = device().create_buffer<float4>(pixels, "RTXPT GI native deterministic stable radiance");
    native_reblur_motion_y_ = device().create_buffer<float>(size_t(pixels) * 3u, "RTXPT GI native REBLUR FP32 motion Y x3");
}

bool ReSTIRGI::set_native_reblur_enabled(bool enabled) noexcept {
    enabled = enabled && rtxpt_backend_;
    if (enabled == native_reblur_enabled_) return false;
    native_reblur_enabled_ = enabled;
    if (rtxpt_) {
        if (enabled) prepare_native_reblur_buffers();
        compile_initial_samples();
        rtxpt_->compile_native_reblur_final();
    }
    invalidate_history();
    if (!enabled && native_reblur_diffuse_.size() != 0u) {
        native_reblur_diffuse_.destroy(); native_reblur_specular_.destroy();
        native_reblur_bypass_.destroy();
        native_reblur_motion_y_.destroy();
    }
    return true;
}
}// namespace vision

VS_REGISTER_HOTFIX(vision, ReSTIRGI)
