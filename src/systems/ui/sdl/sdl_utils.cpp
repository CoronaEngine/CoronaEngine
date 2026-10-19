#include "sdl_utils.h"

#include <horizon/core/logging.h>

#include <cmath>

namespace Corona::Systems::UI {

// ============================================================================
// KeyUtils 实现
// ============================================================================

namespace KeyUtils {

int convert_sdl_key_code_to_windows(int sdl_key) {
    if (sdl_key >= SDLK_A && sdl_key <= SDLK_Z) {
        return 0x41 + (sdl_key - SDLK_A);
    }

    if (sdl_key >= SDLK_0 && sdl_key <= SDLK_9) {
        return 0x30 + (sdl_key - SDLK_0);
    }

    switch (sdl_key) {
        case SDLK_RETURN:
            return 0x0D;
        case SDLK_GRAVE:
            return 0xC0;
        case SDLK_MINUS:
            return 0xBD;
        case SDLK_EQUALS:
            return 0xBB;
        case SDLK_LEFTBRACKET:
            return 0xDB;
        case SDLK_RIGHTBRACKET:
            return 0xDD;
        case SDLK_BACKSLASH:
            return 0xDC;
        case SDLK_SEMICOLON:
            return 0xBA;
        case SDLK_APOSTROPHE:
            return 0xDE;
        case SDLK_COMMA:
            return 0xBC;
        case SDLK_PERIOD:
            return 0xBE;
        case SDLK_SLASH:
            return 0xBF;
        case SDLK_LEFT:
            return 0x25;
        case SDLK_UP:
            return 0x26;
        case SDLK_RIGHT:
            return 0x27;
        case SDLK_DOWN:
            return 0x28;
        case SDLK_HOME:
            return 0x24;
        case SDLK_END:
            return 0x23;
        case SDLK_PAGEUP:
            return 0x21;
        case SDLK_PAGEDOWN:
            return 0x22;
        case SDLK_INSERT:
            return 0x2D;
        case SDLK_DELETE:
            return 0x2E;
        case SDLK_BACKSPACE:
            return 0x08;
        case SDLK_TAB:
            return 0x09;
        case SDLK_ESCAPE:
            return 0x1B;
        case SDLK_SPACE:
            return 0x20;
        case SDLK_LSHIFT:
            return 0xA0;
        case SDLK_RSHIFT:
            return 0xA1;
        case SDLK_LCTRL:
            return 0xA2;
        case SDLK_RCTRL:
            return 0xA3;
        case SDLK_LALT:
            return 0xA4;
        case SDLK_RALT:
            return 0xA5;
        case SDLK_LGUI:
            return 0x5B;
        case SDLK_RGUI:
            return 0x5C;
        case SDLK_CAPSLOCK:
            return 0x14;
        case SDLK_NUMLOCKCLEAR:
            return 0x90;
        case SDLK_KP_0:
            return 0x60;
        case SDLK_KP_1:
            return 0x61;
        case SDLK_KP_2:
            return 0x62;
        case SDLK_KP_3:
            return 0x63;
        case SDLK_KP_4:
            return 0x64;
        case SDLK_KP_5:
            return 0x65;
        case SDLK_KP_6:
            return 0x66;
        case SDLK_KP_7:
            return 0x67;
        case SDLK_KP_8:
            return 0x68;
        case SDLK_KP_9:
            return 0x69;
        case SDLK_KP_MULTIPLY:
            return 0x6A;
        case SDLK_KP_PLUS:
            return 0x6B;
        case SDLK_KP_MINUS:
            return 0x6D;
        case SDLK_KP_DECIMAL:
            return 0x6E;
        case SDLK_KP_DIVIDE:
            return 0x6F;
        case SDLK_KP_ENTER:
            return 0x0D;
        case SDLK_F1:
            return 0x70;
        case SDLK_F2:
            return 0x71;
        case SDLK_F3:
            return 0x72;
        case SDLK_F4:
            return 0x73;
        case SDLK_F5:
            return 0x74;
        case SDLK_F6:
            return 0x75;
        case SDLK_F7:
            return 0x76;
        case SDLK_F8:
            return 0x77;
        case SDLK_F9:
            return 0x78;
        case SDLK_F10:
            return 0x79;
        case SDLK_F11:
            return 0x7A;
        case SDLK_F12:
            return 0x7B;
        default:
            return sdl_key;
    }
}

bool is_modifier_key(int key) {
    return key == SDLK_LCTRL || key == SDLK_RCTRL ||
           key == SDLK_LSHIFT || key == SDLK_RSHIFT ||
           key == SDLK_LALT || key == SDLK_RALT ||
           key == SDLK_LGUI || key == SDLK_RGUI;
}

bool should_send_char_event(int key, int modifiers) {
    if (is_modifier_key(key)) return false;
    if (key >= SDLK_F1 && key <= SDLK_F12) return false;
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) return true;
    if (modifiers & EVENTFLAG_CONTROL_DOWN) {
        if ((key >= SDLK_A && key <= SDLK_Z) || (key >= SDLK_0 && key <= SDLK_9)) {
            return true;
        }
    }
    return false;
}

}  // namespace KeyUtils

