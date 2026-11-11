#pragma once

// Reading scene values as the contracts see them: one actor, its transform parts and the
// bounds used for picking.
//
// CEF-free on purpose. `best_effort` says whether an engine read may give up instead of
// waiting for a loading resource; the default is decided by the *caller's* thread, which
// is the only place that knows whether it is the CEF UI thread - so the default lives in
// the handler unit and this header only declares the parameter.

#include "native_editor_state.h"

#include <corona/resource/types/animation_player.h>

#include <array>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

const char* actor_load_status_name(ActorLoadStatus status);
std::string body_type_name(const Corona::API::Mechanics& mechanics);
std::string actor_file_extension(const std::string& route);
nlohmann::json aabb_to_json(const std::array<float, 6>& aabb);

std::shared_ptr<Corona::Resource::AnimationPlayer> actor_animation_player(
    const NativeEditorActor& actor);
nlohmann::json native_actor_ik_configuration(const NativeEditorActor& actor);

std::optional<std::array<float, 6>> native_actor_local_aabb(const NativeEditorActor& actor,
                                                            bool best_effort);
std::optional<std::array<float, 6>> native_actor_world_aabb(const NativeEditorActor& actor,
                                                            bool best_effort);
nlohmann::json actor_to_json(const NativeEditorScene& scene, const NativeEditorActor& actor,
                             bool best_effort);

}  // namespace Corona::Systems::UI
