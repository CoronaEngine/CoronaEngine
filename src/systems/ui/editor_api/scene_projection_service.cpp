#include "scene_projection_service.h"

namespace Corona::Systems::UI::SceneProjectionService {

std::string display_type(const ActorIdentity& actor) {
    return actor.extension.empty() ? actor.actor_type : actor.extension;
}

std::string semantic_role(const std::string& role, const std::string& entity_type) {
    return role.empty() ? entity_type : role;
}

bool physics_enabled(const std::string& body_type) {
    return body_type != "phantom";
}

std::vector<ActorIndexEntry> project_index(const std::vector<ActorIdentity>& actors) {
    std::vector<ActorIndexEntry> rows;
    rows.reserve(actors.size());
    for (const auto& actor : actors) {
        rows.push_back({actor.actor_guid, actor.name, display_type(actor)});
    }
    return rows;
}

}  // namespace Corona::Systems::UI::SceneProjectionService
