"""Run native pointer mapping against the compositor's fitted image, without CEF/GPU."""
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope="module")
def pointer_harness():
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("Standalone pointer mapping regression requires g++")
    realtime = (ROOT / "src/systems/ui/cef/cef_realtime_bridge.cpp").read_text(encoding="utf-8")
    native = (ROOT / "src/systems/ui/cef/cef_editor_native_api_handlers.cpp").read_text(encoding="utf-8")
    pick_start = realtime.index("\n\n", realtime.index('emit_error("duplicate requestId");')) + 2
    pick_end = realtime.index("    if (actor_pick_handle == 0)", pick_start)
    publish_start = realtime.index("        image_point = InputDetail::map_viewport_image_point", realtime.index("bool handle_viewport_pick("))
    publish_end = realtime.index("\n", publish_start)
    gizmo_start = realtime.index("    const float camera_width =", realtime.index("bool handle_viewport_gizmo_pointer("))
    gizmo_end = realtime.index("    const auto ui_state =", gizmo_start)
    bounds_start = realtime.index("    const bool cancel_event =", gizmo_end)
    bounds_end = realtime.index("    if (down_event &&", bounds_start)
    cursor_start = realtime.index("    Corona::ViewportUiPointerCommand command;", realtime.index("bool handle_viewport_ui_pointer("))
    cursor_end = realtime.index("    double buttons =", cursor_start)
    api_start = native.index("\n            }\n", native.index('return native_failure("Invalid viewport size", 2);')) + len("\n            }\n")
    api_end = native.index("            const auto picked =", api_start)
    includes = '\n'.join(line for line in realtime.splitlines()
                         if line.startswith('#include "') and "viewport_image" in line)
    imgui = (ROOT / "src/systems/ui/imgui/imgui_ui.cpp").read_text(encoding="utf-8")
    hide_start = imgui.index("    if (auto* hover = manager.get_tab(hovered_tab_);")
    hide_end = imgui.index("    render_imgui_platform_windows", hide_start)
    program = r'''
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "src/systems/display/aspect_fit.h"
MAPPING_INCLUDE
#define CFW_LOG_DEBUG(...)
#define CFW_LOG_WARNING(...)
struct Point { double x=0,y=0; } observed;
bool mapped=false;
struct Result;
struct AxisState { int active=0; bool dragging=false; } last_state;
struct Completion {
    int camera; std::string scene, request, status; unsigned handle,x,y;
};
struct EngineCamera { int get_handle() const { return 1; } } engine;
struct Camera {
    unsigned width=1920,height=1080;
    bool viewport_rect_active=true,view_open=false;
    int view_width=1536,view_height=825;
    EngineCamera* engine_camera=&engine;
} camera_value;
struct Hub {
    static Hub& instance() { static Hub value; return value; }
    Hub& camera_storage() { return *this; }
    Camera* try_acquire_read(int) { return &camera_value; }
    void enqueue_actor_pick_completion(Completion) {}
    template<class Axis> void update_viewport_gizmo_interaction(int,Axis axis,bool dragging) {
        last_state={int(axis),dragging};
    }
} hub;
namespace ktm { struct fvec2 { float x,y; }; struct fvec3 { float x,y,z; }; }
namespace Corona {
enum class ViewportGizmoAxis { None, X };
using SharedDataHub=::Hub;
struct ViewportUiPointerCommand { int camera_handle=0; std::string event_type; float x=0,y=0; };
}
namespace InputDetail=Corona::Systems::UI::InputDetail;
struct Result {
    Corona::ViewportGizmoAxis axis=Corona::ViewportGizmoAxis::None;
    bool consumed=false,ended=false,dragging=false,cancelled=false;
    ktm::fvec3 position{};
} last_result;
void emit_viewport_gizmo_result(Result result,int) { last_result=result; }
void emit_viewport_gizmo_transform(int,ktm::fvec3) {}
bool write_actor_gizmo_position(int,ktm::fvec3) { return true; }
struct Drag {
    int actor_handle=1; Corona::ViewportGizmoAxis axis=Corona::ViewportGizmoAxis::X;
    ktm::fvec3 last_position{5,6,7},initial_position{1,2,3};
    std::string actor_name="actor";
};
std::unordered_map<int,Drag> s_viewport_gizmo_drags;
struct Browser { int GetIdentifier() { return 1; } } browser_value;
struct Client { Browser* GetBrowser() { return &browser_value; } } client;
struct Tab {
    Client* client=&::client; std::mutex mutex;
    int web_camera_handle=1,web_camera_render_width=1536,web_camera_render_height=825;
    std::atomic_bool hide_system_cursor{false},use_custom_system_cursor{false};
    std::atomic_bool cursor_inside_camera_image{true};
};
struct BrowserManager {
    static BrowserManager& instance() { static BrowserManager value;return value; }
    std::shared_ptr<Tab> tab=std::make_shared<Tab>();
    auto tabs_snapshot() { return std::vector{std::pair{1,tab}}; }
};
struct Args {
    std::string event_type="pointermove";
    struct Text { std::string value; std::string ToString() { return value; } };
    Text GetString(int) { return {event_type}; }
} args_value;
bool system_cursor_hidden=false;
void SDL_HideCursor() { system_cursor_hidden=true; }
void SDL_ShowCursor() { system_cursor_hidden=false; }
void present_cursor() {
    struct Manager { Tab* get_tab(int) { return BrowserManager::instance().tab.get(); } } manager;
    int hovered_tab_=1;
    CURSOR_PRESENTATION
}
std::optional<Point> native_success(std::initializer_list<std::pair<const char*,const char*>>) {
    return std::nullopt;
}
bool live_pick(double x,double y,double vp_w,double vp_h) {
    const double cam_w=camera_value.width,cam_h=camera_value.height;
    const int camera_handle=1;
    const std::string scene_id="scene",request_id="pick";
    const auto* cam=&camera_value;
    std::optional<InputDetail::ImagePoint> image_point;
    PUBLISH_MAPPING
    PICK_MAPPING
    observed={double(pick_x),double(pick_y)};mapped=true;
    return true;
}
bool gizmo(double x_value,double y_value,double width_value,double height_value,std::string event_type="pointermove") {
    const auto* camera=&camera_value;
    const int camera_handle=1;
    Result result;
    struct { int target=0; } state;
    GIZMO_MAPPING
    auto drag_it=s_viewport_gizmo_drags.find(camera_handle);
    GIZMO_BOUNDS
    observed={pointer.x,pointer.y};mapped=true;
    return true;
}
void cursor(double x,double y) {
    auto* args=&args_value;
    auto* browser=&browser_value;
    const double camera_value=1;
    CURSOR_MAPPING
    mapped=command.event_type!="pointerleave";
    observed={command.x,command.y};
}
std::optional<Point> api_pick(double x,double y,double viewport_width,double viewport_height) {
    const auto* camera=&camera_value;
    API_MAPPING
    return Point{double(pick_x),double(pick_y)};
}
int main(int argc,char** argv) {
    const int mode=std::stoi(argv[1]);
    const double x=std::stod(argv[2]),y=std::stod(argv[3]);
    if (argc>4) {
        camera_value.view_width=std::stoi(argv[4]);
        camera_value.view_height=std::stoi(argv[5]);
        camera_value.width=std::stoi(argv[6]);
        camera_value.height=std::stoi(argv[7]);
    }
    const double input_width=argc>8 ? std::stod(argv[8]) : camera_value.view_width;
    const double input_height=argc>9 ? std::stod(argv[9]) : camera_value.view_height;
    auto tab=BrowserManager::instance().tab;
    tab->web_camera_render_width=int(input_width);
    tab->web_camera_render_height=int(input_height);
    if(mode==0) live_pick(x,y,input_width,input_height);
    else if(mode==1) gizmo(x,y,input_width,input_height);
    else if(mode==2) {
        auto result=api_pick(x,y,input_width,input_height);
        if(result) { observed=*result;mapped=true; }
    }
    else if(mode==3) cursor(x,y);
    else if(mode==7) {
        tab->hide_system_cursor=true;tab->use_custom_system_cursor=true;
        cursor(768,400);present_cursor();std::cout << system_cursor_hidden << ' ';
        cursor(0,400);present_cursor();std::cout << system_cursor_hidden << ' ';
        cursor(768,400);present_cursor();std::cout << system_cursor_hidden << ' ';
        args_value.event_type="pointerleave";
        cursor(768,400);present_cursor();std::cout << system_cursor_hidden << ' ';
        args_value.event_type="pointermove";
        tab->hide_system_cursor=false;tab->use_custom_system_cursor=false;
        cursor(768,400);present_cursor();std::cout << system_cursor_hidden;
        return 0;
    }
    else if(mode==6) {
        tab->web_camera_handle=0;
        cursor(x,y);
    }
    else if(mode==4 || mode==5) {
        s_viewport_gizmo_drags[1]=Drag{};
        gizmo(x,y,camera_value.view_width,camera_value.view_height,mode==4 ? "pointerup" : "pointercancel");
        std::cout << last_result.consumed << ' ' << last_result.ended << ' '
                  << last_result.cancelled << ' ' << s_viewport_gizmo_drags.size() << ' '
                  << last_result.position.x << ' ' << last_result.position.y << ' ' << last_result.position.z;
        return 0;
    }
    std::cout << mapped << ' ' << observed.x << ' ' << observed.y;
}
'''.replace("MAPPING_INCLUDE", includes).replace("PICK_MAPPING", realtime[pick_start:pick_end])
    program = program.replace("PUBLISH_MAPPING", realtime[publish_start:publish_end])
    program = program.replace("GIZMO_MAPPING", realtime[gizmo_start:gizmo_end])
    program = program.replace("GIZMO_BOUNDS", realtime[bounds_start:bounds_end])
    program = program.replace("CURSOR_MAPPING", realtime[cursor_start:cursor_end])
    program = program.replace("API_MAPPING", native[api_start:api_end])
    program = program.replace("CURSOR_PRESENTATION", imgui[hide_start:hide_end])
    temporary_root = ROOT / ".workspace/tmp/classified-commits/viewport-input"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as directory:
        source, executable = Path(directory) / "pointer.cpp", Path(directory) / "pointer.exe"
        source.write_text(program, encoding="utf-8")
        built = subprocess.run([compiler, "-std=c++20", "-I" + str(ROOT),
                                "-I" + str(ROOT / "src/systems/ui/cef"),
                                str(source), "-o", str(executable)], capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        yield executable


def run_pointer(executable, mode, x, y, dimensions=()):
    result = subprocess.run([str(executable), str(mode), str(x), str(y), *map(str, dimensions)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    mapped, actual_x, actual_y = map(float, result.stdout.split())
    return bool(mapped), actual_x, actual_y


@pytest.mark.parametrize("mode", [0, 1, 2, 3], ids=["live-pick", "gizmo", "scene-api-pick", "stereo-cursor"])
def test_displayed_left_edge_maps_to_camera_left_edge(pointer_harness, mode):
    mapped, x, y = run_pointer(pointer_harness, mode, 34, 0)
    assert mapped
    assert (x, y) == (0, 0), "The displayed image starts at x=34, after the black bar"


@pytest.mark.parametrize("mode", [0, 1, 2, 3], ids=["live-pick", "gizmo", "scene-api-pick", "stereo-cursor"])
@pytest.mark.parametrize("x,y", [(0, 400), (33, 400), (1501, 400), (768, 825)])
def test_black_bars_and_image_bottom_do_not_hit_camera(pointer_harness, mode, x, y):
    assert not run_pointer(pointer_harness, mode, x, y)[0]


@pytest.mark.parametrize("mode", [0, 1, 2], ids=["live-pick", "gizmo", "scene-api-pick"])
def test_top_bar_is_rejected_and_visible_top_maps_to_zero(pointer_harness, mode):
    dimensions = (1024, 1024, 1920, 1080)
    assert not run_pointer(pointer_harness, mode, 512, 223, dimensions)[0]
    mapped, x, y = run_pointer(pointer_harness, mode, 512, 224, dimensions)
    assert mapped
    assert (x, y) == (960, 0)


@pytest.mark.parametrize("mode", [0, 1, 2], ids=["live-pick", "gizmo", "scene-api-pick"])
def test_matching_aspect_keeps_existing_mapping(pointer_harness, mode):
    assert run_pointer(pointer_harness, mode, 480, 270, (960, 540, 1920, 1080)) == (True, 960, 540)


@pytest.mark.parametrize("mode,want", [
    (4, [1, 1, 0, 0, 5, 6, 7]),
    (5, [1, 0, 1, 0, 1, 2, 3]),
], ids=["release-finishes-at-last-position", "cancel-restores-initial-position"])
def test_captured_gizmo_drag_ends_in_black_bar(pointer_harness, mode, want):
    result = subprocess.run([str(pointer_harness), str(mode), "0", "400"],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert list(map(float, result.stdout.split())) == want


@pytest.mark.parametrize("mode", [0, 1, 2, 3], ids=["live-pick", "gizmo", "scene-api-pick", "stereo-cursor"])
def test_css_input_uses_the_physical_viewport_rounding(pointer_harness, mode):
    # At 150%, the physical 2304x1238 viewport fits to x=51,width=2201.
    # CSS x=34 is exactly that displayed left edge, independently of image size.
    dimensions = (2304, 1238, 1920, 1080, 1536, 825)
    mapped, x, y = run_pointer(pointer_harness, mode, 34, 0, dimensions)
    assert mapped
    assert (x, y) == (0, 0)
    assert not run_pointer(pointer_harness, mode, 33, 0, dimensions)[0]


def test_detached_cursor_keeps_its_existing_render_pixel_contract(pointer_harness):
    assert run_pointer(pointer_harness, 6, 480, 270) == (True, 480, 270)


def test_cursor_handoff_tracks_image_bounds_without_new_frontend_visibility_messages(pointer_harness):
    result = subprocess.run([str(pointer_harness), "7", "0", "0"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert list(map(int, result.stdout.split())) == [1, 0, 1, 0, 0]
