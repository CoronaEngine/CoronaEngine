"""Execute production pick mapping and its live V8 call without CEF/GPU."""
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope="module")
def pick_harness(tmp_path_factory):
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("Standalone pick regression requires g++")
    source = (ROOT / "src/systems/ui/cef/cef_viewport_pick.cpp").read_text(encoding="utf-8")
    mapping = source[source.index("ViewportPickPoint map_viewport_pick_point("):
                     source.index("std::string ViewportPickRequests::add(")]
    call = next(line.strip() for line in source.splitlines()
                if "const auto point = map_viewport_pick_point(request.x" in line)
    program = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include "src/systems/ui/cef/viewport_image_input.h"
namespace Corona {
struct CameraDevice {
 unsigned width=1920,height=1080;
 bool viewport_rect_active=true,view_open=false;
 int view_width=1536,view_height=825;
};
}
namespace Corona::Systems::UI {
struct ViewportPickPoint { bool valid=false,inside=false; unsigned x=0,y=0; };
MAPPING
void run(int argc,char** argv) {
 Corona::CameraDevice camera;
 struct { double x,y,width,height; } request{
  std::stod(argv[1]),std::stod(argv[2]),std::stod(argv[3]),std::stod(argv[4])};
 if(argc>5) { camera.view_width=std::stoi(argv[5]);camera.view_height=std::stoi(argv[6]); }
 if(argc>7) camera.view_open=std::stoi(argv[7]);
 auto* native_camera=&camera;
 CALL
 std::cout << point.valid << ' ' << point.inside << ' ' << point.x << ' ' << point.y;
}
}
int main(int argc,char** argv) { Corona::Systems::UI::run(argc,argv); }
'''.replace("MAPPING", mapping).replace("CALL", call)
    directory = tmp_path_factory.mktemp("typed-pick")
    cpp, exe = directory / "pick.cpp", directory / "pick.exe"
    cpp.write_text(program, encoding="utf-8")
    built = subprocess.run([compiler, "-std=c++20", "-I" + str(ROOT), str(cpp), "-o", str(exe)],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr
    return exe


@pytest.mark.parametrize("args,want", [
    ((34, 0, 1536, 825), (1, 1, 0, 0)),
    ((0, 400, 1536, 825), (1, 0, 0, 0)),
    ((33, 400, 1536, 825), (1, 0, 0, 0)),
    ((1501, 400, 1536, 825), (1, 0, 0, 0)),
    ((768, 825, 1536, 825), (1, 0, 0, 0)),
    ((512, 223, 1024, 1024, 1024, 1024), (1, 0, 0, 0)),
    ((512, 224, 1024, 1024, 1024, 1024), (1, 1, 960, 0)),
    ((480, 270, 960, 540, 960, 540), (1, 1, 960, 540)),
    ((34, 0, 1536, 825, 2304, 1238), (1, 1, 0, 0)),
    ((33, 0, 1536, 825, 2304, 1238), (1, 0, 0, 0)),
    ((480, 270, 1920, 1080, 1536, 825, 1), (1, 1, 480, 270)),
    ((0, 0, "inf", 825), (0, 0, 0, 0)),
])
def test_live_pick_uses_displayed_image(pick_harness, args, want):
    result = subprocess.run([str(pick_harness), *map(str, args)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert tuple(map(int, result.stdout.split())) == want
