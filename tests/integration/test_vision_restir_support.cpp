// Exercise the real material and ReSTIR GI GPU methods on a receiver whose
// shading normal tilts across its geometric hemisphere. A positive BSDF value
// with a rejected (zero-PDF) direction must not become reusable GI energy.
#define VISION_PLUGIN_NAME "ReSTIR"
#define VISION_CATEGORY "integrator"
#include "render_core/integrator/ReSTIR/indirect.h"
#include "render_core/integrator/ReSTIR/direct.h"
#include "base/integral/integrator.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void check_restir_gi_receiver_support(vision::Pipeline& pipeline) {
    using namespace vision;
    pipeline.activate_global_context();
    auto integrator = std::dynamic_pointer_cast<IlluminationIntegrator>(pipeline.renderer().integrator().impl());
    if (!integrator || pipeline.scene().instances().size() != 1u) {
        throw std::runtime_error("GI support regression needs the tilted diffuse receiver fixture");
    }
    integrator->set_denoise_enabled(false);
    pipeline.frame_buffer()->set_enable_accumulation(false);
    pipeline.prepare();
    pipeline.upload_data();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto& sampler = pipeline.renderer().sampler();
    auto& spectrum = pipeline.renderer().spectrum();
    auto& camera = pipeline.scene().sensor();

    // Separate kernels keep each outlined material evaluation's DSL state local.
    // The entry points are production methods, linked from vision-integrator-ReSTIR.
    auto build_shader = [&](unsigned check) {
        ParameterSet parameters{DataWrap{{"spatial", DataWrap::object()}, {"temporal", DataWrap::object()}}};
        ReSTIRGI gi{integrator, parameters};
        Kernel kernel = [&](BufferVar<float4> output) {
            Env::instance().clear_global_vars();
            sampler->load_data();
            camera->load_data();
            gi.initial(sampler, Uint{0u}, spectrum);
            sampler->set_seed(dispatch_idx().xy(), Uint{0u}, Dimension::PathTracing);
            RayVar ray = make_ray(Float3{make_float3(0.f)}, Float3{make_float3(0.f, 0.f, -1.f)});
            TriangleHitVar hit = pipeline.geometry().trace_closest(ray);
            $if(hit->is_miss()) {
                output.write(dispatch_id(), make_float4(-1.f));
                $return();
            };
            Interaction it = pipeline.geometry().compute_surface_interaction(hit, ray);
            Bool fixture_valid = it.ng.z > 0.99f && it.shading.normal().x > 0.7f &&
                                 it.shading.normal().z > 0.7f && it.wo.z > 0.99f;
            $if(!fixture_valid) {
                output.write(dispatch_id(), make_float4(-1.f));
                $return();
            };
            GISampleVar sample;
            // id 0 crosses the geometric hemisphere but stays above the tilted
            // shading hemisphere; id 1 is a supported, nonzero control.
            Float z = ocarina::select(dispatch_id() == 0u, -0.1f, 0.1f);
            sample.sp->set_position(it.pos + make_float3(Float{1.f}, Float{0.f}, z));
            sample.sp->set_normal(Float3{make_float3(0.f, 0.f, 1.f)});
            sample.Lo.set(Float3{make_float3(1.f)});
            if (check == 0u) {
                ScatterEval eval = gi.eval_bsdf(it, sample, MaterialEvalMode::All);
                output.write(dispatch_id(), make_float4(eval.f.vec3(), eval.pdf()));
            } else if (check == 1u) {
                Float target = gi.compute_p_hat(it, sample);
                output.write(dispatch_id(), make_float4(target, target, target, 1.f));
            } else {
                // A positive cached reservoir tests shading independently of
                // the target-weight guard, including a stale imported sample.
                GIReservoirVar reservoir;
                reservoir.sample = sample;
                reservoir.C = 1.f;
                reservoir.W = 1.f;
                reservoir.weight_sum = 1.f;
                SurfaceDataVar surface;
                surface.hit = hit;
                surface.is_replaced = false;
                output.write(dispatch_id(), make_float4(gi.shading(reservoir, surface), 1.f));
            }
        };
        const char* names[]{"restir_gi_support_material", "restir_gi_support_target", "restir_gi_support_shading"};
        return pipeline.device().compile(kernel, names[check]);
    };
    auto material_shader = build_shader(0u);
    auto target_shader = build_shader(1u);
    auto shading_shader = build_shader(2u);
    auto output = pipeline.device().create_buffer<float4>(2u, "restir_gi_receiver_support_results");
    std::vector<float4> material(2u), target(2u), shading(2u);
    pipeline.stream() << material_shader(output).dispatch(2u) << output.download(material.data())
                      << target_shader(output).dispatch(2u) << output.download(target.data())
                      << shading_shader(output).dispatch(2u) << output.download(shading.data())
                      << synchronize() << commit();
    for (const auto* results : {&material, &target, &shading}) {
        for (const auto& value : *results) {
            if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z) || !std::isfinite(value.w)) {
                throw std::runtime_error("GI receiver support regression returned nonfinite data");
            }
        }
    }
    if (material[0].x <= 0.05f || material[0].w != 0.f ||
        material[1].x <= 0.05f || material[1].w <= 0.f) {
        throw std::runtime_error("tilted receiver must produce positive BSDF with zero rejected PDF and positive supported PDF");
    }
    std::cout << "gi_support_rejected_f=" << material[0].x << " rejected_pdf=" << material[0].w
              << " rejected_target=" << target[0].x << " rejected_shading=" << shading[0].x
              << " supported_target=" << target[1].x << " supported_shading=" << shading[1].x << '\n';
    if (target[1].x <= 0.05f || shading[1].x <= 0.05f) {
        throw std::runtime_error("GI support guards must preserve the supported nonzero control");
    }
    if (std::abs(target[0].x) > 1e-7f || std::abs(shading[0].x) > 1e-7f ||
        std::abs(shading[0].y) > 1e-7f || std::abs(shading[0].z) > 1e-7f) {
        throw std::runtime_error("GI target and shading must reject positive-F directions whose material PDF is zero");
    }
    std::cout << "PASS: GI target and shading enforce the receiver's sampling support\n";
}

