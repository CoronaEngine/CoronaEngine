#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

enum class ScriptsMethod { RunFile, GetRun, Stop, GetStatus };
struct ScriptsRequest {
    ScriptsMethod method;
    std::string request_id, run_id, archive_session, project_path, path, revision;
    std::string target_type = "actor", scene_name, actor_name;
    bool restore_state = true;
};

struct ScriptMarker { std::string id, name; };
// Only this explicit projection crosses the native owner/renderer boundary.
// Missing receipt fields stay absent; Python remains the execution-state owner.
struct ScriptSnapshot {
    std::optional<std::string> status, outcome, error, code, message, filepath;
    std::optional<std::string> runId, requestId, contextId, sceneName, actorName, targetType;
    std::optional<std::string> requestedScene, requestedActor, resolvedSceneName, resolvedActorName;
    std::optional<std::string> bindingMode, nativeScene, restoreStatus, restoreError, currentNodeId;
    std::optional<bool> inputLocked, snapshotCaptured, threadAlive, restored;
    std::optional<double> startedAt, finishedAt;
    std::optional<std::vector<std::string>> actorCandidates, pendingThreads;
    std::optional<ScriptMarker> marker, waiting;
};
struct ScriptsReply {
    std::string error_code, error_message;
    ScriptSnapshot snapshot;
};

// CEF UI thread, sharing the native script start and Python file-runtime owner.
ScriptsReply execute_scripts_operation(const ScriptsRequest& request);

} // namespace Corona::Systems::UI
