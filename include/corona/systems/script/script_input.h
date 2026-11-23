#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

// Owned SDL/Web input consumed by the Python runtime. No CEF or JSON types.
//
// Named fields, not positional slots. The Web reader already speaks these names
// (`code`/`key`/`modifiers` for a key, `event`/`button`/`x`/`y` for the mouse), so the
// struct keeps the spelling instead of flattening it into `arg0..arg4`: a Python consumer
// reading `event.code` cannot silently read the wrong slot, and a reviewer reading the
// binding cannot mistake one for another.
//
// `kind` is the wire spelling the Web request uses as `type` - "keyDown", "keyUp" or
// "mouse" - so the value Python sees is the value JavaScript sent.
//
// The three identity fields are stamped from the accepted Web request. They are empty for
// SDL input, which has no Web context and therefore no archive, scene or camera to name.
struct InputEvent {
    std::string kind;  // "keyDown" | "keyUp" | "mouse"

    // Identity of the accepted Web request; empty for SDL input.
    std::string archive_session, scene_id, camera_id;

    // Keyboard payload; empty for mouse events.
    std::string code, key, modifiers;

    // Mouse payload; empty for keyboard events.
    std::string event, button;
    double x = 0, y = 0;

    double viewport_x = 0, viewport_y = 0, viewport_width = 0, viewport_height = 0;
};

// A Web event remains valid only while its native context and archive are alive.
// SDL input has no Web context and uses the same queue without a guard.
void enqueue_input_event(InputEvent event, std::function<bool()> current = {});
std::vector<InputEvent> drain_input_events();

} // namespace Corona::Systems::UI
