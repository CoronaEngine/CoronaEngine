#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace Corona::Systems::UI {

// Uses the editor owner thread and the existing Python host. No renderer frame is needed.
// Read SceneTools.read_active_scene first. For Scripts.run_path pass one object with
// that archive_session, a project-relative .py path, a requestId and optionally an actor_name.
// Scripts.run_file additionally verifies the supplied revision. Query or stop with Scripts.get_run,
// Scripts.get_status and Scripts.stop. The result contains success, data, error and code.
[[nodiscard]] nlohmann::json invoke_file_script_api(
    const std::string& api, const nlohmann::json& args = nlohmann::json::array());

}  // namespace Corona::Systems::UI
