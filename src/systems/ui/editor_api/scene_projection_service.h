#pragma once

#include "cef/cef_scene_operations.h"

#include <string>
#include <vector>

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
