#include <corona/systems/ui/camera_viewport_manager.h>

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void camera_view_window_closes_the_tab() {
    expect(Corona::Systems::UI::detached_window_close_action(true) ==
               Corona::Systems::UI::DetachedWindowCloseAction::CloseTab,
           "camera view close should close the tab");
}

void regular_detached_panel_still_redocks() {
    expect(Corona::Systems::UI::detached_window_close_action(false) ==
               Corona::Systems::UI::DetachedWindowCloseAction::Redock,
           "regular detached panel close should redock");
}

}  // namespace

int main() {
    camera_view_window_closes_the_tab();
    regular_detached_panel_still_redocks();
    return 0;
}
