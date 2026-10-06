#include <corona/systems/optics/optics_system.h>
#include <corona/engine/engine_runtime_api.h>
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/switch_profile.h"
#include "base/sensor/sensor.h"
#include "vision/vision_camera_adapter.h"
#include "vision/vision_external_live_aabb.h"
#include <filesystem>
#include <fstream>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <iomanip>
#include <sstream>
#include <chrono>

void check_camera_dolly_reprojection(vision::Pipeline& pipeline);
void check_svgf_restir_motion_history(vision::Pipeline& pipeline);
void check_motion_visibility_history(vision::Pipeline& pipeline);
void check_substrate_sample_classification(vision::Pipeline& pipeline);
void check_svgf_shading_guide(vision::Pipeline& pipeline);
void check_svgf_total_albedo(vision::Pipeline& pipeline);
void check_svgf_spatial_bypass(vision::Pipeline& pipeline);

namespace Corona::Systems {
struct VisionEmbeddedModeSwitchTest {
    static void expect(bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    }
    static void check_mode_api() {
        auto& hub = SharedDataHub::instance();
        const auto handle = hub.camera_storage().allocate();
        expect(!Corona::API::get_vision_denoise(handle),
               "new camera denoise must default to disabled");
        expect(!Corona::API::get_requested_vision_denoise(handle) &&
                   !hub.requested_camera_vision_denoise(handle).has_value(),
               "new cameras must have no retained denoise request");
        Corona::API::set_vision_render_mode("ReSTIR", handle);
        auto updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].camera_handle == handle &&
                   updates[0].vision_render_mode == CameraVisionRenderMode::ReSTIR &&
                   updates[0].fields == CameraStateUpdateField::VisionRenderMode,
               "ReSTIR API must enqueue the requested enum rather than falling back to PT");
        {
            auto camera = hub.camera_storage().acquire_write(handle);
            camera->vision_render_mode = updates[0].vision_render_mode;
        }
        expect(Corona::API::get_vision_render_mode(handle) == "restir",
               "camera snapshots must serialize ReSTIR for persistence and UI");

