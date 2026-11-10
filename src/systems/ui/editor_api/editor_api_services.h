#pragma once

// The CEF-free domain services behind the editor API: validation and projection rules that
// have no browser, V8 or route dependency, so they can be tested on their own.
//
// One header on purpose. These were eight 16-40 line headers, all included by the same
// editor_api translation units, and five of them each redeclared the identical
// ValidationError struct. The services keep their own namespaces - the boundaries are in the
// code, not in the file count - and each re-exports the single ValidationError.

#include "cef/cef_scene_operations.h"

#include <corona/systems/geometry/model_node_state.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

// One validation result shape for every service. It used to be redeclared identically inside
// ActorPropertiesService, CameraPropertiesService, CameraRenderSettingsService,
// ProjectScriptService and ResourceImportService; they now alias this one.
struct ValidationError {
    std::string code;
    std::string message;
    explicit operator bool() const { return !code.empty(); }
};

}  // namespace Corona::Systems::UI

// ============================================================================
// Actor properties: typed updateActor patches and renames.
// ============================================================================
namespace Corona::Systems::UI::ActorPropertiesService {

using ValidationError = ::Corona::Systems::UI::ValidationError;

// Validates the typed updateActor patch before any component write or asset
// import: identity/patch emptiness, mechanics finiteness and body type, camera
// lock offsets. A mixed invalid patch must never partly apply.
ValidationError validate_patch(const ActorPropertiesPatch& patch);

// Validates a requested rename: read-only scenes reject first, then empty
// trimmed names, then names already used by another Actor.
ValidationError validate_rename(const std::string& requested_name,
                                bool read_only,
                                const std::vector<std::string>& other_names);

// Trimmed rename value the caller applies after validation.
std::string normalized_name(const std::string& requested_name);

}  // namespace Corona::Systems::UI::ActorPropertiesService

// ============================================================================
// Camera properties: pose, projection and window geometry patches.
// ============================================================================
namespace Corona::Systems::UI::CameraPropertiesService {

using ValidationError = ::Corona::Systems::UI::ValidationError;

// Window lifecycle does not belong to a camera property patch.
ValidationError validate_view_lifecycle(const CameraPropertiesPatch& patch, bool editor_input);

// A non-empty patch must identify an archive, scene, camera and carry fields.
ValidationError validate_identity(const CameraPropertiesPatch& patch, bool has_archive_session);

// Rename rules: read-only scenes reject first, then empty trimmed names, then
// names already used by another camera.
ValidationError validate_rename(const std::string& requested_name,
                                bool read_only,
                                const std::vector<std::string>& other_names);

// Trimmed rename value the caller applies after validation.
std::string normalized_name(const std::string& requested_name);

// Numeric ranges for pose, projection and window geometry. Unset fields fall
// back to the current projection, mirroring the native camera.
ValidationError validate_numeric(const CameraPropertiesPatch& patch,
                                 const std::array<float, 3>& current_projection);

// Runtime owns camera input while a game/script is running.
ValidationError validate_editor_input(const CameraPropertiesPatch& patch,
                                      bool editor_input,
                                      bool editor_camera_input_enabled);

}  // namespace Corona::Systems::UI::CameraPropertiesService

// ============================================================================
// Camera render settings: the Vision/settings whitelists.
// ============================================================================
namespace Corona::Systems::UI::CameraRenderSettingsService {

using ValidationError = ::Corona::Systems::UI::ValidationError;

// Validates identity, patch shape and the Vision/settings whitelists for both
// ReadCameraRenderSettings and SetCameraRenderSettings. Messages are part of the
// frozen contract.
ValidationError validate_patch(const CameraRenderSettingsPatch& patch,
                               SceneMethod method,
                               bool has_archive_session);

}  // namespace Corona::Systems::UI::CameraRenderSettingsService

// ============================================================================
// Project scripts: file-launch validation and bridge error mapping.
// ============================================================================
namespace Corona::Systems::UI::ProjectScriptService {

using ValidationError = ::Corona::Systems::UI::ValidationError;

// Script file launches must name a project-relative .py file.
ValidationError validate_script_path(const std::filesystem::path& path);

// Launches are rejected when the project is read-only, the payload omits the
// project path, or the declared project differs from the active root.
ValidationError validate_script_project(bool read_only,
                                        bool has_project_path,
                                        bool project_path_matches_root);

// Defaults the launch target kind: an actor name selects the actor target,
// otherwise the whole project runs.
std::string default_target_type(const std::string& actor_name);

// Maps Python-bridge failure codes onto the frozen Web error codes for the
// Scripts namespace: 504 -> EDITOR_API_TIMEOUT, 409 -> PROJECT_CONTEXT_CHANGED,
// else NATIVE_FAILURE.
std::string bridge_error_code(int error_code);

// ProjectFiles only distinguishes timeouts from general native failures.
std::string project_files_error_code(int error_code);

// Project document / metadata requests must carry the active archive session
// (and, for documents, the active project root).
ValidationError validate_document_identity(const std::string& scene_archive_session,
                                           const std::string& payload_archive_session,
                                           bool project_path_matches);

// Persistence writes reject read-only projects.
ValidationError require_writable_project(bool read_only);

// ProjectSettings bridge mapping: 409 -> PROJECT_CONTEXT_CHANGED,
// 403 -> READ_ONLY_PROJECT, 504 -> EDITOR_API_TIMEOUT, else NATIVE_FAILURE.
std::string project_settings_error_code(int error_code);

}  // namespace Corona::Systems::UI::ProjectScriptService

