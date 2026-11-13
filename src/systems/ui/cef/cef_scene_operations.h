#pragma once

#include "scene_tree_actor_row.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

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
    std::optional<std::string> name;
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

struct ActorCreateRequest {
    std::string scene, path, actor_type = "model";
    std::optional<std::string> actor_name;
    std::optional<std::uint64_t> audio_resource_id;
};

struct ActorDeleteRequest {
    std::string scene, actor;
};
struct ActorDeleteSnapshot {
    std::string archive_session, scene, actor_guid, old_name;
    std::uint64_t tree_revision{0};
    bool selection_cleared{false};
};

struct ResourceDiagnosticSnapshot {
    std::string code, message, path;
    bool recoverable = false;
};
struct ActorResourceRebindRequest {
    std::string scene, actor_guid, path;
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
struct ActorResourceRebindSnapshot {
    std::string archive_session, scene, actor_guid, status;
    bool ok = false;
    std::vector<ResourceDiagnosticSnapshot> diagnostics;
    ActorReadSnapshot actor;
};

struct SceneSaveSnapshot {
    std::string status, archive_session, scene, filepath, message;
    std::size_t unresolved_actor_count = 0;
};

struct ResourceImportSnapshot {
    std::string status, archive_session, scene, file_type, path, actor_type, message, code;
    std::string media_name, media_type, resource_id, codec;
    double duration = 0, width = 0, height = 0, fps = 0;
    int sample_rate = 0, channels = 0;
};

struct AudioRequest {
    std::string scene, actor;
    std::uint64_t resource_id = 0;
    bool loop = false;
};
struct AudioSnapshot {
    std::string status, archive_session, scene, actor;
    std::uint64_t resource_id = 0;
    bool spatial = false, playing = false;
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
    std::optional<std::string> name;
    std::optional<std::array<float, 3>> position, forward, world_up;
    std::optional<float> fov, projection_x_sign, near_plane, far_plane, move_speed;
    std::optional<int> width, height, view_x, view_y, view_width, view_height;
    std::optional<bool> view_open;
    bool has_pose() const { return position || forward || world_up || fov || projection_x_sign || near_plane || far_plane; }
    bool has_view() const { return view_open || view_x || view_y || view_width || view_height || move_speed; }
    bool empty() const { return !name && !has_pose() && !has_view() && !width && !height; }
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
struct CameraDeleteRequest {
    std::string scene, camera_id;
};
struct DeletedCameraSnapshot {
    std::string archive_session, scene, camera_id;
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
struct CameraRenderSettingsPatch {
    std::string scene, camera_id;
    std::optional<std::string> family;
    nlohmann::json settings;
    std::optional<std::string> render_backend, output_mode, vision_render_mode, vision_denoiser;
    std::optional<bool> vision_accumulation, vision_denoise, vision_stable_planes;
    bool empty() const {
        if (family) return !settings.is_object() || settings.empty();
        return !render_backend && !output_mode && !vision_render_mode && !vision_denoiser &&
            !vision_accumulation && !vision_denoise && !vision_stable_planes;
    }
};
struct CameraRenderSettingsSnapshot {
    std::string archive_session, scene, camera_id, render_backend, output_mode, vision_render_mode, vision_denoiser;
    std::string family;
    nlohmann::json settings;
    bool vision_accumulation = false, vision_denoise = true, vision_stable_planes = true;
    bool pending = false;
};
struct SceneTreeSnapshot {
    CameraListSnapshot cameras;
    std::uint64_t tree_revision = 0;
    std::vector<Corona::Ui::Cef::SceneTreeActorRow> actors;
};

struct ActorContextRow {
    std::string actor_guid, name, type, semantic_role;
    std::array<float, 3> position{}, rotation{}, scale{1, 1, 1}, size{};
    bool has_size = false, physics_enabled = false;
};
struct ActorContextSnapshot {
    std::string archive_session, scene;
    std::uint64_t tree_revision = 0;
    std::vector<ActorContextRow> actors;
};

struct SkeletonNodeSnapshot {
    std::string name;
    int index = -1;
    int parent = -1;
    bool leaf = false;
};
struct ActorSkeletonSnapshot {
    std::string archive_session, scene, actor;
    bool is_skinned = false;
    int node_count = 0, bone_count = 0;
    std::vector<std::string> leaves;
    std::vector<SkeletonNodeSnapshot> nodes;
};
struct IkRuntimeSnapshot {
    bool has_target = false, grounded = false, releasing = false;
    float weight = 0;
    std::array<float, 3> target{}, normal{0, 1, 0};
};
struct IkChainSnapshot {
    std::string id, bone_name, mode;
    int chain_length = 3, max_iterations = 10;
    float weight = 1, tolerance = 1e-3f, damping = .88f;
    bool enabled = true;
    float contact_weight_rise = 8, contact_weight_decay = .8f;
    float contact_normal_offset = 0, probe_max_drop = .5f;
    float foot_height = 0, plant_threshold = .08f;
    std::array<float, 3> target{};
    std::optional<IkRuntimeSnapshot> runtime;
};
struct ActorIkSnapshot {
    std::string archive_session, scene, actor;
    bool ready = false, is_skinned = false;
    std::uint64_t revision = 0;
    std::vector<IkChainSnapshot> chains;
};
struct ActorIkPatch {
    std::string scene, actor;
    std::vector<IkChainSnapshot> chains;
    std::optional<std::uint64_t> expected_revision;
};

struct ModelNodeSnapshot {
    std::uint32_t node_index{};
    std::int32_t parent_index{-1};
    std::string name, node_path;
    std::array<float, 3> position{}, rotation{}, scale{1, 1, 1};
    bool visible{true}, effective_visible{true}, editable{true};
    std::size_t child_count{0};
};
struct ModelNodePageSnapshot {
    std::string archive_session, scene, actor_guid, actor;
    int actor_version{1};
    std::int64_t parent_index{-1};
    std::size_t total{0}, next_offset{0};
    bool has_more{false};
    std::string status;
    std::vector<ModelNodeSnapshot> nodes;
};
struct ModelNodePatch {
    std::string scene, actor_guid;
    std::uint32_t node_index{};
    std::optional<std::array<float, 3>> position, rotation, scale;
    std::optional<bool> visible;
    std::optional<int> expected_actor_version;
};
struct ModelNodePageRequest {
    std::string scene, actor_guid;
    std::int64_t parent_index{-1};
    std::size_t offset{0}, limit{100};
};

enum class SceneMethod { ReadScene, CreateActor, ReadActive, ReadVisionAvailability, ReadTransforms, SelectActor, SelectModelFile, ImportResourceFile, PlayAudioResource, StopAudioResource, PlayActorAudio, StopActorAudio, SetActorTransform, UpdateActor, DeleteActor, RebindActorResource, Save, GetEnvironment, SetEnvironment, FocusActor, CaptureScreenshot, UpdateCamera, ReadCameraRenderSettings, SetCameraRenderSettings, CreateCamera, DeleteCamera, ReadActorProperties, ReadActorRig, SetActorIkChains, ReadModelNodes, SetModelNodeState };
enum class ActorProjection { None, Index, Tree, Context };
struct SceneReadProjection {
    ActorProjection actors = ActorProjection::None;
    bool cameras = false;
};

struct SceneRequest {
    SceneMethod method;
    ActorSelection selection;
    std::optional<std::string> archive_session;
    std::string file_type;
    std::vector<std::string> actor_guids;
    ActorTransformPatch transform;
    ActorPropertiesPatch properties;
    ActorCreateRequest actor_create;
    ActorDeleteRequest actor_delete;
    ActorResourceRebindRequest actor_resource_rebind;
    AudioRequest audio;
    bool save_actors_only = false;
    EnvironmentPatch environment;
    ActorFocusRequest focus;
    ScreenshotRequest screenshot;
    CameraPropertiesPatch camera_properties;
    CameraRenderSettingsPatch camera_render_settings;
    CameraCreateRequest camera_create;
    CameraDeleteRequest camera_delete;
    ActorIkPatch ik;
    ModelNodePatch model_node;
    ModelNodePageRequest model_node_page;
    std::string read_scene;
    std::string read_actor;
    SceneReadProjection scene_read;
};
// A read reply carries the projection the method was asked for; a write reply carries
// the state the write produced. Grouping them keeps each branch of
// execute_scene_operation from filling (and each V8 branch from reading) fields that
// belong to a different method family.
struct SceneReadReply {
    SceneReadProjection scene_read;
    ActiveSceneSnapshot active;
    bool vision_available = false;
    ActorSelection selection;
    std::vector<ActorTransformSnapshot> transforms;
    CameraListSnapshot camera_list;
    ActorIndexSnapshot actor_index;
    SceneTreeSnapshot tree;
    ActorContextSnapshot actor_context;
    ActorSkeletonSnapshot actor_skeleton;
    ActorIkSnapshot actor_ik;
    ModelNodePageSnapshot model_nodes;
    EnvironmentSnapshot environment;
    CameraRenderSettingsSnapshot camera_render_settings;
    ActorReadSnapshot actor_read;
};

struct SceneWriteReply {
    ActorTransformSnapshot transform;
    ActorPropertiesSnapshot properties;
    ActorDeleteSnapshot actor_delete;
    ActorResourceRebindSnapshot actor_resource_rebind;
    SceneSaveSnapshot save;
    std::string selected_file;
    ResourceImportSnapshot resource_import;
    ActorFocusSnapshot focus;
    ScreenshotSnapshot screenshot;
    CameraPropertiesSnapshot camera;
    DeletedCameraSnapshot deleted_camera;
    ModelNodePageSnapshot model_node;
    AudioSnapshot audio;
};

struct SceneReply {
    std::string error_code;
    std::string error_message;
    SceneReadReply read;
    SceneWriteReply write;
};

// Execute on the native editor scene owner (CEF UI) thread. Non-Web adapters
// share these operations; they serialize only at their own Python boundary.
SceneReply execute_scene_operation(const SceneRequest& request);

}  // namespace Corona::Systems::UI
