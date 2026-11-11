#pragma once

// The editor scene state the Web and ScriptRuntime contracts both read.
//
// These types used to live in the anonymous namespace of the API handler translation
// unit, which made the ~4,000 lines of scene logic unreachable from anywhere else. They
// are plain values: no CEF, no V8, no route strings beyond the scene file name.

#include "camera_window_lifetime.h"
#include "cef_scene_operations.h"

#include <corona/engine/engine_runtime_api.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

struct NativeEditorCamera {
    // First for move assignment: retire the overwritten identity before its
    // engine camera is released. Vector relocation transfers the same lease.
    CameraWindowLifetime camera_window_lifetime;
    NativeEditorCamera() = default;
    NativeEditorCamera(NativeEditorCamera&&) noexcept = default;
    NativeEditorCamera& operator=(NativeEditorCamera&&) noexcept = default;
    ~NativeEditorCamera() { camera_window_lifetime.retire(); }
    std::shared_ptr<CameraWindowState> window_state = std::make_shared<CameraWindowState>();
    std::string camera_id;
    std::string name;
    bool deletable{true};
    std::string vision_spp;
    std::string vision_max_depth;
    int width{1920};
    int height{1080};
    bool view_open{false};
    int view_x{120};
    int view_y{120};
    int view_width{960};
    int view_height{540};
    float move_speed{1.0f};
    std::unique_ptr<Corona::API::Camera> engine_camera;
};

struct NativeEditorActorOpticsState {
    std::optional<std::array<float, 3>> diffuse;
    std::optional<float> metallic;
    std::optional<float> roughness;
    std::optional<float> specular;
    std::optional<float> shininess;
    std::optional<std::array<float, 3>> emission;
    std::string texture;
    nlohmann::json material = nlohmann::json::object();
};

enum class ActorLoadStatus {
    Loaded,
    MissingResource,
    DecodeFailed,
    UnsupportedResource,
};


struct NativeEditorActor {
    std::optional<std::array<float, 16>> authored_matrix;
    Corona::Resource::ModelImportOptions import_options;
    std::string name;
    std::string actor_guid;
    std::string route;
    std::string actor_type{"actor"};
    std::string runtime_entity_id;
    std::string asset_id;
    std::string model_ref;
    std::string entity_type;
    std::string semantic_role;
    std::string source_plan_id;
    std::string source_batch_id;
    int source_scene_version{1};
    int actor_version{1};
    bool follow_camera{false};
    bool camera_lock_enabled{false};
    std::array<float, 3> camera_lock_position_offset{0.0f, 0.0f, 2.0f};
    std::array<float, 3> camera_lock_rotation_offset{0.0f, 0.0f, 0.0f};
    std::uint64_t audio_resource_id{0};  // 音频物体绑定的音频资源 id（actor_type=="audio"）
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    ActorLoadStatus load_status{ActorLoadStatus::Loaded};
    std::string load_error_code;
    std::string load_error_message;
    std::string resolved_asset_path;
    bool persisted_visible{true};
    std::string persisted_body_type{"dynamic"};
    nlohmann::json persisted_snapshot = nlohmann::json::object();
    nlohmann::json persisted_ik_chains = nlohmann::json::array();
    std::vector<std::string> persisted_animation_sources;
    std::uint64_t animation_revision{};
    nlohmann::json persisted_model_node_overrides = nlohmann::json::array();
    std::string model_node_resource;
    std::string model_node_current_resource;
    nlohmann::json model_node_unmatched = nlohmann::json::array();
    NativeEditorActorOpticsState persisted_optics;
    std::unique_ptr<Corona::API::Geometry> geometry;
    std::unique_ptr<Corona::API::Optics> optics;
    std::unique_ptr<Corona::API::Mechanics> mechanics;
    std::unique_ptr<Corona::API::Acoustics> acoustics;
    std::unique_ptr<Corona::API::Actor> engine_actor;
};




struct NativeEditorScene {
    ~NativeEditorScene() {
        for (auto& camera : cameras) camera.camera_window_lifetime.retire();
    }
    std::string archive_session;
    std::uint64_t tree_revision{1};
    // Modal dialogs can dispatch scene replacement even with a reused session.
    const std::shared_ptr<const int> identity = std::make_shared<const int>(0);
    std::optional<ActorSelection> selection;
    bool read_only{false};
    bool frame_when_ready{false};
    std::filesystem::path project_root;
    std::string route;
    std::string name;
    std::string core_version;
    std::string script_path;
    std::string terrain_type;
    std::string terrain_path;
    std::array<float, 3> sun_direction{1.0f, 1.0f, 1.0f};
    bool sun_enabled{true};
    nlohmann::json authored_lighting;
    bool floor_grid_enabled{true};
    std::vector<NativeEditorActor> actors;
    std::vector<NativeEditorCamera> cameras;
    nlohmann::json load_diagnostics = nlohmann::json::array();
    size_t active_camera_index{0};
    std::unique_ptr<Corona::API::Environment> environment;
    std::unique_ptr<Corona::API::Scene> engine_scene;
};

struct NativeEditorState {
    std::string project_path;
    std::unique_ptr<NativeEditorScene> scene;
};



/// The process-global editor scene.
///
/// Deliberately never destroyed: tearing it down from the CRT's atexit table runs
/// `Corona::API::Camera`'s destructor, which enqueues a camera release through
/// `SharedDataHub` - and destruction order between the two translation units is
/// unspecified, so the hub can already be gone (see the CameraWindowIdentityTests
/// teardown crash). The definition keeps the leak; this is only the declaration.
NativeEditorState& native_editor_state();

}  // namespace Corona::Systems::UI
