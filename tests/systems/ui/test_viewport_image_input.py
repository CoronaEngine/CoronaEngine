"""Run native pointer mapping against the compositor's fitted image, without CEF/GPU."""
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope="module")
def pointer_harness(tmp_path_factory):
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("Standalone pointer mapping regression requires g++")
    base = ROOT / "src/systems/ui/cef"
    pick = (base / "cef_viewport_pick.cpp").read_text(encoding="utf-8")
    gizmo = (base / "cef_viewport_gizmo.cpp").read_text(encoding="utf-8")
    native = (base / "cef_editor_native_api_handlers.cpp").read_text(encoding="utf-8")
    window = (base / "cef_window_operations.cpp").read_text(encoding="utf-8")
    header = (base / "cef_window_operations.h").read_text(encoding="utf-8")
    imgui = (ROOT / "src/systems/ui/imgui/imgui_ui.cpp").read_text(encoding="utf-8")
    mapping = pick[pick.index("ViewportPickPoint map_viewport_pick_point("):
                   pick.index("std::string ViewportPickRequests::add(")]
    pick_call = next(line.strip() for line in pick.splitlines()
                     if "const auto point = map_viewport_pick_point(request.x" in line)
    gizmo_start = gizmo.index("    const float camera_width =", gizmo.index("bool run_viewport_gizmo_pointer("))
    gizmo_end = gizmo.index("    const auto ui_state =", gizmo_start)
    bounds_start = gizmo.index("    const bool cancel_event =", gizmo_end)
    bounds_end = gizmo.index("    if (down_event &&", bounds_start)
    api_start = native.index("\n", native.index("const float viewport_height =", native.index('{"pick_actor_at_pixel"'))) + 1
    api_end = native.index("            const auto picked =", api_start)
    window_start = window.index("WindowReply apply_viewport_configuration(")
    window_end = window.index("\nbool request_application_exit()", window_start)
    release_start = window.index("void release_viewport_pointer(")
    release_end = window.index("\nvoid refresh_viewport_state(", release_start)
    cursor_start = window.index("constexpr const char* viewport_cursors[]")
    cursor_end = window.index("\n}", window.index("    return std::nullopt;", cursor_start)) + 2
    hide_start = imgui.index("    if (auto* hover = manager.get_tab(hovered_tab_);")
    hide_end = imgui.index("    render_imgui_platform_windows", hide_start)
    program = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include "src/systems/ui/cef/viewport_image_input.h"
