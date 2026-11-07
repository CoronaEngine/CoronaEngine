#pragma once

#include "scene_tree_actor_row.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

// Owned native values. No V8 handle, JSON value or serialized envelope crosses
// the scene-owner/context task-runner boundary.
struct ActorSelection {
    std::string scene;
    std::string actor_type;
    std::string actor;
    std::string source_viewport = "main";
    std::string source_camera_id;
    std::optional<std::uint32_t> node_index;
    std::optional<std::string> actor_guid;
};

struct ActiveSceneSnapshot {
    std::string archive_session;
    std::string scene;
    std::string name;
    std::string project_path;
    bool read_only = false;
    std::uint64_t tree_revision = 0;
    std::optional<ActorSelection> selection;
};

struct ActorTransformPatch {
    std::string scene;
    std::string actor;
    std::optional<std::array<float, 3>> position;
    std::optional<std::array<float, 3>> rotation;
    std::optional<std::array<float, 3>> scale;
};
struct ActorTransformSnapshot {
    std::string scene;
    std::string actor;
    std::string actor_guid;
    int actor_version = 0;
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
    std::array<float, 3> scale{};
};

struct ActorIndexEntry {
    std::string actor_guid, name, type;
};
struct ActorIndexSnapshot {
    std::string archive_session, scene;
    std::uint64_t tree_revision = 0;
    std::vector<ActorIndexEntry> actors;
};

struct ActorMechanicsPatch {
    std::optional<float> mass, restitution, damping;
    std::optional<std::string> body_type;
    std::optional<std::array<bool, 3>> linear_lock, angular_lock;
    bool empty() const { return !mass && !restitution && !damping && !body_type && !linear_lock && !angular_lock; }
};
struct ActorCameraLockPatch {
    bool enabled = false;
    std::optional<std::array<float, 3>> position_offset, rotation_offset;
};
struct ActorPropertiesPatch {
    std::string scene, actor;
    std::optional<bool> visible, follow_camera;
    std::optional<ActorMechanicsPatch> mechanics;
    std::optional<ActorCameraLockPatch> camera_lock;
    std::optional<std::vector<std::string>> animation_sources;
    bool transient_assets = false; // Non-Web ScriptRuntime imports remain memory-only.
};
struct ActorMechanicsSnapshot {
    float mass = 1, restitution = 0.8f, damping = 0.99f;
    std::string body_type;
    std::array<bool, 3> linear_lock{}, angular_lock{};
};
struct ActorCameraLockSnapshot {
    bool enabled = false;
    std::array<float, 3> position_offset{}, rotation_offset{};
};
struct ActorPropertiesSnapshot {
    std::string scene, actor, actor_guid;
    int actor_version = 0;
    bool follow_camera = false;
    std::optional<bool> visible;
    std::optional<ActorMechanicsSnapshot> mechanics;
    ActorCameraLockSnapshot camera_lock;
    std::optional<std::vector<std::string>> requested_animation_sources;
};

// Property-panel projection, deliberately excluding bounds, materials, model
// nodes and animation state. Reuses the same property values as write replies.
struct ActorReadSnapshot {
    ActorPropertiesSnapshot properties;
    std::string archive_session, actor_type, model, body_type, load_status;
    std::string load_error_code, load_error_message, resolved_asset_path;
    std::uint64_t handle = 0;
    std::array<float, 3> position{}, rotation{}, scale{1, 1, 1};
};

struct EnvironmentSunPatch {
    std::optional<bool> enabled;
    std::optional<std::array<float, 3>> direction;
    bool empty() const { return !enabled && !direction; }
};
struct EnvironmentPhysicsPatch {
    std::optional<std::array<float, 3>> gravity;
    std::optional<float> floor_y, floor_restitution, fixed_dt;
    bool empty() const { return !gravity && !floor_y && !floor_restitution && !fixed_dt; }
};
struct EnvironmentPatch {
    std::string scene;
    std::optional<EnvironmentSunPatch> sun;
    std::optional<bool> grid_enabled;
    std::optional<EnvironmentPhysicsPatch> physics;
};
struct EnvironmentPhysicsSnapshot {
    std::array<float, 3> gravity{};
    float floor_y = 0, floor_restitution = 0, fixed_dt = 0;
};
struct EnvironmentSnapshot {
    std::string archive_session, scene;
    bool sun_enabled = true, grid_enabled = true;
    std::array<float, 3> sun_direction{};
    EnvironmentPhysicsSnapshot physics;
};

