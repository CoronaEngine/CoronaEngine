#pragma once

#include <nlohmann/json_fwd.hpp>
#include <corona/engine/engine_runtime_api.h>
#include "cef_scene_operations.h"
#include <filesystem>
#include <functional>
#include <optional>

namespace Corona::Systems::UI {

// Narrow integration-test seam for installing a validated portable scene into
// the process-global native editor state used by the production API handlers.
void load_native_editor_scene_for_test(const nlohmann::json& snapshot);
void reset_native_editor_scene_for_test();

// Inject only the interactive/decoder host boundaries; validation and result
// construction run through the same operation used by V8 and Python adapters.
using ResourceFilePicker = std::function<std::optional<std::filesystem::path>(
    const std::filesystem::path&, const std::string&)>;
using MediaFileImporter = std::function<Corona::API::MediaInfo(const std::string&)>;
SceneReply import_native_resource_for_test(const SceneRequest& request,
    const ResourceFilePicker& picker, const MediaFileImporter& importer);

}  // namespace Corona::Systems::UI