#define CFW_LOG_WARNING(...)
struct Point { double x=0,y=0; } observed;
bool mapped=false;
namespace ktm { struct fvec2 { float x,y; }; struct fvec3 { float x,y,z; }; }
struct EngineCamera { int get_handle() const { return 1; } } engine;
struct Camera {
 unsigned width=1920,height=1080;
 bool viewport_rect_active=true,view_open=false;
 int view_width=1536,view_height=825;
 EngineCamera* engine_camera=&engine;
} camera_value;
namespace Corona {
using CameraDevice=::Camera;
enum class ViewportGizmoAxis { None, X };
enum class ViewportUiMode { Flat2D, Stereo3D };
enum class ViewportUiCursorShape { Arrow, Hand, Crosshair, Grab, Grabbing, Hidden };
struct ViewportUiCalibration {
 float lenticular_pitch=1,slant_angle_radians=0,phase_offset=0,parallax_scale=1;
};
struct Pointer { float x=0,y=0; ViewportUiCursorShape cursor_shape=ViewportUiCursorShape::Hidden; std::uint64_t owner_context=0; };
struct UiState { ViewportUiMode mode=ViewportUiMode::Stereo3D; ViewportUiCalibration calibration; Pointer pointer; };
}
struct Hub {
 Corona::UiState state;
 static Hub& instance() { static Hub value; return value; }
 Hub& camera_storage() { return *this; }
 Camera* try_acquire_read(int) { return &camera_value; }
 void update_viewport_gizmo_interaction(int,Corona::ViewportGizmoAxis,bool) {}
 auto viewport_ui_state(int) { return state; }
 void set_viewport_ui_mode(int,Corona::ViewportUiMode mode) { state.mode=mode; }
 void set_viewport_ui_calibration(int,Corona::ViewportUiCalibration c) { state.calibration=c; }
 void set_viewport_ui_pointer(int,Corona::Pointer pointer) { state.pointer=pointer; }
 void clear_viewport_ui_pointer(int,std::uint64_t owner) { if(state.pointer.owner_context==owner) state.pointer={}; }
};
namespace Corona { using SharedDataHub=::Hub; }
namespace InputDetail=Corona::Systems::UI::InputDetail;
struct Result {
 Corona::ViewportGizmoAxis axis=Corona::ViewportGizmoAxis::None;
 bool consumed=false,ended=false,dragging=false,cancelled=false;
 ktm::fvec3 position{};
} last_result;
void complete_gizmo_reply(const Result& result,Result* direct) { *direct=result; }
bool write_actor_gizmo_position(int,ktm::fvec3) { return true; }
struct Drag {
 int actor_handle=1; Corona::ViewportGizmoAxis axis=Corona::ViewportGizmoAxis::X;
 ktm::fvec3 last_position{5,6,7},initial_position{1,2,3};
 std::string actor_name="actor";
};
std::unordered_map<int,Drag> s_viewport_gizmo_drags;
struct CameraWindowAuthority { std::mutex mutex; bool active=true; };
struct BrowserTab {
 bool open=true,camera_view=false; int tab_id=1;
 std::string docking_pos="main";
 int web_camera_handle=1,web_camera_render_width=1536,web_camera_render_height=825;
 int web_camera_x=0,web_camera_y=0,web_camera_width=1536,web_camera_height=825;
 std::shared_ptr<CameraWindowAuthority> web_camera_authority;
 std::atomic_bool hide_system_cursor{false};
 std::uint64_t viewport_pointer_owner=0; int viewport_pointer_camera=0;
 std::weak_ptr<CameraWindowAuthority> viewport_pointer_authority;
} tab;
struct CameraViewportManager {
 struct Record { int camera_handle=1; };
 static CameraViewportManager& instance() { static CameraViewportManager v;return v; }
 std::optional<Record> find_by_tab(int) { return Record{}; }
};
VIEWPORT_DTOS
struct WindowRequest {
 int camera_handle=1; std::uint64_t context_id=1;
 std::string archive_session="archive",scene_id="scene",camera_id="camera";
 std::shared_ptr<CameraWindowAuthority> camera_authority=std::make_shared<CameraWindowAuthority>();
 std::optional<ViewportConfiguration> viewport;
};
struct WindowReply {
 std::string error_code,error_message; bool accepted=false;
 std::optional<ViewportConfigurationSnapshot> viewport;
};
CURSOR_SHAPES
RELEASE_POINTER
WINDOW_CONFIGURATION
namespace Corona::Systems::UI {
struct ViewportPickPoint { bool valid=false,inside=false; unsigned x=0,y=0; };
PICK_MAPPING
}
using Corona::Systems::UI::map_viewport_pick_point;
using Corona::Systems::UI::ViewportPickPoint;
std::optional<Point> native_success(std::initializer_list<std::pair<const char*,const char*>>) { return std::nullopt; }
std::optional<Point> native_failure(const char*,int) { return std::nullopt; }
bool live_pick(double x,double y,double width,double height) {
 struct { double x,y,width,height; } request{x,y,width,height};
 auto* native_camera=&camera_value;
 PICK_CALL
 if(point.inside) { observed={double(point.x),double(point.y)};mapped=true; }
 return true;
}
bool gizmo(double x,double y,double width,double height,std::string event_type="pointermove") {
 struct { double x,y,width,height; } request{x,y,width,height};
 auto* camera=&camera_value;
 const int camera_handle=1;
 auto& hub=Hub::instance();
 Result result; auto* direct=&last_result;
 GIZMO_MAPPING
 auto drag_it=s_viewport_gizmo_drags.find(camera_handle);
 GIZMO_BOUNDS
 observed={pointer.x,pointer.y};mapped=true;
 return true;
}
std::optional<Point> api_pick(double x,double y,double viewport_width,double viewport_height) {
 auto* camera=&camera_value;
 API_MAPPING
 return Point{double(point.x),double(point.y)};
}
void cursor(double x,double y,bool hidden=false) {
 WindowRequest request; request.viewport.emplace();
 request.viewport->pointer=ViewportPointer{x,y,hidden ? "hidden" : "arrow"};
 auto reply=apply_viewport_configuration(request,tab);
 if(!reply.accepted) { std::cerr<<reply.error_code; std::exit(2); }
 const auto p=Hub::instance().state.pointer;
 mapped=p.cursor_shape!=Corona::ViewportUiCursorShape::Hidden;
 observed={p.x,p.y};
}
bool system_cursor_hidden=false;
void SDL_HideCursor() { system_cursor_hidden=true; }
void present_cursor() {
 // A new UI frame starts with the system cursor visible; production hides it for the owning tab.
 system_cursor_hidden=false;
 struct Manager { BrowserTab* get_tab(int) { return &tab; } } manager;
 int hovered_tab_=1;
 CURSOR_PRESENTATION
}
int main(int argc,char** argv) {
 const int mode=std::stoi(argv[1]);
 const double x=std::stod(argv[2]),y=std::stod(argv[3]);
 if(argc>4) {
  camera_value.view_width=std::stoi(argv[4]);camera_value.view_height=std::stoi(argv[5]);
  camera_value.width=std::stoi(argv[6]);camera_value.height=std::stoi(argv[7]);
 }
 const double input_width=argc>8 ? std::stod(argv[8]) : camera_value.view_width;
 const double input_height=argc>9 ? std::stod(argv[9]) : camera_value.view_height;
 tab.web_camera_render_width=int(input_width);tab.web_camera_render_height=int(input_height);
 if(mode==0) live_pick(x,y,input_width,input_height);
 else if(mode==1) gizmo(x,y,input_width,input_height);
 else if(mode==2) { auto result=api_pick(x,y,input_width,input_height);if(result) { observed=*result;mapped=true; } }
 else if(mode==3) cursor(x,y);
 else if(mode==4 || mode==5) {
  s_viewport_gizmo_drags[1]=Drag{};
  gizmo(x,y,input_width,input_height,mode==4 ? "pointerup" : "pointercancel");
  std::cout<<last_result.consumed<<' '<<last_result.ended<<' '<<last_result.cancelled<<' '
   <<s_viewport_gizmo_drags.size()<<' '<<last_result.position.x<<' '<<last_result.position.y<<' '<<last_result.position.z;
  return 0;
 } else if(mode==6) { tab.camera_view=true;camera_value.view_open=true;cursor(x,y); }
 else if(mode==7) {
  cursor(768,400);present_cursor();std::cout<<system_cursor_hidden<<' ';
  cursor(0,400);present_cursor();std::cout<<system_cursor_hidden<<' ';
  cursor(768,400);present_cursor();std::cout<<system_cursor_hidden<<' ';
  cursor(768,400,true);present_cursor();std::cout<<system_cursor_hidden<<' ';
  Hub::instance().state.mode=Corona::ViewportUiMode::Flat2D;
  cursor(768,400);present_cursor();std::cout<<system_cursor_hidden;
  return 0;
 }
 std::cout<<mapped<<' '<<observed.x<<' '<<observed.y;
}
'''
    replacements = {
        "VIEWPORT_DTOS": header[header.index("struct ViewportBounds {"):header.index("struct PanelOptions {")],
        "PICK_MAPPING": mapping, "PICK_CALL": pick_call,
        "GIZMO_MAPPING": gizmo[gizmo_start:gizmo_end],
        "GIZMO_BOUNDS": gizmo[bounds_start:bounds_end],
        "API_MAPPING": native[api_start:api_end],
        "CURSOR_SHAPES": window[cursor_start:cursor_end],
        "RELEASE_POINTER": window[release_start:release_end],
        "WINDOW_CONFIGURATION": window[window_start:window_end],
        "CURSOR_PRESENTATION": imgui[hide_start:hide_end],
    }
    for key, value in replacements.items():
        program = program.replace(key, value)
    directory = tmp_path_factory.mktemp("viewport-input")
    source, executable = directory / "pointer.cpp", directory / "pointer.exe"
    source.write_text(program, encoding="utf-8")
    built = subprocess.run([compiler, "-std=c++20", "-I" + str(ROOT),
                            str(source), "-o", str(executable)], capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return executable


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
