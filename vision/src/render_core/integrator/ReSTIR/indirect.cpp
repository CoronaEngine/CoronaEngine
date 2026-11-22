//
// Created by Zero on 2023/10/29.
//

#include "indirect.h"
#include "base/scattering/gi_ideal_glass.h"
#include "base/integral/path_work_capture.h"
#include "rtxpt_gi.h"
#include "rtxpt_gi_candidate.h"
#include "rtxpt_gi_material_inputs.h"
#include "rtxpt_gi_lights.h"
#include "rtxpt_gi_prefix.h"
#include "rtxpt_gi_pt.h"
#include "base/integral/integrator.h"

namespace vision {
using namespace ocarina;
ReSTIRGI::ReSTIRGI(IntegratorPtr integrator,
                   const vision::ParameterSet &desc)
    : ReSTIR(integrator, desc) {
    const char *override_backend = std::getenv("VISION_RESTIR_GI_BACKEND");
    const string backend = override_backend && *override_backend ? override_backend : desc["backend"].as_string("rtxpt");
    if (backend == "legacy") {
        OC_WARNING("The legacy ReSTIR GI backend has been removed; using RTXPT GI.");
    } else if (backend != "rtxpt") {
        unsupported_backend_ = "Unsupported ReSTIR GI backend '" + backend + "'. Expected rtxpt.";
    }
    // Two GI neighbors balance reuse quality and the cost of support queries.
    spatial_.sample_num = spatial_.open ? desc["spatial"]["sample_num"].as_uint(2u) : 1u;
    spatial_.sampling_radius = desc["spatial"]["radius"].as_float(12.f);
}

void ReSTIRGI::compile(bool refresh_lighting) noexcept {
    switch_profile::Scope profile{"ReSTIR.GI.compile", "compile"};
    backend_error_.clear();
    if (!unsupported_backend_.empty()) {
        backend_error_ = unsupported_backend_;
        return;
    }
    if (scene().process_mediums() || spectrum()->dimension() != 3u) {
        backend_error_ = scene().process_mediums()
            ? "RTXPT GI does not support participating media" : "RTXPT GI requires an RGB spectrum";
        return;
    }
    if (native_reblur_enabled_) prepare_native_reblur_buffers();
    if (!rtxpt_materials_) rtxpt_materials_ = make_shared<rtxpt_gi::StandardMaterialInputs>();
    rtxpt_materials_->prepare();
    // A whole-pipeline compile can follow light or topology replacement.
    // Rebuild the native emitter mapping after the scene is rebound.
    if (refresh_lighting || !rtxpt_lights_ || !rtxpt_) {
        if (!rtxpt_lights_) rtxpt_lights_ = make_shared<rtxpt_gi::NativeLightSampler>(*this);
        try {
            rtxpt_lights_->prepare();
        } catch (const std::runtime_error &error) {
            backend_error_ = error.what();
            rtxpt_lights_.reset();
            return;
        }
        if (!rtxpt_) {
            rtxpt_ = make_shared<RtxptGI>(*this);
            rtxpt_->prepare();
        }
        // These buffers register bindless views after the pipeline's prepare-time upload.
        pipeline()->upload_bindless_array();
    }
    compile_initial_samples();
    rtxpt_->compile();
    OC_INFO("RTXPT ReSTIR GI active: shared ordinary PT, complete BSDF, private proposals, packed32 reservoirs, temporal/spatial reuse");
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

}// namespace

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

void ReSTIRGI::compile_initial_samples() noexcept {
    switch_profile::Scope profile{"GI.initial.compile", "compile"};
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
    Kernel build = [&](Uint frame_index, BufferVar<uint> flags) {
        gi_principled::CompilationScope parameters{false};
        initial(sampler(), frame_index, spectrum());
        scene().sensor()->load_data();
        sampler()->load_data();
        integrator()->load_data();
        rtxpt_gi::build_pt_prefix(prefix_storage(), *this, flags.read(dispatch_id()));
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
    Kernel kernel = [&](Uint frame_index, Var<RtxptGICameraParam> camera, BufferVar<uint> flags) {
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
        capture.first_reflection_covered_by_di = !BxDFFlag::is_specular(flags.read(dispatch_id()));
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
}

GIParam ReSTIRGI::construct_param() const noexcept {
    GIParam param;
    param.camera_jitter = integrator()->jitter_primary_samples();
    param.max_age = max_age_;

    param.spatial = static_cast<uint>(spatial_.open);
    param.N = spatial_.sample_num;
    param.s_dot = spatial_.dot_threshold();
    param.s_depth = spatial_.depth_threshold;
    param.s_radius = spatial_.sampling_radius;

    param.temporal = static_cast<uint>(temporal_.open);
    param.history_limit = temporal_.limit;
    param.t_dot = temporal_.dot_threshold();
    param.t_depth = temporal_.depth_threshold;
    return param;
}

CommandBatch ReSTIRGI::dispatch(uint frame_index, bool precise_lobes, BufferView<uint> primary_flags,
                                float di_factor, float gi_factor, bool di_active) const noexcept {
    CommandBatch ret;
    const Pipeline *rp = pipeline();
    GIParam param = construct_param();
    param.precise_lobes = uint(precise_lobes);
    param.denoiser_di_active = uint(di_active);
    param.denoiser_di_factor = di_factor;
    param.denoiser_gi_factor = gi_factor;
    // The first frame after a mode/scene reset must not reuse old reservoirs.
    bool history_valid = history_.begin_frame(frame_index, param.camera_jitter);
    param.temporal = param.temporal && history_valid;
    const char *stage_profile = std::getenv("VISION_STAGE_PROFILE");
    const bool synchronize_native_stages = stage_profile && string_view(stage_profile) == "1";
    if (synchronize_native_stages) ret << synchronize();
    ret << rtxpt_prefix_build_(frame_index, primary_flags).dispatch(rp->resolution());
    if (synchronize_native_stages) ret << synchronize();
    RtxptGICameraParam camera;
    const float4x4 world_to_camera = inverse(scene().sensor()->host_c2w());
    const float4x4 camera_to_raster = inverse(scene().sensor()->host_raster_to_sensor());
    for (uint column = 0u; column < 4u; ++column) {
        camera.columns[column] = world_to_camera[column];
        camera.columns[4u + column] = camera_to_raster[column];
    }
    ret << rtxpt_initial_samples_(frame_index, camera, primary_flags).dispatch(rp->resolution());
    if (path_cost::enabled()) ret << path_cost::capture(path_work_counters_.view(), frame_index, rp->resolution(), "gi");
    if (synchronize_native_stages) ret << synchronize();
    ret << rtxpt_->dispatch(frame_index, param);
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
    if (rtxpt_) rtxpt_->update_resolution(res);
    if (rtxpt_lights_) rtxpt_lights_->update_resolution(res);
    prepare_rtxpt_candidate_buffers();
    if (native_reblur_enabled_) {
        prepare_native_reblur_buffers();
        if (rtxpt_ && backend_error_.empty()) {
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
    rtxpt_->download_storage(samples, reservoirs);
}

void ReSTIRGI::restore(RuntimeObject *old_obj) noexcept {
    const auto *previous = dynamic_cast<ReSTIRGI *>(old_obj);
    OC_ERROR_IF(!previous->radiance_->name().ends_with(" [gi-radiance-v3-rtxpt]"),
                "GI radiance storage ABI changed; restart the engine before loading the new shaders.");
    ReSTIR::restore(old_obj);
    VS_HOTFIX_MOVE_ATTRS(radiance_, samples_, raw_radiance_, compact_radiance_, radiance_prepared_,
                       unsupported_backend_, backend_error_, path_work_counters_, rtxpt_, rtxpt_proposals_, rtxpt_bypass_, rtxpt_lights_, rtxpt_materials_,
                       rtxpt_prefix_planes_, rtxpt_prefix_headers_, rtxpt_prefix_stable_, rtxpt_prefix_aux_, rtxpt_prefix_build_, rtxpt_initial_samples_,
                       native_reblur_enabled_, native_reblur_diffuse_, native_reblur_specular_, native_reblur_bypass_, native_reblur_motion_y_)
    if (rtxpt_) rtxpt_->rebind_owner(*this);
    if (rtxpt_lights_) rtxpt_lights_->rebind_owner(*this);
}

void ReSTIRGI::set_compact_radiance(bool compact) noexcept {
    if (radiance_prepared_ && compact == compact_radiance_) return;
    const string suffix = " [gi-radiance-v3-rtxpt]";
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

    samples_.super() = device().create_buffer<GISample>(rp->pixel_num(),
                                                        "ReSTIRGI::samples_");
    samples_.register_self();
    vector<GISample> vec{rp->pixel_num(), GISample{}};
    samples_.upload_immediately(vec.data());
    prepare_rtxpt_candidate_buffers();
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
    if (enabled == native_reblur_enabled_) return false;
    native_reblur_enabled_ = enabled;
    if (rtxpt_ && backend_error_.empty()) {
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
