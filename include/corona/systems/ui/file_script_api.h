#pragma once

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

// The project file-script routes, one named entry per operation.
//
// These used to be reachable through a single `invoke_file_script_api("Scripts.run_file",
// args)` name switch - the last name dispatch in the editor surface. The only caller
// is the native file-script probe (`tests/integration/native_file_probe.cpp`), which
// keeps its own name-to-function mapping because that protocol is test-only.
//
// Every entry runs on the editor owner thread and through the existing Python host;
// no renderer frame is needed. Read the active scene first. The payload shapes are
// unchanged: run_file takes the published document revision, run_path a project-relative
// .py path, both with the archive session and a requestId; query or stop with the run id.
// The reply is the `{success, code, data, error}` envelope the probe relays verbatim.
[[nodiscard]] nlohmann::json run_project_script_file(const nlohmann::json& payload);
[[nodiscard]] nlohmann::json run_project_script_path(const nlohmann::json& payload);
[[nodiscard]] nlohmann::json project_script_status(const nlohmann::json& request);
[[nodiscard]] nlohmann::json project_script_run(const nlohmann::json& request);
[[nodiscard]] nlohmann::json stop_project_script(const nlohmann::json& request);

}  // namespace Corona::Systems::UI
