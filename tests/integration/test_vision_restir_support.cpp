// Exercise the real material and ReSTIR GI GPU methods on a receiver whose
// shading normal tilts across its geometric hemisphere. A positive BSDF value
// with a rejected (zero-PDF) direction must not become reusable GI energy.
#define VISION_PLUGIN_NAME "ReSTIR"
#define VISION_CATEGORY "integrator"
#include "render_core/integrator/ReSTIR/indirect.h"
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
