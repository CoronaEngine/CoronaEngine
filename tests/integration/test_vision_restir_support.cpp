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
