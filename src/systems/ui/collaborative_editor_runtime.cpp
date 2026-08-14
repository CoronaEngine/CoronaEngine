#include "collaborative_editor_runtime.h"

#include <string>
#include <utility>

#include <corona/systems/network/network_system.h>

namespace Corona::Systems::UI {

namespace {

bool is_retry(const CollaborativeEditorApplyOutcome& outcome) {
    return outcome.result == CollaborativeEditorApplyResult::Retry;
}

}  // namespace

void tick_collaborative_editor_runtime(std::size_t batch_limit) {
    auto sys = collaborative_editor_network_system();
    if (!sys) return;
    for (std::size_t i = 0; i < batch_limit; ++i) {
        std::string actor_guid, scene_name, model_path, actor_json;
        Corona::Network::ActorCreatePacked packed{};
        if (!sys->peek_pending_actor_create(actor_guid, scene_name, model_path,
                                             &packed, sizeof(packed), &actor_json)) {
            break;
        }
        nlohmann::json actor_data = nlohmann::json::object();
        try {
            if (!actor_json.empty()) actor_data = nlohmann::json::parse(actor_json);
            if (!actor_data.is_object()) {
                sys->ack_pending_actor_create(actor_guid);
                continue;
            }
        } catch (const nlohmann::json::parse_error&) {
            sys->ack_pending_actor_create(actor_guid);
            continue;
        }
        actor_data["geometry"]["position"] = {
            packed.transform[0], packed.transform[1], packed.transform[2]};
        actor_data["geometry"]["rotation"] = {
            packed.transform[3], packed.transform[4], packed.transform[5]};
        actor_data["geometry"]["scale"] = {
            packed.transform[6], packed.transform[7], packed.transform[8]};
        const auto outcome = apply_collaborative_actor_create(
            actor_guid, scene_name, model_path, std::move(actor_data));
        if (is_retry(outcome)) break;
        sys->ack_pending_actor_create(actor_guid);
    }

    for (std::size_t i = 0; i < batch_limit; ++i) {
        std::string actor_guid, scene_name, actor_json;
        if (!sys->peek_pending_actor_state_update(actor_guid, scene_name, actor_json)) {
            break;
        }
        try {
            const auto actor_data = nlohmann::json::parse(actor_json);
            const auto outcome = apply_collaborative_actor_state(
                actor_guid, scene_name, actor_data);
            if (is_retry(outcome)) break;
            sys->ack_pending_actor_state_update(actor_guid);
        } catch (const nlohmann::json::parse_error&) {
            sys->ack_pending_actor_state_update(actor_guid);
        }
    }

    for (std::size_t i = 0; i < batch_limit; ++i) {
        std::string actor_guid, scene_name, source_user_id, correlation_id;
        float transform[9] = {0, 0, 0, 0, 0, 0, 1, 1, 1};
        if (!sys->peek_pending_actor_transform_update(
                actor_guid, scene_name, transform, 9, source_user_id, correlation_id)) {
            break;
        }
        nlohmann::json transform_data = {
            {"position", {transform[0], transform[1], transform[2]}},
            {"rotation", {transform[3], transform[4], transform[5]}},
            {"scale", {transform[6], transform[7], transform[8]}},
        };
        const auto outcome = apply_collaborative_actor_transform(
            actor_guid, scene_name, transform_data);
        if (is_retry(outcome)) break;
        sys->ack_pending_actor_transform_update(actor_guid);
    }

    for (std::size_t i = 0; i < batch_limit; ++i) {
        std::string actor_guid, scene_name, actor_name;
        if (!sys->peek_pending_actor_delete(actor_guid, scene_name, actor_name)) {
            break;
        }
        const auto outcome = apply_collaborative_actor_delete(actor_guid, scene_name);
        if (is_retry(outcome)) break;
        sys->ack_pending_actor_delete(actor_guid);
    }
}

}  // namespace Corona::Systems::UI
