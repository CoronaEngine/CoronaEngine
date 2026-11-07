// Frozen pre-C control and real production entry points run in separate GPU kernels.
// Included only by the existing ReSTIR support test TU; no CMake target changes.
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace {
using namespace vision;
class WinnerTargetOracle final : public ReSTIRGI {
    bool oracle_debias_;
public:
    WinnerTargetOracle(const std::shared_ptr<IlluminationIntegrator>& integrator,
                       const ParameterSet& settings, bool debias)
        : ReSTIRGI(integrator, settings), oracle_debias_(debias) {}
    GIReservoirVar old_temporal(const GIReservoirVar&, SurfaceDataVar, GIReservoirVar&,
                                SurfaceDataVar*, Float3, Float3) const noexcept;
    GIReservoirVar old_spatial_cached(const GIReservoirVar&, const Container<uint>&,
                                      const Interaction&, MaterialEvaluator*) const noexcept;
    GIReservoirVar old_spatial(const GIReservoirVar& canonical, const Container<uint>& indices) const noexcept {
        SurfaceDataVar surface = cur_surfaces().read(dispatch_id());
        Interaction it = pipeline()->geometry().compute_surface_interaction(surface.hit, cur_view_pos(surface.is_replaced));
        return old_spatial_cached(canonical, indices, it, nullptr);
    }
};
GIReservoirVar WinnerTargetOracle::old_temporal(const GIReservoirVar &cur_rsv, SurfaceDataVar cur_surf,
                                          GIReservoirVar &other_rsv, SurfaceDataVar *neighbor_surf,
                                          Float3 view_pos, Float3 prev_view_pos) const noexcept {
    other_rsv.sample.age += 1;
    TSensor &camera = scene().sensor();
    Interaction it = pipeline()->geometry().compute_surface_interaction(cur_surf.hit, view_pos);
    GIReservoirVar ret;
    auto combine = [&](MaterialEvaluator *bsdf) {
        Float cur_p_hat = compute_p_hat(it, cur_rsv.sample, bsdf);
        ret->update(sampler()->next_1d(), cur_rsv.sample,
                    GIReservoir::safe_weight(cur_rsv.C, cur_p_hat, cur_rsv.W), cur_rsv.C);
        Float other_weight = 0.f;
        Interaction neighbor_it{false};
        if (neighbor_surf) {
            neighbor_it = pipeline()->geometry().compute_surface_interaction(neighbor_surf->hit, prev_view_pos);
            $if(other_rsv.W > 0.f && other_rsv.sample.sp->valid()) {
                Float other_p_hat = compute_p_hat(it, other_rsv.sample, bsdf);
                Float jacobian = Jacobian_det(it.pos, neighbor_it.pos, other_rsv.sample.sp);
                $if(other_p_hat > 0.f && jacobian > 0.f) {
                    Float visibility = pipeline()->geometry().visibility(it, other_rsv.sample.sp->position());
                    other_weight = GIReservoir::safe_weight(other_rsv.C, other_p_hat * jacobian, other_rsv.W) * visibility;
                };
            };
        }
        // A null or occluded realization still represents its original stream.
        ret->update(sampler()->next_1d(), other_rsv.sample, other_weight, other_rsv.C);
        Float p_hat = compute_p_hat(it, ret.sample, bsdf);
        if (oracle_debias_) {
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

GIReservoirVar WinnerTargetOracle::old_spatial_cached(const GIReservoirVar &canonical_rsv,
                                                 const Container<uint> &rsv_idx,
                                                 const Interaction &canonical_it,
                                                 MaterialEvaluator *cached_bsdf) const noexcept {
    GIReservoirVar ret = canonical_rsv;

    auto combine = [&](MaterialEvaluator *bsdf) {
        // Re-express the canonical stream under this receiver's current target.
        Float canonical_p_hat = compute_p_hat(canonical_it, ret.sample, bsdf);
        ret.weight_sum = GIReservoir::safe_weight(ret.C, canonical_p_hat, ret.W);
        rsv_idx.for_each([&](const Uint &idx) {
            GIReservoirVar rsv = passthrough_reservoirs().read(idx);
            Float weight = 0.f;
            $if(rsv.W > 0.f && rsv.sample.sp->valid()) {
                SurfaceDataVar neighbor_surf = cur_surfaces().read(idx);
                Float3 neighbor_pos = neighbor_surf->position();
                Float p_hat = compute_p_hat(canonical_it, rsv.sample, bsdf);
                p_hat = p_hat * Jacobian_det(canonical_it.pos, neighbor_pos, rsv.sample.sp);
                $if(p_hat > 0.f) {
                    Float v = pipeline()->geometry().visibility(canonical_it, rsv.sample.sp->position());
                    weight = GIReservoir::safe_weight(rsv.C, p_hat, rsv.W) * v;
                };
            };
            // Do not condition the denominator on this donor's sampled radiance
            // or visibility. Cross-domain support correction is a separate step.
            ret->update(sampler()->next_1d(), rsv.sample, weight, rsv.C);
        });
        Float p_hat = compute_p_hat(canonical_it, ret.sample, bsdf);
        if (oracle_debias_) {
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
} // namespace


void check_restir_gi_winner_target(vision::Pipeline& pipeline, const char* label) {
    using namespace vision;
    pipeline.activate_global_context();
    auto integrator = std::dynamic_pointer_cast<IlluminationIntegrator>(pipeline.renderer().integrator().impl());
    if (!integrator) throw std::runtime_error("GI winner target oracle needs an illumination integrator");
    integrator->set_denoise_enabled(false);
    pipeline.frame_buffer()->set_enable_accumulation(false);
    pipeline.prepare(); pipeline.upload_data();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto& sampler = pipeline.renderer().sampler();
    auto& spectrum = pipeline.renderer().spectrum();
    auto& camera = pipeline.scene().sensor();
    constexpr uint words = 20u, cases = 32u;
    const uint count = uint(pipeline.pixel_num());
    if (count < 256u) throw std::runtime_error("winner oracle needs 32 cases times at least 8 RNG streams");
    auto output = pipeline.device().create_buffer<uint>(count * words, "GI winner target full precision oracle");
    std::vector<uint> reference(count * words), candidate(count * words);
    const char* domains[]{"temporal-history", "temporal-empty-history", "spatial-canonical", "update-sanitizer"};
    for (bool debias : {false, true}) {
        ParameterSet settings{DataWrap{{"debias", debias}, {"spatial", DataWrap::object()},
                                       {"temporal", DataWrap::object()}}};
        // Keep this object and its actual A/B storage live through every download.
        WinnerTargetOracle gi{integrator, settings, debias};
        gi.radiance() = make_shared<ScreenBuffer>(std::string{"GI winner target oracle "} + label
                                                   + " debias" + std::to_string(debias));
        const uint first_slot = pipeline.bindless_array().buffer_num();
        gi.prepare();
        const uint last_slot = pipeline.bindless_array().buffer_num();
        pipeline.upload_bindless_array();
        auto cleanup = [&] {
            pipeline.frame_buffer()->unregister(gi.radiance());
            ByteBufferView empty{handle_ty{0u}, 0u};
            for (uint slot = first_slot; slot < last_slot; ++slot) pipeline.bindless_array().set_buffer(slot, empty);
            pipeline.invalidate();
        };
        try {
        Kernel seed = [&] {
            Env::instance().clear_global_vars();
            camera->load_data(); sampler->load_data(); gi.initial(sampler, Uint{0u}, spectrum);
            Uint i = dispatch_id(), c = i % cases;
            Float x = (cast<float>(i / cases) - 3.5f) * 0.03f;
            Float3 origin = camera->device_position();
            RayVar ray = make_ray(origin, normalize(make_float3(x, Float{0.f}, Float{-1.f}) - origin));
            auto hit = pipeline.geometry().trace_closest(ray);
            Interaction it = pipeline.geometry().compute_surface_interaction(hit, ray);
            SurfaceDataVar surface;
            surface.hit = hit;
            surface->set_position(it.pos);
            surface->set_normal(it.ng);
            surface.flag = SurfaceData::Diffuse;
            gi.cur_surfaces().write(i, surface);
            GIReservoirVar a, b;
            a.C = 1.25f; a.W = 0.7f; a.weight_sum = 3.25f;
            b.C = 2.5f; b.W = 1.3f; b.weight_sum = 5.5f;
            a.sample.sp->set_position(it.pos + make_float3(0.2f, 0.125f, 1.f));
            // Odd cases cross the real baffle in receiver-oracle.obj.
            b.sample.sp->set_position(it.pos + make_float3(select((c & 1u) != 0u, 2.f, 0.25f), Float{0.125f}, Float{1.5f}));
            Float3 normal = normalize(make_float3(-0.13f, 0.21f, -1.f));
            // Exercise the real typed FP16 storage conversion as an input
            // boundary, followed by the production GI44 oct16 codec below.
            $if(c >= 16u) {
                normal = unpack_storage_half4(pack_storage_half4(make_float4(normal, Float{0.f}))).xyz();
            };
            a.sample.sp->set_normal(normal); b.sample.sp->set_normal(normal);
            a.sample.Lo.set(Float3{make_float3(0.7f, 1.1f, 0.3f)});
            b.sample.Lo.set(Float3{make_float3(1.3f, 0.4f, 0.8f)});
            a.sample.age = 0.25f; b.sample.age = 7.5f;
            $if(c == 1u) { a.sample.sp->set_normal(make_float3(0.f)); };
            $if(c == 2u) { a.C = 0.f; };
            $if(c == 3u) { a.W = 0.f; };
            $if(c == 4u) { a.sample.Lo.set(Float3{make_float3(0.f)}); };
            $if(c == 5u) { b.sample.sp->set_normal(make_float3(0.f)); };
            $if(c == 6u) { b.C = 0.f; };
            $if(c == 7u) { b.W = 0.f; };
            $if(c == 8u) { b.sample.Lo.set(Float3{make_float3(0.f)}); };
            $if(c == 9u) { a.W = as<float>(Uint{0x7fc12345u}); };
            $if(c == 10u) { b.W = as<float>(Uint{0x7f800000u}); };
            $if(c == 11u) { a.C = -1.f; };
            $if(c == 12u) { b.C = as<float>(Uint{0x7fc12345u}); };
            $if(c == 13u) { a.sample.Lo.set(Float3{make_float3(0.f)}); b.sample.Lo.set(Float3{make_float3(0.f)}); };
            $if(c == 14u) { b.sample.Lo.set(make_float3(as<float>(Uint{0x7f000000u}))); };
            $if(c == 15u) { b.sample.Lo.set(make_float3(as<float>(Uint{0x7f800000u}))); };
            $if(c == 16u) { b.sample.Lo.set(make_float3(as<float>(Uint{0x7fc54321u}))); };
            $if(c == 17u) { b.sample.sp->set_position(make_float3(1e20f, -1e20f, 1e20f)); };
            $if(c == 18u) { b.sample.sp->set_position(it.pos); };
            $if(c == 19u) { b.sample.sp->set_position(it.pos - make_float3(0.f, 0.f, 1.f)); };
            $if(c == 20u) { b.sample.sp->set_position(it.pos + make_float3(1.f, 0.f, 1e-5f)); };
            $if(c == 21u) { b.sample.sp->set_normal(make_float3(0.f, 1.f, 0.f)); };
            $if(c == 22u) { a.C = as<float>(Uint{0x7f800000u}); };
            $if(c == 23u) { a.C = as<float>(Uint{0x7f7fffffu}); b.C = a.C; a.W = 1e-38f; b.W = 1e-38f; };
            $if(c == 24u) { a.W = as<float>(Uint{0xff800000u}); };
            $if(c == 25u) { a.C = -0.f; b.C = 0.75f; };
            $if(c == 26u) { a.sample.sp->set_position(it.pos); };
            $if(c == 27u) { a.W = 0.f; b.W = 0.f; };
            $if(c == 28u) { a.weight_sum = as<float>(Uint{0x7f800000u}); };
            $if(c == 29u) { b.W = -3.f; };
            $if(c == 30u) { a.sample.Lo.set(Float3{make_float3(1e-30f)}); };
            $if(c == 31u) { b.C = 0.5f; b.sample.age = as<float>(Uint{0x7fcabcdeu}); };
            gi.cur_reservoirs().write(i, a);
            gi.passthrough_reservoirs().write(i, b);
        };
        auto initialize = pipeline.device().compile(seed, "gi_winner_target_actual_GI44_inputs");
        pipeline.stream() << initialize().dispatch(pipeline.resolution()) << synchronize() << commit();
        for (uint domain = 0u; domain < 4u; ++domain) {
            if (domain == 3u && debias) continue;
            auto build = [&](bool tracked) {
                Kernel kernel = [&](BufferVar<uint> result) {
                    Env::instance().clear_global_vars();
                    camera->load_data(); sampler->load_data(); gi.initial(sampler, Uint{0u}, spectrum);
                    sampler->set_seed(dispatch_idx().xy(), Uint{137u}, Dimension::PathTracing);
                    Uint i = dispatch_id(), c = i % cases, base = i * words;
                    for (uint word = 0u; word < words; ++word) result.write(base + word, 0u);
                    SurfaceDataVar surface = gi.cur_surfaces().read(i);
                    $if(surface.hit->is_miss()) { $return(); };
                    Uint neighbor = (i + cases) % count;
                    SurfaceDataVar donor_surface = gi.cur_surfaces().read(neighbor);
                    GIReservoirVar current = gi.cur_reservoirs().read(i);
                    GIReservoirVar donor = gi.passthrough_reservoirs().read(neighbor);
                    GIReservoirVar ret;
                    if (domain <= 1u) {
                        auto* history_surface = domain == 0u ? &donor_surface : nullptr;
                        ret = tracked ? gi.ReSTIRGI::combine_temporal(current, surface, donor, history_surface,
                                                      camera->device_position(), camera->device_position())
                                      : gi.old_temporal(current, surface, donor, history_surface,
                                                      camera->device_position(), camera->device_position());
                        result.write(base + 15u, as<uint>(donor.sample.age));
                    } else if (domain == 2u) {
                        Container<uint> indices{2u};
                        $if(c > 3u && c != 27u) { indices.push_back(neighbor); indices.push_back((neighbor + 1u) % count); };
                        ret = tracked ? gi.ReSTIRGI::constant_combine(current, indices) : gi.old_spatial(current, indices);
                    } else {
                        // Direct real-update controls reach invalid raw weights
                        // which production safe_weight deliberately filters out.
                        Interaction it = pipeline.geometry().compute_surface_interaction(surface.hit, camera->device_position());
                        auto merge = [&](MaterialEvaluator* bsdf) {
                            Float current_target = gi.compute_p_hat(it, current.sample, bsdf);
                            Float donor_target = gi.compute_p_hat(it, donor.sample, bsdf);
                            Float first_weight = 99.f, second_weight = 1.f;
                            $if(c == 0u) { first_weight = 0.f; second_weight = 0.f; };
                            $if(c == 1u) { first_weight = -1.f; second_weight = -3.f; };
                            $if(c == 2u) { first_weight = as<float>(Uint{0x7fc12345u}); };
                            $if(c == 3u) { second_weight = as<float>(Uint{0x7f800000u}); };
                            $if(c == 6u) { first_weight = as<float>(Uint{0x7f400000u}); second_weight = first_weight; };
                            $if(c == 5u) { first_weight = 0.f; second_weight = 1.f; };
                            $if(c >= 8u && c <= 11u) { first_weight = 0.f; second_weight = 0.f; };
                            $if((c & 8u) != 0u) { ret = current; };
                            // Spatial canonical initialization must retain its
                            // target even when C/W or rebased weight_sum is zero.
                            Float winner_target = select((c & 8u) != 0u, current_target, 0.f);
                            Bool first = ret->update(sampler->next_1d(), current.sample, first_weight, current.C);
                            winner_target = select(first, current_target, winner_target);
                            Bool second = ret->update(sampler->next_1d(), donor.sample, second_weight, donor.C);
                            winner_target = select(second, donor_target, winner_target);
                            Float target = tracked ? winner_target : gi.compute_p_hat(it, ret.sample, bsdf);
                            ret->update_W(target);
                            result.write(base + 15u, as<uint>(target));
                            result.write(base + 16u, cast<uint>(first) | (cast<uint>(second) << 1u));
                        };
                        pipeline.scene().materials().dispatch(it.material_id(), [&](const Material* material) {
                            auto bsdf = material->create_evaluator(it, gi.sampled_wavelengths()); merge(&bsdf);
                        });
                    }
                    // Full precision fields, including decoded normals: comparing
                    // re-packed GI44 alone could hide sub-quantization differences.
                    result.write(base, 1u);
                    result.write(base + 1u, as<uint>(ret.W));
                    result.write(base + 2u, as<uint>(ret.C));
                    result.write(base + 3u, as<uint>(ret.weight_sum));
                    for (uint component = 0u; component < 3u; ++component) {
                        result.write(base + 4u + component, as<uint>(ret.sample.sp.pos[component]));
                        result.write(base + 7u + component, as<uint>(ret.sample.sp->normal()[component]));
                        result.write(base + 10u + component, as<uint>(ret.sample.Lo[component]));
                    }
                    result.write(base + 13u, as<uint>(ret.sample.age));
                    result.write(base + 14u, as<uint>(sampler->next_1d()));
                    // Independent path coverage runs after the saved nextRNG.
                    Interaction canonical_it = pipeline.geometry().compute_surface_interaction(surface.hit, camera->device_position());
                    Interaction donor_it = pipeline.geometry().compute_surface_interaction(donor_surface.hit, camera->device_position());
                    Float jacobian = gi.Jacobian_det(canonical_it.pos, donor_it.pos, donor.sample.sp);
                    result.write(base + 17u, as<uint>(jacobian));
                    if (domain == 0u || domain == 2u) {
                        Bool donor_won = all(ret.sample.sp->position() == donor.sample.sp->position()) &&
                                         ret.sample.age == donor.sample.age;
                        result.write(base + 16u, as<uint>(select(donor_won, jacobian, 0.f)));
                    }
                    Float visible = -1.f;
                    Float distance_squared = length_squared(donor.sample.sp->position() - canonical_it.pos);
                    $if(donor.sample.sp->valid() && distance_squared > 0.f && !ocarina::isnan(distance_squared) && !ocarina::isinf(distance_squared)) {
                        visible = pipeline.geometry().visibility(canonical_it, donor.sample.sp->position());
                    };
                    result.write(base + 18u, as<uint>(visible));
                    result.write(base + 19u, as<uint>(gi.selected_sample_support(canonical_it, donor_it, current.sample)));
                };
                const std::string name = std::string{tracked ? "gi_winner_target_candidate_" : "gi_winner_target_frozen_control_"}
                    + label + "_debias" + std::to_string(debias) + "_domain" + std::to_string(domain);
                return pipeline.device().compile(kernel, name);
            };
            auto old_shader = build(false);
            auto tracked_shader = build(true);
            pipeline.stream() << old_shader(output).dispatch(pipeline.resolution()) << output.download(reference.data())
                              << tracked_shader(output).dispatch(pipeline.resolution()) << output.download(candidate.data())
                              << synchronize() << commit();
            uint hits = 0u, positive = 0u, zero = 0u, nonunit_jacobians = 0u, blocked = 0u, support = 0u, nonunit_donor_winners = 0u;
            for (uint i = 0u; i < count; ++i) {
                for (uint word = 0u; word < words; ++word) {
                    if (reference[i * words + word] != candidate[i * words + word]) {
                        std::cerr << "GI winner target mismatch fixture=" << label << " debias=" << debias
                                  << " domain=" << domains[domain] << " case=" << i % cases << " seed=" << i / cases
                                  << " field=" << word << " old_bits=" << reference[i * words + word]
                                  << " tracked_bits=" << candidate[i * words + word] << '\n';
                        throw std::runtime_error("GI winner target reservoir/RNG changed bitwise");
                    }
                }
                const auto* value = reference.data() + i * words;
                hits += value[0];
                positive += value[1] > 0u && value[1] < 0x7f800000u;
                zero += value[1] == 0u;
                nonunit_jacobians += value[17] > 0u && value[17] < 0x7f800000u && value[17] != 0x3f800000u;
                if (domain == 0u || domain == 2u)
                    nonunit_donor_winners += value[16] > 0u && value[16] < 0x7f800000u && value[16] != 0x3f800000u;
                blocked += value[18] == 0u;
                support += value[19] == 0x3f800000u;
            }
            if (hits != count || positive == 0u || zero == 0u || nonunit_jacobians == 0u || blocked == 0u || support == 0u)
                throw std::runtime_error("GI winner target oracle fixture did not exercise required receiver/weight/J/visibility/support domains");
            if ((domain == 0u || domain == 2u) && nonunit_donor_winners == 0u)
                throw std::runtime_error("GI winner target oracle did not select a real non-unit-J donor");
            std::cout << "PASS: GI winner target fixture=" << label << " debias=" << debias << " domain=" << domains[domain]
                      << " samples=" << count << " positive_W=" << positive << " zero_W=" << zero
                      << " nonunit_J=" << nonunit_jacobians << " blocked=" << blocked << " supported=" << support
                      << " nonunit_donor_winners=" << nonunit_donor_winners
                      << " compared=full_reservoir_bits+nextRNG+history_age\n";
        }
        } catch (...) { cleanup(); throw; }
        cleanup();
    }
}