// ============================================================================
// MouseUtils 实现
// ============================================================================

namespace MouseUtils {

CefBrowserHost::MouseButtonType convert_mouse_button(Uint8 sdl_button) {
    switch (sdl_button) {
        case SDL_BUTTON_LEFT:
            return MBT_LEFT;
        case SDL_BUTTON_MIDDLE:
            return MBT_MIDDLE;
        case SDL_BUTTON_RIGHT:
            return MBT_RIGHT;
        default:
            return MBT_LEFT;
    }
}

// ----------------------------------------------------------------------------
// Phase 5: ImGui-free explicit-modifier overloads. These mirror the CEF event
// construction of the functions above exactly, but take all state explicitly.
// ----------------------------------------------------------------------------

uint32_t make_modifiers(bool left_down, bool right_down, bool shift, bool ctrl, bool alt) {
    uint32_t modifiers = 0;
    if (left_down) modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (right_down) modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    if (shift) modifiers |= EVENTFLAG_SHIFT_DOWN;
    if (ctrl) modifiers |= EVENTFLAG_CONTROL_DOWN;
    if (alt) modifiers |= EVENTFLAG_ALT_DOWN;
    return modifiers;
}

CefMouseEvent create_mouse_event_ex(float mouse_x, float mouse_y, float item_x, float item_y,
                                    uint32_t modifiers) {
    CefMouseEvent mouse_event;
    mouse_event.x = static_cast<int>(mouse_x - item_x);
    mouse_event.y = static_cast<int>(mouse_y - item_y);
    mouse_event.modifiers = modifiers;
    return mouse_event;
}

void send_mouse_click_ex(CefRefPtr<CefBrowser> browser, float mouse_x, float mouse_y,
                         float item_x, float item_y, CefBrowserHost::MouseButtonType button,
                         bool mouse_up, int click_count, bool shift, bool ctrl, bool alt) {
    if (!browser) return;

    const bool is_left = (button == MBT_LEFT);
    const bool is_right = (button == MBT_RIGHT);
    const uint32_t modifiers =
        make_modifiers(!mouse_up && is_left, !mouse_up && is_right, shift, ctrl, alt);

    CefMouseEvent mouse_event = create_mouse_event_ex(mouse_x, mouse_y, item_x, item_y, modifiers);
    browser->GetHost()->SendMouseClickEvent(mouse_event, button, mouse_up, click_count);
}

void send_mouse_move_ex(CefRefPtr<CefBrowser> browser, float mouse_x, float mouse_y,
                        float item_x, float item_y, bool left_down, bool right_down,
                        bool shift, bool ctrl, bool alt, bool mouse_leave) {
    if (!browser) return;

    const uint32_t modifiers = make_modifiers(left_down, right_down, shift, ctrl, alt);
    CefMouseEvent mouse_event = create_mouse_event_ex(mouse_x, mouse_y, item_x, item_y, modifiers);
    browser->GetHost()->SendMouseMoveEvent(mouse_event, mouse_leave);
}

void send_mouse_wheel_ex(CefRefPtr<CefBrowser> browser, float mouse_x, float mouse_y,
                         float item_x, float item_y, float wheel_delta, bool left_down,
                         bool right_down, bool shift, bool ctrl, bool alt) {
    if (!browser) return;

    const uint32_t modifiers = make_modifiers(left_down, right_down, shift, ctrl, alt);
    CefMouseEvent mouse_event = create_mouse_event_ex(mouse_x, mouse_y, item_x, item_y, modifiers);

    // Windows 标准滚轮增量通常是 120
    browser->GetHost()->SendMouseWheelEvent(mouse_event, 0, static_cast<int>(wheel_delta * 120));
}

}  // namespace MouseUtils

