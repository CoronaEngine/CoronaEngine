#pragma once

// The actor property patch pipeline: everything an actor can be told to change - name,
// transform, visibility, body, optics, camera lock, animation sources - validated and
// written in one place, plus the snapshot that reports the result back.
//
// The patch type and the validation rules belong to ActorPropertiesService; this unit owns
// the ordering: validate the whole patch first, then apply, so a rejected field cannot
// leave the actor half-updated.

#include "cef_scene_operations.h"
#include "native_editor_state.h"

#include <string>
#include <vector>

namespace Corona::Systems::UI {

SceneReply apply_native_actor_properties(const SceneRequest& request);
ActorPropertiesSnapshot actor_properties_snapshot(const NativeEditorScene& scene,
                                                  const NativeEditorActor& actor);

/// Resolves animation source references to resource ids. `memory_only` keeps them in the
/// actor instead of publishing them to the scene file (the script channel owns no assets).
std::vector<std::string> prepare_animation_sources(const NativeEditorScene& scene,
                                                   const std::vector<std::string>& value,
                                                   bool memory_only = false);

void request_actor_animation_sources(const NativeEditorScene& scene, const NativeEditorActor& actor,
                                     std::vector<std::string> sources);

}  // namespace Corona::Systems::UI
