#include "scene_folder.h"
#include "editor_api/cef_editor_api.h"

namespace Corona::Systems::UI::SceneFolders {

nlohmann::json archive_asset_request(nlohmann::json request) {
    const auto result = invoke_python_script_service(
        {"ProjectArchive", "asset_operation", nlohmann::json::array({std::move(request)})},
        "python-archive", std::chrono::seconds(300));
    if (!result.success) throw std::runtime_error(result.error);
    return result.data;
}

}  // namespace Corona::Systems::UI::SceneFolders