// Check independently produced frame data, not a SurfaceData record populated
// by this test. The reference reconstructs geometry using the old GI donor path.
void check_restir_gi_receiver_positions(vision::Pipeline& pipeline, const char* label,
                                       bool require_replaced, bool require_jacobian) {
    using namespace vision;
    pipeline.activate_global_context();
    auto integrator = std::dynamic_pointer_cast<IlluminationIntegrator>(pipeline.renderer().integrator().impl());
    if (!integrator) throw std::runtime_error("GI position parity needs a real illumination integrator");
    auto& camera = pipeline.scene().sensor();
    auto& sampler = pipeline.renderer().sampler();
    auto& fb = *pipeline.frame_buffer();
    const auto original_position = camera->position();
    const auto original_yaw = camera->yaw();
    const bool original_planes = integrator->stable_planes_enabled();
    const bool original_accumulation = fb.enable_accumulation();
    ParameterSet settings{DataWrap{{"spatial", {{"open", true}, {"sample_num", 4u}, {"radius", 12.f}}},
                                    {"temporal", {{"open", false}}}}};
    ReSTIRGI gi{integrator, settings};
    constexpr uint words = 16u;
    Kernel kernel = [&](Uint frame, Var<GIParam> param, BufferVar<uint> output) {
        Env::instance().clear_global_vars();
        camera->load_data(); sampler->load_data();
        sampler->set_seed(dispatch_idx().xy(), frame, 5u);
        const auto& geometry = pipeline.geometry();
        Uint base = dispatch_id() * words;
        for (uint word = 0u; word < words; ++word) output.write(base + word, 0u);
        auto surfaces = fb.cur_surfaces_var(frame);
        SurfaceDataVar surface = surfaces.read(dispatch_id());
        $if(surface.hit->is_miss()) { $return(); };
        output.write(base + 9u, 1u);
        // Invalid replacement paths are excluded by production spatial validation.
        $if(surface.is_replaced &&
            (surface.stable_branch == 0u || surface.stable_branch == InvalidUI32)) { $return(); };
        Float3 view = camera->device_position();
        $if(surface.is_replaced) {
            view = fb.cur_surface_exts_var(frame).read(dispatch_id()).view_pos.as_vec3();
        };
        Interaction receiver = geometry.compute_surface_interaction(surface.hit, view);
        Float3 stored = surface->position();
        Uint position_mismatch = 0u, nonfinite = 0u;
        for (uint component = 0u; component < 3u; ++component) {
            Uint cached_bits = as<uint>(stored[component]);
            Uint reference_bits = as<uint>(receiver.pos[component]);
            position_mismatch |= cast<uint>(cached_bits != reference_bits) << component;
            nonfinite += cast<uint>(ocarina::isnan(stored[component]) || ocarina::isinf(stored[component]) ||
                                    ocarina::isnan(receiver.pos[component]) || ocarina::isinf(receiver.pos[component]));
            output.write(base + 10u + component, cached_bits);
            output.write(base + 13u + component, reference_bits);
        }
        Uint accepted = 0u, donor_mismatch = 0u, endpoints = 0u, positive_jacobians = 0u, jacobian_mismatch = 0u;
        $for(i, 4u) {
            Float2 offset = square_to_disk(sampler->next_2d()) * param.s_radius;
            Int2 neighbor = make_int2(dispatch_idx().xy()) + make_int2(ocarina::round(offset));
            neighbor = ocarina::clamp(neighbor, make_int2(0u), make_int2(dispatch_dim().xy()) - 1);
            Uint index = dispatch_id(neighbor);
            SurfaceDataVar donor = surfaces.read(index);
            $if(ReSTIRGI::is_valid_neighbor(surface, donor, param)) {
                accepted += 1u;
                Float3 donor_view = camera->device_position();
                $if(donor.is_replaced) {
                    donor_view = fb.cur_surface_exts_var(frame).read(index).view_pos.as_vec3();
                };
                Interaction reference = geometry.compute_surface_interaction(donor.hit, donor_view);
                Float3 cached = donor->position();
                for (uint component = 0u; component < 3u; ++component) {
                    donor_mismatch += cast<uint>(as<uint>(cached[component]) != as<uint>(reference.pos[component]));
                }
                // Use the first secondary endpoint of this frame's real DI BSDF
                // candidate, the same segment that seeds production GI. A lone
                // triangle has only environment misses; multi-surface fixtures
                // below must also supply positive nontrivial Jacobian controls.
                HitBSDFVar bsdf = fb.hit_bsdfs().read(index);
                $if(bsdf->valid() && !ocarina::isinf(bsdf.pdf)) {
                    RayVar ray = reference.spawn_ray(bsdf.wi.as_vec3());
                    TriangleHitVar endpoint_hit = geometry.trace_closest(ray);
                    $if(endpoint_hit->is_hit()) {
                        Interaction endpoint = geometry.compute_surface_interaction(endpoint_hit, ray);
                        Var<SurfacePoint> sample_point;
                        sample_point->set(endpoint);
                        Float old_jacobian = gi.Jacobian_det(receiver.pos, reference.pos, sample_point);
                        Float cached_jacobian = gi.Jacobian_det(receiver.pos, cached, sample_point);
                        endpoints += 1u;
                        positive_jacobians += cast<uint>(old_jacobian > 0.f);
                        jacobian_mismatch += cast<uint>(as<uint>(old_jacobian) != as<uint>(cached_jacobian));
                    };
                };
            };
        };
        output.write(base, 1u);
        output.write(base + 1u, cast<uint>(surface.is_replaced));
        output.write(base + 2u, position_mismatch);
        output.write(base + 3u, nonfinite);
        output.write(base + 4u, accepted);
        output.write(base + 5u, donor_mismatch);
        output.write(base + 6u, endpoints);
        output.write(base + 7u, positive_jacobians);
        output.write(base + 8u, jacobian_mismatch);
    };
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    fb.prepare_view_texture();
    auto shader = pipeline.device().compile(kernel, "restir_produced_receiver_position_parity");
    const auto param = gi.construct_param();
    std::vector<uint> values(pipeline.pixel_num() * words);
    auto output = pipeline.device().create_buffer<uint>(values.size(), "restir_receiver_position_parity_results");
    uint sp_replaced = 0u, sp_jacobians = 0u;
    auto restore = [&] {
        camera->set_position(original_position); camera->set_yaw(original_yaw); camera->update_device_data();
        integrator->set_stable_planes_enabled(original_planes);
        fb.set_enable_accumulation(original_accumulation);
        pipeline.invalidate(); pipeline.upload_data(); pipeline.display(1.0 / 60.0);
    };
    try {
        fb.set_enable_accumulation(false);
        for (const bool planes : {false, true}) {
            integrator->set_stable_planes_enabled(planes);
            pipeline.invalidate();
            for (uint pose = 0u; pose < 3u; ++pose) {
                auto position = original_position;
                if (pose > 0u) position.x += 0.037f;
                camera->set_position(position);
                camera->set_yaw(original_yaw + (pose == 2u ? 1.25f : 0.f));
                camera->update_device_data();
                pipeline.upload_data(); pipeline.display(1.0 / 60.0);
                const uint frame = pipeline.frame_index() - 1u;
                pipeline.stream() << shader(frame, param, output).dispatch(pipeline.resolution())
                                  << output.download(values.data()) << synchronize() << commit();
                uint hits = 0u, receivers = 0u, replaced = 0u, donors = 0u, jacobians = 0u;
                for (uint pixel = 0u; pixel < pipeline.pixel_num(); ++pixel) {
                    const uint* value = values.data() + pixel * words;
                    hits += value[9];
                    if (value[0] == 0u) continue;
                    receivers += 1u; replaced += value[1]; donors += value[4]; jacobians += value[7];
                    if (value[2] || value[3] || value[5] || value[8]) {
                        std::cerr << "position parity fixture=" << label << " SP=" << planes << " pose=" << pose
                                  << " pixel=" << pixel << " stored_bits=" << value[10] << ',' << value[11] << ',' << value[12]
                                  << " reference_bits=" << value[13] << ',' << value[14] << ',' << value[15] << '\n';
                        throw std::runtime_error("produced ReSTIR receiver position or GI Jacobian differs bitwise from geometry reconstruction");
                    }
                }
                std::cout << "GI position fixture=" << label << " SP=" << planes << " pose=" << pose
                          << " frame=" << frame << " hits=" << hits << " receivers=" << receivers << " replaced=" << replaced
                          << " donors=" << donors << " positive_jacobians=" << jacobians << '\n';
                if (hits == 0u)
                    throw std::runtime_error("GI position parity fixture must produce real hit receivers at every pose");
                // Pure specular SP-off fixtures can contain only unsupported
                // replacement chains, which production correctly excludes from
                // spatial reuse. Their mandatory reuse controls are SP-on.
                const bool expect_donors = planes || (!require_replaced && !require_jacobian);
                if (expect_donors && (receivers == 0u || donors == 0u))
                    throw std::runtime_error("GI position parity fixture must exercise its expected spatial-reuse domain");
                if (planes) { sp_replaced += replaced; sp_jacobians += jacobians; }
            }
        }
        if ((require_replaced && sp_replaced == 0u) || (require_jacobian && sp_jacobians == 0u))
            throw std::runtime_error("GI position parity fixture did not exercise required SP-on replacements or positive real-endpoint Jacobians");
    } catch (...) {
        restore();
        throw;
    }
    restore();
    std::cout << "PASS: produced ReSTIR receiver positions and GI Jacobians are bit-exact in both SP modes and camera motion: " << label << '\n';
}

