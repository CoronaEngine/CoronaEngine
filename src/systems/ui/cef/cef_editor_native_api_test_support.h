#pragma once

#include <nlohmann/json_fwd.hpp>

namespace Corona::Systems::UI {

// Narrow integration-test seam for installing a validated portable scene into
// the process-global native editor state used by the production API handlers.
void load_native_editor_scene_for_test(const nlohmann::json& snapshot);
void reset_native_editor_scene_for_test();

}  // namespace Corona::Systems::UI
