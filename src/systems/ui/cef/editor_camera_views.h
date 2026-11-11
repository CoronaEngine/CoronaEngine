#pragma once

// The camera views of a scene: lookup, unique naming and creation. Creation allocates the
// native camera and its window identity, so this unit owns the CameraWindowLifetime lease
// that a scene replacement has to retire.

#include "cef_scene_operations.h"
#include "native_api_types.h"
#include "native_editor_state.h"

#include <string>

namespace Corona::Systems::UI {

NativeEditorCamera* find_native_camera(NativeEditorScene& scene, const std::string& camera_name);
std::string unique_camera_name(const NativeEditorScene& scene, const std::string& requested_name);
/// Adds a camera view to the scene. initially_open decides whether its window starts
/// visible; the caller owns that policy, this unit only allocates the identity.
NativeEditorCamera& create_native_camera_view(NativeEditorScene& scene,
                                              const std::string& requested_name,
                                              bool initially_open = true);
NativeResult camera_not_found_result(const std::string& camera_name);

/// Stable identity for a scene object: scene route + name + index, plus a nonce so two
/// actors created in the same frame cannot collide.
std::string make_actor_guid(const std::string& scene_route, const std::string& actor_name,
                            std::size_t index);

}  // namespace Corona::Systems::UI