// coverage bits: 1 positive area target, 2 all area targets zero,
// 4 positive non-area target, 8 grazing area target, 16 varying area UVs,
// 32 directional samples, 64 both primitive IDs.
// The one-argument entry keeps its API.
void check_di_light_context_equivalence(vision::Pipeline& pipeline, const char* label,
                                        unsigned coverage) {
    using namespace vision;
    pipeline.activate_global_context();
    auto integrator = std::dynamic_pointer_cast<IlluminationIntegrator>(pipeline.renderer().integrator().impl());
    if (!integrator) throw std::runtime_error("DI context oracle needs an illumination integrator");
    integrator->set_denoise_enabled(false);
    pipeline.frame_buffer()->set_enable_accumulation(false);
    pipeline.upload_data();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto& sampler = pipeline.renderer().sampler();
    auto& spectrum = pipeline.renderer().spectrum();
    auto& camera = pipeline.scene().sensor();
    auto& lights = pipeline.renderer().light_sampler();
    constexpr uint candidate_words = 32u;
    constexpr uint words = 10u * candidate_words + 16u;
    constexpr uint count = 64u;
    bool expects_area = false;
    lights->for_each([&](TLight light) { expects_area |= light->match(LightType::Area); });
    auto build = [&](bool reuse_context) {
        ParameterSet settings{DataWrap{{"spatial", DataWrap::object()}, {"temporal", DataWrap::object()}}};
        ReSTIRDI di{integrator, settings};
        Kernel kernel = [&](BufferVar<uint> output) {
            Env::instance().clear_global_vars();
            sampler->load_data(); camera->load_data();
            di.initial(sampler, Uint{0u}, spectrum);
            sampler->set_seed(dispatch_idx().xy(), Uint{31u}, Dimension::PathTracing);
            const auto& swl = di.sampled_wavelengths();
            const auto& geometry = pipeline.geometry();
            Uint base = dispatch_id() * words;
            $for(word, words) { output.write(base + word, 0u); };
            // Existing fixtures place their primary receiver at z=-1. Vary the
            // barycentrics across threads without consuming candidate RNG.
            Float2 offset = make_float2(cast<float>(dispatch_id() % 8u) - 3.5f,
                                       cast<float>(dispatch_id() / 8u) - 3.5f) * 0.02f;
            Float3 origin = camera->device_position();
            RayVar ray = make_ray(origin, normalize(make_float3(offset, Float{-1.f}) - origin));
            TriangleHitVar hit = geometry.trace_closest(ray);
            $if(hit->is_miss()) { $return(); };
            Interaction it = geometry.compute_surface_interaction(hit, ray);
            DIReservoirVar reservoir;
            Float selected_correction = 1.f;
            // Preserve one material build per candidate and its mutable shading
            // frame/wavelength effects. Kernels start from independent state.
            $for(candidate, 10u) {
                LightSurfacePoint lsp = lights->sample_only(it, sampler);
                LightEvalContext context;
                LightSample ls{swl.dimension()};
                if (reuse_context) {
                    ls = lights->evaluate_point(it, lsp, swl, LightEvalMode::All, &context);
                } else {
                    ls = lights->evaluate_point(it, lsp, swl);
                }
                Float3 wi = normalize(ls.p_light - it.pos);
                ScatterEval eval{swl.dimension(), 1u};
                pipeline.scene().materials().dispatch(it.material_id(), [&](const Material* material) {
                    auto bsdf = material->create_evaluator(it, swl);
                    swl.check_dispersive(spectrum, bsdf);
                    eval = bsdf.evaluate(it.wo, wi);
                });
                Float converted_pdf = reuse_context ? lights->PDF_point(it, lsp, eval.pdf(), &context)
                                                   : lights->PDF_point(it, lsp, eval.pdf());
                // Compare the actual context produced by the new evaluate call
                // with the old independently reconstructed AreaLight context.
                Uint kind = 0u;
                auto [type, instance] = lights->extract_light_id(lsp.light_index);
                lights->dispatch_light(type, instance, [&](const Light* light) {
                    kind = static_cast<uint>(light->type());
                    if (!reuse_context && light->match(LightType::Area)) {
                        context = light->compute_light_eval_context(it, lsp);
                    }
                });
                Float p_hat = luminance((eval.f * ls.eval.L).vec3());
                DISampleVar sample;
                sample->set_lsp(lsp); sample->set_pos(ls.p_light); sample.p_hat = p_hat;
                Bool delta = ls.eval.pdf < 0.f;
                Float light_pdf = ocarina::select(delta, -ls.eval.pdf, ls.eval.pdf);
                Float light_mass = 10.f * light_pdf;
                Float mis = ocarina::select(delta, 0.1f, light_pdf / (light_mass + converted_pdf));
                Float weight = DIReservoir::safe_weight(mis, p_hat, 1.f / light_pdf);
                Bool selected = reservoir->update(sampler->next_1d(), sample, weight);
                Float correction = 1.f;
                $if(!delta && light_mass > 0.f) { correction = (light_mass + converted_pdf) / light_mass; };
                selected_correction = ocarina::select(selected, correction, selected_correction);
                Uint start = base + candidate * candidate_words;
                auto put_float = [&](uint word, const Float& value) { output.write(start + word, as<uint>(value)); };
                output.write(start, 1u); output.write(start + 1u, kind);
                output.write(start + 2u, lsp.light_index); output.write(start + 3u, lsp.prim_id);
                put_float(4u, lsp.bary.x); put_float(5u, lsp.bary.y);
                for (uint component = 0u; component < 3u; ++component) {
                    put_float(6u + component, ls.p_light[component]);
                    put_float(9u + component, ls.eval.L.vec3()[component]);
                    put_float(12u + component, eval.f.vec3()[component]);
                }
                put_float(15u, ls.eval.pdf); put_float(16u, eval.pdf());
                put_float(17u, converted_pdf); put_float(18u, p_hat);
                put_float(19u, weight); put_float(20u, selected_correction);
                output.write(start + 21u, cast<uint>(selected));
                $if(kind == static_cast<uint>(LightType::Area)) {
                    for (uint component = 0u; component < 3u; ++component) {
                        put_float(22u + component, context.pos[component]);
                        put_float(25u + component, context.ng[component]);
                    }
                    put_float(28u, context.uv.x); put_float(29u, context.uv.y);
                    put_float(30u, context.PDF_pos);
                    put_float(31u, abs(dot(normalize(context.pos - it.pos), context.ng)));
                };
            };
            // The unaffected BSDF candidate must remain in the reservoir and
            // consume its original RNG/ray-query sequence (10+1 production RIS).
            DISampleVar bsdf_sample;
            BSDFSample bsdf_sample_result{swl.dimension(), 1u};
            Float bsdf_light_pdf = 0.f;
            HitBSDFVar hit_bsdf;
            Uint flag = 0u;
            Float bsdf_target = di.compute_p_hat(it, nullptr, &bsdf_sample, &bsdf_sample_result,
                                                &bsdf_light_pdf, &hit_bsdf, &flag);
            bsdf_sample.p_hat = bsdf_target;
            Float bsdf_weight = DIReservoir::safe_weight(
                bsdf_sample_result.eval.pdf() / (10.f * bsdf_light_pdf + bsdf_sample_result.eval.pdf()),
                bsdf_target, 1.f / bsdf_sample_result.eval.pdf());
            Bool bsdf_selected = reservoir->update(sampler->next_1d(), bsdf_sample, bsdf_weight);
            selected_correction = ocarina::select(bsdf_selected, 1.f, selected_correction);
            Bool occluded = false;
            $if(reservoir->valid() && selected_correction != 1.f) {
                occluded = geometry.occluded(it, reservoir.sample->p_light());
                reservoir.weight_sum *= ocarina::select(occluded, selected_correction, 1.f);
            };
            reservoir->update_W(reservoir.sample.p_hat); reservoir->truncation(1.f);
            Uint start = base + 10u * candidate_words;
            auto put_float = [&](uint word, const Float& value) { output.write(start + word, as<uint>(value)); };
            put_float(0u, reservoir.weight_sum); put_float(1u, reservoir.C); put_float(2u, reservoir.W);
            output.write(start + 3u, reservoir.sample.light_index);
            output.write(start + 4u, reservoir.sample.prim_id);
            put_float(5u, reservoir.sample.bary.x); put_float(6u, reservoir.sample.bary.y);
            output.write(start + 7u, reservoir.sample.age); put_float(8u, reservoir.sample.p_hat);
            for (uint component = 0u; component < 3u; ++component)
                put_float(9u + component, reservoir.sample->p_light()[component]);
            output.write(start + 12u, cast<uint>(occluded));
            put_float(13u, sampler->next_1d());
            put_float(14u, bsdf_light_pdf); put_float(15u, bsdf_target);
        };
        return pipeline.device().compile(kernel, reuse_context ? "di_light_context_cached" : "di_light_context_reference");
    };
    auto reference_shader = build(false);
    auto candidate_shader = build(true);
    auto buffer = pipeline.device().create_buffer<uint>(count * words, "di_light_context_results");
    std::vector<uint> reference(count * words), candidate(count * words);
    pipeline.stream() << reference_shader(buffer).dispatch(count) << buffer.download(reference.data())
                      << candidate_shader(buffer).dispatch(count) << buffer.download(candidate.data())
                      << synchronize() << commit();
    uint active = 0u, area = 0u, non_area = 0u;
    uint area_positive = 0u, area_zero = 0u, non_area_positive = 0u;
    uint grazing = 0u, directional = 0u;
    bool have_area_uv = false, varied_area_uv = false;
    uint first_u = 0u, first_v = 0u, first_primitive = 0u;
    bool varied_primitive = false;
    for (uint thread = 0u; thread < count; ++thread) {
        for (uint sample = 0u; sample < 10u; ++sample) {
            const uint start = thread * words + sample * candidate_words;
            if (reference[start]) {
                ++active;
                const uint type = reference[start + 1u];
                const uint target = reference[start + 18u];
                const bool finite = (target & 0x7f800000u) != 0x7f800000u;
                if (!finite) throw std::runtime_error("DI context candidate target must be finite");
                const bool positive = (target & 0x80000000u) == 0u && (target & 0x7fffffffu) != 0u;
                if (type == static_cast<uint>(LightType::Area)) {
                    ++area; area_positive += positive; area_zero += (target & 0x7fffffffu) == 0u;
                    const uint cosine = reference[start + 31u];
                    // abs(cos) is nonnegative; float uint ordering is monotone.
                    grazing += positive && cosine < 0x3c23d70au; // 0.01f
                    if (!have_area_uv) {
                        first_u = reference[start + 28u]; first_v = reference[start + 29u];
                        first_primitive = reference[start + 3u];
                        have_area_uv = true;
                    } else {
                        varied_area_uv |= first_u != reference[start + 28u] || first_v != reference[start + 29u];
                        varied_primitive |= first_primitive != reference[start + 3u];
                    }
                } else {
                    ++non_area; non_area_positive += positive;
                    directional += (type & static_cast<uint>(LightType::DeltaDirection)) != 0u;
                }
            }
        }
    }
    if (active == 0u) throw std::runtime_error("DI light context oracle missed every fixture receiver");
    if (expects_area && area == 0u) throw std::runtime_error("DI context oracle did not sample an AreaLight fixture");
    for (uint word = 0u; word < count * words; ++word) {
        if (reference[word] != candidate[word]) {
            throw std::runtime_error("DI light context mismatch at uint word " + std::to_string(word) +
                                     " old=" + std::to_string(reference[word]) + " new=" + std::to_string(candidate[word]));
        }
    }
    if ((coverage & 1u) && area_positive == 0u)
        throw std::runtime_error("DI context fixture needs positive area contributions");
    if ((coverage & 2u) && (area_zero == 0u || area_positive != 0u))
        throw std::runtime_error("DI context fixture needs exclusively zero area contributions");
    if ((coverage & 4u) && non_area_positive == 0u)
        throw std::runtime_error("DI context fixture needs positive non-area contributions");
    if ((coverage & 8u) && grazing == 0u)
        throw std::runtime_error("DI context fixture needs a positive grazing area target");
    if ((coverage & 16u) && !varied_area_uv)
        throw std::runtime_error("DI context texture fixture needs varying sampled UVs");
    if ((coverage & 64u) && !varied_primitive)
        throw std::runtime_error("DI context unequal-area fixture must sample both primitive IDs");
    if ((coverage & 32u) && directional == 0u)
        throw std::runtime_error("DI context control must sample directional lights");
    std::cout << "PASS: DI light context " << label << " candidate/reservoir/RNG bit parity; area=" << area
              << " non_area=" << non_area << " area_positive=" << area_positive << " area_zero=" << area_zero
              << " non_area_positive=" << non_area_positive << " grazing=" << grazing
              << " varied_uv=" << varied_area_uv << " varied_primitive=" << varied_primitive << " directional=" << directional << '\n';
}

void check_di_light_context_equivalence(vision::Pipeline& pipeline) {
    check_di_light_context_equivalence(pipeline, "existing-fixture", 0u);
}
