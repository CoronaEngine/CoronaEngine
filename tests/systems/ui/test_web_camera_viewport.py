"""Execute the real ImGui camera placement block without starting a GPU engine."""
from pathlib import Path
import shutil
import subprocess


def test_web_camera_display_uses_the_dom_rect_for_main_and_detached_tabs():
    root = Path(__file__).resolve().parents[3]
    source = (root / "src/systems/ui/imgui/imgui_ui.cpp").read_text(encoding="utf-8")
    start = source.index("if (tab->web_camera_handle && tab->web_camera_width > 0) {")
    end = start
    depth = 0
    for end in range(source.index("{", start), len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            break
    block = source[start:end + 1]
    output = root / ".workspace/tmp/viewport-placement-test"
    output.mkdir(parents=True, exist_ok=True)
    program = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
struct Update { std::uintptr_t camera_handle; void* surface; bool view_open;
 int x,y,width,height,render_width,render_height; int authority; } last;
struct SharedDataHub {
 static SharedDataHub& instance() { static SharedDataHub value; return value; }
 void enqueue_camera_viewport_update(Update value) { last=value; }
};
struct Tab { std::uintptr_t web_camera_handle=1; std::string docking_pos="main";
 int web_camera_x=0,web_camera_y=0,web_camera_width=1536,web_camera_height=825;
 int web_camera_render_width=1920,web_camera_render_height=1080;
 int web_camera_authority=42; };
void update(Tab* tab, int x,int y, float sx,float sy) {
 void* surface=nullptr; int pixel_w=1920,pixel_h=1080;
 BLOCK
}
int main() {
 Tab tab;
 update(&tab,0,0,1,1);
 assert(last.x==0 && last.y==0 && last.width==1536 && last.height==825);
 assert(last.render_width==1920 && last.render_height==1080);
 assert(last.authority==42);
 tab.web_camera_x=24;tab.web_camera_y=12;tab.web_camera_width=600;tab.web_camera_height=400;
 update(&tab,117,39,1.5f,1.5f);
 assert(last.x==153 && last.y==57 && last.width==900 && last.height==600);
 assert(last.render_width==2880 && last.render_height==1620);
 assert(last.authority==42);
 tab.docking_pos="left";
 update(&tab,117,39,1.5f,1.5f);
 assert(last.x==153 && last.y==57 && last.width==900 && last.height==600);
}
'''.replace("BLOCK", block)
    cpp = output / "test.cpp"
    exe = output / "test.exe"
    cpp.write_text(program, encoding="utf-8")
    subprocess.run([shutil.which("g++") or "g++", "-std=c++20", str(cpp), "-o", str(exe)],
                   check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
