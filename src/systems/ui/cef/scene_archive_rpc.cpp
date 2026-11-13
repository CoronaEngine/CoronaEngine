#include "scene_folder.h"
#include "editor_api/python_service_bridge.h"

namespace Corona::Systems::UI::SceneFolders {

nlohmann::json archive_asset_request(nlohmann::json request) {
    const auto result = invoke_python_script_service(
        {"ProjectArchive", "asset_operation", nlohmann::json::array({std::move(request)})},
        "python-archive");
    if (!result.success) throw std::runtime_error(result.error);
    return result.data;
}

}  // namespace Corona::Systems::UI::SceneFolders
