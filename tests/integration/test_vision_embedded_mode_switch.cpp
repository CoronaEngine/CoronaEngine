#include <corona/systems/optics/optics_system.h>
#include <corona/engine/engine_runtime_api.h>
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/switch_profile.h"
#include "base/scattering/material.h"
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
#include <limits>

void check_camera_dolly_reprojection(vision::Pipeline& pipeline);
void check_svgf_restir_motion_history(vision::Pipeline& pipeline);
void check_motion_visibility_history(vision::Pipeline& pipeline);
void check_substrate_sample_classification(vision::Pipeline& pipeline);
void check_svgf_shading_guide(vision::Pipeline& pipeline);
void check_svgf_total_albedo(vision::Pipeline& pipeline);
void check_svgf_spatial_bypass(vision::Pipeline& pipeline);
void check_svgf_layered_history(vision::Pipeline& pipeline);

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
    static void check_frame_timing() {
        auto& hub = SharedDataHub::instance();
        const auto first = hub.camera_storage().allocate();
        const auto second = hub.camera_storage().allocate();
        expect(!hub.camera_frame_timing(first), "new camera must not have stale frame timing");
        const auto start = std::chrono::steady_clock::now();
        hub.publish_camera_frame_timing(first, 24.9, 6.5, CameraVisionRenderMode::ReSTIR, start);
        hub.publish_camera_frame_timing(second, 10.5, 0.0, CameraVisionRenderMode::PathTracing, start);
        expect(hub.camera_frame_timing(first)->frame_ms == 0.0,
               "first frame must not invent a total frame interval");
        auto timing = hub.camera_frame_timing(first);
        expect(timing && timing->render_ms == 24.9 && timing->denoise_ms == 6.5 &&
                   timing->mode == CameraVisionRenderMode::ReSTIR,
               "frame timing must retain per-camera render/denoise duration and mode");
        hub.publish_camera_frame_timing(first, 12.3, 3.1, CameraVisionRenderMode::ReSTIR,
                                       start + std::chrono::milliseconds(33));
        expect(hub.camera_frame_timing(first)->frame_ms == 33.0 &&
                   hub.camera_frame_timing(second)->frame_ms == 0.0,
               "total frame interval must include waits and remain camera-local");
        expect(hub.camera_frame_timing(first)->render_ms == 12.3 &&
                   hub.camera_frame_timing(first)->denoise_ms == 3.1 &&
                   hub.camera_frame_timing(second)->render_ms == 10.5 &&
                   hub.camera_frame_timing(second)->denoise_ms == 0.0,
               "publishing a frame must replace only that camera's latest timing");
        hub.publish_camera_frame_timing(first, 8.0, 0.0, CameraVisionRenderMode::PathTracing,
                                       start + std::chrono::milliseconds(70));
        expect(hub.camera_frame_timing(first)->frame_ms == 0.0 && hub.camera_frame_timing(first)->denoise_ms == 0.0,
               "render mode change must reset the frame interval baseline");
        hub.publish_camera_frame_timing(first, 7.0, 0.0, CameraVisionRenderMode::PathTracing,
                                       start + std::chrono::milliseconds(90));
        expect(hub.camera_frame_timing(first)->frame_ms == 20.0,
               "second frame after mode change must measure the new cadence");
        hub.enqueue_camera_release({first});
        hub.enqueue_camera_release({second});
        hub.drain_camera_releases();
        expect(!hub.camera_frame_timing(first) && !hub.camera_frame_timing(second),
               "released camera timings must be discarded");
        hub.camera_storage().deallocate(first);
        hub.camera_storage().deallocate(second);
    }
    static void check_stable_planes_api() {
        auto& hub = SharedDataHub::instance();
        const auto handle = hub.camera_storage().allocate();
        const auto other = hub.camera_storage().allocate();
        expect(Corona::API::get_vision_stable_planes(handle) &&
                   Corona::API::get_requested_vision_stable_planes(handle) &&
                   !hub.requested_camera_vision_stable_planes(handle).has_value(),
               "new cameras must enable stable planes without a pending request");
        Corona::API::set_vision_stable_planes(false, handle);
        expect(Corona::API::get_vision_stable_planes(handle) &&
                   !Corona::API::get_requested_vision_stable_planes(handle) &&
                   Corona::API::get_requested_vision_stable_planes(other),
               "queued stable-plane preferences must remain camera-local and immediately persistable");
        auto updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].camera_handle == handle &&
                   updates[0].fields == CameraStateUpdateField::VisionStablePlanes &&
                   !updates[0].vision_stable_planes &&
                   !Corona::API::get_requested_vision_stable_planes(handle),
               "stable-plane false must remain requested while the render update is in flight");
        const auto in_flight = updates[0];
        Corona::API::set_vision_stable_planes(true, handle);
        {
            auto camera = hub.camera_storage().acquire_write(handle);
            camera->vision_stable_planes = in_flight.vision_stable_planes;
        }
        hub.acknowledge_camera_vision_stable_planes(handle, in_flight.sequence);
        expect(!Corona::API::get_vision_stable_planes(handle) &&
                   Corona::API::get_requested_vision_stable_planes(handle) &&
                   hub.requested_camera_vision_stable_planes(handle).has_value(),
               "an old stable-plane acknowledgement must preserve the newer request");
        for (const auto* mode : {"path_tracing", "restir", "ssat"}) {
            Corona::API::set_vision_stable_planes(false, handle);
            Corona::API::set_vision_accumulation(true, handle);
            Corona::API::set_vision_denoise(true, handle);
            Corona::API::set_vision_render_mode(mode, handle);
            updates = hub.drain_camera_state_updates();
            expect(updates.size() == 1 && !updates[0].vision_stable_planes &&
                       updates[0].vision_accumulation && updates[0].vision_denoise &&
                       updates[0].fields == (CameraStateUpdateField::VisionStablePlanes |
                                             CameraStateUpdateField::VisionAccumulation |
                                             CameraStateUpdateField::VisionDenoise |
                                             CameraStateUpdateField::VisionRenderMode),
                   "stable planes must merge independently with mode, accumulation and denoise");
            {
                auto camera = hub.camera_storage().acquire_write(handle);
                camera->vision_stable_planes = updates[0].vision_stable_planes;
                camera->vision_render_mode = updates[0].vision_render_mode;
            }
            hub.acknowledge_camera_vision_stable_planes(handle, updates[0].sequence);
            expect(!hub.requested_camera_vision_stable_planes(handle).has_value() &&
                       !Corona::API::get_requested_vision_stable_planes(handle) &&
                       Corona::API::get_vision_stable_planes(other),
                   "stable-plane acknowledgement must expose committed state without changing other cameras");
            Corona::API::set_vision_render_mode(mode, handle);
            updates = hub.drain_camera_state_updates();
            expect(updates.size() == 1 && updates[0].fields == CameraStateUpdateField::VisionRenderMode &&
                       !Corona::API::get_requested_vision_stable_planes(handle),
                   "algorithm changes must preserve the stable-plane preference");
        }
        Corona::API::set_vision_stable_planes(true, handle);
        hub.drain_camera_state_updates();
        Corona::API::set_vision_stable_planes(false, handle);
        hub.enqueue_camera_release({handle});
        expect(!hub.requested_camera_vision_stable_planes(handle).has_value() &&
                   hub.drain_camera_state_updates().empty(),
               "camera release must clear queued and in-flight stable-plane requests");
        hub.drain_camera_releases();
        hub.camera_storage().deallocate(handle);
        hub.camera_storage().deallocate(other);
    }
    static void check_denoiser_api() {
        auto& hub = SharedDataHub::instance();
        const auto handle = hub.camera_storage().allocate();
        const auto other = hub.camera_storage().allocate();
        expect(Corona::API::get_vision_denoiser(handle) == "scene" &&
                   !hub.requested_camera_vision_denoiser(handle).has_value(),
               "new cameras must inherit the scene denoiser without a pending override");
        Corona::API::set_vision_denoiser("ReBLUR", handle);
        Corona::API::set_vision_denoise(true, handle);
        expect(Corona::API::get_vision_denoiser(handle) == "scene" &&
                   Corona::API::get_requested_vision_denoiser(handle) == "reblur" &&
                   Corona::API::get_requested_vision_denoiser(other) == "scene",
               "denoiser requests must be camera-local and immediately persistable");
        auto updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].vision_denoiser == CameraVisionDenoiser::ReBLUR &&
                   updates[0].vision_denoise && updates[0].fields ==
                       (CameraStateUpdateField::VisionDenoiser | CameraStateUpdateField::VisionDenoise) &&
                   Corona::API::get_requested_vision_denoiser(handle) == "reblur",
               "coalescing and draining must retain the denoiser request while it is in flight");
        const auto in_flight = updates[0];
        Corona::API::set_vision_denoiser("svgf", handle);
        {
            auto camera = hub.camera_storage().acquire_write(handle);
            camera->vision_denoiser = in_flight.vision_denoiser;
            camera->vision_denoise = in_flight.vision_denoise;
        }
        hub.acknowledge_camera_vision_denoiser(handle, in_flight.sequence);
        hub.acknowledge_camera_vision_denoise(handle, in_flight.sequence);
        expect(Corona::API::get_vision_denoiser(handle) == "reblur" &&
                   Corona::API::get_requested_vision_denoiser(handle) == "svgf",
               "an older acknowledgement must not discard a newer denoiser request");
        updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].fields == CameraStateUpdateField::VisionDenoiser,
               "denoiser type changes must not change the enable switch");
        {
            auto camera = hub.camera_storage().acquire_write(handle);
            camera->vision_denoiser = updates[0].vision_denoiser;
        }
        hub.acknowledge_camera_vision_denoiser(handle, updates[0].sequence);
        expect(!hub.requested_camera_vision_denoiser(handle).has_value() &&
                   Corona::API::get_requested_vision_denoiser(handle) == "svgf" &&
                   Corona::API::get_vision_denoise(handle),
               "acknowledged denoiser preferences must fall back to committed state");
        Corona::API::set_vision_denoiser("invalid", handle);
        expect(hub.drain_camera_state_updates().empty(), "unknown denoisers must be ignored");
        Corona::API::set_vision_denoiser("scene", handle);
        hub.enqueue_camera_release({handle});
        expect(!hub.requested_camera_vision_denoiser(handle).has_value() &&
                   hub.drain_camera_state_updates().empty(),
               "camera release must clear pending denoiser preferences");
        hub.drain_camera_releases();
        hub.camera_storage().deallocate(handle);
        hub.camera_storage().deallocate(other);
    }
    static void check_reblur_settings_api() {
        auto& hub = SharedDataHub::instance();
        const auto camera = hub.camera_storage().allocate();
        const auto other = hub.camera_storage().allocate();
        Corona::API::set_vision_reblur_settings(R"({"max_history":12})", camera);
        Corona::API::set_vision_denoise(false, camera);
        auto updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].vision_reblur_settings == R"({"max_history":12})" &&
                   Corona::API::get_requested_vision_reblur_settings(camera) == R"({"max_history":12})" &&
                   Corona::API::get_requested_vision_reblur_settings(other) == "{}",
               "REBLUR preferences must remain camera-local and persistable while in flight");
        Corona::API::set_vision_reblur_settings(R"({"max_history":20})", camera);
        hub.acknowledge_camera_vision_reblur_settings(camera, updates[0].sequence);
        expect(Corona::API::get_requested_vision_reblur_settings(camera) == R"({"max_history":20})",
               "older acknowledgements must preserve newer REBLUR settings");
        updates = hub.drain_camera_state_updates();
        {
            auto state = hub.camera_storage().acquire_write(camera);
            state->vision_reblur_settings = updates[0].vision_reblur_settings;
        }
        hub.acknowledge_camera_vision_reblur_settings(camera, updates[0].sequence);
        expect(!hub.requested_camera_vision_reblur_settings(camera).has_value() &&
                   Corona::API::get_requested_vision_reblur_settings(camera) == R"({"max_history":20})",
               "saving after acknowledgement must use the committed camera values");
        Corona::API::set_vision_reblur_settings("{}", camera);
        hub.enqueue_camera_release({camera});
        expect(!hub.requested_camera_vision_reblur_settings(camera) && hub.drain_camera_state_updates().empty(),
               "camera release must discard REBLUR requests");
        hub.drain_camera_releases();
        hub.camera_storage().deallocate(camera);
        hub.camera_storage().deallocate(other);
    }
    static void check_relax_settings_api() {
        auto& hub = SharedDataHub::instance();
        const auto camera = hub.camera_storage().allocate();
        const auto other = hub.camera_storage().allocate();
        Corona::API::set_vision_denoiser("RELAX", camera);
        Corona::API::set_vision_relax_settings(R"({"diffuse_history":12})", camera);
        Corona::API::set_vision_denoise(false, camera);
        auto updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].vision_denoiser == CameraVisionDenoiser::ReLAX &&
                   Corona::API::get_requested_vision_denoiser(camera) == "relax" &&
                   updates[0].vision_relax_settings == R"({"diffuse_history":12})" &&
                   Corona::API::get_requested_vision_relax_settings(camera) == R"({"diffuse_history":12})" &&
                   Corona::API::get_requested_vision_relax_settings(other) == "{}",
               "RELAX preferences must remain camera-local and persistable while in flight");
        Corona::API::set_vision_relax_settings(R"({"diffuse_history":20})", camera);
        hub.acknowledge_camera_vision_relax_settings(camera, updates[0].sequence);
        expect(Corona::API::get_requested_vision_relax_settings(camera) == R"({"diffuse_history":20})",
               "older acknowledgements must preserve newer RELAX settings");
        updates = hub.drain_camera_state_updates();
        {
            auto state = hub.camera_storage().acquire_write(camera);
            state->vision_relax_settings = updates[0].vision_relax_settings;
        }
        hub.acknowledge_camera_vision_relax_settings(camera, updates[0].sequence);
        expect(!hub.requested_camera_vision_relax_settings(camera).has_value() &&
                   Corona::API::get_requested_vision_relax_settings(camera) == R"({"diffuse_history":20})",
               "saving after acknowledgement must use the committed camera values");
        Corona::API::set_vision_relax_settings("{}", camera);
        hub.enqueue_camera_release({camera});
        expect(!hub.requested_camera_vision_relax_settings(camera) && hub.drain_camera_state_updates().empty(),
               "camera release must discard RELAX requests");
        hub.drain_camera_releases();
        hub.camera_storage().deallocate(camera);
        hub.camera_storage().deallocate(other);
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
    static void check_common_path_settings(vision::Pipeline& pipeline, bool integer_type_required) {
        auto* integrator = pipeline.renderer().integrator().get();
        const auto original = integrator->settings();
        for (const auto& invalid : {
                vision::DataWrap{{"max_depth", -1}}, vision::DataWrap{{"max_depth", 129}},
                vision::DataWrap{{"max_depth", 1.5}}, vision::DataWrap{{"min_depth", 21}},
                vision::DataWrap{{"min_depth", false}}, vision::DataWrap{{"rr_threshold", 1.01}},
                vision::DataWrap{{"rr_threshold", "0.5"}},
                vision::DataWrap{{"rr_threshold", std::numeric_limits<double>::infinity()}},
                vision::DataWrap{{"rr_threshold", std::numeric_limits<double>::quiet_NaN()}},
                vision::DataWrap{{"max_depth", 4}, {"unknown", 1}}}) {
            expect(!integrator->set_settings(invalid) && integrator->settings() == original,
                   "common path settings must reject invalid inputs without partially applying them");
        }
        const bool changed = integrator->set_settings(vision::DataWrap{{"min_depth", 3.0}});
        if (integer_type_required) {
            expect(!changed && integrator->settings() == original,
                   "ReSTIR must retain its strict integer JSON input contract");
        } else {
            expect(changed && integrator->settings()["min_depth"] == 3,
                   "PT must retain support for mathematically integral numeric inputs");
            expect(!integrator->set_settings(vision::DataWrap{{"min_depth", 3}}),
                   "normalizing integral numeric input must preserve unchanged-value detection");
            expect(integrator->set_settings(vision::DataWrap::object()) && integrator->settings() == original,
                   "clearing common path overrides must restore the source scene defaults");
        }
        expect(integrator->set_settings(vision::DataWrap{{"rr_threshold", 0.2}}),
               "common RR settings must accept a non-exact float32 value");
        expect(!integrator->set_settings(vision::DataWrap{{"rr_threshold", 0.2}}) &&
                   !integrator->set_settings(vision::DataWrap{{"rr_threshold", integrator->settings()["rr_threshold"]}}),
               "reapplying normalized common RR settings must remain a no-op");
        expect(integrator->set_settings(vision::DataWrap::object()) && integrator->settings() == original,
               "clearing RR overrides must restore the original scene values");
    }
    static void check_pt_settings(OpticsSystem& system, vision::Pipeline& pipeline,
                                  vision::Pipeline* previous_pipeline = nullptr) {
        auto& hub = SharedDataHub::instance();
        const auto first = hub.camera_storage().allocate();
        const auto second = hub.camera_storage().allocate();
        const std::string settings = R"({"max_depth":0,"min_depth":2,"rr_threshold":0.5})";
        Corona::API::set_vision_pt_settings(settings, first);
        auto updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && Corona::API::get_requested_vision_pt_settings(first) == settings &&
                   Corona::API::get_requested_vision_pt_settings(second) == "{}",
               "PT preferences must be persistable while in flight and independent per camera");
        Corona::API::set_vision_pt_settings(R"({"max_depth":8})", first);
        hub.acknowledge_camera_vision_pt_settings(first, updates[0].sequence);
        expect(Corona::API::get_requested_vision_pt_settings(first) == R"({"max_depth":8})",
               "an older acknowledgement must not discard a newer PT preference");
        hub.clear_camera_state_updates(first);
        auto prepare = [&](std::uintptr_t handle, const std::string& json) {
            if (previous_pipeline) previous_pipeline->activate_global_context();
            expect(system.prepare_vision_camera_view(system.active_vision_runtime(), handle,
                       16, 16, false, true, true, CameraVisionDenoiser::Scene, "{}", "{}", json),
                   "PT camera settings must prepare a usable view");
        };
        prepare(second, "{}");
        render(pipeline, "PT-settings-second-camera");
        const auto other_history = pipeline.frame_index();
        prepare(first, "{}");
        render(pipeline, "PT-settings-before-edit");
        check_common_path_settings(pipeline, false);
        const auto framebuffer = pipeline.renderer().frame_buffer_sp();
        prepare(first, settings);
        expect(pipeline.frame_buffer() == framebuffer.get() && pipeline.frame_index() == 0,
               "PT edits must reset accumulated samples without recreating the view");
        auto effective = vision::DataWrap::parse(Corona::API::get_effective_vision_pt_settings(first));
        expect(effective["max_depth"] == 0 && effective["min_depth"] == 2 && effective["rr_threshold"] == 0.5,
               "all three PT parameters must reach the active integrator and runtime feedback");
        expect(effective["adaptive"]["available"] == false,
               "the inactive adaptive sampler must be reported as unavailable");
        render(pipeline, "PT-settings-zero-depth", false);
        const auto history = pipeline.frame_index();
        prepare(first, settings);
        expect(pipeline.frame_index() == history, "unchanged PT settings must preserve history");
        prepare(second, "{}");
        expect(pipeline.frame_index() == other_history, "editing PT must preserve another camera's history");
        render(pipeline, "PT-settings-second-camera-return");
        prepare(first, R"({"max_depth":1})");
        render(pipeline, "PT-settings-one-depth");
        prepare(first, "{}");
        render(pipeline, "PT-settings-restored-scene-depth");
        if (previous_pipeline) {
            std::vector<vision::float4> pixels(pipeline.pixel_num());
            pipeline.final_picture(pipeline.output_desc(), pixels.data());
            float red = 0.f, green = 0.f, blue = 0.f;
            for (const auto& pixel : pixels) {
                red += pixel.x; green += pixel.y; blue += pixel.z;
            }
            expect(red > 2.f * green && red > 2.f * blue,
                   "runtime PT recompilation must use the target scene's red light, not the previous scene's white light");
        }
        effective = vision::DataWrap::parse(Corona::API::get_effective_vision_pt_settings(first));
        expect(effective["max_depth"] == 2 && effective["min_depth"] == 5 && effective["rr_threshold"] == 1,
               "clearing camera overrides must restore source scene values");
        pipeline.activate_view_context(0);
        pipeline.remove_view_context(first);
        pipeline.remove_view_context(second);
        for (auto handle : {first, second}) {
            hub.clear_camera_state_updates(handle);
            hub.camera_storage().deallocate(handle);
        }
        std::cout << "PASS: camera-local PT parameters, history reset, GPU depth and scene defaults\n";
    }
    static void check_restir_settings(OpticsSystem& system, vision::Pipeline& pipeline) {
        auto& hub = SharedDataHub::instance();
        const auto first = hub.camera_storage().allocate();
        const auto second = hub.camera_storage().allocate();
        const std::string settings = R"({"max_depth":3,"min_depth":2,"rr_threshold":0.5,
          "direct":{"M_light":7,"M_bsdf":2,"max_age":12,"diffuse_factor":0.2,
            "temporal":{"open":false,"history_limit":4,"theta":25,"depth":0.2,"radius":6},
            "spatial":{"open":false,"theta":30,"depth":0.4,"radius":20}},
          "indirect":{"max_age":15,"diffuse_factor":0.4,
            "temporal":{"open":false,"history_limit":3,"theta":20,"depth":0.1,"radius":5},
            "spatial":{"open":false,"theta":40,"depth":0.5,"radius":10}}})";
        Corona::API::set_vision_restir_settings(settings, first);
        auto updates = hub.drain_camera_state_updates();
        expect(updates.size() == 1 && updates[0].fields == CameraStateUpdateField::VisionRestirSettings &&
                   Corona::API::get_requested_vision_restir_settings(first) == settings &&
                   Corona::API::get_requested_vision_restir_settings(second) == "{}",
               "ReSTIR settings must be queued independently per camera and persist while in flight");
        Corona::API::set_vision_restir_settings(R"({"direct":{"max_age":8}})", first);
        hub.acknowledge_camera_vision_restir_settings(first, updates[0].sequence);
        expect(Corona::API::get_requested_vision_restir_settings(first) == R"({"direct":{"max_age":8}})",
               "an old acknowledgement must retain a newer ReSTIR request");
        hub.clear_camera_state_updates(first);
        auto prepare = [&](std::uintptr_t camera, const std::string& json) {
            expect(system.prepare_vision_camera_view(system.active_vision_runtime(), camera,
                       16, 16, false, true, true, CameraVisionDenoiser::Scene, "{}", "{}", "{}", json),
                   "ReSTIR camera settings must prepare a usable view");
        };
        prepare(second, "{}");
        render(pipeline, "ReSTIR-settings-second-camera");
        const auto other_history = pipeline.frame_index();
        const auto defaults = pipeline.renderer().integrator()->settings();
        expect(defaults["direct"]["M_light"] == 10 && defaults["direct"]["pairwise"] == true &&
                   defaults["indirect"]["spatial"]["radius"] == 12,
               "ReSTIR UI feedback must include actual estimator defaults");
        prepare(first, "{}");
        render(pipeline, "ReSTIR-settings-before-edit");
        check_common_path_settings(pipeline, true);
        const auto framebuffer = pipeline.renderer().frame_buffer_sp();
        const auto initial_history = pipeline.frame_index();
        prepare(first, defaults.dump());
        {
            auto camera = hub.camera_storage().acquire_read(first);
            expect(camera->vision_restir_applied_settings == defaults.dump() &&
                       pipeline.frame_index() == initial_history,
                   "equivalent override inputs must publish readiness without resetting history");
        }
        prepare(first, R"({"direct.M_light":7})");
        {
            auto camera = hub.camera_storage().acquire_read(first);
            expect(camera->vision_restir_applied_settings == defaults.dump() &&
                       pipeline.frame_index() == initial_history,
                   "invalid direct engine inputs must not advance the applied marker or reset history");
        }
        prepare(first, settings);
        expect(pipeline.frame_buffer() == framebuffer.get() && pipeline.frame_index() == 0,
               "ReSTIR settings must reset only the target history without recreating the view");
        auto effective = vision::DataWrap::parse(Corona::API::get_effective_vision_restir_settings(first));
        expect(effective["max_depth"] == 3 && effective["min_depth"] == 2 && effective["rr_threshold"] == 0.5 &&
                   effective["direct"]["M_light"] == 7 && effective["direct"]["M_bsdf"] == 2 &&
                   effective["direct"]["temporal"]["open"] == false &&
                   effective["direct"]["temporal"]["history_limit"] == 4 &&
                   effective["direct"]["spatial"]["radius"] == 20 &&
                   effective["indirect"]["max_age"] == 15 &&
                   effective["indirect"]["temporal"]["theta"] == 20 &&
                   effective["indirect"]["spatial"]["radius"] == 10,
               "nested DI/GI settings must reach the active estimator and feedback");
        render(pipeline, "ReSTIR-settings-after-edit");
        const auto history = pipeline.frame_index();
        prepare(first, settings);
        expect(pipeline.frame_index() == history,
               "reapplying the same ReSTIR float32 values must preserve history");
        auto* integrator = pipeline.renderer().integrator().get();
        const auto before_invalid = integrator->settings();
        for (const char* invalid : {R"({"direct":{"max_age":101}})",
                R"({"direct":{"max_recursion":0}})", R"({"direct":{"M_light":1.5}})",
                R"({"indirect":{"temporal":{"open":1}}})", R"({"indirect":{"ratio":2}})",
                R"({"indirect":{"spatial":{"theta":91}}})", R"({"direct":{"diffuse_factor":-0.1}})",
                R"({"max_depth":4,"direct":{"unknown":true}})", R"({"direct":null})"}) {
            expect(!integrator->set_settings(vision::DataWrap::parse(invalid, nullptr, false)) &&
                       integrator->settings() == before_invalid,
                   "invalid nested ReSTIR requests must be rejected atomically");
        }
        prepare(second, "{}");
        expect(pipeline.frame_index() == other_history &&
                   pipeline.renderer().integrator()->settings() == defaults,
               "editing ReSTIR must preserve another camera's values and history");
        prepare(first, R"({"max_depth":0,"min_depth":20,"rr_threshold":0,
          "direct":{"pairwise":false,"max_recursion":1},"indirect":{"open":false}})");
        render(pipeline, "ReSTIR-settings-compiled-pairwise-and-recursion");
        effective = vision::DataWrap::parse(Corona::API::get_effective_vision_restir_settings(first));
        expect(effective["max_depth"] == 0 && effective["direct"]["pairwise"] == false &&
                   effective["direct"]["max_recursion"] == 1,
               "compiled ReSTIR settings and zero-depth boundary must apply");
        prepare(first, R"({"max_depth":0,"direct":{"M_light":0,"M_bsdf":0,"pairwise":false,
          "max_recursion":1},"indirect":{"open":false}})");
        render(pipeline, "ReSTIR-settings-zero-candidates", false);
        // Keep every other compiled setting unchanged so it cannot hide a
        // missing zero-to-positive candidate-generator specialization update.
        prepare(first, R"({"max_depth":0,"direct":{"M_light":7,"M_bsdf":2,"pairwise":false,
          "max_recursion":1},"indirect":{"open":false}})");
        render(pipeline, "ReSTIR-settings-restored-candidates");
        prepare(first, R"({"direct":{"open":false},"indirect":{"open":false}})");
        render(pipeline, "ReSTIR-settings-disabled-contributions", false);
        std::vector<vision::float4> pixels(pipeline.pixel_num());
        pipeline.final_picture(pipeline.output_desc(), pixels.data());
        for (const auto& pixel : pixels) {
            expect(std::isfinite(pixel.x) && std::abs(pixel.x) < 1e-6f &&
                       std::abs(pixel.y) < 1e-6f && std::abs(pixel.z) < 1e-6f,
                   "disabling DI and GI must remove their actual GPU output");
        }
        prepare(first, "{}");
        expect(vision::DataWrap::parse(Corona::API::get_effective_vision_restir_settings(first)) == defaults,
               "clearing ReSTIR overrides must restore source scene defaults");
        render(pipeline, "ReSTIR-settings-restored-defaults");
        pipeline.activate_view_context(0);
        pipeline.remove_view_context(first);
        pipeline.remove_view_context(second);
        for (auto camera : {first, second}) {
            hub.clear_camera_state_updates(camera);
            hub.camera_storage().deallocate(camera);
        }
        std::cout << "PASS: camera-local ReSTIR settings, GPU contributions, recompilation and history\n";
    }
    static void check_pt_settings_with_material_lut(
        OpticsSystem& system, const OpticsSystem::VisionSceneLoadRequest& source) {
        auto previous_pipeline = vision::Global::instance().pipeline_shared();
        auto request = source;
        request.scene_key += ".principled";
        auto data = vision::DataWrap::parse(request.scene_json);
        data["scene"]["materials"][0] = {
            {"type", "principled_bsdf"}, {"name", "mat"},
            {"param", {{"color", {0.8f, 0.8f, 0.8f}}, {"roughness", 0.4f}}}};
        data["scene"]["lights"][0]["param"]["color"] = {1.f, 0.f, 0.f};
        request.scene_json = data.dump();
        expect(system.load_external_vision_scene_from_json(request, CameraVisionRenderMode::PathTracing),
               "PT material-LUT scene must initialize");
        expect(vision::Global::instance().scene_bindless_array() == nullptr,
               "runtime PT edits must be tested outside pipeline compilation's scene scope");
        check_pt_settings(system, *vision::Global::instance().pipeline_shared(), previous_pipeline.get());
        expect(system.load_external_vision_scene_from_json(source, CameraVisionRenderMode::PathTracing),
               "PT material-LUT test must restore the original scene");
        std::cout << "PASS: runtime PT edits with a material lookup table\n";
    }
    static void check_independent_denoiser(OpticsSystem& system, vision::Pipeline& pipeline,
                                          const char* algorithm) {
        auto& camera_storage = SharedDataHub::instance().camera_storage();
        const auto first_camera = camera_storage.allocate();
        const auto second_camera = camera_storage.allocate();
        auto prepare = [&](std::uintptr_t camera, CameraVisionDenoiser type, bool enabled = true) {
            expect(system.prepare_vision_camera_view(system.active_vision_runtime(), camera,
                       16, 16, enabled, false, true, type), "denoiser selection must prepare a usable view");
            auto* integrator = dynamic_cast<vision::IlluminationIntegrator*>(pipeline.renderer().integrator().get());
            expect(integrator && integrator->impl_type() == algorithm &&
                       integrator->denoiser()->enabled() == enabled &&
                       integrator->denoiser()->requires_lobe_signals() ==
                           (type == CameraVisionDenoiser::ReBLUR || type == CameraVisionDenoiser::ReLAX),
                   "camera selection must control the actual plugin while preserving the integrator");
        };
        prepare(first_camera, CameraVisionDenoiser::SVGF);
        render(pipeline, "denoiser-camera-401-SVGF");
        const auto first_framebuffer = pipeline.renderer().frame_buffer_sp();
        prepare(second_camera, CameraVisionDenoiser::SVGF);
        render(pipeline, "denoiser-camera-402-SVGF");
        const auto other_history = pipeline.frame_index();
        const auto other_framebuffer = pipeline.renderer().frame_buffer_sp();
        prepare(first_camera, CameraVisionDenoiser::ReBLUR);
        expect(pipeline.frame_buffer() != first_framebuffer.get() && pipeline.frame_index() == 0,
               "changing denoiser type must rebuild only that camera with fresh history");
        render(pipeline, "denoiser-camera-401-REBLUR");
        const auto reblur_history = pipeline.frame_index();
        const auto reblur_framebuffer = pipeline.renderer().frame_buffer_sp();
        prepare(first_camera, CameraVisionDenoiser::ReBLUR);
        expect(pipeline.frame_buffer() == reblur_framebuffer.get() && pipeline.frame_index() == reblur_history,
               "reapplying the same denoiser must preserve compiled kernels and history");
        const std::string parameters = R"({"max_history":12,"fast_history":4,"history_fix_frames":2,"history_fix_stride":8,"diffuse_prepass":0,"specular_prepass":25,"min_radius":2,"max_radius":20,"lobe_fraction":0.25,"roughness_fraction":0.5,"plane_sensitivity":0.125,"min_hit_weight":0.5,"stabilization":0.5,"reconstruction":2,"anti_firefly":false,"antilag_sigma":2,"antilag_sensitivity":1})";
        expect(system.prepare_vision_camera_view(system.active_vision_runtime(), first_camera,
                   16, 16, true, false, true, CameraVisionDenoiser::ReBLUR, parameters),
               "REBLUR parameters must update a live view");
        expect(pipeline.frame_buffer() == reblur_framebuffer.get() && pipeline.frame_index() == 0,
               "parameter edits must reset history without recreating the framebuffer");
        auto* filter = dynamic_cast<vision::IlluminationIntegrator*>(pipeline.renderer().integrator().get())->denoiser();
        const auto applied = vision::DataWrap::parse(filter->reblur_settings_json());
        expect(applied == vision::DataWrap::parse(parameters),
               "all seventeen editor values must reach the actual REBLUR filter");
        vision::DenoiserDesc source_desc{"ReBLUR"};
        for (const auto& [key, value] : applied.items()) source_desc.set_value(key, value);
        const auto source_filter = vision::Node::create_shared<vision::Denoiser>(source_desc);
        expect(vision::DataWrap::parse(source_filter->reblur_settings_json()) == applied,
               "scene descriptors must load all exposed parameters");
        source_filter->set_reblur_settings(R"({"max_history":7})");
        const auto overridden = vision::DataWrap::parse(source_filter->reblur_settings_json());
        expect(overridden["max_history"] == 7 && overridden["reconstruction"] == 2 && overridden["antilag_sigma"] == 2,
               "partial camera overrides must retain scene values");
        source_filter->set_reblur_settings("{}");
        expect(vision::DataWrap::parse(source_filter->reblur_settings_json()) == applied,
               "clearing overrides must restore source settings");
        render(pipeline, "REBLUR-edited-parameters");
        const auto edited_history = pipeline.frame_index();
        system.prepare_vision_camera_view(system.active_vision_runtime(), first_camera,
            16, 16, true, false, true, CameraVisionDenoiser::ReBLUR, parameters);
        expect(pipeline.frame_index() == edited_history, "unchanged parameters must preserve temporal history");
        prepare(second_camera, CameraVisionDenoiser::SVGF);
        expect(pipeline.frame_buffer() == other_framebuffer.get() && pipeline.frame_index() == other_history,
               "switching one camera's denoiser must preserve the other camera's history");
        prepare(first_camera, CameraVisionDenoiser::SVGF, false);
        expect(pipeline.frame_index() == 0, "switching back to SVGF must reset REBLUR history");
        render(pipeline, "denoiser-camera-401-raw-SVGF");
        prepare(first_camera, CameraVisionDenoiser::ReBLUR, false);
        render(pipeline, "denoiser-camera-401-raw-REBLUR");
        const auto disabled_framebuffer = pipeline.renderer().frame_buffer_sp();
        prepare(first_camera, CameraVisionDenoiser::ReBLUR);
        expect(pipeline.frame_buffer() == disabled_framebuffer.get() && pipeline.frame_index() == 0,
               "enabling a REBLUR view created while disabled must reuse its lobe kernels with fresh history");
        render(pipeline, "denoiser-camera-401-REBLUR-enabled");
        prepare(first_camera, CameraVisionDenoiser::ReLAX);
        render(pipeline, "denoiser-camera-401-RELAX");
        const auto relax_framebuffer = pipeline.renderer().frame_buffer_sp();
        const auto default_relax_history = pipeline.frame_index();
        auto* default_relax = dynamic_cast<vision::IlluminationIntegrator*>(pipeline.renderer().integrator().get())->denoiser();
        system.prepare_vision_camera_view(system.active_vision_runtime(), first_camera,
            16, 16, true, false, true, CameraVisionDenoiser::ReLAX, "{}", default_relax->relax_settings_json());
        expect(pipeline.frame_index() == default_relax_history,
               "applying unchanged effective RELAX defaults must preserve history even when JSON changes");
        const std::string relax_parameters = R"({"diffuse_history":12,"specular_history":16,"diffuse_fast_history":4,"specular_fast_history":5,"history_fix_frames":2,"history_fix_stride":8,"history_fix_normal_power":4,"variance_history_threshold":2,"diffuse_prepass":0,"specular_prepass":25,"min_hit_weight":0.5,"atrous_iterations":3,"diffuse_phi":4,"specular_phi":2,"lobe_fraction":0.25,"roughness_fraction":0.5,"specular_variance_boost":1,"specular_lobe_slack":0.125,"color_box_sigma":3,"depth_threshold":0.03125,"luminance_relaxation":0.75,"normal_relaxation":0.5,"roughness_relaxation":0.25,"antilag_acceleration":0.5,"antilag_spatial_sigma":2,"antilag_temporal_sigma":1,"antilag_reset":0.75,"anti_firefly":true,"roughness_stopping":false,"reconstruction":1})";
        expect(system.prepare_vision_camera_view(system.active_vision_runtime(), first_camera,
                   16, 16, true, false, true, CameraVisionDenoiser::ReLAX, parameters, relax_parameters),
               "RELAX parameters must update a live view");
        expect(pipeline.frame_buffer() == relax_framebuffer.get() && pipeline.frame_index() == 0,
               "RELAX parameter edits must reset history without recreating the framebuffer");
        filter = dynamic_cast<vision::IlluminationIntegrator*>(pipeline.renderer().integrator().get())->denoiser();
        const auto relax_applied = vision::DataWrap::parse(filter->relax_settings_json());
        expect(filter->impl_type() == "ReLAX" && relax_applied == vision::DataWrap::parse(relax_parameters),
               "all thirty editor values must reach the actual RELAX filter independently of REBLUR");
        vision::DenoiserDesc relax_desc{"ReLAX"};
        for (const auto& [key, value] : relax_applied.items()) relax_desc.set_value(key, value);
        const auto relax_source = vision::Node::create_shared<vision::Denoiser>(relax_desc);
        expect(vision::DataWrap::parse(relax_source->relax_settings_json()) == relax_applied,
               "RELAX scene descriptors must load all exposed parameters");
        relax_source->set_relax_settings(R"({"diffuse_history":7})");
        const auto relax_partial = vision::DataWrap::parse(relax_source->relax_settings_json());
        expect(relax_partial["diffuse_history"] == 7 && relax_partial["specular_history"] == 16 &&
                   relax_partial["reconstruction"] == 1 && relax_partial["roughness_stopping"] == false,
               "partial RELAX camera overrides must retain scene values");
        relax_source->set_relax_settings("{}");
        expect(vision::DataWrap::parse(relax_source->relax_settings_json()) == relax_applied,
               "clearing RELAX overrides must restore source settings");
        render(pipeline, "RELAX-edited-parameters");
        const auto relax_history = pipeline.frame_index();
        system.prepare_vision_camera_view(system.active_vision_runtime(), first_camera,
            16, 16, true, false, true, CameraVisionDenoiser::ReLAX, parameters,
            vision::DataWrap::parse(relax_parameters).dump(2));
        expect(pipeline.frame_index() == relax_history,
               "unchanged RELAX parameters must preserve temporal history");
        prepare(second_camera, CameraVisionDenoiser::SVGF);
        expect(pipeline.frame_buffer() == other_framebuffer.get() && pipeline.frame_index() == other_history,
               "RELAX edits must preserve the other camera's history");
        prepare(first_camera, CameraVisionDenoiser::Scene);
        render(pipeline, "denoiser-camera-401-scene-default");
        pipeline.activate_view_context(0);
        pipeline.set_output_denoise(false);
        pipeline.commit_command();
        pipeline.remove_view_context(first_camera);
        pipeline.remove_view_context(second_camera);
        camera_storage.deallocate(first_camera);
        camera_storage.deallocate(second_camera);
    }
    static void check_independent_stable_planes(OpticsSystem& system, vision::Pipeline& pipeline) {
        auto prepare = [&](std::uintptr_t handle, bool enabled) {
            expect(system.prepare_vision_camera_view(system.active_vision_runtime(), handle,
                       16, 16, false, false, enabled), "stable-plane view must prepare");
            expect(pipeline.renderer().integrator()->stable_planes_enabled() == enabled &&
                       pipeline.renderer().integrator()->impl_type() == "rt" &&
                       !pipeline.output_desc().denoise && !pipeline.frame_buffer()->enable_accumulation(),
                   "stable-plane toggles must preserve ReSTIR, denoise and accumulation settings");
        };
        prepare(301, true);
        render(pipeline, "stable-planes-camera-301-on");
        const auto first_history = pipeline.frame_index();
        const auto first_integrator = pipeline.renderer().integrator();
        const auto first_framebuffer = pipeline.renderer().frame_buffer_sp();
        prepare(302, false);
        render(pipeline, "stable-planes-camera-302-off");
        const auto other_history = pipeline.frame_index();
        prepare(301, true);
        expect(pipeline.frame_index() == first_history,
               "another camera's stable-plane preference must preserve this camera's history");
        prepare(301, false);
        expect(pipeline.frame_index() == 0 && pipeline.renderer().integrator() == first_integrator &&
                   pipeline.frame_buffer() == first_framebuffer.get(),
               "stable-plane toggle must reset history while retaining the renderer");
        render(pipeline, "stable-planes-camera-301-off");
        const auto disabled_history = pipeline.frame_index();
        prepare(301, false);
        expect(pipeline.frame_index() == disabled_history,
               "reapplying the same stable-plane preference must retain history");
        prepare(302, false);
        expect(pipeline.frame_index() == other_history,
               "toggling one camera must preserve another camera's ReSTIR history");
        prepare(301, true);
        expect(pipeline.frame_index() == 0 && pipeline.renderer().integrator() == first_integrator,
               "re-enabling stable planes must reuse the renderer and clear stale history");
        render(pipeline, "stable-planes-camera-301-reenabled");
        pipeline.activate_view_context(0);
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
        ocarina::DynamicModule::clear_search_path();
        ocarina::RHIContext::instance().init(fs::current_path());
        static auto device = ocarina::RHIContext::instance().create_device();
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
    static void profile_switches(const char* scene, bool denoise, unsigned binding_padding = 0) {
        namespace profile = vision::switch_profile;
        ocarina::DynamicModule::clear_search_path();
        ocarina::RHIContext::instance().init(std::filesystem::current_path());
        static auto device = ocarina::RHIContext::instance().create_device();
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
        std::vector<ocarina::Buffer<unsigned>> padding_buffers;
        std::vector<const vision::Pipeline*> padded_pipelines;
        const auto step = [&](CameraVisionRenderMode mode, const char* label) {
            profile::Scope switching{label, "switch"};
            ocarina::SP<vision::Pipeline> pipeline;
            {
                profile::Scope preparation{"switch.prepare", "prepare"};
                expect(system.load_external_vision_scene(scene, mode, Vision::VisionPipelineSource::ExternalLive),
                       "profile scene must load");
                pipeline = vision::Global::instance().pipeline_shared();
                if (binding_padding != 0 &&
                    std::find(padded_pipelines.begin(), padded_pipelines.end(), pipeline.get()) == padded_pipelines.end()) {
                    for (unsigned i = 0; i < binding_padding; ++i) {
                        padding_buffers.push_back(device.create_buffer<unsigned>(1, "cache regression padding"));
                        (void)pipeline->bindless_array().emplace(padding_buffers.back());
                    }
                    padded_pipelines.push_back(pipeline.get());
                    std::cout << "BINDING_PADDING " << label << " slots=" << binding_padding << std::endl;
                }
                const auto res = pipeline->resolution();
                std::cout << "PROFILE_EXTENT " << res.x << 'x' << res.y << std::endl;
                expect(system.prepare_vision_camera_view(system.active_vision_runtime(), handle,
                           res.x, res.y, denoise, false), "profile view must prepare");
                std::cout << "BINDING_SLOTS " << label
                          << " visibility=" << pipeline->frame_buffer()->visibility_buffer_base()
                          << " surfaces=" << pipeline->frame_buffer()->surfaces_base() << std::endl;
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
        ocarina::DynamicModule::clear_search_path();
        ocarina::RHIContext::instance().init(fs::current_path());
        static auto device = ocarina::RHIContext::instance().create_device();
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
        static auto device = ocarina::RHIContext::instance().create_device();
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
        ocarina::DynamicModule::clear_search_path();
        ocarina::RHIContext::instance().init(original_cwd);
        // Keep the device alive until process exit: Vision's global kernel tools own GPU state.
        static auto device = ocarina::RHIContext::instance().create_device();
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
        check_pt_settings(system, *pt);
        check_pt_settings_with_material_lut(system, request);
        check_independent_accumulation(system, *pt, "pt");
        check_independent_denoiser(system, *pt, "pt");
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
        auto base_framebuffer = restir->renderer().frame_buffer_sp();
        expect(base_framebuffer->stable_planes_size() == 0u,
               "unused embedded base view must not allocate full-resolution ReSTIR planes");
        expect(restir->create_view_context(98, vision::make_uint2(16, 16)),
               "camera must render before the deferred base view allocates its resources");
        render(*restir, "ReSTIR-view-before-base");
        expect(base_framebuffer->stable_planes_size() == 0u,
               "rendering a camera must keep unused base-view resources deferred");
        restir->activate_view_context(0);
        restir->change_resolution(vision::make_uint2(20, 18));
        expect(vision::all(base_framebuffer->resolution() == vision::make_uint2(20, 18)) &&
                   base_framebuffer->stable_planes_size() == 0u,
               "resizing an unused base view must preserve deferred ReSTIR resources");
        restir->activate_view_context(98);
        render(*restir, "ReSTIR-view-after-deferred-base-resize");
        restir->activate_view_context(0);
        restir->change_resolution(vision::make_uint2(16, 16));
        restir->remove_view_context(98);
        render(*restir, "ReSTIR");
        expect(base_framebuffer->stable_planes_size() == 16u * 16u * vision::StablePlaneCount,
               "first explicit base render must prepare its deferred ReSTIR resources");
        base_framebuffer.reset();
        check_stationary_history_reset(*restir);
        check_independent_denoise(system, *restir, "rt", false);
        check_independent_denoiser(system, *restir, "rt");
        check_independent_accumulation(system, *restir, "rt");
        check_independent_stable_planes(system, *restir);
        check_restir_settings(system, *restir);
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
        // Keep this fixture independent of the example engine's build layout.
        const auto alternate_cwd = base / "alternate-cwd";
        fs::create_directories(alternate_cwd);
        fs::copy(original_cwd / "slang", alternate_cwd / "slang",
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        fs::current_path(alternate_cwd);
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
        check_svgf_layered_history(*svgf);
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
        expect(fs::canonical(alternate_cwd).parent_path() == fs::canonical(base),
               "alternate working directory must remain inside the test fixture");
        fs::remove_all(alternate_cwd);
        fs::remove(base);
        std::cout << "PASS: PT/ReSTIR/SVGF mode switching and history reset\n";
    }
};
}
int main(int argc, char** argv) {
    // Test devices are function-static; release global texture owners before
    // those devices are destroyed, including when a test throws.
    struct RuntimeCleanup {
        ~RuntimeCleanup() {
            vision::Global::instance().set_pipeline(nullptr);
            vision::ImagePool::destroy_instance();
            vision::MaterialLut::destroy_instance();
            vision::Global::instance().set_device(nullptr);
        }
    } runtime_cleanup;
    try {
        Corona::Systems::VisionEmbeddedModeSwitchTest::check_mode_api();
        Corona::Systems::VisionEmbeddedModeSwitchTest::check_accumulation_api();
        Corona::Systems::VisionEmbeddedModeSwitchTest::check_stable_planes_api();
        Corona::Systems::VisionEmbeddedModeSwitchTest::check_denoiser_api();
        Corona::Systems::VisionEmbeddedModeSwitchTest::check_reblur_settings_api();
        Corona::Systems::VisionEmbeddedModeSwitchTest::check_relax_settings_api();
        Corona::Systems::VisionEmbeddedModeSwitchTest::check_frame_timing();
        if (argc == 2 && std::string(argv[1]) == "--scene-asset-reuse") {
            Corona::Systems::VisionEmbeddedModeSwitchTest::check_scene_asset_reuse();
        } else if ((argc == 3 || (argc == 4 && std::string(argv[3]) == "--denoise")) &&
            std::string(argv[1]) == "--profile-switches") {
            Corona::Systems::VisionEmbeddedModeSwitchTest::profile_switches(
                argv[2], argc == 4);
        } else if (argc == 4 && std::string(argv[1]) == "--profile-bindings") {
            const auto padding = std::stoul(argv[3]);
            if (padding > 256) throw std::invalid_argument("binding padding must be in [0, 256]");
            Corona::Systems::VisionEmbeddedModeSwitchTest::profile_switches(
                argv[2], false, static_cast<unsigned>(padding));
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
