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
    cpp = r'''
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <string>
using Json = nlohmann::json;
struct EngineCamera {
    int width=1920, height=1080, x=0, y=0, view_width=1536, view_height=825;
    bool open=false; float speed=1; int geometry_calls=0, size_calls=0;
    void set_size(int w, int h) { width=w; height=h; ++size_calls; }
    void set_view_state(bool o,int px,int py,int w,int h,float s) {
        open=o; x=px; y=py; view_width=w; view_height=h; speed=s; ++geometry_calls;
    }
    void set_move_speed(float s) { speed=std::max(s,.01f); }
};
EngineCamera engine;
struct Camera {
    bool view_open=false; int view_x=120, view_y=120, view_width=960, view_height=540;
    int width=1280, height=720; float move_speed=1; EngineCamera* engine_camera=&engine;
} camera;
struct Scene { std::string route="scene"; } scene;
struct NativeRequest { Json args; }; struct NativeContext {};
Scene* scene_for_request_route(const NativeRequest&) { return &scene; }
std::string arg_string(const Json& a,int i) { return a[i].get<std::string>(); }
Camera* find_native_camera(Scene&,const std::string&) { return &camera; }
Json camera_not_found_result(const std::string&) { return {{"status","error"}}; }
bool json_bool_value(const Json& j,const char* k,bool v) { return j.value(k,v); }
int json_int_value(const Json& j,const char* k,int v) { return j.value(k,v); }
float json_float_value(const Json& j,const char* k,float v) { return j.value(k,v); }
void persist_native_scene_cameras(Scene&) {}
Json camera_to_json(Camera&) { return Json::object(); }
Json native_success(Json j) { return j; }
Json update(const NativeRequest& request, const NativeContext&) {
HANDLER
}
int main(int argc,char** argv) {
    update({Json::array({"scene","source-camera",Json::parse(argv[1])})},{});
    std::cout << Json{{"width",engine.width},{"height",engine.height},
      {"rect",{engine.x,engine.y,engine.view_width,engine.view_height}},
      {"open",engine.open},{"speed",engine.speed},
      {"geometry_calls",engine.geometry_calls},{"size_calls",engine.size_calls}}.dump();
}
'''.replace("HANDLER", handler)
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