struct ActorFocusRequest {
    std::string scene, actor, camera_id;
};
struct CameraPoseSnapshot {
    std::string camera_id, name;
    std::uint64_t handle = 0;
    std::array<float, 3> position{}, forward{}, world_up{};
    float fov = 45;
};
struct ActorFocusSnapshot {
    std::string archive_session, scene, actor, actor_guid;
    std::array<float, 3> center{};
    float distance = 0;
    CameraPoseSnapshot camera;
};

struct ScreenshotRequest {
    std::string scene, camera_id;
    std::optional<std::string> path; // Omitted: choose a PNG path interactively, then capture once.
};
struct ScreenshotSnapshot {
    std::string status, archive_session, scene, camera_id, path;
};

struct CameraPropertiesPatch {
    std::string scene, camera_id;
    std::optional<std::array<float, 3>> position, forward, world_up;
    std::optional<float> fov, projection_x_sign, near_plane, far_plane, move_speed;
    std::optional<int> width, height, view_x, view_y, view_width, view_height;
    std::optional<bool> view_open;
    bool has_pose() const { return position || forward || world_up || fov || projection_x_sign || near_plane || far_plane; }
    bool has_view() const { return view_open || view_x || view_y || view_width || view_height || move_speed; }
    bool empty() const { return !has_pose() && !has_view() && !width && !height; }
};
struct CameraPropertiesSnapshot : CameraPoseSnapshot {
    std::string archive_session, scene;
    float projection_x_sign = 1, near_plane = 0.1f, far_plane = 100, move_speed = 1;
    int width = 1920, height = 1080, view_x = 120, view_y = 120, view_width = 960, view_height = 540;
    bool view_open = false;
};

struct CameraCreateRequest {
    std::string scene;
    std::optional<std::string> name;
};

struct CameraReadSnapshot {
    CameraPropertiesSnapshot properties;
    std::string output_mode, render_backend, vision_render_mode, vision_denoiser;
    bool vision_accumulation = false, vision_denoise = true, vision_stable_planes = true, deletable = true;
};
struct CameraListSnapshot {
    std::string archive_session, scene, active_camera_id;
    std::vector<CameraReadSnapshot> cameras;
};
struct SceneTreeSnapshot {
    CameraListSnapshot cameras;
    std::uint64_t tree_revision = 0;
    std::vector<Corona::Ui::Cef::SceneTreeActorRow> actors;
};

enum class SceneMethod { ReadActive, ReadTransforms, SelectActor, SetActorTransform, SetActorProperties, GetEnvironment, SetEnvironment, FocusActor, CaptureScreenshot, SetCameraProperties, CreateCamera, ReadActorIndex, ReadActorProperties, ReadCameras, ReadTree };
struct SceneRequest {
    SceneMethod method;
    ActorSelection selection;
    std::optional<std::string> archive_session;
    std::vector<std::string> actor_guids;
    ActorTransformPatch transform;
    ActorPropertiesPatch properties;
    EnvironmentPatch environment;
    ActorFocusRequest focus;
    ScreenshotRequest screenshot;
    CameraPropertiesPatch camera_properties;
    CameraCreateRequest camera_create;
    std::string read_scene;
    std::string read_actor;
};
struct SceneReply {
    ActiveSceneSnapshot active;
    ActorSelection selection;
    std::string error_code;
    std::string error_message;
    ActorTransformSnapshot transform;
    std::vector<ActorTransformSnapshot> transforms;
    ActorPropertiesSnapshot properties;
    EnvironmentSnapshot environment;
    ActorFocusSnapshot focus;
    ScreenshotSnapshot screenshot;
    CameraPropertiesSnapshot camera;
    ActorIndexSnapshot actor_index;
    ActorReadSnapshot actor_read;
    CameraListSnapshot camera_list;
    SceneTreeSnapshot tree;
};

// Execute on the native editor scene owner (CEF UI) thread. Non-Web adapters
// share these operations; they serialize only at their own Python boundary.
SceneReply execute_scene_operation(const SceneRequest& request);

}  // namespace Corona::Systems::UI
