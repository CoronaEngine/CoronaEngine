#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

// Owned SDL/Web input consumed by the Python runtime. No CEF or JSON types.
struct InputEvent {
    int type = 0; // 0=keyDown, 1=keyUp, 2=mouse
    std::string arg0, arg1, arg2;
    double arg3 = 0, arg4 = 0;
    double viewport_x = 0, viewport_y = 0, viewport_width = 0, viewport_height = 0;
    std::string picked_actor;
};

// A Web event remains valid only while its native context and archive are alive.
// SDL input has no Web context and uses the same queue without a guard.
void enqueue_input_event(InputEvent event, std::function<bool()> current = {});
std::vector<InputEvent> drain_input_events();

} // namespace Corona::Systems::UI
