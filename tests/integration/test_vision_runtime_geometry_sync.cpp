#include <corona/systems/optics/optics_system.h>
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "vision/vision_geometry_snapshot.h"
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace Corona::Systems {
struct VisionRuntimeGeometrySyncTest {
    static void expect(bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    }
    static void render(vision::Pipeline& pipeline, const char* label, bool lit = true) {
        const auto before = pipeline.frame_index();
        for (int i = 0; i < 3; ++i) pipeline.display(1.0 / 60.0);
        expect(pipeline.frame_index() > before, "real integrator must advance frames");
        std::vector<vision::float4> pixels(pipeline.pixel_num());
        pipeline.final_picture(pipeline.output_desc(), pixels.data());
        float sum = 0;
        for (const auto& pixel : pixels) {
            expect(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z),
                   "GPU output must be finite");
            sum += pixel.x + pixel.y + pixel.z;
        }
        expect(lit ? sum > 0.f : sum == 0.f, "GPU output must match geometry visibility");
        std::cout << "GPU frames: " << label << " frame=" << pipeline.frame_index()
                  << " rgb_sum=" << sum << '\n';
    }
    static void run(std::string scenario) {
        namespace fs = std::filesystem;
        const auto original_cwd = fs::current_path();
        const auto base = original_cwd / ("runtime-geometry-sync-test-assets-" + scenario);
        fs::create_directories(base);
        std::ofstream(base / "triangle.obj") << "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n";
        const unsigned char white_tga[] = {0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,24,0,255,255,255};
        std::ofstream(base / "albedo.tga", std::ios::binary).write(
            reinterpret_cast<const char*>(white_tga), sizeof(white_tga));
        ocarina::RHIContext::instance().init(original_cwd);
        // Keep the device alive until process exit: Vision's global kernel tools own GPU state.
        static auto device = ocarina::RHIContext::instance().create_device("cuda");
        device.init_rtx();
        vision::Global::instance().set_device(&device);
        OpticsSystem system;
        OpticsSystem::VisionSceneLoadRequest request;
        request.scene_key = (base / "memory-only.embedded").string();
        request.base_dir = base.filename().string();
        request.external_live = true;
        request.scene_json = R"({
          "scene": {
            "camera":{"type":"thin_lens","param":{"transform":{"type":"look_at","param":{"position":[0,0,3],"target_pos":[0,0,0],"up":[0,1,0]}}}},
            "materials":[{"type":"diffuse","name":"mat","param":{"color":{"channels":"xyz","node":{"type":"image","param":{"fn":"albedo.tga"}}}}}],"shapes":[{"type":"model","param":{"fn":"triangle.obj","material":"mat"}}],"lights":[{"type":"point","param":{"position":[0,0,2]}}]
          },
          "render":{"sampler":{"type":"independent","param":{"spp":1}},"integrator":{"type":"pt","param":{"max_depth":2}},"light_sampler":{"type":"uniform"}},
          "pipeline":{"type":"fixed","param":{"frame_buffer":{"type":"normal","param":{"resolution":[16,16]}}}},
          "output":{"spp":1,"denoise":false}
        })";
        if (scenario == "lights") {
            auto data = vision::DataWrap::parse(request.scene_json);
            data["scene"]["shapes"][0]["param"]["emission"] = vision::DataWrap::parse(R"({"type":"area","param":{}})");
            request.scene_json = data.dump();
        }
        expect(system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::PathTracing), "PT import");
        auto pt = vision::Global::instance().pipeline_shared();
        const auto pk = system.make_vision_pipeline_key(request.scene_key, CameraVisionRenderMode::PathTracing, Vision::VisionPipelineSource::ExternalLive);
        auto* pr = system.ensure_external_vision_runtime(pk);
        const auto sk = system.make_vision_pipeline_key(request.scene_key, CameraVisionRenderMode::SVGF, Vision::VisionPipelineSource::ExternalLive);
        auto* sr = system.ensure_external_vision_runtime(sk);
        expect(sr != nullptr, "SVGF import");
        auto svgf = vision::Global::instance().pipeline_shared();
        expect(pt->shared_scene_data() != svgf->shared_scene_data(), "independent CPU scenes");
        auto* pt_framebuffer = pt->frame_buffer(); auto* svgf_framebuffer = svgf->frame_buffer();
        expect(pt_framebuffer != svgf_framebuffer, "independent framebuffer state");
        expect(pt->geometry().gpu_resource() != svgf->geometry().gpu_resource(), "independent GPU geometry");
        auto& hub = SharedDataHub::instance();
        auto th = hub.model_transform_storage().allocate();
        auto gh = hub.geometry_storage().allocate();
        auto ph = hub.profile_storage().allocate();
        auto ah = hub.actor_storage().allocate();
        auto sh = hub.scene_storage().allocate();
        { auto v = hub.geometry_storage().acquire_write(gh); v->transform_handle = th; }
        { auto v = hub.profile_storage().acquire_write(ph); v->geometry_handle = gh; }
        { auto v = hub.actor_storage().acquire_write(ah); v->profile_handles.push_back(ph); }
        { auto v = hub.scene_storage().acquire_write(sh); v->actor_handles.push_back(ah); }
        ExternalVisionBindingDevice binding;
        binding.enabled = true; binding.source_path = request.scene_key;
        binding.shape_index = 0; binding.shape_guid = "triangle-guid";
        binding.shape_identity_key = "triangle-identity";
        hub.set_external_vision_binding(ah, binding);
        system.resident_actors_.insert(ah);
        system.vision_initialized_ = true;
        system.sync_external_live_vision_transforms(*pr);
        system.sync_shared_vision_scene(*sr);
        auto probe = [&](vision::Pipeline& pipeline, float x = 0.f) {
            using namespace vision;
            pipeline.activate_global_context();
            Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
            vision::Kernel kernel = [&](BufferVar<float4> out) {
                auto ray = make_ray(Float3{make_float3(x, 0.f, 3.f)}, Float3{make_float3(0.f, 0.f, -1.f)});
                auto hit = pipeline.geometry().trace_closest(ray);
                out.write(0u, make_float4(0.f, 0.f, -100.f, 0.f));
                $if(!hit->is_miss()) {
                    auto pos = pipeline.geometry().compute_surface_interaction(hit, false).pos;
                    out.write(0u, make_float4(pos, 1.f));
                };
            };
            auto shader = pipeline.device().compile(kernel, "runtime_geometry_probe");
            auto out = pipeline.device().create_buffer<float4>(1, "runtime_geometry_result");
            float4 result{};
            pipeline.stream() << shader(out).dispatch(1u) << out.download(&result) << synchronize() << commit();
            return result.z;
        };
        expect(std::abs(probe(*pt)) < 1e-4f && std::abs(probe(*svgf)) < 1e-4f, "initial CUDA hits at z=0");
        auto mesh = pt->scene().instances()[0]->mesh();
        auto* address = mesh->vertices().data();
        for (auto& v : mesh->vertices()) v.pos[2] = 1.f;
        expect(address == mesh->vertices().data(), "edit retains address and count");
        system.sync_external_live_vision_transforms(*pr);
        system.sync_shared_vision_scene(*sr);
        const auto pz = probe(*pt), sz = probe(*svgf);
        const float cpu_z = svgf->scene().instances()[0]->mesh()->vertices()[0].pos[2];
        std::cout << "EDIT cpu_second_z=" << cpu_z << " GPU_PT_z=" << pz << " GPU_SVGF_z=" << sz << " expected=1" << std::endl;
        expect(std::abs(pz - 1.f) < 1e-4f, "producer CUDA geometry updates");
        expect(cpu_z == 1.f && std::abs(sz - 1.f) < 1e-4f, "consumer CPU and CUDA must receive in-place vertices");
        auto resource = system.vision_scene_resources_.at(system.make_vision_scene_resource_key(request.scene_key, Vision::VisionPipelineSource::ExternalLive));
        auto gpu_pt = pt->geometry().gpu_resource();
        auto gpu_svgf = svgf->geometry().gpu_resource();
        if (scenario == "eviction") {
            auto record = *resource->find_logical_instance({0,0});
            record.object_to_world[12] = 4.f;
            resource->upsert_logical_instance(record); resource->mark_transforms_changed();
            expect(system.sync_shared_vision_scene(*pr) && system.sync_shared_vision_scene(*sr), "latest transform before eviction");
            std::weak_ptr<vision::GeometryGpuResource> retired_pt=gpu_pt, retired_svgf=gpu_svgf;
            pt.reset(); svgf.reset(); gpu_pt.reset(); gpu_svgf.reset();
            system.activate_single_vision_runtime_key(system.make_vision_pipeline_key("",CameraVisionRenderMode::PathTracing,Vision::VisionPipelineSource::EngineBuilt));
            expect(retired_pt.expired() && retired_svgf.expired(),"last runtime retirement releases both GPU resources");
            expect(resource->geometry_snapshot != nullptr,"CPU snapshot survives retirement");
            sr=system.ensure_external_vision_runtime(sk);
            expect(sr != nullptr,"recreate from retained CPU geometry");
            svgf=vision::Global::instance().pipeline_shared();
            std::cout << "eviction restored x=" << svgf->scene().instances()[0]->o2w()[3][0] << std::endl;
            expect(svgf->scene().instances()[0]->o2w()[3][0] == 4.f,"eviction must retain transforms newer than geometry snapshot");
            expect(std::abs(probe(*svgf,4.f)-1.f)<1e-4f,"eviction restores latest edited GPU vertices and transform");
            system.clear_vision_runtimes(); return;
        }
        if (scenario == "material" || scenario == "topology") {
            render(*svgf, "SVGF-before-material-change");
            svgf->set_output_denoise(false);
            pt->activate_global_context();
            vision::MaterialDesc desc;
            desc.init(vision::ParameterSet{vision::DataWrap::parse(R"({"type":"diffuse","name":"new-runtime-material","param":{"color":[0.2,0.5,0.8]}})")});
            if (scenario == "topology") desc.sub_type = "principled_bsdf";
            auto material = vision::Material::create_root(desc);
            pt->scene().add_material(material);
            pt->scene().instances()[0]->set_material(material);
            pt->scene().prepare();
            for (auto& v : mesh->vertices()) v.pos[2] = 1.5f;
            system.sync_external_live_vision_transforms(*pr);
            expect(system.sync_shared_vision_scene(*sr), "new material topology must import into independent runtime");
            expect(svgf->scene().instances()[0]->material()->hash() == material->hash(), "new material mapping");
            expect(std::abs(probe(*svgf)-1.5f)<1e-4f,"new material geometry CUDA hit");
            svgf->set_output_denoise(true);
            render(*svgf,"new-material-SVGF");
            system.clear_vision_runtimes(); return;
        }
        if (scenario == "lights") {
            auto emitter = pt->scene().instances()[0];
            auto reordered = std::make_shared<vision::ShapeGroup>();
            reordered->geometry_sync_identity = pt->scene().groups()[0]->geometry_sync_identity;
            vision::ShapeInstance diffuse{emitter->mesh()};
            auto transform = vision::make_float4x4(1.f); transform[3][0] = 4.f;
            diffuse.set_o2w(transform);
            reordered->add_instance(std::move(diffuse));
            reordered->instance(0).set_material(emitter->material());
            reordered->add_instance(*emitter);
            pt->scene().groups()[0] = reordered;
            system.sync_external_live_vision_transforms(*pr);
            expect(pt->scene().instances()[1]->emission()->instance()->geometry_sync_identity == emitter->geometry_sync_identity,
                   "producer area light must follow stable instance identity after reorder");
            expect(system.sync_shared_vision_scene(*sr), "emissive consumer");
            expect(svgf->scene().instances()[1]->emission()->instance()->geometry_sync_identity == emitter->geometry_sync_identity,
                   "consumer area light must follow stable instance identity after reorder");
            // Removing a light after a long stationary interval must not retain
            // its old radiance in the post-denoise coverage history.
            svgf->activate_global_context();
            for (int i = 0; i < 320; ++i) svgf->display(1.0 / 60.0);
            render(*svgf, "emitter-before-removal-SVGF");
            auto reduced = std::make_shared<vision::ShapeGroup>(reordered->instance(0));
            reduced->geometry_sync_identity = reordered->geometry_sync_identity;
            pt->scene().groups()[0] = reduced;
            system.sync_external_live_vision_transforms(*pr);
            expect(system.sync_shared_vision_scene(*sr), "emitter deletion consumer");
            for (auto* pipeline : {pt.get(),svgf.get()}) {
                for (const auto& light : pipeline->scene().light_manager().lights())
                    expect(!light->match(vision::LightType::Area), "removed emitter must not remain in light registry");
                expect(std::abs(probe(*pipeline,4.f)-1.f)<1e-4f,"remaining diffuse instance GPU hit");
            }
            render(*svgf,"emitter-removed-SVGF",false);
            system.clear_vision_runtimes(); return;
        }
        if (scenario == "stale") {
            auto invalid = std::make_shared<Vision::VisionGeometrySnapshot>(*resource->geometry_snapshot);
            invalid->meshes[0].triangles[0].i=999999;
            resource->geometry_snapshot=invalid; ++resource->geometry_version;
            const auto version=resource->geometry_version;
            resource->engine_mixed_shapes_by_actor.emplace(999,Vision::EngineMixedShapeRecord{999,0,0});
            system.sync_external_live_vision_transforms(*sr);
            system.sync_engine_native_mixed_shapes(*sr);
            expect(resource->geometry_snapshot==invalid && resource->geometry_version==version,
                   "failed consumer cannot reverse publish through pending mixed removal");
            system.clear_vision_runtimes(); return;
        }
        auto unchanged = [&] {
            auto snapshot = resource->geometry_snapshot;
            auto version = resource->geometry_version;
            auto a = gpu_pt->build_count(), b = gpu_svgf->build_count();
            for (int i = 0; i < 5; ++i) {
                system.sync_external_live_vision_transforms(*pr);
                expect(system.sync_shared_vision_scene(*sr), "steady consumer");
            }
            expect(snapshot == resource->geometry_snapshot && version == resource->geometry_version,
                   "steady sync must not copy or publish geometry");
            expect(a == gpu_pt->build_count() && b == gpu_svgf->build_count(), "steady sync must not build BLAS");
        };
        auto sync_edit = [&](const char* label, float expected) {
            auto version = resource->geometry_version;
            auto a = gpu_pt->build_count(), b = gpu_svgf->build_count();
            system.sync_external_live_vision_transforms(*pr);
            expect(resource->geometry_version == version + 1, "edit publishes exactly once");
            expect(system.sync_shared_vision_scene(*sr), "consume geometry");
            expect(a + 1 == gpu_pt->build_count() && b + 1 == gpu_svgf->build_count(), "one BLAS build per runtime per edit");
            float p = probe(*pt), s = probe(*svgf);
            std::cout << label << " GPU_PT_z=" << p << " GPU_SVGF_z=" << s << " expected=" << expected << std::endl;
            expect(std::abs(p-expected)<1e-4f && std::abs(s-expected)<1e-4f, "both GPU hits match changed geometry");
            for (auto* pipeline : {pt.get(), svgf.get()}) {
                expect(std::abs(pipeline->scene().groups()[0]->aabb.center().z - expected) < 1e-4f &&
                       std::abs(pipeline->scene().world_center().z - expected) < 1e-4f,
                       "group and scene bounds follow edited triangle plane");
            }
            expect(pt->frame_buffer() == pt_framebuffer && svgf->frame_buffer() == svgf_framebuffer, "geometry consumption retains framebuffer histories");
            unchanged();
        };
        unchanged();
        if (scenario == "fresh") {
        // A newly consumed mode must detect edits before its first producer pass.
        for (auto& v : svgf->scene().instances()[0]->mesh()->vertices()) v.pos[2] = 0.75f;
        system.sync_external_live_vision_transforms(*sr);
        expect(system.sync_shared_vision_scene(*pr), "fresh producer publication");
        std::cout << "fresh-consumer-edit GPU=" << probe(*svgf) << " expected=0.75" << std::endl;
        expect(std::abs(probe(*svgf)-0.75f)<1e-4f && std::abs(probe(*pt)-0.75f)<1e-4f,
               "fresh consumer edit must rebuild and publish before first producer pass");
        mesh = pt->scene().instances()[0]->mesh();
        for (auto& v : mesh->vertices()) v.pos[2] = 1.f;
        sync_edit("restore-after-fresh-edit",1.f);
            system.clear_vision_runtimes(); return;
        }
        pt->activate_global_context();
        auto camera_position = pt->scene().sensor()->position();
        pt->scene().sensor()->move(vision::make_float3(0.1f,0.f,0.f));
        pt->scene().sensor()->update_device_data(); unchanged();
        pt->scene().sensor()->set_position(camera_position);
        pt->scene().sensor()->update_device_data(); unchanged();
        // Replace the mesh, then switch its triangle to a second plane without
        // changing either vector's address or size.
        auto vertices = mesh->vertices();
        for (auto v : mesh->vertices()) { v.pos[2] = 2.f; vertices.push_back(v); }
        auto replacement = std::make_shared<vision::Mesh>(vertices, mesh->triangles());
        pt->scene().groups()[0]->instance(0).set_mesh(replacement);
        sync_edit("mesh-replacement", 1.f);
        auto* triangle_address = replacement->triangles().data();
        replacement->triangles()[0] = vision::Triangle{3,4,5};
        expect(triangle_address == replacement->triangles().data(), "same-address index edit");
        sync_edit("index-edit", 2.f);
        const auto original_id = pt->scene().groups()[0]->instance(0).geometry_sync_identity;
        vision::ShapeInstance extra{replacement};

        auto moved = vision::make_float4x4(1.f); moved[3][0] = 4.f;
        extra.set_o2w(moved);
        pt->scene().groups()[0]->add_instance(std::move(extra));
        pt->scene().groups()[0]->instance(1).set_material(pt->scene().instances()[0]->material());
        sync_edit("instance-add", 2.f);
        expect(std::abs(probe(*svgf,4.f)-2.f)<1e-4f, "added instance GPU position");
        auto old = pt->scene().groups()[0];
        auto reordered = std::make_shared<vision::ShapeGroup>();
        reordered->geometry_sync_identity = old->geometry_sync_identity;
        reordered->add_instance(old->instance(1)); reordered->add_instance(old->instance(0));
        pt->scene().groups()[0] = reordered;
        sync_edit("instance-reorder", 2.f);
        expect(svgf->scene().groups()[0]->instance(1).geometry_sync_identity == original_id,
               "instance identity survives reordered snapshot");
        auto reduced = std::make_shared<vision::ShapeGroup>(reordered->instance(1));
        reduced->geometry_sync_identity = reordered->geometry_sync_identity;
        pt->scene().groups()[0] = reduced;
        sync_edit("instance-remove", 2.f);
        expect(probe(*svgf,4.f) == -100.f, "deleted instance must miss on GPU");
        binding.visible = false; hub.set_external_vision_binding(ah,binding);
        system.sync_external_live_vision_transforms(*pr);
        expect(system.sync_shared_vision_scene(*sr), "hidden consumer");
        expect(probe(*pt) == -100.f && probe(*svgf) == -100.f, "hidden geometry must miss both GPUs");
        binding.visible = true; hub.set_external_vision_binding(ah,binding);
        system.sync_external_live_vision_transforms(*pr);
        expect(system.sync_shared_vision_scene(*sr), "restored consumer");
        expect(std::abs(probe(*svgf)-2.f)<1e-4f, "restored geometry CUDA hit");
        // Let the second runtime sleep through two versions; it consumes only latest.
        auto sleeping_builds = gpu_svgf->build_count();
        mesh = pt->scene().instances()[0]->mesh();
        for (auto& v : mesh->vertices()) v.pos[2] = 1.5f;
        system.sync_external_live_vision_transforms(*pr);
        for (auto& v : mesh->vertices()) v.pos[2] = 1.25f;
        system.sync_external_live_vision_transforms(*pr);
        expect(system.sync_shared_vision_scene(*sr), "wake latest geometry");
        expect(gpu_svgf->build_count() == sleeping_builds + 1 && std::abs(probe(*svgf)-1.25f)<1e-4f,
               "sleeping runtime consumes latest snapshot once");
        // Inject an invalid import payload. Failure must leave GPU and version
        // unconsumed; correcting the SAME version must cause a real upload.
        auto good = resource->geometry_snapshot;
        auto invalid = std::make_shared<Vision::VisionGeometrySnapshot>(*good);
        invalid->meshes[0].triangles[0].i = 999999;
        resource->geometry_snapshot = invalid; ++resource->geometry_version;
        auto before_failed_build = gpu_svgf->build_count();
        expect(!system.sync_shared_vision_scene(*sr), "invalid snapshot import must fail");
        expect(gpu_svgf->build_count() == before_failed_build && std::abs(probe(*svgf)-1.25f)<1e-4f,
               "failed staged import preserves usable old GPU geometry");
        resource->geometry_snapshot = good;
        expect(system.sync_shared_vision_scene(*sr), "retry same unconsumed version");
        expect(gpu_svgf->build_count() == before_failed_build + 1, "failed version must not have been consumed early");
        expect(system.sync_shared_vision_scene(*pr), "producer consumes repaired publication");
        auto version_before_switch = resource->geometry_version;
        system.sync_external_live_vision_transforms(*sr);
        expect(resource->geometry_version == version_before_switch && std::abs(probe(*svgf)-1.25f)<1e-4f,
               "consumer becoming producer must not republish old geometry");
        render(*pt, "PT"); render(*svgf, "SVGF");
        std::cout << "actual_runtimes=" << system.vision_runtimes_.size() << std::endl;
        expect(system.vision_runtimes_.size() == 2, "two actual rendering runtimes");
        const auto old_source = resource->source_revision;
        expect(system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::PathTracing,true), "source reload");
        pt = vision::Global::instance().pipeline_shared();
        pr = system.ensure_external_vision_runtime(pk); sr = system.ensure_external_vision_runtime(sk);
        svgf = vision::Global::instance().pipeline_shared();
        expect(resource->source_revision == old_source + 1 && resource->geometry_snapshot->source_revision == resource->source_revision,
               "source reload retires old snapshot namespace");
        expect(std::abs(probe(*pt))<1e-4f && std::abs(probe(*svgf))<1e-4f, "reload both GPU scenes from new source");
        system.clear_vision_runtimes();
        std::cout << "PASS: production runtime geometry sync\n";
    }
};
}
int main(int argc, char** argv) {
    try { Corona::Systems::VisionRuntimeGeometrySyncTest::run(argc > 1 ? argv[1] : "base"); }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