// ============================================================================
// SDLEventHandler 实现
// ============================================================================

bool SDLEventHandler::is_input_method_switch(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP) {
        return false;
    }

    int key = static_cast<int>(event.key.key);
    bool ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
    bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
    bool alt = (event.key.mod & SDL_KMOD_ALT) != 0;

    return ((ctrl && shift) || (alt && shift) ||
            (ctrl && key == SDLK_SPACE) ||
            (event.key.mod & SDL_KMOD_GUI && key == SDLK_SPACE));
}

EventProcessResult SDLEventHandler::process_events(
    SDL_Window* window,
    int current_url_input_active_tab,
    KeyEventCallback on_key_event,
    KeyEventCallback on_text_event,
    KeyEventCallback on_ime_event,
    KeyEventCallback on_mouse_event) {
    EventProcessResult result;
    result.url_input_active_tab = current_url_input_active_tab;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Input-method switch shortcuts used to be fed to ImGui; with ImGui gone they are
        // simply not treated as text-affecting key events here (SDL/OS handles IME switch).
        if (is_input_method_switch(event)) {
            continue;
        }

        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            if (on_key_event) on_key_event(event);
        } else if (event.type == SDL_EVENT_TEXT_INPUT) {
            if (on_text_event) on_text_event(event);
        } else if (event.type == SDL_EVENT_TEXT_EDITING) {
            if (on_ime_event) on_ime_event(event);
        } else if (event.type == SDL_EVENT_MOUSE_MOTION ||
                   event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                   event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                   event.type == SDL_EVENT_MOUSE_WHEEL) {
            if (on_mouse_event) on_mouse_event(event);
        }

        switch (event.type) {
            case SDL_EVENT_QUIT:
                CFW_LOG_INFO("[SDL] quit event received");
                result.should_quit = true;
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                CFW_LOG_INFO("[SDL] close requested window={} main={}",
                             event.window.windowID,
                             window ? SDL_GetWindowID(window) : 0);
                if (event.window.windowID == SDL_GetWindowID(window)) {
                    result.should_quit = true;
                } else {
                    // A secondary (detached) window's close button: report its id so the frame
                    // runner can redock that panel (promise-synced teardown). Never destroy the
                    // window here — destruction is the UI thread's reconcile job.
                    result.closed_window_ids.push_back(event.window.windowID);
                }
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                if (SDL_Window* focused_window = SDL_GetWindowFromID(event.window.windowID);
                    focused_window && !SDL_TextInputActive(focused_window)) {
                    SDL_StartTextInput(focused_window);
                }
                break;

            case SDL_EVENT_WINDOW_RESIZED:
                if (event.window.windowID == SDL_GetWindowID(window)) {
                    result.window_resized = true;
                }
                break;

            default:
                break;
        }
    }

    return result;
}

}  // namespace Corona::Systems::UI
