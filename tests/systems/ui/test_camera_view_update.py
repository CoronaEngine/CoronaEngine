"""Run the native camera-update handler against a small CPU camera boundary.

This compiles the actual handler body, with storage and renderer calls replaced
by a camera whose saved window state differs from the active embedded viewport.
It needs no engine, GPU, CEF, or writes to the running engine's build directory.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope="module")
def camera_update_harness():
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("Standalone CPU regression requires g++")
    headers = list((Path.home() / ".conan2/p").glob("nlohm*/p/include/nlohmann/json.hpp"))
    if not headers:
        pytest.skip("Standalone CPU regression requires the project's nlohmann header")
    source = (ROOT / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp").read_text(encoding="utf-8")
    start = source.index('{"update_camera_view", []')
    start = source.index("{", source.index("const NativeContext&)", start)) + 1
    end = source.index("\n        }},", start)
    handler = source[start:end]
    operation_start = source.index("SceneReply apply_native_camera_properties(")
    operation_end = source.index("\nSceneReply apply_native_camera_render_settings(", operation_start)
    adapter_start = source.index("NativeResult apply_non_web_camera_properties(")
    adapter_end = source.index("\nnamespace {", adapter_start)
    header = (ROOT / "src/systems/ui/cef/cef_scene_operations.h").read_text(encoding="utf-8")
    patch = header[header.index("struct CameraPropertiesPatch {"):header.index("struct CameraPropertiesSnapshot :")]
    runtime = (ROOT / "src/engine/engine_runtime_api.cpp").read_text(encoding="utf-8")
    runtime_start = runtime.index("void Corona::API::Camera::set_view_state(")
    runtime_end = runtime.index("std::array<float, 6> Corona::API::Camera::get_view_state()", runtime_start)
    optics = (ROOT / "src/systems/optics/optics_system.cpp").read_text(encoding="utf-8")
    consume_start = optics.index("            if (Corona::has_camera_state_field(\n                    update.fields, Corona::CameraStateUpdateField::ViewState)) {")
    consume_end = optics.index("\n        }\n    }\n    return updates;", consume_start)
    cpp = r'''
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>
#include <iostream>
#include <string>
using Json = nlohmann::json;
using NativeResult = Json;
PATCH
struct Snapshot { float projection_x_sign=1, near_plane=.1f, far_plane=100; };
struct SceneReply { std::string error_code,error_message; Snapshot camera; };
enum class SceneMethod { SetCameraProperties };
struct SceneRequest {
 SceneMethod method;
 std::optional<std::string> archive_session;
 CameraPropertiesPatch camera_properties;
};
namespace Corona::API { bool is_editor_camera_input_enabled() { return true; } }
struct EngineCamera {
    int width=1920, height=1080, x=0, y=0, view_width=1536, view_height=825;
    bool open=false; float speed=1; int geometry_calls=0, size_calls=0;
    void set_size(int w, int h) { width=w; height=h; ++size_calls; }
    int handle_=1;
    bool& view_open=open; int& view_x=x; int& view_y=y; float& move_speed=speed;
    void set_view_state(bool,int,int,int,int,float);
    void set_view_settings(int,int,int,int,float);
    std::array<float,6> get_view_state() { return {float(open),float(x),float(y),float(view_width),float(view_height),speed}; }
    std::array<float,3> get_projection() { return {1,.1f,100}; }
    void set_projection(float,float,float) {}
    std::array<float,3> get_position() { return {}; }
    std::array<float,3> get_forward() { return {0,0,-1}; }
    std::array<float,3> get_world_up() { return {0,1,0}; }
    float get_fov() { return 60; }
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
    std::shared_ptr<WindowState> window_state=std::make_shared<WindowState>();
    bool view_open=false; int view_x=120, view_y=120, view_width=960, view_height=540;
    int width=1280, height=720; float move_speed=1; EngineCamera* engine_camera=&engine;
};
struct Scene {
 std::string route="scene",archive_session="archive";
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
Json arg_object(const Json& a,int i) { return a[i]; }
std::optional<std::array<float,3>> json_float3_value(const Json& j) { return j.get<std::array<float,3>>(); }
Json native_failure(const std::string& message,int) { return {{"status","error"},{"message",message}}; }
Json camera_not_found_result(const std::string&) { return {{"status","error"}}; }
bool json_bool_value(const Json& j,const char* k,bool v) { return j.value(k,v); }
int json_int_value(const Json& j,const char* k,int v) { return j.value(k,v); }
float json_float_value(const Json& j,const char* k,float v) { return j.value(k,v); }
void persist_native_scene_cameras(Scene&) {}
Json camera_to_json(Camera&) { return Json::object(); }
Json native_success(Json j) { return j; }
OPERATION
ADAPTER
Json update(const NativeRequest& request, const NativeContext&) {
HANDLER
}
int main(int argc,char** argv) {
    if (argc > 2) {
        SceneRequest request{SceneMethod::UpdateCamera};
        request.archive_session=scene.archive_session;
        request.camera_properties.scene=scene.route;
        request.camera_properties.camera_id=camera.camera_id;
        request.camera_properties.move_speed=Json::parse(argv[1])["move_speed"].get<float>();
        const auto reply=apply_native_camera_properties(request);
        if (!reply.error_code.empty()) return 2;
    } else {
        update({Json::array({"scene","source-camera",Json::parse(argv[1])})},{});
    }
    std::cout << Json{{"width",engine.width},{"height",engine.height},
      {"rect",{engine.x,engine.y,engine.view_width,engine.view_height}},
      {"open",engine.open},{"speed",engine.speed},
      {"geometry_calls",engine.geometry_calls},{"size_calls",engine.size_calls}}.dump();
}
'''.replace("HANDLER", handler).replace("PATCH", patch)
    cpp = cpp.replace("OPERATION", source[operation_start:operation_end])
    cpp = cpp.replace("ADAPTER", source[adapter_start:adapter_end])
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


def run_update(executable, state, *, typed=False):
    import json
    result = subprocess.run([str(executable), json.dumps(state), *(["typed"] if typed else [])], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def test_resolution_only_keeps_live_viewport_rect(camera_update_harness):
    actual = run_update(camera_update_harness, {"width": 1920, "height": 1080})
    assert actual["rect"] == [0, 0, 1536, 825]
    assert (actual["width"], actual["height"]) == (1920, 1080)
    assert actual["geometry_calls"] == 0


@pytest.mark.parametrize("typed", [False, True], ids=["non-web-adapter", "typed-scene-operation"])
def test_speed_only_keeps_live_viewport_and_render_size(camera_update_harness, typed):
    actual = run_update(camera_update_harness, {"move_speed": 3.5}, typed=typed)
    assert actual["rect"] == [0, 0, 1536, 825]
    assert actual["speed"] == 3.5
    assert actual["size_calls"] == 0
    assert actual["geometry_calls"] == 0


def test_empty_update_does_not_replay_saved_window_state(camera_update_harness):
    actual = run_update(camera_update_harness, {})
    assert actual["rect"] == [0, 0, 1536, 825]
    assert actual["size_calls"] == 0
    assert actual["geometry_calls"] == 0


@pytest.mark.parametrize("opened", [True, False])
def test_explicit_window_geometry_preserves_open_close_semantics(camera_update_harness, opened):
    actual = run_update(camera_update_harness, {
        "view_open": opened, "view_x": 40, "view_y": 50,
        "view_width": 800, "view_height": 600,
    })
    assert actual["rect"] == [40, 50, 800, 600]
    assert actual["open"] is opened
    assert actual["geometry_calls"] == 1
