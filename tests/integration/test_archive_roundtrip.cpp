#include <corona/kernel/core/kernel_context.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include <corona/systems/script/camera_follow_controller.h>
#include "cef/cef_editor_native_api_registry.h"
#include "cef/cef_editor_native_api_test_support.h"
#include "cef/scene_folder.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace UI = Corona::Systems::UI;
namespace fs = std::filesystem;
using Json = nlohmann::json;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
Json invoke(const char* module, const char* function, Json args) {
    auto result = UI::NativeApiRegistry::instance().dispatch({module, function, std::move(args)}, {});
    require(result.has_value() && result->success, std::string(module) + "." + function + " failed");
    return result->data;
}

int main(int argc, char** argv) {
    try {
        require(argc == 4, "expected mode input output");
        const std::string mode = argv[1];
        Json output;
        if (mode == "fixture") {
            const fs::path root = fs::u8path(argv[2]);
            require(UI::SceneFolders::create_scene_folder(root, "Roundtrip").has_value(), "create scene");
            const auto source = root.parent_path() / "triangle.ply";
            {
                std::ofstream file(source);
                file << "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n2 0 0\n0 3 0\n3 0 1 2\n";
            }
            UI::SceneFolders::SceneAssetStore store(root);
            const auto asset = store.import_model(source);
            require(asset.ok() && store.write_manifest(), "archive PLY");
            auto& manager = Corona::Resource::ResourceManager::get_instance();
            manager.register_parser<Corona::Resource::SceneParser>();
            Corona::Resource::ModelImportOptions options;
            options.scene_geometry = true;
            const auto id = manager.import_sync(root / asset.main_route, options);
            auto model = manager.acquire_read<Corona::Resource::Scene>(id);
            require(model && !model->data.meshes.empty() && model->data.meshes[0].indices.size() == 3, "decode archived PLY");
            std::vector<UI::SceneFolders::Diagnostic> diagnostics;
            require(UI::SceneFolders::SceneDocumentStore(root).replace_sections({
                {"physics", {"[physics]", "gravity=1,-3,2", "floor_y=-4", "floor_restitution=0.25", "fixed_dt=0.02"}},
                {"actors", {"[actors]", "a.name=Probe", "a.actor_guid=probe", "a.route=" + asset.main_route,
                    "a.mechanics.body_type=static", "a.camera_lock.enabled=true", "a.camera_lock.position_offset=1,2,3",
                    "a.camera_lock.rotation_offset=0.1,0.2,0.3"}}}, diagnostics), "write scene fixture");
            output = {{"route", asset.main_route}};
        } else {
            auto& kernel = Corona::Kernel::KernelContext::instance();
            require(kernel.initialize(), "initialize kernel");
            UI::register_builtin_native_api_handlers();
            if (mode == "open") {
                output = invoke("ProjectLauncher", "open_project", {argv[2], {{"load_policy", "degraded"}}});
            } else {
                require(mode == "roundtrip", "unknown mode");
                std::ifstream file(fs::u8path(argv[2]));
                const auto snapshot = Json::parse(file);
                UI::load_native_editor_scene_for_test(snapshot);
                const auto route = snapshot.at("scene").at("route");
                output["environment"] = invoke("SceneTools", "get_environment", {route});
                output["scene"] = invoke("SceneTools", "get_scene_snapshot", {route});
                output["camera_lock_active"] = Corona::Systems::CameraFollowController::instance().is_active();
                output["save"] = invoke("MainView", "scene_save", {route});
            }
            UI::reset_native_editor_scene_for_test();
            kernel.shutdown();
        }
        std::ofstream file(fs::u8path(argv[3]));
        file << output.dump(2);
        require(bool(file), "write test result");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
