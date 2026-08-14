#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace Corona::Systems {
class NetworkSystem;
}

namespace Corona::Systems::UI {

enum class CollaborativeEditorApplyResult {
    Applied,
    Retry,
    Discard,
};

struct CollaborativeEditorApplyOutcome {
    CollaborativeEditorApplyResult result{CollaborativeEditorApplyResult::Discard};
    std::string error;
    nlohmann::json actor;
};

std::shared_ptr<Corona::Systems::NetworkSystem> collaborative_editor_network_system();

CollaborativeEditorApplyOutcome apply_collaborative_actor_create(
    const std::string& actor_guid,
    const std::string& scene_name,
    const std::string& model_path,
    nlohmann::json actor_data);

CollaborativeEditorApplyOutcome apply_collaborative_actor_state(
    const std::string& actor_guid,
    const std::string& scene_name,
    const nlohmann::json& actor_data);

CollaborativeEditorApplyOutcome apply_collaborative_actor_transform(
    const std::string& actor_guid,
    const std::string& scene_name,
    const nlohmann::json& transform_data);

CollaborativeEditorApplyOutcome apply_collaborative_actor_delete(
    const std::string& actor_guid,
    const std::string& scene_name);

// Applies queued collaborative editor mutations on the engine/UI thread.
// The editor page is not required to be mounted for this work to progress.
void tick_collaborative_editor_runtime(std::size_t batch_limit = 8);

}  // namespace Corona::Systems::UI
