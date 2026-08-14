#include "collaborative_editor_runtime.h"

#include <string>
#include <utility>

#include <corona/kernel/core/i_logger.h>
#include <corona/systems/network/network_system.h>

namespace Corona::Systems::UI {

namespace {

bool is_retry(const CollaborativeEditorApplyOutcome& outcome) {
    return outcome.result == CollaborativeEditorApplyResult::Retry;
}

void log_apply_outcome(const char* kind,
                       const std::string& actor_guid,
                       const std::string& scene_name,
                       const CollaborativeEditorApplyOutcome& outcome) {
    switch (outcome.result) {
        case CollaborativeEditorApplyResult::Applied:
            CFW_LOG_DEBUG("CollaborativeEditorRuntime: {} applied actor='{}' scene='{}'",
                          kind, actor_guid, scene_name);
            break;
        case CollaborativeEditorApplyResult::Retry:
            CFW_LOG_WARNING(
                "CollaborativeEditorRuntime: {} retry actor='{}' scene='{}' reason='{}'",
                kind, actor_guid, scene_name, outcome.error);
            break;
        case CollaborativeEditorApplyResult::Discard:
            CFW_LOG_WARNING(
                "CollaborativeEditorRuntime: {} discarded actor='{}' scene='{}' reason='{}'",
                kind, actor_guid, scene_name, outcome.error);
            break;
    }
}

}  // namespace

void tick_collaborative_editor_runtime(std::size_t batch_limit) {
    auto sys = collaborative_editor_network_system();
    if (!sys) return;
    for (std::size_t i = 0; i < batch_limit; ++i) {
        bool retry = false;
        if (!sys->process_pending_actor_create(
                [&](const std::string& actor_guid, const std::string& scene_name,
                    const std::string& model_path,
                    const Corona::Network::ActorCreatePacked& packed,
                    const std::string& actor_json) {
                    nlohmann::json actor_data = nlohmann::json::object();
                    try {
                        if (!actor_json.empty()) actor_data = nlohmann::json::parse(actor_json);
                        if (!actor_data.is_object()) return true;
                    } catch (const nlohmann::json::parse_error&) {
                        return true;
                    }
                    actor_data["geometry"]["position"] = {
                        packed.transform[0], packed.transform[1], packed.transform[2]};
                    actor_data["geometry"]["rotation"] = {
                        packed.transform[3], packed.transform[4], packed.transform[5]};
                    actor_data["geometry"]["scale"] = {
                        packed.transform[6], packed.transform[7], packed.transform[8]};
                    const auto outcome = apply_collaborative_actor_create(
                        actor_guid, scene_name, model_path, std::move(actor_data));
                    log_apply_outcome("create", actor_guid, scene_name, outcome);
                    retry = is_retry(outcome);
                    return !retry;
                })) {
            break;
        }
        if (retry) break;
    }

    for (std::size_t i = 0; i < batch_limit; ++i) {
        bool retry = false;
        if (!sys->process_pending_actor_state_update(
                [&](const std::string& actor_guid, const std::string& scene_name,
                    const std::string& actor_json) {
                    try {
                        const auto actor_data = nlohmann::json::parse(actor_json);
                        const auto outcome = apply_collaborative_actor_state(
                            actor_guid, scene_name, actor_data);
                        log_apply_outcome("state", actor_guid, scene_name, outcome);
                        retry = is_retry(outcome);
                        return !retry;
                    } catch (const nlohmann::json::parse_error&) {
                        return true;
                    }
                })) {
            break;
        }
        if (retry) break;
    }

    for (std::size_t i = 0; i < batch_limit; ++i) {
        bool retry = false;
        if (!sys->process_pending_actor_transform_update(
                [&](const std::string& actor_guid, const std::string& scene_name,
                    const float* transform, const std::string&, const std::string&) {
                    nlohmann::json transform_data = {
                        {"position", {transform[0], transform[1], transform[2]}},
                        {"rotation", {transform[3], transform[4], transform[5]}},
                        {"scale", {transform[6], transform[7], transform[8]}},
                    };
                    const auto outcome = apply_collaborative_actor_transform(
                        actor_guid, scene_name, transform_data);
                    log_apply_outcome("transform", actor_guid, scene_name, outcome);
                    retry = is_retry(outcome);
                    return !retry;
                })) {
            break;
        }
        if (retry) break;
    }

    for (std::size_t i = 0; i < batch_limit; ++i) {
        bool retry = false;
        if (!sys->process_pending_actor_delete(
                [&](const std::string& actor_guid, const std::string& scene_name,
                    const std::string&) {
                    const auto outcome = apply_collaborative_actor_delete(
                        actor_guid, scene_name);
                    log_apply_outcome("delete", actor_guid, scene_name, outcome);
                    retry = is_retry(outcome);
                    return !retry;
                })) {
            break;
        }
        if (retry) break;
    }
}

}  // namespace Corona::Systems::UI