        Corona::API::set_vision_denoise(true, handle);
        expect(!Corona::API::get_vision_denoise(handle),
               "denoise setter must queue the update instead of mutating the camera cross-thread");
        expect(Corona::API::get_requested_vision_denoise(handle),
               "saving immediately after a setter must observe the requested value");
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].camera_handle == handle &&
                   updates[0].fields == CameraStateUpdateField::VisionDenoise &&
                   updates[0].vision_denoise,
               "denoise setter must enqueue only its independent field");
        expect(Corona::API::get_requested_vision_denoise(handle),
               "saving while a drained update is in flight must retain the requested value");
        {
            auto camera = hub.camera_storage().acquire_write(handle);
            camera->vision_denoise = updates[0].vision_denoise;
        }
        hub.acknowledge_camera_vision_denoise(handle, updates[0].sequence);
        expect(!hub.requested_camera_vision_denoise(handle).has_value() &&
                   Corona::API::get_requested_vision_denoise(handle),
               "acknowledged requests must be erased and persistence must fall back to committed state");
        expect(Corona::API::get_vision_denoise(handle) &&
                   Corona::API::get_vision_render_mode(handle) == "restir",
               "committing denoise must preserve the selected algorithm");

        for (const auto* mode : {"path_tracing", "restir", "ssat"}) {
            Corona::API::set_vision_render_mode(mode, handle);
            updates = hub.drain_camera_state_updates();
            expect(updates.size() == 1 &&
                       updates[0].fields == CameraStateUpdateField::VisionRenderMode &&
                       Corona::API::get_vision_denoise(handle),
                   "ordinary algorithm switches must not enqueue or reset the denoise preference");
        }

        Corona::API::set_vision_denoise(true, handle);
        Corona::API::set_vision_render_mode("path_tracing", handle);
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].vision_denoise &&
                   updates[0].vision_render_mode == CameraVisionRenderMode::PathTracing &&
                   updates[0].fields == (CameraStateUpdateField::VisionRenderMode |
                                         CameraStateUpdateField::VisionDenoise),
               "merging a later algorithm command must retain pending denoise");
        hub.acknowledge_camera_vision_denoise(handle, updates[0].sequence);
        expect(!hub.requested_camera_vision_denoise(handle).has_value(),
               "a merged command sequence must acknowledge its earlier denoise field");

        Corona::API::set_vision_render_mode("restir", handle);
        Corona::API::set_vision_denoise(true, handle);
        Corona::API::set_vision_denoise(false, handle);
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && !updates[0].vision_denoise &&
                   updates[0].vision_render_mode == CameraVisionRenderMode::ReSTIR &&
                   updates[0].fields == (CameraStateUpdateField::VisionRenderMode |
                                         CameraStateUpdateField::VisionDenoise),
               "latest denoise value must win without losing a pending algorithm switch");
        {
            auto camera = hub.camera_storage().acquire_write(handle);
            camera->vision_denoise = updates[0].vision_denoise;
        }
        expect(!Corona::API::get_vision_denoise(handle),
               "committed explicit false must remain false");

        Corona::API::set_vision_render_mode("VISION-SVGF", handle);
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].vision_denoise &&
                   updates[0].vision_render_mode == CameraVisionRenderMode::PathTracing &&
                   updates[0].fields == (CameraStateUpdateField::VisionRenderMode |
                                         CameraStateUpdateField::VisionDenoise),
               "legacy SVGF must atomically enqueue path tracing with denoise enabled");
        Corona::API::set_vision_render_mode("svgf", handle);
        Corona::API::set_vision_denoise(false, handle);
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && !updates[0].vision_denoise &&
                   updates[0].vision_render_mode == CameraVisionRenderMode::PathTracing &&
                   updates[0].fields == (CameraStateUpdateField::VisionRenderMode |
                                         CameraStateUpdateField::VisionDenoise),
               "explicit persisted false must override the legacy SVGF default");
        expect(hub.requested_camera_vision_denoise(handle).has_value() &&
                   !Corona::API::get_requested_vision_denoise(handle),
               "an in-flight false request must remain a present false preference");

        Corona::API::set_vision_denoise(true, handle);
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1, "expected one in-flight denoise request");
        const auto in_flight = updates[0];
        Corona::API::set_vision_denoise(false, handle);
        {
            auto camera = hub.camera_storage().acquire_write(handle);
            camera->vision_denoise = in_flight.vision_denoise;
        }
        hub.acknowledge_camera_vision_denoise(handle, in_flight.sequence);
        expect(Corona::API::get_vision_denoise(handle) &&
                   hub.requested_camera_vision_denoise(handle).has_value() &&
                   !Corona::API::get_requested_vision_denoise(handle),
               "acknowledging an older in-flight request must not erase a newer queued false");
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && !updates[0].vision_denoise,
               "newer false must remain queued after the old request is acknowledged");
        hub.acknowledge_camera_vision_denoise(handle, in_flight.sequence);
        expect(!Corona::API::get_requested_vision_denoise(handle),
               "a repeated old acknowledgement must not erase a newer in-flight request");
        {
            auto camera = hub.camera_storage().acquire_write(handle);
            camera->vision_denoise = updates[0].vision_denoise;
        }
        hub.acknowledge_camera_vision_denoise(handle, updates[0].sequence);
        expect(!hub.requested_camera_vision_denoise(handle).has_value() &&
                   !Corona::API::get_requested_vision_denoise(handle),
               "latest acknowledgement must remove transient state and expose committed false");
        {
            auto camera = hub.camera_storage().acquire_write(handle);
            camera->vision_denoise = true;
        }
        expect(Corona::API::get_requested_vision_denoise(handle),
               "acknowledged preference must not permanently shadow later committed camera changes");

        Corona::API::set_vision_denoise(false, handle);
        updates = hub.drain_camera_state_updates();
        Corona::API::set_vision_denoise(true, handle);
        hub.enqueue_camera_release({handle});
        expect(!hub.requested_camera_vision_denoise(handle).has_value() &&
                   hub.drain_camera_state_updates().empty(),
               "camera release must clear both queued and in-flight denoise requests");
        const auto releases = hub.drain_camera_releases();
        expect(releases.size() == 1 && releases[0].camera_handle == handle,
               "release cleanup must preserve the render-thread release command");
        hub.camera_storage().deallocate(handle);
    }
    static void check_accumulation_api() {
        auto& hub = SharedDataHub::instance();
        const auto handle = hub.camera_storage().allocate();
        expect(!Corona::API::get_vision_accumulation(handle), "new cameras must default to no accumulation");
        Corona::API::set_vision_render_mode("progressive_path_tracing", handle);
        auto updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].vision_accumulation &&
                   updates[0].vision_render_mode == CameraVisionRenderMode::PathTracing &&
                   updates[0].fields == (CameraStateUpdateField::VisionRenderMode |
                                         CameraStateUpdateField::VisionAccumulation),
               "legacy progressive PT must migrate to PT with a separate accumulation preference");
        expect(Corona::API::get_requested_vision_accumulation(handle) &&
                   !Corona::API::get_vision_accumulation(handle),
               "save must see the accepted accumulation request before render-thread commit");
        const auto old_sequence = updates[0].sequence;
        Corona::API::set_vision_accumulation(false, handle);
        hub.acknowledge_camera_vision_accumulation(handle, old_sequence);
        expect(hub.requested_camera_vision_accumulation(handle).has_value() &&
                   !Corona::API::get_requested_vision_accumulation(handle),
               "a stale ACK must not erase a newer explicit false");
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && !updates[0].vision_accumulation &&
                   updates[0].fields == CameraStateUpdateField::VisionAccumulation,
               "the accumulation setter must change only its own field");
        hub.acknowledge_camera_vision_accumulation(handle, updates[0].sequence);
        expect(!hub.requested_camera_vision_accumulation(handle).has_value(),
               "committed accumulation requests must retire");

        for (const auto* mode : {"path_tracing", "restir"}) {
            Corona::API::set_vision_accumulation(true, handle);
            Corona::API::set_vision_denoise(true, handle);
            Corona::API::set_vision_render_mode(mode, handle);
            updates = hub.drain_camera_state_updates();
            expect(updates.size() == 1 && updates[0].vision_accumulation && updates[0].vision_denoise &&
                       updates[0].fields == (CameraStateUpdateField::VisionRenderMode |
                                             CameraStateUpdateField::VisionAccumulation |
                                             CameraStateUpdateField::VisionDenoise),
                   "mode and both checkboxes must merge without replacing each other");
            {
                auto camera = hub.camera_storage().acquire_write(handle);
                camera->vision_render_mode = updates[0].vision_render_mode;
                camera->vision_accumulation = updates[0].vision_accumulation;
                camera->vision_denoise = updates[0].vision_denoise;
            }
            hub.acknowledge_camera_vision_accumulation(handle, updates[0].sequence);
            hub.acknowledge_camera_vision_denoise(handle, updates[0].sequence);
            expect(Corona::API::get_vision_render_mode(handle) == mode &&
                       Corona::API::get_vision_accumulation(handle) && Corona::API::get_vision_denoise(handle),
                   "camera snapshots must expose three independent settings");
            Corona::API::set_vision_render_mode(mode, handle);
            updates = hub.drain_camera_state_updates();
            expect(updates.size() == 1 && updates[0].fields == CameraStateUpdateField::VisionRenderMode,
                   "selecting an algorithm must leave both preferences untouched");
        }
        Corona::API::set_vision_render_mode("progressive_path_tracing", handle);
        Corona::API::set_vision_accumulation(false, handle);
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && !updates[0].vision_accumulation,
               "an explicit saved false must override the legacy progressive default");
        Corona::API::set_vision_accumulation(true, handle);
        hub.enqueue_camera_release({handle});
        expect(!hub.requested_camera_vision_accumulation(handle).has_value() &&
                   hub.drain_camera_state_updates().empty(),
               "camera release must clear queued and in-flight accumulation preferences");
        hub.drain_camera_releases();
        hub.camera_storage().deallocate(handle);
    }
    static void render(vision::Pipeline& pipeline, const char* label, bool lit = true) {
        const auto before = pipeline.frame_index();
        for (int i = 0; i < 3; ++i) {
            // Match the editor frame loop, including changed camera/buffer data.
            pipeline.upload_data();
            pipeline.display(1.0 / 60.0);
        }
        expect(pipeline.frame_index() > before, "real integrator must advance frames");
        std::vector<vision::float4> pixels(pipeline.pixel_num());
        pipeline.final_picture(pipeline.output_desc(), pixels.data());
        float sum = 0;
        for (const auto& pixel : pixels) {
            expect(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z),
                   "GPU output must be finite");
            sum += pixel.x + pixel.y + pixel.z;
        }
        std::cout << "GPU frames: " << label << " frame=" << pipeline.frame_index()
                  << " rgb_sum=" << sum << '\n';
        expect(lit ? sum > 0.f : sum == 0.f, "GPU output must match geometry visibility");
    }
    static void check_independent_denoise(OpticsSystem& system, vision::Pipeline& pipeline,
                                         const char* algorithm, bool accumulation) {
        auto prepare = [&](std::uintptr_t camera, bool enabled) {
            const auto started = std::chrono::steady_clock::now();
            expect(system.prepare_vision_camera_view(system.active_vision_runtime(), camera,
                       16, 16, enabled, accumulation), "camera denoise state must prepare a usable view");
            std::cout << "Denoise switch: algorithm=" << algorithm << " camera=" << camera
                      << " enabled=" << enabled << " ms="
                      << std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - started).count() << '\n';
            auto* integrator = dynamic_cast<vision::IlluminationIntegrator*>(
                pipeline.renderer().integrator().get());
            expect(pipeline.renderer().integrator()->impl_type() == algorithm,
                   "SVGF toggle must preserve the selected ray tracing algorithm");
            expect(pipeline.output_desc().denoise == enabled && integrator &&
                       integrator->denoiser()->enabled() == enabled,
                   "active camera must control both output and actual SVGF state");
            expect(pipeline.frame_buffer()->enable_accumulation() == accumulation,
                   "SVGF must preserve the independent accumulation preference");
        };
        prepare(101, false);
        render(pipeline, "camera-101-raw");
        const auto original_framebuffer = pipeline.renderer().frame_buffer_sp();
        prepare(101, true);
        expect(pipeline.frame_buffer() == original_framebuffer.get(),
               "enabling SVGF must retain the camera renderer and its compiled kernels");
        expect(pipeline.frame_index() == 0, "enabling SVGF must discard incompatible history");
        render(pipeline, "camera-101-SVGF");
        const auto history = pipeline.frame_index();
        prepare(102, false);
        render(pipeline, "camera-102-raw");
        prepare(101, true);
        expect(pipeline.frame_index() == history, "another camera must not reset this camera's history");
        render(pipeline, "camera-101-SVGF-return");
        prepare(101, false);
        expect(pipeline.frame_buffer() == original_framebuffer.get(),
               "disabling SVGF must retain the camera renderer and its compiled kernels");
        expect(pipeline.frame_index() == 0, "disabling SVGF must discard filtered history");
        render(pipeline, "camera-101-raw-return");
        prepare(101, true);
        expect(pipeline.frame_buffer() == original_framebuffer.get() && pipeline.frame_index() == 0,
               "re-enabling SVGF must reuse the renderer with fresh history");
        render(pipeline, "camera-101-SVGF-reenabled");
        expect(system.prepare_vision_camera_view(system.active_vision_runtime(), 101,
                   24, 16, false, accumulation), "resizing a view with cached SVGF must prepare");
        render(pipeline, "camera-101-resized-raw");
        expect(system.prepare_vision_camera_view(system.active_vision_runtime(), 101,
                   24, 16, true, accumulation), "SVGF must prepare at the resized extent");
        render(pipeline, "camera-101-resized-SVGF");
        expect(system.prepare_vision_camera_view(system.active_vision_runtime(), 101,
                   24, 16, false, accumulation), "cached SVGF must disable before an in-place resize");
        pipeline.change_resolution(vision::make_uint2(32, 16));
        expect(system.prepare_vision_camera_view(system.active_vision_runtime(), 101,
                   32, 16, true, accumulation), "disabled SVGF resources must follow an in-place resize");
        render(pipeline, "camera-101-SVGF-after-disabled-resize");
        pipeline.activate_view_context(0);
        pipeline.set_output_denoise(false);
    }
    static void check_independent_accumulation(OpticsSystem& system, vision::Pipeline& pipeline,
                                              const char* algorithm) {
        Corona::CameraDevice camera;
        camera.width = 16; camera.height = 16;
        camera.position = {0.f, 0.f, -3.f}; camera.forward = {0.f, 0.f, 1.f};
        camera.world_up = {0.f, 1.f, 0.f}; camera.fov = 45.f;
        for (const bool denoise : {false, true}) {
            auto prepare = [&](std::uintptr_t handle, bool accumulation) {
                expect(system.prepare_vision_camera_view(system.active_vision_runtime(), handle,
                           16, 16, denoise, accumulation), "independent accumulation view must prepare");
                expect(pipeline.frame_buffer()->enable_accumulation() == accumulation &&
                           pipeline.output_desc().denoise == denoise &&
                           pipeline.renderer().integrator()->impl_type() == algorithm,
                       "accumulation must not change the algorithm or SVGF preference");
            };
            prepare(201, false);
            Vision::sync_vision_camera(pipeline, camera);
            render(pipeline, "accumulation-off");
            prepare(201, true);
            expect(pipeline.frame_index() == 0, "enabling accumulation must restart at sample zero");
            auto* fb = pipeline.frame_buffer();
            std::vector<vision::float4> first(pipeline.pixel_num()), second(first.size()), average(first.size());
            pipeline.upload_data();
            pipeline.display(1.0 / 60.0);
            pipeline.stream() << fb->rt_buffer().device_buffer().download(first.data())
                              << vision::synchronize() << vision::commit();
            pipeline.upload_data();
            pipeline.display(1.0 / 60.0);
            pipeline.stream() << fb->rt_buffer().device_buffer().download(second.data())
                              << fb->accumulation_buffer().device_buffer().download(average.data())
                              << vision::synchronize() << vision::commit();
            for (size_t i = 0; i < first.size(); ++i) {
                expect(std::isfinite(average[i].x) &&
                           std::abs(average[i].x - (first[i].x + second[i].x) * 0.5f) < 0.0001f &&
                           std::abs(average[i].y - (first[i].y + second[i].y) * 0.5f) < 0.0001f &&
                           std::abs(average[i].z - (first[i].z + second[i].z) * 0.5f) < 0.0001f,
                       "the checkbox must average real consecutive GPU output samples");
            }
            const auto history = pipeline.frame_index();
            prepare(202, false);
            render(pipeline, "other-camera-no-accumulation");
            prepare(201, true);
            expect(pipeline.frame_index() == history, "another camera must preserve this camera's samples");
            Vision::sync_vision_camera(pipeline, camera);
            expect(pipeline.frame_index() == history, "stationary camera must retain accumulated samples");
            camera.position.x += 0.1f;
            Vision::sync_vision_camera(pipeline, camera);
            expect(pipeline.frame_index() == 0, "camera movement must restart either accumulating algorithm");
            pipeline.stream() << fb->accumulation_buffer().device_buffer().download(average.data())
                              << vision::synchronize() << vision::commit();
            for (const auto& pixel : average) {
                expect(pixel.x == 0.f && pixel.y == 0.f && pixel.z == 0.f,
                       "movement must clear the accumulated pixel buffer");
            }
            render(pipeline, "accumulation-after-movement");
            prepare(201, false);
            expect(pipeline.frame_index() == 0, "disabling accumulation must discard accumulated history");
            render(pipeline, "accumulation-disabled-again");
        }
        pipeline.activate_view_context(0);
        pipeline.set_output_denoise(false);
    }
    static void check_stationary_history_reset(vision::Pipeline& pipeline) {
        pipeline.activate_global_context();
        pipeline.invalidate();
        pipeline.display(1.0 / 60.0);
        std::vector<vision::float4> first(pipeline.pixel_num()), restarted(first.size());
        pipeline.final_picture(pipeline.output_desc(), first.data());
        // Build stationary boundary history well beyond the interior EMA window.
        for (int i = 0; i < 320; ++i) pipeline.display(1.0 / 60.0);
        expect(pipeline.frame_index() == 321u, "stationary rendering must advance its sample sequence");
        pipeline.invalidate();
        pipeline.display(1.0 / 60.0);
        pipeline.final_picture(pipeline.output_desc(), restarted.data());
        for (size_t i = 0; i < first.size(); ++i) {
            expect(std::isfinite(restarted[i].x) &&
                   std::abs(first[i].x - restarted[i].x) < 1e-6f &&
                   std::abs(first[i].y - restarted[i].y) < 1e-6f &&
                   std::abs(first[i].z - restarted[i].z) < 1e-6f,
                   "explicit invalidation must discard long rendering history on its first frame");
        }
        std::cout << "PASS: stationary rendering history resets after 321 frames\n";
    }
    static void check_realtime_camera_history(vision::Pipeline& pipeline) {
        Corona::CameraDevice camera;
        camera.width = 16; camera.height = 16;
        camera.position = {0.f, 0.f, -3.f}; camera.forward = {0.f, 0.f, 1.f};
        camera.world_up = {0.f, 1.f, 0.f}; camera.fov = 45.f;
        Vision::sync_vision_camera(pipeline, camera);
        pipeline.upload_data();
        render(pipeline, "SVGF-before-camera-motion");
        const auto initial_frame = pipeline.frame_index();
        std::vector<vision::float4> before(pipeline.pixel_num()), after(before.size());
        pipeline.final_picture(pipeline.output_desc(), before.data());
        for (unsigned i = 0; i < 16; ++i) {
            camera.position.x += 0.01f;
            camera.forward.x += 0.005f;
            Vision::sync_vision_camera(pipeline, camera);
            expect(pipeline.frame_index() == initial_frame + i,
                   "realtime camera movement must retain SVGF reprojection history and sample sequence");
            pipeline.upload_data();
            pipeline.display(1.0 / 60.0);
        }
        pipeline.final_picture(pipeline.output_desc(), after.data());
        float difference = 0.f;
        for (size_t i = 0; i < after.size(); ++i) {
            expect(std::isfinite(after[i].x) && std::isfinite(after[i].y) && std::isfinite(after[i].z),
                   "moving realtime output must remain finite");
            difference += std::abs(after[i].x - before[i].x);
        }
        expect(difference > 0.01f, "retaining history must still render the new camera pose");
        camera.fov += 5.f;
        Vision::sync_vision_camera(pipeline, camera);
        expect(pipeline.frame_index() == 0, "projection changes must discard incompatible history");
        pipeline.upload_data();
        pipeline.display(1.0 / 60.0);
        camera.width = 24;
        Vision::sync_vision_camera(pipeline, camera);
        expect(pipeline.frame_index() == 0 && pipeline.resolution().x == 24,
               "resizing must discard old history and allocate the new output");
        pipeline.upload_data();
        render(pipeline, "SVGF-after-camera-resize");
        camera.width = 16;
        Vision::sync_vision_camera(pipeline, camera);
        pipeline.upload_data();
        std::cout << "PASS: realtime camera translation/rotation preserves history; projection/resize resets\n";
    }
    // Optional real-scene latency probe, using the editor's production view
    // preparation path. Kept out of CTest because scene shader cold starts vary.
    static void benchmark_switches(const char* scene) {
        namespace fs = std::filesystem;
        ocarina::RHIContext::instance().init(fs::current_path());
        static auto device = ocarina::RHIContext::instance().create_device("cuda");
        device.init_rtx();
        vision::Global::instance().set_device(&device);
        OpticsSystem system;
        auto& storage = SharedDataHub::instance().camera_storage();
        const auto handle = storage.allocate();
        int surface_token = 0;
        {
            auto camera = storage.acquire_write(handle);
            camera->surface = &surface_token;
            camera->render_backend = CameraRenderBackend::Vision;
        }
        const auto step = [&](CameraVisionRenderMode mode, bool denoise, const char* label) {
            const auto started = std::chrono::steady_clock::now();
            std::cout << "SWITCH_BEGIN " << label << std::endl;
            expect(system.load_external_vision_scene(scene, mode, Vision::VisionPipelineSource::ExternalLive),
                   "benchmark scene must load");
            auto pipeline = vision::Global::instance().pipeline_shared();
            const auto res = pipeline->resolution();
            expect(system.prepare_vision_camera_view(system.active_vision_runtime(), handle,
                       res.x, res.y, denoise, false), "benchmark view must prepare");
            const auto prepared = std::chrono::steady_clock::now();
            render(*pipeline, label);
            std::cout << "SWITCH_RESULT " << label << " extent=" << res.x << 'x' << res.y
                      << " prepare_ms=" << std::chrono::duration<double, std::milli>(prepared - started).count()
                      << " three_frames_ms=" << std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - prepared).count() << std::endl;
            pipeline->activate_view_context(0);
            system.evict_idle_vision_runtimes(10000);
        };
        step(CameraVisionRenderMode::PathTracing, false, "PT-first");
        step(CameraVisionRenderMode::PathTracing, true, "PT-SVGF-first");
        step(CameraVisionRenderMode::PathTracing, false, "PT-SVGF-off");
        step(CameraVisionRenderMode::PathTracing, true, "PT-SVGF-reenable");
        step(CameraVisionRenderMode::ReSTIR, false, "ReSTIR-first");
        step(CameraVisionRenderMode::ReSTIR, true, "ReSTIR-SVGF-first");
        step(CameraVisionRenderMode::ReSTIR, false, "ReSTIR-SVGF-off");
        step(CameraVisionRenderMode::ReSTIR, true, "ReSTIR-SVGF-reenable");
        step(CameraVisionRenderMode::PathTracing, true, "PT-return");
        step(CameraVisionRenderMode::ReSTIR, true, "ReSTIR-return");
        system.clear_vision_runtimes();
        storage.deallocate(handle);
    }
    // Explicit profiling mode: scene unchanged, same runtime/view path as editor.
    static void profile_switches(const char* scene, bool denoise) {
        namespace profile = vision::switch_profile;
        ocarina::DynamicModule::clear_search_path();
        ocarina::RHIContext::instance().init(std::filesystem::current_path());
        static auto device = ocarina::RHIContext::instance().create_device("cuda");
        device.init_rtx();
        vision::Global::instance().set_device(&device);
        OpticsSystem system;
        auto& storage = SharedDataHub::instance().camera_storage();
        const auto handle = storage.allocate();
        int surface_token = 0;
        {
            auto camera = storage.acquire_write(handle);
            camera->surface = &surface_token;
            camera->render_backend = CameraRenderBackend::Vision;
        }
        const auto step = [&](CameraVisionRenderMode mode, const char* label) {
            profile::Scope switching{label, "switch"};
            ocarina::SP<vision::Pipeline> pipeline;
            {
                profile::Scope preparation{"switch.prepare", "prepare"};
                expect(system.load_external_vision_scene(scene, mode, Vision::VisionPipelineSource::ExternalLive),
                       "profile scene must load");
                pipeline = vision::Global::instance().pipeline_shared();
                const auto res = pipeline->resolution();
                std::cout << "PROFILE_EXTENT " << res.x << 'x' << res.y << std::endl;
                expect(system.prepare_vision_camera_view(system.active_vision_runtime(), handle,
                           res.x, res.y, denoise, false), "profile view must prepare");
            }
            {
                profile::Scope first_frame{"first_frame.complete", "first_frame"};
                pipeline->upload_data();
                pipeline->display(1.0 / 60.0);
                pipeline->stream() << ocarina::synchronize() << ocarina::commit();
            }
            // Keep per-mode runtime/view contexts for the in-process reuse probe.
        };
        step(CameraVisionRenderMode::PathTracing, "PT.initial");
        step(CameraVisionRenderMode::ReSTIR, "PT_to_ReSTIR");
        step(CameraVisionRenderMode::PathTracing, "ReSTIR_to_PT");
        step(CameraVisionRenderMode::ReSTIR, "PT_to_ReSTIR.repeat");
        system.clear_vision_runtimes();
        storage.deallocate(handle);
    }
    // Optional visual regression capture uses the same camera adapter and active
    // view context as OpticsSystem's editor render loop. It is not a CTest job.
    static void capture_camera_motion(const char* scene, const char* destination, bool fast = false) {
        namespace fs = std::filesystem;
        ocarina::RHIContext::instance().init(fs::current_path());
        static auto device = ocarina::RHIContext::instance().create_device("cuda");
        device.init_rtx();
        vision::Global::instance().set_device(&device);
        OpticsSystem system;
        expect(system.load_external_vision_scene(scene, CameraVisionRenderMode::SVGF,
                   Vision::VisionPipelineSource::ExternalFile), "motion capture scene must load");
        auto pipeline = vision::Global::instance().pipeline_shared();
        const auto resolution = pipeline->resolution();
        // The editor owns a separate renderer/sensor for each camera.
        expect(pipeline->create_view_context(42, resolution), "capture view must be created");
        expect(pipeline->activate_view_context(42), "capture view must be active");
        auto* sensor = pipeline->scene().sensor().get();
        const auto position = sensor->position();
        const float yaw = sensor->yaw(), pitch = sensor->pitch();
        Corona::CameraDevice camera;
        camera.width = resolution.x; camera.height = resolution.y;
        camera.fov = sensor->fov_y(); camera.world_up = {0.f, 1.f, 0.f};
        const fs::path out = fs::absolute(destination);
        fs::create_directories(out);
        std::ofstream metrics(out / "frames.csv");
        metrics << "frame,history_before,history_after,yaw,pitch,x,y,z,gpu_ms\n" << std::setprecision(9);
        std::vector<vision::float4> pixels(pipeline->pixel_num());
        constexpr float radians = 0.017453292519943295f;
        for (unsigned frame = 0; frame < 224; ++frame) {
            const auto excursion = [fast](unsigned step, float distance) {
                // The fast path reaches its endpoint in eight frames and holds
                // there to expose trails and the first frames after stopping.
                return fast ? std::min(float(step) / 8.f, 1.f) * distance
                            : float(step) / 32.f * distance;
            };
            const float rotation = frame <= 64 ? 0.f : frame <= 96 ? excursion(frame - 64, fast ? 15.f : 3.f)
                : frame <= 128 ? (fast ? 15.f - excursion(frame - 96, 15.f) : excursion(128 - frame, 3.f)) : 0.f;
            const float translation = frame <= 128 ? 0.f : frame <= 160 ? excursion(frame - 128, fast ? 0.48f : 0.12f)
                : frame <= 192 ? (fast ? 0.48f - excursion(frame - 160, 0.48f) : excursion(192 - frame, 0.12f)) : 0.f;
            const float yr = (yaw + rotation) * radians, pr = pitch * radians;
            camera.position = {position.x + translation, position.y, -position.z};
            camera.forward = {std::sin(yr)*std::cos(pr), std::sin(pr), std::cos(yr)*std::cos(pr)};
            Vision::sync_vision_camera(*pipeline, camera);
            const auto history = pipeline->frame_index();
            pipeline->upload_data();
            pipeline->display(1.0 / 60.0);
            metrics << frame << ',' << history << ',' << pipeline->frame_index() << ','
                    << sensor->yaw() << ',' << sensor->pitch() << ',' << sensor->position().x << ','
                    << sensor->position().y << ',' << sensor->position().z << ','
                    << pipeline->cur_render_time() << '\n';
            if (frame >= 64) {
                std::ostringstream name;
                name << "frame_" << std::setfill('0') << std::setw(4) << frame << ".png";
                auto desc = pipeline->output_desc();
                desc.fn = name.str();
                pipeline->final_picture(desc, pixels.data());
                vision::Image::save_image(out / name.str(), vision::PixelStorage::FLOAT4, resolution, pixels.data());
            }
        }
        pipeline.reset();
        system.clear_vision_runtimes();
        std::cout << "PASS: captured 160 frames through the production camera adapter\n";
    }
    static void check_engine_modes(OpticsSystem& system) {
        Corona::CameraDevice camera;
        camera.width = 16; camera.height = 16;
        camera.position = {0.f, 0.f, -3.f}; camera.forward = {0.f, 0.f, 1.f};
        camera.world_up = {0.f, 1.f, 0.f}; camera.fov = 45.f;
        expect(system.load_engine_built_vision_scene(CameraVisionRenderMode::ReSTIR),
               "engine-built ReSTIR must initialize");
        auto engine_restir = vision::Global::instance().pipeline_shared();
        Vision::sync_vision_camera(*engine_restir, camera);
        expect(engine_restir->renderer().integrator()->impl_type() == "rt",
               "engine-built mode must select rt");
        // The fixture registers no engine geometry or environment. Its empty
        // engine-built scene must remain finite and black through both modes.
        render(*engine_restir, "Engine-ReSTIR", false);
        system.apply_vision_render_mode(CameraVisionRenderMode::ProgressivePathTracing);
        auto engine_pt = vision::Global::instance().pipeline_shared();
        Vision::sync_vision_camera(*engine_pt, camera);
        expect(engine_pt != engine_restir && engine_pt->renderer().integrator()->impl_type() == "pt" &&
                   engine_pt->frame_buffer()->enable_accumulation(),
               "engine-built switch must replace rt with progressive pt");
        render(*engine_pt, "Engine-Progressive", false);
        system.apply_vision_render_mode(CameraVisionRenderMode::ReSTIR);
        auto engine_return = vision::Global::instance().pipeline_shared();
        Vision::sync_vision_camera(*engine_return, camera);
        expect(engine_return != engine_pt && engine_return->renderer().integrator()->impl_type() == "rt" &&
                   !engine_return->frame_buffer()->enable_accumulation(),
               "engine-built return must recreate ReSTIR history");
        render(*engine_return, "Engine-ReSTIR-return", false);
        engine_restir.reset(); engine_pt.reset(); engine_return.reset();
    }
    static void check_scene_asset_reuse() {
        namespace fs = std::filesystem;
        const auto runtime_dir = fs::current_path();
        ocarina::DynamicModule::clear_search_path();
        ocarina::RHIContext::instance().init(runtime_dir);
        static auto device = ocarina::RHIContext::instance().create_device("cuda");
        device.init_rtx();
        vision::Global::instance().set_device(&device);
        for (bool embedded : {true, false}) {
            const auto base = runtime_dir / (embedded ? "scene-reuse-embedded-assets" : "scene-reuse-file-assets");
            fs::create_directories(base);
            const auto scene_file = base / "scene.json";
            const unsigned char white_tga[] = {0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,24,0,255,255,255};
            const std::string json = R"({
              "scene": {
                "camera":{"type":"thin_lens","param":{"transform":{"type":"look_at","param":{"position":[0,0,3],"target_pos":[0,0,0],"up":[0,1,0]}}}},
                "materials":[{"type":"diffuse","name":"mat","param":{"color":{"channels":"xyz","node":{"type":"image","param":{"fn":"albedo.tga"}}}}}],
                "shapes":[{"type":"model","param":{"fn":"triangle.obj","material":"mat","emission":{"type":"area","param":{}}}}],
                "lights":[{"type":"point","param":{"position":[0,0,2]}},{"type":"AREA","param":{"width":0.5,"height":0.5,"two_sided":true}}]
              },
              "render":{"sampler":{"type":"independent","param":{"spp":1}},"integrator":{"type":"pt","param":{"max_depth":2}},"light_sampler":{"type":"uniform"}},
              "pipeline":{"type":"fixed","param":{"frame_buffer":{"type":"normal","param":{"resolution":[16,16]}}}},
              "output":{"spp":1,"denoise":false}
            })";
            auto write_assets = [&](bool changed) {
                std::ofstream(base / "triangle.obj") << (changed
                    ? "v -2 -1 0\nv 2 -1 0\nv 0 1 0\nf 1 2 3\n"
                    : "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n");
                auto pixels = std::vector<unsigned char>(std::begin(white_tga), std::end(white_tga));
                if (changed) { pixels[18] = 0; pixels[20] = 0; }
                std::ofstream(base / "albedo.tga", std::ios::binary).write(
                    reinterpret_cast<const char*>(pixels.data()), pixels.size());
                std::ofstream(scene_file) << json;
            };
            auto remove_assets = [&] {
                fs::remove(base / "triangle.obj");
                fs::remove(base / "albedo.tga");
                fs::remove(scene_file);
            };
            write_assets(false);
            OpticsSystem system;
            const auto source = embedded ? Vision::VisionPipelineSource::ExternalLive
                                         : Vision::VisionPipelineSource::ExternalFile;
            OpticsSystem::VisionSceneLoadRequest request;
            request.scene_key = scene_file.string();
            request.base_dir = base.string();
            request.external_live = true;
            request.scene_json = json;
            expect(embedded ? system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::PathTracing)
                            : system.load_external_vision_scene(scene_file.string(), CameraVisionRenderMode::PathTracing, source),
                   "initial scene asset import");
            auto pt = vision::Global::instance().pipeline_shared();
            pt->commit_command();
            const auto resource_key = system.make_vision_scene_resource_key(scene_file.string(), source);
            auto resource = system.vision_scene_resources_.at(resource_key);
            remove_assets();
            expect(system.load_external_vision_scene(scene_file.string(), CameraVisionRenderMode::ReSTIR, source),
                   "first PT to ReSTIR must reuse scene assets without reopening scene, model or texture files");
            auto restir = vision::Global::instance().pipeline_shared();
            expect(restir != pt && restir->shared_scene_data() != pt->shared_scene_data() &&
                       restir->geometry().gpu_resource() != pt->geometry().gpu_resource() &&
                       restir->frame_buffer() != pt->frame_buffer(), "reused assets must preserve runtime isolation");
            expect(restir->scene().groups().size() == pt->scene().groups().size() &&
                       restir->scene().instances().size() == pt->scene().instances().size() &&
                       restir->scene().light_manager().lights().all_instance_num() == pt->scene().light_manager().lights().all_instance_num(),
                   "standalone area lights must not duplicate geometry or lights during scene restoration");
            expect(restir->image_pool().size() == pt->image_pool().size() &&
                       restir->scene().instances().back()->material()->hash() == pt->scene().instances().back()->material()->hash(),
                   "cached geometry and textured material must match the source");
            for (size_t i = 0; i < restir->scene().instances().size(); ++i) {
                const auto& instance = restir->scene().instances()[i];
                expect(instance->has_emission() && instance->emission()->instance() == instance.get(),
                       "cached area light must bind to the new runtime instance");
                expect(Vision::aabb_matrix_values(instance->o2w()) ==
                           Vision::aabb_matrix_values(pt->scene().instances()[i]->o2w()),
                       "cached shape order and transforms must match the source");
            }
            render(*restir, "asset-reuse-ReSTIR");
            render(*pt, "asset-reuse-original-PT");
            expect(resource->source_revision == 1, "mode switch must not republish source");
            expect(!system.load_external_vision_scene(scene_file.string(), CameraVisionRenderMode::PathTracing, source, true),
                   "explicit reload must still reject missing source assets");
            expect(resource->source_revision == 1, "failed reload must preserve the cached source");
            write_assets(true);
            expect(system.load_external_vision_scene(scene_file.string(), CameraVisionRenderMode::PathTracing, source, true),
                   "explicit reload must replace cached scene assets");
            auto reloaded = vision::Global::instance().pipeline_shared();
            expect(resource->source_revision == 2 && reloaded->scene().groups().back()->aabb.upper.x == 2.f,
                   "force reload must observe changed model vertices");
            pt.reset(); restir.reset();
            std::weak_ptr<vision::GeometryGpuResource> gpu = reloaded->geometry().gpu_resource();
            reloaded.reset();
            system.activate_single_vision_runtime_key(system.make_vision_pipeline_key("",
                CameraVisionRenderMode::PathTracing, Vision::VisionPipelineSource::EngineBuilt));
            expect(gpu.expired(), "CPU asset cache must not retain retired GPU geometry");
            remove_assets();
            expect(system.load_external_vision_scene(scene_file.string(), CameraVisionRenderMode::ProgressivePathTracing, source),
                   "runtime recreation must reuse CPU assets after GPU retirement");
            reloaded = vision::Global::instance().pipeline_shared();
            expect(reloaded->scene().groups().back()->aabb.upper.x == 2.f, "recreated runtime must use refreshed assets");
            const auto texture_desc = reloaded->scene().instances().back()->material()->source_desc()
                .slot("color", ocarina::make_float3(0.5f), vision::Albedo).node;
            expect(reloaded->image_pool().is_contain(texture_desc.hash()),
                   "refreshed material texture must already exist in the recreated runtime");
            const auto average = reloaded->image_pool().obtain_texture(texture_desc,
                reloaded->geometry().bindless_array(), reloaded->device()).host_tex().average<4>();
            expect(average.x == 0.f && average.y == 1.f && average.z == 0.f,
                   "retained CPU image must contain the new green pixels and decoded average after force reload");
            render(*reloaded, "asset-reuse-after-retirement");
            reloaded.reset();
            system.clear_vision_runtimes();
            fs::remove(base);
        }
        std::cout << "PASS: scene assets reused across algorithms, reload and GPU retirement\n";
    }
    static void run() {
        namespace fs = std::filesystem;
        const auto original_cwd = fs::current_path();
        const auto base = original_cwd / "embedded-mode-switch-test-assets";
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
        expect(!fs::exists(request.scene_key), "fixture must never create an embedded file");
        expect(system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::PathTracing),
               "in-memory PT must initialize");
        auto pt = vision::Global::instance().pipeline_shared();
        // Editor imports need only camera kernels. Drain and render a camera
        // before the base renderer is used; lazy base compilation must remain
        // safe during view creation/retirement and when rendered afterwards.
        pt->commit_command();
        expect(system.prepare_vision_camera_view(system.active_vision_runtime(), 101,
                   16, 16, false, false), "camera must render before the unused base renderer");
        render(*pt, "PT-camera-before-base");
        pt->activate_view_context(0);
        pt->commit_command();
        render(*pt, "PT");
        check_independent_accumulation(system, *pt, "pt");
        expect(!pt->frame_buffer()->enable_accumulation(), "realtime PT must not accumulate SVGF output");
        expect(system.load_external_vision_scene(request.scene_key,
            CameraVisionRenderMode::ProgressivePathTracing, Vision::VisionPipelineSource::ExternalLive),
            "progressive PT must load from the same embedded source");
        auto progressive = vision::Global::instance().pipeline_shared();
        expect(progressive != pt, "PT modes must keep independent histories");
        expect(!progressive->output_desc().denoise && progressive->frame_buffer()->enable_accumulation(),
            "progressive PT must accumulate without realtime denoise");
        auto* illumination = dynamic_cast<vision::IlluminationIntegrator*>(progressive->renderer().integrator().get());
        expect(illumination && illumination->denoiser() && !illumination->denoiser()->enabled(),
            "the actual denoiser must remain disabled, not just the output flag");
        progressive->invalidate();
        auto* fb = progressive->frame_buffer();
        std::vector<vision::float4> first(progressive->pixel_num()), second(first.size()), average(first.size());
        progressive->display(1.0 / 60.0);
        progressive->stream() << fb->rt_buffer().device_buffer().download(first.data())
                              << vision::synchronize() << vision::commit();
        progressive->display(1.0 / 60.0);
        progressive->stream() << fb->rt_buffer().device_buffer().download(second.data())
                              << fb->accumulation_buffer().device_buffer().download(average.data())
                              << vision::synchronize() << vision::commit();
        for (size_t i = 0; i < first.size(); ++i) {
            expect(std::isfinite(average[i].x) &&
                   std::abs(average[i].x - (first[i].x + second[i].x) * 0.5f) < 0.0001f &&
                   std::abs(average[i].y - (first[i].y + second[i].y) * 0.5f) < 0.0001f &&
                   std::abs(average[i].z - (first[i].z + second[i].z) * 0.5f) < 0.0001f,
                   "progressive output must average actual consecutive PT samples");
        }
        Corona::CameraDevice camera;
        camera.width = 16; camera.height = 16;
        camera.position = {0.f, 0.f, -3.f}; camera.forward = {0.f, 0.f, 1.f};
        camera.world_up = {0.f, 1.f, 0.f}; camera.fov = 45.f;
        Vision::sync_vision_camera(*progressive, camera);
        render(*progressive, "Progressive");
        const auto accumulated_frames = progressive->frame_index();
        Vision::sync_vision_camera(*progressive, camera);
        expect(progressive->frame_index() == accumulated_frames, "a stationary camera must keep accumulating");
        camera.position.x += 0.1f;
        Vision::sync_vision_camera(*progressive, camera);
        expect(progressive->frame_index() == 0, "moving the camera must restart convergence");
        progressive->stream() << fb->accumulation_buffer().device_buffer().download(average.data())
                              << vision::synchronize() << vision::commit();
        for (const auto& pixel : average) {
            expect(pixel.x == 0.f && pixel.y == 0.f && pixel.z == 0.f,
                   "camera changes must clear old accumulated pixels");
        }
        check_independent_denoise(system, *progressive, "pt", true);
        expect(system.load_external_vision_scene(request.scene_key, CameraVisionRenderMode::PathTracing,
            Vision::VisionPipelineSource::ExternalLive), "switch back to realtime PT");
        render(*pt, "PT-after-progressive");
        progressive.reset();
        expect(system.load_external_vision_scene(request.scene_key, CameraVisionRenderMode::ReSTIR,
            Vision::VisionPipelineSource::ExternalLive), "embedded ReSTIR must initialize");
        auto restir = vision::Global::instance().pipeline_shared();
        expect(restir != pt && restir->renderer().integrator()->impl_type() == "rt",
               "ReSTIR must own a real rt integrator and separate history");
        expect(!restir->frame_buffer()->enable_accumulation(), "ReSTIR must stay realtime");
        render(*restir, "ReSTIR");
        check_stationary_history_reset(*restir);
        check_independent_denoise(system, *restir, "rt", false);
        check_independent_accumulation(system, *restir, "rt");
        expect(restir->create_view_context(99, vision::make_uint2(16, 16)),
               "ReSTIR camera view context must initialize");
        expect(restir->renderer().integrator()->impl_type() == "rt", "detached view must also use rt");
        render(*restir, "ReSTIR-view");
        restir->activate_view_context(0);
        expect(system.load_external_vision_scene(request.scene_key, CameraVisionRenderMode::PathTracing,
            Vision::VisionPipelineSource::ExternalLive), "return to PT after ReSTIR");
        expect(vision::Global::instance().pipeline() == pt.get() && pt->frame_index() == 0,
               "switching back must activate PT and discard stale history");
        render(*pt, "PT-after-ReSTIR");
        expect(system.load_external_vision_scene(request.scene_key, CameraVisionRenderMode::ReSTIR,
            Vision::VisionPipelineSource::ExternalLive), "return to cached ReSTIR");
        expect(vision::Global::instance().pipeline() == restir.get() && restir->frame_index() == 0,
               "cached ReSTIR must discard its stale reservoirs");
        restir->activate_view_context(99);
        expect(restir->frame_index() == 0, "all ReSTIR camera histories must reset");
        restir->activate_view_context(0);
        render(*restir, "ReSTIR-return");
        auto& camera_storage = SharedDataHub::instance().camera_storage();
        const auto visible_camera = camera_storage.allocate();
        int surface_token = 0;
        {
            auto view = camera_storage.acquire_write(visible_camera);
            view->surface = &surface_token;
            view->render_backend = CameraRenderBackend::Vision;
        }
        expect(system.prepare_vision_camera_view(system.active_vision_runtime(), visible_camera,
                   16, 16, false, false), "visible ReSTIR view must prepare");
        auto cached_framebuffer = restir->renderer().frame_buffer_sp();
        const auto restir_key = system.make_vision_pipeline_key(request.scene_key,
            CameraVisionRenderMode::ReSTIR, Vision::VisionPipelineSource::ExternalLive);
        restir->activate_view_context(0);
        system.load_external_vision_scene(request.scene_key, CameraVisionRenderMode::PathTracing,
            Vision::VisionPipelineSource::ExternalLive);
        system.evict_idle_vision_runtimes(1000);
        expect(system.vision_runtimes_.contains(restir_key),
               "an open viewport must retain cached ReSTIR beyond the idle eviction interval");
        const auto switch_started = std::chrono::steady_clock::now();
        expect(system.load_external_vision_scene(request.scene_key, CameraVisionRenderMode::ReSTIR,
                   Vision::VisionPipelineSource::ExternalLive) &&
                   system.prepare_vision_camera_view(system.active_vision_runtime(), visible_camera,
                       16, 16, false, false), "cached ReSTIR must reactivate");
        std::cout << "Cached ReSTIR switch ms=" << std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - switch_started).count() << '\n';
        expect(vision::Global::instance().pipeline() == restir.get() &&
                   restir->frame_buffer() == cached_framebuffer.get() && restir->frame_index() == 0,
               "cached mode switch must reuse compiled resources and discard stale history");
        render(*restir, "ReSTIR-after-idle");
        cached_framebuffer.reset();
        restir->activate_view_context(0);
        system.load_external_vision_scene(request.scene_key, CameraVisionRenderMode::PathTracing,
            Vision::VisionPipelineSource::ExternalLive);
        {
            auto view = camera_storage.acquire_write(visible_camera);
            view->surface = nullptr;
        }
        system.evict_idle_vision_runtimes(1001);
        expect(!system.vision_runtimes_.contains(restir_key),
               "closing the viewport must allow unused mode resources to be reclaimed");
        camera_storage.deallocate(visible_camera);
        restir.reset();
        const auto resource_key = system.make_vision_scene_resource_key(request.scene_key,
            Vision::VisionPipelineSource::ExternalLive);
        auto resource = system.vision_scene_resources_.at(resource_key);
        expect(resource->is_embedded() && resource->source_revision == 1,
               "only successful import publishes first source revision");
        expect(fs::path(resource->source_desc->base_dir).is_absolute(), "source base must be absolute");
        request.base_dir = resource->source_desc->base_dir;
        Vision::ExternalLiveAabbCache pt_cache;
        auto original_matrix = pt->scene().instances()[0]->o2w();
        const auto hidden_matrix = vision::make_float4x4(0.f);
        auto hidden = Vision::sync_external_live_group(*resource, pt_cache, 42, 0,
            pt->scene().groups()[0], 10, 11, hidden_matrix, true, true);
        expect(hidden.changed, "hiding must update production instance state");
        resource->mark_transforms_changed();
        pt->update_geometry();
        // Changing CWD must not change the meaning of relative source resources.
        // The alternate working directory also contains Vision CUDA compiler headers.
        fs::current_path(original_cwd.parent_path() / "examples" / "engine");
        const auto key = system.make_vision_pipeline_key(request.scene_key, CameraVisionRenderMode::SVGF,
                                                         Vision::VisionPipelineSource::ExternalLive);
        expect(system.ensure_external_vision_runtime(key) != nullptr,
               "second mode must import the in-memory source, not the identity as a file");
        auto svgf = vision::Global::instance().pipeline_shared();
        expect(svgf && svgf != pt, "new mode must own a distinct pipeline");
        expect(svgf->frame_buffer() != pt->frame_buffer(), "framebuffer histories must be per runtime");
        expect(svgf->scene().instances().size() == 1, "relative model must load in new mode");
        render(*svgf, "SVGF-hidden", false);
        Vision::ExternalLiveAabbCache svgf_cache;
        auto restored = Vision::sync_external_live_group(*resource, svgf_cache, 42, 0,
            svgf->scene().groups()[0], 10, 10, original_matrix, false, true);
        expect(restored.changed, "restoring after mode switch must update geometry");
        expect(Vision::aabb_matrix_values(svgf->scene().instances()[0]->o2w()) ==
                   Vision::aabb_matrix_values(original_matrix), "hidden mode must preserve original transform");
        resource->mark_transforms_changed();
        svgf->update_geometry();
        render(*svgf, "SVGF-restored");
        check_stationary_history_reset(*svgf);
        check_motion_visibility_history(*svgf);
        check_realtime_camera_history(*svgf);
        check_camera_dolly_reprojection(*svgf);
        check_svgf_restir_motion_history(*svgf);
        check_svgf_spatial_bypass(*svgf);
        // Restore the PT view through the same production group helper. The
        // editor test additionally exercises automatic runtime transform upload.
        Vision::sync_external_live_group(*resource, pt_cache, 42, 0,
            pt->scene().groups()[0], 10, 10, original_matrix, false, true);
        pt->update_geometry();
        pt->invalidate_all_view_contexts();
        expect(system.load_external_vision_scene(request.scene_key, CameraVisionRenderMode::PathTracing,
            Vision::VisionPipelineSource::ExternalLive), "switch back to PT");
        render(*pt, "PT-return");
        expect(resource->source_revision == 1, "mode changes do not republish source");
        const auto original_json = request.scene_json;
        for (const auto& invalid : {std::string{}, std::string{"{"}, std::string{"[]"}, std::string{"{}"},
                std::string{R"({"scene":{"shapes":1}})"}, std::string{R"({"scene":{"mediums":1}})"},
                std::string{R"({"scene":{"mediums":{"global":2}}})"},
                std::string{R"({"scene":{"mediums":{"process":1}}})"},
                std::string{R"({"scene":{"mediums":{"list":1}}})"}}) {
            request.scene_json = invalid;
            expect(!system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::SVGF, true),
                   "invalid reload must fail");
            expect(resource->source_revision == 1 && resource->source_desc->scene_json == original_json,
                   "failed reload must preserve last successful source");
        }
        request.scene_json = original_json;
        const auto filename = request.scene_json.find("triangle.obj");
        request.scene_json.replace(filename, 12, "missing.obj");
        expect(!system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::SVGF, true),
               "missing model must fail before GPU import");
        expect(resource->source_revision == 1, "missing resource cannot publish a revision");
        request.scene_json = original_json;
        request.scene_json.replace(request.scene_json.find("albedo.tga"), 10, "missing.tga");
        expect(!system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::SVGF, true),
               "missing relative texture must fail before GPU import");
        expect(resource->source_revision == 1, "missing texture cannot publish a revision");
        render(*pt, "PT-after-failed-reload");
        // Same identity, different JSON must replace both previous modes even without force.
        request.scene_json = original_json;
        const auto resolution = request.scene_json.find("[16,16]");
        request.scene_json.replace(resolution, 7, "[24,16]");
        expect(system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::SVGF),
               "same-key new JSON must load");
        expect(resource->source_revision == 2, "new JSON must publish a new revision");
        expect(system.vision_runtimes_.size() == 1, "reload must retire all previous modes");
        pt.reset(); svgf.reset();
        auto reloaded = vision::Global::instance().pipeline_shared();
        expect(reloaded->resolution().x == 24, "new JSON must actually change pipeline resolution");
        render(*reloaded, "SVGF-new-JSON");
        // A force reload with identical JSON also reloads changed on-disk resources.
        std::ofstream(base / "triangle.obj") << "v -2 -1 0\nv 2 -1 0\nv 0 1 0\nf 1 2 3\n";
        expect(system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::PathTracing, true),
               "resource reload must import again");
        reloaded.reset();
        reloaded = vision::Global::instance().pipeline_shared();
        expect(resource->source_revision == 3, "forced resource reload must publish a new revision");
        expect(reloaded->scene().groups()[0]->aabb.upper.x == 2.f, "reloaded model must use new vertices");
        render(*reloaded, "PT-resource-reload");
        std::weak_ptr<vision::GeometryGpuResource> retired_gpu = reloaded->scene().geometry().gpu_resource();
        reloaded.reset();
        // Use production teardown to remove every external runtime, leaving only
        // an engine-built placeholder. The shared source must survive this boundary.
        system.activate_single_vision_runtime_key(system.make_vision_pipeline_key("",
            CameraVisionRenderMode::PathTracing, Vision::VisionPipelineSource::EngineBuilt));
        expect(retired_gpu.expired(), "retiring the last runtime must release scene GPU resources");
        expect(system.ensure_external_vision_runtime(key) != nullptr, "rebuild after runtime retirement");
        reloaded = vision::Global::instance().pipeline_shared();
        expect(reloaded->resolution().x == 24 && resource->source_revision == 3,
               "recreated runtime must consume latest source without republishing");
        render(*reloaded, "SVGF-recreated");
        reloaded.reset();
        expect(!fs::exists(request.scene_key), "embedded scene must remain memory-only");
        std::ofstream(base / "file-scene.json") << original_json;
        expect(system.load_external_vision_scene((base / "file-scene.json").string(),
            CameraVisionRenderMode::SVGF, Vision::VisionPipelineSource::ExternalFile),
            "explicit file source must continue to import");
        const auto file_resource = system.vision_scene_resources_.at(system.make_vision_scene_resource_key(
            (base / "file-scene.json").string(), Vision::VisionPipelineSource::ExternalFile));
        expect(file_resource->source_desc->kind == Vision::VisionSceneSourceKind::File &&
                   fs::path(file_resource->source_desc->base_dir).is_absolute(),
               "file source must retain explicit kind and absolute base");
        expect(system.load_external_vision_scene((base / "file-scene.json").string(),
            CameraVisionRenderMode::PathTracing, Vision::VisionPipelineSource::ExternalFile),
            "file source must also support a second mode");
        expect(file_resource->source_revision == 1, "file mode switch must not republish source");
        render(*vision::Global::instance().pipeline(), "File-PT");
        check_substrate_sample_classification(*vision::Global::instance().pipeline());
        check_svgf_total_albedo(*vision::Global::instance().pipeline());
        check_svgf_shading_guide(*vision::Global::instance().pipeline());
        expect(system.load_external_vision_scene((base / "file-scene.json").string(),
            CameraVisionRenderMode::ReSTIR, Vision::VisionPipelineSource::ExternalFile),
            "file source must support ReSTIR");
        render(*vision::Global::instance().pipeline(), "File-ReSTIR");
        check_engine_modes(system);
        // Two distinct lights make stale temporal reservoirs observable; a
        // single point light always reuses the same sample and hides the bug.
        auto history_scene = vision::DataWrap::parse(original_json);
        history_scene["scene"]["lights"] = vision::DataWrap::array({
            {{"type", "point"}, {"param", {{"position", {0, 0, 2}}, {"color", {1, 0, 0}}}}},
            {{"type", "point"}, {"param", {{"position", {1, 0, 2}}, {"color", {0, 1, 0}}}}}
        });
        history_scene["render"]["integrator"]["param"]["direct"] = {
            {"M_light", 1}, {"M_bsdf", 0}, {"max_age", 1000},
            {"spatial", {{"open", false}}}, {"temporal", {{"open", true}}}
        };
        request.scene_json = history_scene.dump();
        expect(system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::ReSTIR),
               "two-light ReSTIR history fixture must load");
        auto history_pipeline = vision::Global::instance().pipeline_shared();
        history_pipeline->set_output_denoise(false);
        history_pipeline->upload_data();
        check_stationary_history_reset(*history_pipeline);
        history_pipeline.reset();
        system.clear_vision_runtimes();
        resource.reset();
        fs::current_path(original_cwd);
        fs::remove(base / "file-scene.json");
        fs::remove(base / "albedo.tga");
        fs::remove(base / "triangle.obj");
        fs::remove(base);
        std::cout << "PASS: PT/ReSTIR/SVGF mode switching and history reset\n";
    }
};
}
int main(int argc, char** argv) {
    try {
        Corona::Systems::VisionEmbeddedModeSwitchTest::check_mode_api();
        Corona::Systems::VisionEmbeddedModeSwitchTest::check_accumulation_api();
        if (argc == 2 && std::string(argv[1]) == "--scene-asset-reuse") {
            Corona::Systems::VisionEmbeddedModeSwitchTest::check_scene_asset_reuse();
        } else if ((argc == 3 || (argc == 4 && std::string(argv[3]) == "--denoise")) &&
            std::string(argv[1]) == "--profile-switches") {
            Corona::Systems::VisionEmbeddedModeSwitchTest::profile_switches(
                argv[2], argc == 4);
        } else if (argc == 3 && std::string(argv[1]) == "--benchmark-switches") {
            Corona::Systems::VisionEmbeddedModeSwitchTest::benchmark_switches(argv[2]);
        } else if (argc == 4 && (std::string(argv[1]) == "--capture-camera-motion" ||
                          std::string(argv[1]) == "--capture-fast-camera-motion")) {
            Corona::Systems::VisionEmbeddedModeSwitchTest::capture_camera_motion(
                argv[2], argv[3], std::string(argv[1]) == "--capture-fast-camera-motion");
        } else {
            Corona::Systems::VisionEmbeddedModeSwitchTest::run();
        }
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
