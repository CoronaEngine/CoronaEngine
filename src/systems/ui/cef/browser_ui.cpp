#include "browser_ui.h"

#include <corona/systems/ui/camera_viewport_manager.h>
#include <corona/systems/ui/viewport_gizmo_manager.h>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

#include "browser_manager.h"
#include "cef_client.h"
#include "sdl/sdl_utils.h"

namespace Corona::Systems::UI {

namespace {
float clamp_float(float value, float min_value, float max_value) {
    if (max_value < min_value) {
        return min_value;
    }
    return std::clamp(value, min_value, max_value);
}

#ifdef _WIN32
bool is_extended_windows_key(int windows_key_code) {
    switch (windows_key_code) {
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_END:
        case VK_HOME:
        case VK_INSERT:
        case VK_DELETE:
        case VK_DIVIDE:
        case VK_RCONTROL:
        case VK_RMENU:
            return true;
        default:
            return false;
    }
}

int make_windows_native_key_code(int windows_key_code, bool pressed) {
    const UINT scan_code = MapVirtualKeyW(
        static_cast<UINT>(windows_key_code), MAPVK_VK_TO_VSC);
    int native_key_code = 1 | (static_cast<int>(scan_code) << 16);
    if (is_extended_windows_key(windows_key_code)) {
        native_key_code |= 1 << 24;
    }
    if (!pressed) {
        native_key_code |= static_cast<int>(0xC0000000u);
    }
    return native_key_code;
}
#endif
}  // namespace

// ============================================================================
// BrowserInputHandler 实现
// ============================================================================

void BrowserInputHandler::clear_pending_events() {
    pending_key_events_.clear();
}

void BrowserInputHandler::process_sdl_key_event(const SDL_Event& event) {
    bool pressed = (event.type == SDL_EVENT_KEY_DOWN);
    int key_code = static_cast<int>(event.key.key);
    int scan_code = static_cast<int>(event.key.scancode);
    int modifiers = 0;

    Uint32 sdl_mod = event.key.mod;
    if (sdl_mod & SDL_KMOD_CTRL) modifiers |= EVENTFLAG_CONTROL_DOWN;
    if (sdl_mod & SDL_KMOD_SHIFT) modifiers |= EVENTFLAG_SHIFT_DOWN;
    if (sdl_mod & SDL_KMOD_ALT) modifiers |= EVENTFLAG_ALT_DOWN;
    if (sdl_mod & SDL_KMOD_GUI) modifiers |= EVENTFLAG_COMMAND_DOWN;
    if (sdl_mod & SDL_KMOD_CAPS) modifiers |= EVENTFLAG_CAPS_LOCK_ON;
    if (sdl_mod & SDL_KMOD_NUM) modifiers |= EVENTFLAG_NUM_LOCK_ON;

    bool is_common_edit_shortcut = false;
    if (modifiers & EVENTFLAG_CONTROL_DOWN) {
        switch (key_code) {
            case SDLK_A:
            case SDLK_C:
            case SDLK_V:
            case SDLK_Z:
            case SDLK_Y:
                is_common_edit_shortcut = true;
                break;
            default:
                break;
        }
    }

    bool is_modifier_combo = (modifiers & (EVENTFLAG_CONTROL_DOWN | EVENTFLAG_ALT_DOWN)) &&
                             ((key_code >= 'a' && key_code <= 'z') ||
                              (key_code >= 'A' && key_code <= 'Z') ||
                              (key_code >= '0' && key_code <= '9'));

    PendingKeyEvent key_event(PendingKeyEvent::kKeyEvent);
    key_event.key_code = key_code;
    key_event.scan_code = scan_code;
    key_event.modifiers = modifiers;
    key_event.pressed = pressed;
    key_event.is_modifier_combo = is_modifier_combo || is_common_edit_shortcut;

    pending_key_events_.push_back(key_event);
}

void BrowserInputHandler::process_sdl_text_event(const SDL_Event& event) {
    if (event.text.text && event.text.text[0]) {
        PendingKeyEvent text_event(PendingKeyEvent::kTextEvent);
        text_event.text = event.text.text;
        pending_key_events_.push_back(text_event);
    }
}

void BrowserInputHandler::process_sdl_ime_event(const SDL_Event& event) {
    if (event.edit.text && event.edit.text[0]) {
        PendingKeyEvent ime_event(PendingKeyEvent::kImeComposition);
        ime_event.text = event.edit.text;
        ime_event.ime_start = event.edit.start;
        ime_event.ime_length = event.edit.length;
        pending_key_events_.push_back(ime_event);
    }
}

void BrowserInputHandler::send_key_events_to_browser(const CefRefPtr<CefBrowser>& browser) {
    if (!browser) return;

    for (const auto& pending_event : pending_key_events_) {
        if (pending_event.type == PendingKeyEvent::kKeyEvent) {
            CefKeyEvent cef_key_event;
            cef_key_event.type = pending_event.pressed ? KEYEVENT_RAWKEYDOWN : KEYEVENT_KEYUP;
            cef_key_event.windows_key_code = KeyUtils::convert_sdl_key_code_to_windows(pending_event.key_code);
#ifdef _WIN32
            cef_key_event.native_key_code = make_windows_native_key_code(
                cef_key_event.windows_key_code, pending_event.pressed);
#else
            cef_key_event.native_key_code = pending_event.scan_code;
            cef_key_event.character = pending_event.key_code;
            cef_key_event.unmodified_character = pending_event.key_code;
#endif
            cef_key_event.modifiers = pending_event.modifiers;
            cef_key_event.is_system_key =
                (pending_event.modifiers & EVENTFLAG_ALT_DOWN) != 0;

            bool is_common_edit_shortcut = false;
            if (pending_event.modifiers & EVENTFLAG_CONTROL_DOWN) {
                switch (pending_event.key_code) {
                    case SDLK_A:
                    case SDLK_C:
                    case SDLK_V:
                    case SDLK_Z:
                    case SDLK_Y:
                        is_common_edit_shortcut = true;
                        break;
                    default:
                        break;
                }
            }

            browser->GetHost()->SendKeyEvent(cef_key_event);

            if (pending_event.pressed &&
                (pending_event.key_code == SDLK_RETURN || pending_event.key_code == SDLK_KP_ENTER)) {
                CefKeyEvent char_event = cef_key_event;
                char_event.type = KEYEVENT_CHAR;
                char_event.character = 0x0D;
                char_event.unmodified_character = 0x0D;
                browser->GetHost()->SendKeyEvent(char_event);
            }

            if (pending_event.pressed && pending_event.is_modifier_combo) {
                if (is_common_edit_shortcut) {
                    cef_key_event.type = KEYEVENT_CHAR;
                    browser->GetHost()->SendKeyEvent(cef_key_event);
                } else {
                    switch (pending_event.key_code) {
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                        case SDLK_TAB:
                        case SDLK_BACKSPACE:
                        case SDLK_DELETE:
                        case SDLK_ESCAPE:
                            cef_key_event.type = KEYEVENT_CHAR;
                            browser->GetHost()->SendKeyEvent(cef_key_event);
                            break;
                        default:
                            break;
                    }
                }
            }
        } else if (pending_event.type == PendingKeyEvent::kTextEvent) {
            const std::string& text = pending_event.text;
            if (!text.empty()) {
                bool has_control_chars = false;
                for (char c : text) {
                    if (c == '\b' || c == '\t' || c == '\n' || c == '\r') {
                        has_control_chars = true;
                        break;
                    }
                }

                if (!has_control_chars) {
                    bool is_ascii = true;
                    for (char c : text) {
                        if (static_cast<unsigned char>(c) >= 128) {
                            is_ascii = false;
                            break;
                        }
                    }

                    if (is_ascii) {
                        for (char c : text) {
                            if (c >= 32 && c < 127) {
                                CefKeyEvent cef_text_event;
                                cef_text_event.type = KEYEVENT_CHAR;
                                cef_text_event.modifiers = 0;
                                cef_text_event.windows_key_code = static_cast<uint16_t>(c);
                                cef_text_event.native_key_code = static_cast<uint16_t>(c);
                                cef_text_event.character = static_cast<uint16_t>(c);
                                cef_text_event.unmodified_character = static_cast<uint16_t>(c);
                                browser->GetHost()->SendKeyEvent(cef_text_event);
                            }
                        }
                    } else {
                        if (char* utf16_text = SDL_iconv_string("UTF-16LE", "UTF-8", text.c_str(), text.length() + 1)) {
                            auto* utf16_chars = reinterpret_cast<uint16_t*>(utf16_text);
                            size_t utf16_len = 0;
                            while (utf16_chars[utf16_len] != 0) {
                                utf16_len++;
                            }
                            for (size_t i = 0; i < utf16_len; i++) {
                                CefKeyEvent cef_text_event;
                                cef_text_event.type = KEYEVENT_CHAR;
                                cef_text_event.modifiers = 0;
                                cef_text_event.windows_key_code = utf16_chars[i];
                                cef_text_event.native_key_code = utf16_chars[i];
                                cef_text_event.character = utf16_chars[i];
                                cef_text_event.unmodified_character = utf16_chars[i];
                                browser->GetHost()->SendKeyEvent(cef_text_event);
                            }
                            SDL_free(utf16_text);
                        }
                    }
                }
            }
        }
    }

    clear_pending_events();
}

// ============================================================================
// BrowserRenderer 实现
// ============================================================================

static MouseUtils::MouseStateManager mouse_state;

namespace {

void emit_native_viewport_gizmo_events(BrowserTab* tab) {
    if (!tab || !tab->client) {
        return;
    }
    const auto browser = tab->client->GetBrowser();
    if (!browser) {
        return;
    }
    const auto frame = browser->GetMainFrame();
    if (!frame) {
        return;
    }

    auto emit_json_event = [&](const char* event_name, const nlohmann::json& payload) {
        const std::string script =
            std::string("window.__coronaEmit && window.__coronaEmit(") +
            nlohmann::json(event_name).dump() + "," + payload.dump() + ");";
        frame->ExecuteJavaScript(script, frame->GetURL(), 0);
    };

    for (const auto& event : ViewportGizmoManager::instance().drain_selection_events()) {
        nlohmann::json payload;
        payload["sceneId"] = event.scene_id;
        payload["cameraHandle"] = static_cast<std::uint64_t>(event.camera_handle);
        payload["actorHandle"] = static_cast<std::uint64_t>(event.actor_handle);
        emit_json_event("native-gizmo-selection", payload);
    }

    for (const auto& event : ViewportGizmoManager::instance().drain_transform_events()) {
        nlohmann::json payload;
        payload["sceneId"] = event.scene_id;
        payload["cameraHandle"] = static_cast<std::uint64_t>(event.camera_handle);
        payload["actorHandle"] = static_cast<std::uint64_t>(event.actor_handle);
        payload["transform"]["position"] = {event.position[0], event.position[1], event.position[2]};
        payload["transform"]["rotation"] = {event.rotation[0], event.rotation[1], event.rotation[2]};
        payload["transform"]["scale"] = {event.scale[0], event.scale[1], event.scale[2]};
        emit_json_event("native-gizmo-transform", payload);
    }
}

SDL_Window* sdl_window_from_viewport(ImGuiViewport* viewport) {
    if (!viewport || !viewport->PlatformHandle) {
        return nullptr;
    }

    auto window_id = static_cast<SDL_WindowID>(
        reinterpret_cast<std::intptr_t>(viewport->PlatformHandle));
    return SDL_GetWindowFromID(window_id);
}

void sync_tab_platform_window(BrowserTab* tab) {
    if (!tab) {
        return;
    }

    auto* viewport = ImGui::GetWindowViewport();
    SDL_Window* platform_window = sdl_window_from_viewport(viewport);
    void* native_surface = viewport ? viewport->PlatformHandleRaw : nullptr;
    if (!native_surface && platform_window) {
        native_surface = SDL_GetPointerProperty(
            SDL_GetWindowProperties(platform_window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER,
            nullptr);
    }
    tab->platform_handle_raw = native_surface;
    tab->platform_window_id = platform_window
                                  ? SDL_GetWindowID(platform_window)
                                  : 0;
}

void focus_browser_tab_exclusively(int focused_tab_id) {
    for (const auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
        if (!tab || !tab->client) {
            continue;
        }
        auto browser = tab->client->GetBrowser();
        if (!browser) {
            continue;
        }
        if (tab_id == focused_tab_id) {
            browser->GetHost()->SetFocus(true);
        } else {
            browser->GetHost()->SetFocus(false);
        }
    }
}

}  // namespace

void BrowserRenderer::setup_window_transform(BrowserTab* tab,
                                             ImGuiID dock_space_id,
                                             bool is_main_tab) {
    if (is_main_tab) {
        (void)dock_space_id;
        ImGuiViewport* main_viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowViewport(main_viewport->ID);
        ImGui::SetNextWindowPos(main_viewport->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(main_viewport->WorkSize, ImGuiCond_Always);
        tab->dock_initialized = true;
    } else {
        if (tab->camera_view) {
            ImGui::SetNextWindowPos(
                ImVec2(static_cast<float>(tab->initial_x), static_cast<float>(tab->initial_y)),
                tab->needs_reposition ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(
                ImVec2(static_cast<float>(tab->dock_width), static_cast<float>(tab->dock_height)),
                tab->needs_resize ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
            tab->dock_initialized = true;
            return;
        }

        ImGuiViewport* browser_viewport = ImGui::GetMainViewport();
        ImVec2 work_pos = browser_viewport->WorkPos;
        ImVec2 work_size = browser_viewport->WorkSize;

        constexpr float panel_margin = 24.0f;
        constexpr float scene_bar_offset = 64.0f;
        constexpr float right_stack_gap = 16.0f;

        auto target_w = static_cast<float>(tab->dock_width);
        auto target_h = static_cast<float>(tab->dock_height);

        const bool left_panel = tab->docking_pos == "left_bottom";
        const bool right_panel = tab->docking_pos == "right_top" ||
                                 tab->docking_pos == "right_bottom";

        const float min_w = left_panel ? 300.0f : 320.0f;
        const float width_ratio = left_panel ? 0.22f : (right_panel ? 0.27f : 0.42f);
        const float max_w = std::max(min_w, work_size.x * width_ratio);
        target_w = clamp_float(target_w, min_w, max_w);

        const float usable_h = std::max(
            260.0f, work_size.y - scene_bar_offset - panel_margin - right_stack_gap);
        if (tab->docking_pos == "right_top") {
            target_h = clamp_float(target_h, 320.0f, usable_h * 0.54f);
        } else if (tab->docking_pos == "right_bottom") {
            target_h = clamp_float(target_h, 300.0f, usable_h * 0.46f);
        } else if (tab->docking_pos == "left_bottom") {
            target_h = clamp_float(target_h, 300.0f, std::max(300.0f, usable_h * 0.48f));
        } else {
            target_h = clamp_float(target_h, 320.0f, usable_h);
        }

        ImVec2 final_pos = work_pos;

        if (tab->docking_pos == "left_top") {
            final_pos.x = work_pos.x + panel_margin;
            final_pos.y = work_pos.y + scene_bar_offset;
        } else if (tab->docking_pos == "left_bottom") {
            final_pos.x = work_pos.x + panel_margin;
            final_pos.y = work_pos.y + work_size.y - target_h - panel_margin;
        } else if (tab->docking_pos == "right_top") {
            final_pos.x = work_pos.x + work_size.x - target_w - panel_margin;
            final_pos.y = work_pos.y + scene_bar_offset;
        } else if (tab->docking_pos == "right_bottom") {
            final_pos.x = work_pos.x + work_size.x - target_w - panel_margin;
            final_pos.y = work_pos.y + work_size.y - target_h - panel_margin;
        } else if (tab->docking_pos == "bottom_left") {
            final_pos.x = work_pos.x + 300.f;
            final_pos.y = work_pos.y + work_size.y - target_h;
        } else if (tab->docking_pos == "bottom_right") {
            final_pos.x = work_pos.x + work_size.x - target_w - 300.f;
            final_pos.y = work_pos.y + work_size.y - target_h;
        } else if (tab->docking_pos == "center") {
            final_pos.x = work_pos.x + (work_size.x - target_w) * 0.5f;
            final_pos.y = work_pos.y + (work_size.y - target_h) * 0.5f;
        } else {
            final_pos.x = work_pos.x + work_size.x - target_w - panel_margin;
            final_pos.y = work_pos.y + scene_bar_offset + right_stack_gap;
        }

        ImGui::SetNextWindowPos(final_pos, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(target_w, target_h), ImGuiCond_FirstUseEver);

        tab->dock_initialized = true;
    }
}

void BrowserRenderer::handle_browser_mouse_events(BrowserTab* tab,
                                                  int tab_id,
                                                  int& active_tab_id,
                                                  int& url_input_active_tab,
                                                  const ImGuiIO* io,
                                                  bool is_dragging) {
    if (is_dragging) return;  // 拖拽期间不处理浏览器鼠标事件

    const bool is_hovered = ImGui::IsItemHovered();
    const bool is_active = (active_tab_id == tab_id);
    const ImVec2 mouse_pos = ImGui::GetMousePos();
    const ImVec2 item_pos = ImGui::GetItemRectMin();  // 窗口内容区域起点

    auto browser = tab->client ? tab->client->GetBrowser() : nullptr;
    if (!browser) return;

    // 1. 处理点击事件 (按下/释放)
    auto process_button = [&](ImGuiMouseButton imgui_btn, CefBrowserHost::MouseButtonType cef_btn) {
        if (is_hovered) {
            if (ImGui::IsMouseClicked(imgui_btn)) {
                active_tab_id = tab_id;
                url_input_active_tab = -1;
                focus_browser_tab_exclusively(tab_id);

                int clicks = (imgui_btn == ImGuiMouseButton_Left) ? mouse_state.handle_mouse_click(mouse_pos, SDL_GetTicks()) : 1;

                if (imgui_btn == ImGuiMouseButton_Left) mouse_state.set_mouse_down(true);

                MouseUtils::send_mouse_click(browser, mouse_pos, item_pos, cef_btn, false, clicks);
            }
        }

        if (ImGui::IsMouseReleased(imgui_btn)) {
            bool was_down = (imgui_btn == ImGuiMouseButton_Left && mouse_state.is_mouse_down());
            if (is_hovered || (was_down && is_active)) {
                MouseUtils::send_mouse_click(browser, mouse_pos, item_pos, cef_btn, true, 1);
                if (imgui_btn == ImGuiMouseButton_Left) {
                    mouse_state.set_mouse_down(false);
                    mouse_state.set_dragging(false);
                }
            }
        }
    };

    process_button(ImGuiMouseButton_Left, MBT_LEFT);
    process_button(ImGuiMouseButton_Right, MBT_RIGHT);
    process_button(ImGuiMouseButton_Middle, MBT_MIDDLE);

    // 2. 处理移动事件
    if (is_hovered || mouse_state.is_mouse_down()) {
        MouseUtils::send_mouse_move(browser, mouse_pos, item_pos, !is_hovered);

        if (mouse_state.is_mouse_down()) {
            ImVec2 drag_delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            if (drag_delta.x * drag_delta.x + drag_delta.y * drag_delta.y > 4.0f) {
                mouse_state.set_dragging(true);
            }
        }
    }

    // 3. 处理滚轮
    if (is_hovered && io->MouseWheel != 0) {
        MouseUtils::send_mouse_wheel(browser, mouse_pos, item_pos, io->MouseWheel);
    }
}

void BrowserRenderer::render_single_tab(int tab_id,
                                        ImGuiID dock_space_id,
                                        int& active_tab_id,
                                        int& url_input_active_tab,
                                        ImGuiIO* io) {
    auto* tab = BrowserManager::instance().get_tab(tab_id);
    if (!tab || !tab->open) return;

    BrowserManager::instance().update_texture(tab_id);

    std::string window_id = tab->name + "##" + std::to_string(tab_id);

    ImGuiWindowFlags browser_window_flags = ImGuiWindowFlags_NoTitleBar |
                                            ImGuiWindowFlags_NoScrollbar |
                                            ImGuiWindowFlags_NoNavInputs |
                                            ImGuiWindowFlags_NoNavFocus |
                                            ImGuiWindowFlags_NoMove;

    bool is_main_tab = (tab->docking_pos == "main");
    if (is_main_tab) {
        browser_window_flags |= ImGuiWindowFlags_NoBackground |
                                ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoBringToFrontOnFocus;
    }
    if (is_main_tab) {
        browser_window_flags |= ImGuiWindowFlags_NoMove;  // 主窗口始终禁止移动
    }
    if (tab->camera_view) {
        browser_window_flags |= ImGuiWindowFlags_NoDocking |
                                ImGuiWindowFlags_NoBackground;
        ImGuiWindowClass window_class;
        window_class.ClassId = ImGui::GetID("CoronaCameraViewport");
        window_class.DockingAllowUnclassed = false;
        window_class.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        ImGui::SetNextWindowClass(&window_class);
    }

    setup_window_transform(tab, dock_space_id, is_main_tab);

    if (tab->camera_view) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    }

    if (ImGui::Begin(window_id.c_str(), &tab->open, browser_window_flags)) {
        if (!tab->open) {
            ImGui::End();
            if (tab->camera_view) {
                ImGui::PopStyleVar(2);
            }
            return;
        }

        ImVec2 window_pos = ImGui::GetWindowPos();
        ImVec2 content_min = ImGui::GetWindowContentRegionMin();
        ImVec2 cef_origin = ImVec2(window_pos.x + content_min.x, window_pos.y + content_min.y);
        const auto window_size = ImGui::GetWindowSize();

        ImVec2 avail_size = ImGui::GetContentRegionAvail();
        int new_width = static_cast<int>(avail_size.x);
        int new_height = static_cast<int>(avail_size.y);
        if (tab->needs_reposition &&
            std::abs(static_cast<int>(window_pos.x) - tab->initial_x) <= 4 &&
            std::abs(static_cast<int>(window_pos.y) - tab->initial_y) <= 4) {
            tab->needs_reposition = false;
        }
        if (tab->needs_resize && new_width > 0 && new_height > 0 &&
            std::abs(new_width - tab->dock_width) <= 4 &&
            std::abs(new_height - tab->dock_height) <= 4) {
            tab->needs_resize = false;
        }
        if (new_width > 0 && new_height > 0 &&
            (new_width != tab->width || new_height != tab->height)) {
            BrowserManager::instance().resize_tab(tab_id, new_width, new_height);
        }

        sync_tab_platform_window(tab);

        if (tab->camera_view) {
            auto* viewport = ImGui::GetWindowViewport();
            SDL_Window* platform_window = sdl_window_from_viewport(viewport);
            void* native_surface = viewport ? viewport->PlatformHandleRaw : nullptr;
            if (!native_surface && platform_window) {
                native_surface = SDL_GetPointerProperty(
                    SDL_GetWindowProperties(platform_window),
                    SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                    nullptr);
            }
            tab->platform_handle_raw =
                native_surface;
            tab->platform_window_id = platform_window
                                          ? SDL_GetWindowID(platform_window)
                                          : 0;
            int render_width = new_width;
            int render_height = new_height;
            if (platform_window != nullptr) {
                int pixel_width = 0;
                int pixel_height = 0;
                if (SDL_GetWindowSizeInPixels(platform_window, &pixel_width, &pixel_height) &&
                    pixel_width > 0 && pixel_height > 0) {
                    render_width = pixel_width;
                    render_height = pixel_height;
                }
            }
            if (native_surface) {
                CameraViewportManager::instance().bind_surface(
                    tab_id, native_surface,
                    static_cast<int>(window_pos.x), static_cast<int>(window_pos.y),
                    render_width, render_height);
            }
            CameraViewportManager::instance().update_layout(
                tab_id, static_cast<int>(window_pos.x), static_cast<int>(window_pos.y),
                static_cast<int>(window_size.x), static_cast<int>(window_size.y));
        }

        bool in_drag_region = false;

        // ----------------- 拖拽区域检测与启动窗口移动 -----------------
        if (!is_main_tab) {
            // 检测鼠标是否在拖拽区域内
            // - 若 drag_regions 不为空，按指定区域检测
            // - 若为空（如 Vue 页面未通过 CEF 设置拖拽区域），顶部 30px 作为默认拖拽句柄
            {
                std::lock_guard<std::mutex> lock(tab->drag_mutex);

                if (tab->drag_regions.empty()) {
                    ImRect default_handle(
                        cef_origin.x,
                        cef_origin.y,
                        cef_origin.x + static_cast<float>(tab->width),
                        cef_origin.y + 30.0f);
                    if (default_handle.Contains(io->MousePos)) {
                        in_drag_region = true;
                    }
                } else {
                    for (const auto& region : tab->drag_regions) {
                        ImRect abs_region(
                            cef_origin.x + region.x,
                            cef_origin.y + region.y,
                            cef_origin.x + region.x + region.width,
                            cef_origin.y + region.y + region.height);
                        if (abs_region.Contains(io->MousePos)) {
                            in_drag_region = true;
                            break;
                        }
                    }
                }
            }

            // 光标样式
            if (in_drag_region) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }

            // 拖拽启动逻辑（区分点击与拖拽）
            if (in_drag_region && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !tab->dragging_window) {
                tab->drag_pending = true;
                tab->drag_pending_start_pos = io->MousePos;
            }

            // 待拖拽状态，检查移动距离
            if (tab->drag_pending && !tab->dragging_window) {
                ImVec2 delta(io->MousePos.x - tab->drag_pending_start_pos.x,
                             io->MousePos.y - tab->drag_pending_start_pos.y);
                float distance = sqrtf(delta.x * delta.x + delta.y * delta.y);
                if (distance > 5.0f) {  // 拖拽阈值
                    tab->dragging_window = true;
                    tab->drag_pending = false;

                    ImGuiWindow* window = ImGui::GetCurrentWindow();

                    // 如果窗口已停靠，先分离
                    if (window->DockNode) {
                        ImGuiContext* g = ImGui::GetCurrentContext();
                        ImGui::DockContextProcessUndockWindow(g, window);
                        window = ImGui::GetCurrentWindow();  // 分离后窗口可能重建
                    }

                    window->Flags &= ~ImGuiWindowFlags_NoMove;  // 允许移动

                    // 启动 ImGui 内置窗口移动（支持Multi-Viewport跨窗口拖拽）
                    ImGui::StartMouseMovingWindow(window);
                } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    tab->drag_pending = false;  // 视为点击
                }
            }

            // 拖拽结束：重置状态
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                tab->dragging_window = false;
                tab->drag_pending = false;
            }
        }
        // -------------------------------------------------------------

        ImVec2 image_origin = ImGui::GetCursorScreenPos();
        ImVec2 image_size = avail_size;
        bool viewport_gizmo_captures_mouse = false;

        // 渲染浏览器纹理
        if (is_valid_texture_id(tab->texture_id)) {
            ImGui::Image(tab->texture_id, avail_size);
            image_origin = ImGui::GetItemRectMin();
            image_size = ImGui::GetItemRectSize();
        }

        if (tab->camera_view) {
            if (auto record = CameraViewportManager::instance().find_by_tab(tab_id)) {
                viewport_gizmo_captures_mouse = ViewportGizmoManager::instance().render(
                    record->scene_id,
                    record->camera_handle,
                    image_origin,
                    image_size,
                    ImGui::GetWindowDrawList());
            }
        } else if (is_main_tab) {
            const auto selected_camera = ViewportGizmoManager::instance().selected_camera_handle();
            if (selected_camera != 0) {
                viewport_gizmo_captures_mouse = ViewportGizmoManager::instance().render(
                    std::string{},
                    selected_camera,
                    image_origin,
                    image_size,
                    ImGui::GetWindowDrawList());
            }
            emit_native_viewport_gizmo_events(tab);
        }

        // 仅当未拖拽时传递鼠标事件给浏览器
        if (!tab->dragging_window && !tab->drag_pending && !in_drag_region &&
            !viewport_gizmo_captures_mouse) {
            handle_browser_mouse_events(tab, tab_id, active_tab_id, url_input_active_tab, io);
        }
    }
    ImGui::End();
    if (tab->camera_view) {
        ImGui::PopStyleVar(2);
    }
}

std::vector<int> BrowserRenderer::render_browser_tabs(ImGuiID dock_space_id,
                                                      int& active_tab_id,
                                                      int& url_input_active_tab,
                                                      ImGuiIO* io) {
    std::vector<int> tabs_to_close;
    BrowserManager::instance().update();
    auto& tabs = BrowserManager::instance().get_tabs();

    for (auto& [tab_id, tab] : tabs) {
        if (!tab->open) {
            tabs_to_close.push_back(tab_id);
            continue;
        }
        if (tab->minimized) {
            continue;
        }

        render_single_tab(tab_id, dock_space_id, active_tab_id, url_input_active_tab, io);
        if (!tab->open) {
            tabs_to_close.push_back(tab_id);
        }
    }

    return tabs_to_close;
}

}  // namespace Corona::Systems::UI
