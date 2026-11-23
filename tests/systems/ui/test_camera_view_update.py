"""Run the native camera-update handler against a small CPU camera boundary.

This compiles the actual handler body, with storage and renderer calls replaced
by a camera whose saved window state differs from the active embedded viewport.
It needs no engine, GPU, CEF, or writes to the running engine's build directory.
"""

from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from toolchain import find_compiler  # noqa: E402
import shutil
import subprocess
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope="module")
def camera_update_harness():
    compiler = find_compiler()
    if not compiler:
        pytest.skip("Standalone CPU regression requires g++")
    headers = list((Path.home() / ".conan2/p").glob("nlohm*/p/include/nlohmann/json.hpp"))
    if not headers:
        pytest.skip("Standalone CPU regression requires the project's nlohmann header")
    source = (ROOT / "src/systems/ui/cef/editor_scene_service.cpp").read_text(encoding="utf-8")
    operation_start = source.index("SceneReply apply_native_camera_properties(")
    operation_end = source.index("\nSceneReply apply_native_camera_render_settings(", operation_start)
    header = (ROOT / "src/systems/ui/cef/cef_scene_operations.h").read_text(encoding="utf-8")
    patch = header[header.index("struct CameraPropertiesPatch {"):header.index("struct CameraPropertiesSnapshot :")]
    # The camera property validation lives in a CEF-free domain service now; the
    # harness inlines it so the extracted handler body still compiles standalone.
    # The services share one header, so take only the pieces this harness needs: the shared
    # ValidationError and the CameraPropertiesService block. Inlining the whole header would
    # drag in engine and JSON includes that the standalone compile has no path for.
    service_text = (ROOT / "src/systems/ui/editor_api/editor_api_services.h").read_text(encoding="utf-8")
    validation_start = service_text.index("struct ValidationError {")
    validation = service_text[validation_start:service_text.index("};", validation_start) + 2]
    camera_start = service_text.index("namespace Corona::Systems::UI::CameraPropertiesService {")
    camera_end = service_text.index("}  // namespace Corona::Systems::UI::CameraPropertiesService")
    camera_block = service_text[camera_start:camera_end].replace(
        "namespace Corona::Systems::UI::CameraPropertiesService {", "namespace CameraPropertiesService {")
    service_header = ("namespace Corona::Systems::UI {\n" + validation + "\n"
                      + camera_block + "}\n}\n")
    service_source = (ROOT / "src/systems/ui/editor_api/camera_properties_service.cpp").read_text(encoding="utf-8")
    service_source = service_source.replace('#include "editor_api_services.h"', "")
    service = service_header + "\n" + service_source
    runtime = (ROOT / "src/engine/engine_runtime_api.cpp").read_text(encoding="utf-8")
    runtime_start = runtime.index("void Corona::API::Camera::set_view(")
    runtime_end = runtime.index("Corona::API::CameraViewState Corona::API::Camera::get_view()", runtime_start)
    optics = (ROOT / "src/systems/optics/optics_system.cpp").read_text(encoding="utf-8")
    consume_start = optics.index("            if (Corona::has_camera_state_field(\n                    update.fields, Corona::CameraStateUpdateField::ViewState)) {")
    consume_end = optics.index("\n        }\n    }\n    return updates;", consume_start)
    cpp = r'''
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <iostream>
#include <string>
using Json = nlohmann::json;
using NativeResult = Json;
PATCH
SERVICE
struct Snapshot { float projection_x_sign=1, near_plane=.1f, far_plane=100; };
struct SceneReply { std::string error_code,error_message; Snapshot camera; struct { Snapshot camera; } write; };
enum class SceneMethod { SetCameraProperties, UpdateCamera };
struct SceneRequest {
 SceneMethod method;
 std::optional<std::string> archive_session;
 CameraPropertiesPatch camera_properties;
};
namespace Corona::API { struct CameraViewState {
    std::optional<bool> open;
    int x=120, y=120, width=960, height=540;
    float move_speed=1.0f;
}; }
namespace Corona::API { struct CameraPose {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> forward{0.0f, 0.0f, 1.0f};
    std::array<float, 3> world_up{0.0f, 1.0f, 0.0f};
    float fov{45.0f};
}; }
struct EngineCamera {
    int width=1920, height=1080, x=0, y=0, view_width=1536, view_height=825;
    bool open=false; float speed=1; int geometry_calls=0, size_calls=0;
    void set_size(int w, int h) { width=w; height=h; ++size_calls; }
    int handle_=1;
    bool& view_open=open; int& view_x=x; int& view_y=y; float& move_speed=speed;
    void set_view(const Corona::API::CameraViewState&);
    Corona::API::CameraViewState get_view() {
        Corona::API::CameraViewState state;
        state.open = open;
        state.x = x; state.y = y; state.width = view_width; state.height = view_height;
        state.move_speed = speed;
        return state;
    }
    std::array<float,3> get_projection() { return {1,.1f,100}; }
    void set_projection(float,float,float) {}
    Corona::API::CameraPose get_pose() { return Corona::API::CameraPose{}; }
                void set(std::array<float,3>,std::array<float,3>,std::array<float,3>,float) {}
    void set_move_speed(float s) { speed=std::max(s,.01f); }
};
EngineCamera engine;
#define CFW_LOG_WARNING(...)
enum class CameraStateUpdateField { ViewState, ViewSettings };
struct CameraStateUpdateCommand {
 int camera_handle=0; CameraStateUpdateField fields; bool view_open=false;
 int view_x=0,view_y=0,view_width=0,view_height=0; float move_speed=1;
};
namespace Corona {
using CameraStateUpdateField=::CameraStateUpdateField;
bool has_camera_state_field(CameraStateUpdateField fields,CameraStateUpdateField field) { return fields==field; }
}
struct SharedDataHub {
 static SharedDataHub& instance() { static SharedDataHub hub;return hub; }
 void enqueue_camera_state_update(const CameraStateUpdateCommand& update) {
  auto* camera=&engine;
  if(update.fields==CameraStateUpdateField::ViewState) ++engine.geometry_calls;
  CONSUME_VIEW_STATE
 }
};
RUNTIME_VIEW_METHODS
struct WindowState { std::atomic_int requested_open{-1}; };
struct Camera {
    std::string camera_id="camera-id";
    std::string name="camera-name";
    std::shared_ptr<WindowState> window_state=std::make_shared<WindowState>();
    bool view_open=false; int view_x=120, view_y=120, view_width=960, view_height=540;
    int width=1280, height=720; float move_speed=1; EngineCamera* engine_camera=&engine;
};
struct Scene {
 std::string route="scene",archive_session="archive";
 std::uint64_t tree_revision=1;
 bool read_only=false;
 std::vector<Camera> cameras{Camera{}};
};
struct State { std::unique_ptr<Scene> scene=std::make_unique<Scene>(); } state;
Scene& scene=*state.scene;
Camera& camera=scene.cameras.front();
State& native_editor_state() { return state; }
bool native_scene_request_matches(Scene& s,const std::string& route) { return s.route==route; }
Snapshot camera_properties_snapshot(Scene&,Camera&) { return {}; }
struct NativeRequest { Json args; }; struct NativeContext {};
Scene* scene_for_request_route(const NativeRequest&) { return &scene; }
std::string arg_string(const Json& a,int i) { return a[i].get<std::string>(); }
Camera* find_native_camera(Scene&,const std::string&) { return &camera; }
// The extracted handler body is namespace-qualified in the engine; this alias
// keeps the snippet compiling at global scope in the harness.
namespace CameraPropertiesService = Corona::Systems::UI::CameraPropertiesService;
OPERATION
int main(int argc,char** argv) {
    const auto state=Json::parse(argv[1]);
    SceneRequest request{SceneMethod::UpdateCamera};
    request.archive_session=scene.archive_session;
    auto& patch=request.camera_properties;
    patch.scene=scene.route;
    patch.camera_id=camera.camera_id;
    if (state.contains("move_speed")) patch.move_speed=state["move_speed"].get<float>();
    if (state.contains("width")) patch.width=state["width"].get<int>();
    if (state.contains("height")) patch.height=state["height"].get<int>();
    if (state.contains("view_open")) patch.view_open=state["view_open"].get<bool>();
    if (state.contains("view_x")) patch.view_x=state["view_x"].get<int>();
    if (state.contains("view_y")) patch.view_y=state["view_y"].get<int>();
    if (state.contains("view_width")) patch.view_width=state["view_width"].get<int>();
    if (state.contains("view_height")) patch.view_height=state["view_height"].get<int>();
    const auto reply=apply_native_camera_properties(request);
    if (!reply.error_code.empty()) {
        std::cout << Json{{"error_code",reply.error_code}}.dump();
        return 0;
    }
    std::cout << Json{{"width",engine.width},{"height",engine.height},
      {"rect",{engine.x,engine.y,engine.view_width,engine.view_height}},
      {"open",engine.open},{"speed",engine.speed},
      {"geometry_calls",engine.geometry_calls},{"size_calls",engine.size_calls}}.dump();
}
'''.replace("PATCH", patch)
    cpp = cpp.replace("SERVICE", service)
    cpp = cpp.replace("OPERATION", source[operation_start:operation_end])
    cpp = cpp.replace("CONSUME_VIEW_STATE", optics[consume_start:consume_end])
    cpp = cpp.replace("RUNTIME_VIEW_METHODS", runtime[runtime_start:runtime_end].replace("Corona::API::Camera::", "EngineCamera::"))
    temporary_root = ROOT / ".workspace/tmp/view-state-regression"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as directory:
        directory = Path(directory)
        source_path, executable = directory / "harness.cpp", directory / "harness.exe"
        source_path.write_text(cpp, encoding="utf-8")
        built = subprocess.run([compiler, "-std=c++17", "-O0", "-I" + str(headers[0].parents[1]),
                                str(source_path), "-o", str(executable)], capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        yield executable


def run_update(executable, state):
    import json
    result = subprocess.run([str(executable), json.dumps(state)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_resolution_only_keeps_live_viewport_rect(camera_update_harness):
    actual = run_update(camera_update_harness, {"width": 1920, "height": 1080})
    assert actual["rect"] == [0, 0, 1536, 825]
    assert (actual["width"], actual["height"]) == (1920, 1080)
    assert actual["geometry_calls"] == 0


def test_speed_only_keeps_live_viewport_and_render_size(camera_update_harness):
    actual = run_update(camera_update_harness, {"move_speed": 3.5})
    assert actual["rect"] == [0, 0, 1536, 825]
    assert actual["speed"] == 3.5
    assert actual["size_calls"] == 0
    assert actual["geometry_calls"] == 0


def test_empty_update_does_not_replay_saved_window_state(camera_update_harness):
    # A patch with no property at all is refused before it touches the camera, so the
    # saved window state cannot be replayed into the live viewport.
    actual = run_update(camera_update_harness, {})
    assert actual["error_code"] == "INVALID_ARGUMENT"


def test_explicit_window_geometry_updates_the_rect_without_owning_the_open_state(camera_update_harness):
    actual = run_update(camera_update_harness, {
        "view_x": 40, "view_y": 50, "view_width": 800, "view_height": 600,
    })
    assert actual["rect"] == [40, 50, 800, 600]
    # Only the rectangle moved: the open state is not this patch's to change, so no
    # ViewState command reached the geometry side.
    assert actual["open"] is False
    assert actual["geometry_calls"] == 0


def test_a_camera_patch_cannot_drive_the_window_lifecycle(camera_update_harness):
    # `view_open` belongs to window operations; the non-Web caller that used to be
    # exempt went with the script runtime, so the patch refuses it like any other.
    actual = run_update(camera_update_harness, {"view_open": True})
    assert actual["error_code"] == "INVALID_ARGUMENT"