// ============================================================================
// Resource import: picker results and engine media import.
// ============================================================================
namespace Corona::Systems::UI::ResourceImportService {

using ValidationError = ::Corona::Systems::UI::ValidationError;

// Neutral copy of the engine importer result. The adapter maps
// Corona::API::MediaInfo into this POD so the service stays engine-header free.
struct MediaImportResult {
    std::uint64_t resource_id{0};
    std::string media_type;
    double duration_seconds{0.0};
    std::string codec;
    int width{0};
    int height{0};
    double fps{0.0};
    int sample_rate{0};
    int channels{0};
};

// Single source of truth for the importable resource types. The dialog filter
// table in the adapter must stay in sync (only presentation lives there).
bool supports_file_type(const std::string& file_type);

// Media files run through the engine importer; other types finish after the
// picker returns.
bool requires_media_import(const std::string& file_type);

// Identity requirements before any dialog or import work.
ValidationError validate_request(const std::optional<std::string>& archive_session,
                                 const std::string& scene,
                                 const std::string& file_type);

// Maps the importer result onto the Web-facing snapshot. Returns false and
// fills the media_unrecognized error when the file was not recognized.
bool apply_media_import(ResourceImportSnapshot& imported,
                        const MediaImportResult& info,
                        const std::string& media_name);

}  // namespace Corona::Systems::UI::ResourceImportService

// ============================================================================
// Scene environment: the editor-owned scalars and the physics hand-off.
// ============================================================================
namespace Corona::Systems::UI::SceneEnvironmentService {

// Editor-scene environment scalars that are not owned by the engine component.
struct EnvironmentScalars {
    bool sun_enabled = true;
    std::array<float, 3> sun_direction{1.f, 1.f, 1.f};
    bool grid_enabled = false;
};

// Physics values a SetEnvironment patch asks the caller to push into the
// engine environment component; unset members mean "keep the current value".
struct PhysicsUpdate {
    std::optional<std::array<float, 3>> gravity;
    std::optional<float> floor_y, floor_restitution, fixed_dt;
    bool empty() const { return !gravity && !floor_y && !floor_restitution && !fixed_dt; }
};

// Validates a SetEnvironment patch. Returns an empty string when valid,
// otherwise the message reported with INVALID_ARGUMENT.
std::string validate_patch(const EnvironmentPatch& patch);

// Applies sun/grid to the editor-owned scalars. A degenerate sun direction is
// replaced with {1,1,1} only while the sun is enabled, mirroring the native
// scene behaviour. Engine-side physics stays with the caller.
void apply_sun_and_grid(EnvironmentScalars& scalars, const EnvironmentPatch& patch);

// Extracts the physics fields present in the patch.
PhysicsUpdate physics_update(const EnvironmentPatch& patch);

}  // namespace Corona::Systems::UI::SceneEnvironmentService

// ============================================================================
// Scene model nodes: the CEF-free projection of engine node state.
// ============================================================================
namespace Corona::Systems::UI::SceneModelNodeService {

// Pure projection of one engine model node into the Web-facing snapshot.
// CEF-free by construction: the service only sees engine node state, never a
// browser, V8 value or JS route.
ModelNodeSnapshot make_snapshot(const Corona::API::ModelNodeState& node,
                                std::size_t child_count,
                                bool actor_visible);

// Paginated projection for ReadModelNodes. Fills nodes/total/next_offset/
// has_more and each snapshot; the caller owns archive/scene/actor identity and
// the GPU build status string (they depend on scene/actor state, not nodes).
ModelNodePageSnapshot project_page(const std::vector<Corona::API::ModelNodeState>& nodes,
                                   const ModelNodePageRequest& request,
                                   bool actor_visible);

// Applies the optional patch fields onto a copy of the base node state.
// Mirror of the engine commit input; the caller still performs the controlled
// write through the actor geometry.
Corona::API::ModelNodeState derive_patched_state(const Corona::API::ModelNodeState& base,
                                                 const ModelNodePatch& patch);

// node_index/node_path identity pairs consumed by ModelNodeConfig::partition
// when override groups are re-keyed after a model node edit.
nlohmann::json node_keys(const std::vector<Corona::API::ModelNodeState>& nodes);

}  // namespace Corona::Systems::UI::SceneModelNodeService

// ============================================================================
// Scene projection: the identity-only actor index.
// ============================================================================
namespace Corona::Systems::UI::SceneProjectionService {

// Identity subset of an editor actor as seen by the Web projections. The route
// extension is pre-decoded by the caller (filesystem decoding stays at the
// adapter boundary); the service owns the projection rules only.
struct ActorIdentity {
    std::string actor_guid;
    std::string name;
    std::string extension;   // route extension without the leading dot, may be empty
    std::string actor_type;  // logical type when the route has no extension
};

// Web-facing actor type: the route's file extension wins over the logical type.
std::string display_type(const ActorIdentity& actor);

// Semantic role falls back to the entity type when unset.
std::string semantic_role(const std::string& role, const std::string& entity_type);

// Phantom bodies carry no physics from the editor's point of view.
bool physics_enabled(const std::string& body_type);

// Identity-only index projection: never reads geometry, optics or bounds.
std::vector<ActorIndexEntry> project_index(const std::vector<ActorIdentity>& actors);

}  // namespace Corona::Systems::UI::SceneProjectionService
