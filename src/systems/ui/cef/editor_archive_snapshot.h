#pragma once

// Deciding whether an archive snapshot may be loaded.
//
// Everything here reads JSON and either accepts it or throws with a message that names the
// field. Nothing mutates the editor scene: validation runs before the scene is replaced, so a
// document that nobody can load never becomes the active one.

#include "native_editor_state.h"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

/// Throws std::runtime_error naming the offending field when the snapshot is unusable.
void validate_archive_snapshot(const nlohmann::json& snapshot);

/// Records what a load could not honour (a missing model, an unsupported resource).
void preflight_scene_snapshot(const nlohmann::json& snapshot, nlohmann::json& diagnostics);

/// The actor a snapshot describes, before it is attached to the scene.
NativeEditorActor native_actor_from_snapshot(const nlohmann::json& actor_data);

bool snapshot_actor_resource_supported(const NativeEditorActor& actor,
                                       const std::filesystem::path& asset_path);

void append_actor_materialization_diagnostic(nlohmann::json& diagnostics,
                                             const NativeEditorActor& actor,
                                             const std::string& code,
                                             const std::string& message);

}  // namespace Corona::Systems::UI
