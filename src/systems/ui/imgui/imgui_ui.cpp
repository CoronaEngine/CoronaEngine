#include <corona/systems/ui/ui_frame_runner.h>
#include <corona/systems/ui/vulkan_backend.h>
#include <corona/systems/ui/sdl_window_manager.h>
#include <corona/systems/ui/camera_viewport_manager.h>
#include <corona/shared_data_hub.h>
#include "imgui_platform.h"
#include "imgui_workspace.h"
#include "cef/browser_manager.h"
#include "cef/cef_client.h"
#include "cef/cef_v8_binding.h"
#include "sdl/sdl_utils.h"
#include <imgui.h>
#include <imgui_internal.h> // DockBuilder default layout; pinned with the core library.
#include <backends/imgui_impl_sdl3.h>
#include <algorithm>
#include <cmath>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Corona::Systems::UI {
namespace {
struct ContentRect {
    int tab_id;
    ImGuiID viewport;
    ImVec2 origin, size;
    bool hovered;
};
std::string window_name(const BrowserTab& tab) {
    if (tab.docking_pos == "main") return kMainWorkspaceName;
    return (tab.panel_title.empty() ? tab.name : tab.panel_title) + "###Corona.Panel." +
        (tab.panel_id.empty() ? std::to_string(tab.tab_id) : tab.panel_id);
}
void set_visible(BrowserTab& tab, bool visible) {
    if (tab.cef_visible == visible || !tab.client) return;
    if (auto browser = tab.client->GetBrowser()) {
        browser->GetHost()->WasHidden(!visible);
        tab.cef_visible = visible;
    }
}
void request_close(BrowserTab& tab) {
    // Let the Web session drain outstanding commands before destroying its context.
    // Legacy camera pages retain their existing native close path.
    if (!tab.panel_id.empty() && tab.client) {
        if (auto browser = tab.client->GetBrowser()) {
            request_corona_window_close(browser->GetIdentifier());
            return;
        }
    }
    tab.open = false;
}
}

void UiFrameRunner::run_frame(UiFrameContext& context) {
    if (!context.running || !context.active_tab_id || !context.vulkan_backend) return;
    auto& manager = BrowserManager::instance();
    auto& io = ImGui::GetIO();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(context.window))) *context.running = false;
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            if (auto* captured = manager.get_tab(captured_tab_); captured && captured->client)
                if (auto browser = captured->client->GetBrowser()) browser->GetHost()->SendCaptureLostEvent();
            focus_browser_tab_exclusively(-1);
            captured_tab_ = -1;
            *context.active_tab_id = -1;
        }
        auto* active = manager.get_tab(*context.active_tab_id);
        if (!active || !active->cef_visible || !active->client) continue;
        // Keyboard/IME belongs to the focused content and its native window only.
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            if (event.key.windowID == active->platform_window_id) input_handler_.process_sdl_key_event(event);
        } else if (event.type == SDL_EVENT_TEXT_INPUT) {
            if (event.text.windowID == active->platform_window_id) input_handler_.process_sdl_text_event(event);
        } else if (event.type == SDL_EVENT_TEXT_EDITING) {
            if (event.edit.windowID == active->platform_window_id) input_handler_.process_sdl_ime_event(event);
        }
    }
    manager.update();
    retry_imgui_platform_retirement();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    auto* main_viewport = ImGui::GetMainViewport();
    dockspace_ = ImGui::GetID("Corona.Editor.Dockspace");
    if (!ImGui::DockBuilderGetNode(dockspace_)) {
        ImGui::DockBuilderAddNode(dockspace_, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_, main_viewport->WorkSize);
        auto center = dockspace_;
        default_docks_["left"] = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
        default_docks_["right"] = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr, &center);
        default_docks_["bottom"] = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr, &center);
        default_docks_["center"] = center;
        ImGui::DockBuilderFinish(dockspace_);
    }
    draw_editor_dockspace(dockspace_, main_viewport);
    auto snapshot = manager.tabs_snapshot();
    std::sort(snapshot.begin(), snapshot.end(), [](auto a, auto b) { return a.tab_id < b.tab_id; });
    std::vector<ContentRect> contents;
    for (auto [tab_id, tab] : snapshot) {
        if (!tab || !tab->open || tab->minimized) {
            if (tab) set_visible(*tab, false);
            continue;
        }
        const bool main = tab->docking_pos == "main";
        if (tab->detach_state == BrowserTab::DetachState::Detaching) tab->native_force_viewport = true;
        if (tab->detach_state == BrowserTab::DetachState::Redocking) tab->native_force_viewport = false;
        const auto name = window_name(*tab);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse;
        if (tab->camera_view) {
            // Display currently owns one optics layer per native surface. Match the old
            // implementation: camera windows cannot merge into another surface.
            flags |= ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;
        }
        ImGuiWindowClass window_class;
        if (tab->camera_view || tab->native_force_viewport)
            window_class.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        ImGui::SetNextWindowClass(&window_class);
        if (!main && !tab->dock_initialized) {
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(std::max(tab->dock_width, 320)),
                static_cast<float>(std::max(tab->dock_height, 240))), ImGuiCond_FirstUseEver);
            if (tab->native_docked && !ImGui::FindWindowSettingsByID(ImHashStr(name.c_str()))) {
                const std::string& zone = tab->docking_pos;
                const auto it = default_docks_.find(zone);
                const auto target = it != default_docks_.end() && ImGui::DockBuilderGetNode(it->second)
                    ? it->second : resolve_default_panel_dock(dockspace_, zone);
                ImGui::SetNextWindowDockID(target, ImGuiCond_FirstUseEver);
            } else if (tab->detach_state != BrowserTab::DetachState::Detaching) {
                ImGui::SetNextWindowPos(ImVec2(main_viewport->Pos.x + tab->initial_x,
                    main_viewport->Pos.y + tab->initial_y), ImGuiCond_FirstUseEver);
            }
            tab->dock_initialized = true;
        }
        if (!main && tab->detach_state == BrowserTab::DetachState::Detaching) {
            ImGui::SetNextWindowDockID(0);
            ImGui::SetNextWindowPos(ImVec2(static_cast<float>(tab->detach_x), static_cast<float>(tab->detach_y)));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(std::max(tab->detach_w, 240)),
                static_cast<float>(std::max(tab->detach_h, 180))));
            tab->detach_state = BrowserTab::DetachState::Docked;
        } else if (!main && tab->detach_state == BrowserTab::DetachState::Redocking) {
            ImGui::SetNextWindowDockID(dockspace_);
            tab->detach_state = BrowserTab::DetachState::Docked;
        }
        bool open = true;
        bool visible;
        if (!main && tab->native_focus_requested) {
            ImGui::SetNextWindowFocus();
            *context.active_tab_id = tab_id;
            focus_browser_tab_exclusively(tab_id);
            tab->native_focus_requested = false;
        }
        if (main) {
            visible = begin_main_workspace();
        } else {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            visible = ImGui::Begin(name.c_str(), &open, flags);
        }
        if (ImGui::IsWindowDocked()) tab->native_force_viewport = false;
        const auto origin = ImGui::GetCursorScreenPos();
        auto size = ImGui::GetContentRegionAvail();
        const bool usable = visible && size.x > 0 && size.y > 0;
        set_visible(*tab, usable);
        if (usable) {
            const int width = std::max(1, static_cast<int>(size.x));
            const int height = std::max(1, static_cast<int>(size.y));
            if (tab->width != width || tab->height != height) manager.resize_tab(tab_id, width, height);
            manager.update_texture(tab_id);
            if (is_valid_texture_id(tab->texture_id)) ImGui::Image(tab->texture_id, size);
            else ImGui::Dummy(size);
            contents.push_back({tab_id, ImGui::GetWindowViewport()->ID, origin, size, ImGui::IsItemHovered()});
        }
        if (main) end_main_workspace();
        else {
            ImGui::End();
            ImGui::PopStyleVar();
        }
        if (!open) request_close(*tab);
    }

    // Input ownership is decided from the visible content item, not WantCaptureMouse:
    // ImGui also reports capture for a Web image. A press keeps its original browser
    // until release, even when the pointer leaves the item/window.
    int hit = -1;
    for (const auto& content : contents) if (content.hovered) hit = content.tab_id;
    bool clicked = false;
    for (int button = 0; button < 3; ++button) clicked |= ImGui::IsMouseClicked(button);
    if (clicked && captured_tab_ < 0) {
        captured_tab_ = hit;
        *context.active_tab_id = hit;
        focus_browser_tab_exclusively(hit);
    }
    const int target = captured_tab_ >= 0 ? captured_tab_ : hit;
    for (const auto& content : contents) {
        auto* tab = manager.get_tab(content.tab_id);
        if (!tab || !tab->client) continue;
        auto browser = tab->client->GetBrowser();
        if (!browser) continue;
        const auto modifiers = MouseUtils::make_modifiers(io.MouseDown[0], io.MouseDown[1], io.KeyShift, io.KeyCtrl, io.KeyAlt);
        auto mouse = MouseUtils::create_mouse_event_ex(io.MousePos.x, io.MousePos.y,
            content.origin.x, content.origin.y, modifiers);
        if (content.tab_id == hovered_tab_ && content.tab_id != target)
            browser->GetHost()->SendMouseMoveEvent(mouse, true);
        if (content.tab_id != target) continue;
        browser->GetHost()->SendMouseMoveEvent(mouse, false);
        constexpr CefBrowserHost::MouseButtonType buttons[] = {MBT_LEFT, MBT_RIGHT, MBT_MIDDLE};
        for (int button = 0; button < 3; ++button) {
            if (ImGui::IsMouseClicked(button)) browser->GetHost()->SendMouseClickEvent(mouse, buttons[button], false, io.MouseClickedCount[button]);
            if (ImGui::IsMouseReleased(button)) browser->GetHost()->SendMouseClickEvent(mouse, buttons[button], true, io.MouseClickedCount[button]);
        }
        if (io.MouseWheel || io.MouseWheelH)
            browser->GetHost()->SendMouseWheelEvent(mouse, static_cast<int>(io.MouseWheelH * 120), static_cast<int>(io.MouseWheel * 120));
    }
    hovered_tab_ = target;
    if (!io.MouseDown[0] && !io.MouseDown[1] && !io.MouseDown[2]) captured_tab_ = -1;
    if (auto* active = manager.get_tab(*context.active_tab_id); active && active->cef_visible && active->client) {
        if (auto browser = active->client->GetBrowser()) input_handler_.send_key_events_to_browser(browser);
    }
    input_handler_.clear_pending_events();

    ImGui::Render();
    ImGui::UpdatePlatformWindows();
    // Platform handles become available here, including a viewport created this frame.
    for (const auto& content : contents) {
        auto* tab = manager.get_tab(content.tab_id);
        auto* viewport = ImGui::FindViewportByID(content.viewport);
        if (!tab || !viewport || !viewport->PlatformHandle) continue;
        const auto id = static_cast<SDL_WindowID>(reinterpret_cast<std::uintptr_t>(viewport->PlatformHandle));
        auto* window = SDL_GetWindowFromID(id);
        const auto surface = native_surface_from_sdl_window(window);
        tab->platform_window_id = id;
        tab->platform_handle_raw = surface;
        tab->host_surface = viewport == main_viewport ? nullptr : surface;
        tab->detach_state = viewport == main_viewport ? BrowserTab::DetachState::Docked : BrowserTab::DetachState::Detached;
        int logical_w = 1, logical_h = 1, pixel_w = 1, pixel_h = 1;
        SDL_GetWindowSize(window, &logical_w, &logical_h);
        SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
        const float sx = static_cast<float>(pixel_w) / std::max(1, logical_w);
        const float sy = static_cast<float>(pixel_h) / std::max(1, logical_h);
        const int x = static_cast<int>((content.origin.x - viewport->Pos.x) * sx);
        const int y = static_cast<int>((content.origin.y - viewport->Pos.y) * sy);
        int screen_x = static_cast<int>(content.origin.x), screen_y = static_cast<int>(content.origin.y);
#ifdef _WIN32
        POINT point{x, y};
        ClientToScreen(static_cast<HWND>(surface), &point);
        screen_x = point.x; screen_y = point.y;
#endif
        {
            std::lock_guard lock(tab->mutex);
            tab->content_screen_x = screen_x;
            tab->content_screen_y = screen_y;
            tab->content_screen_scale = sx;
        }
        if (content.tab_id == *context.active_tab_id) {
            SDL_StartTextInput(window);
            CefRect caret;
            { std::lock_guard lock(tab->mutex); caret = tab->ime_caret; }
            SDL_Rect area{static_cast<int>(content.origin.x - viewport->Pos.x) + caret.x,
                static_cast<int>(content.origin.y - viewport->Pos.y) + caret.y,
                std::max(1, caret.width), std::max(1, caret.height)};
            SDL_SetTextInputArea(window, &area, 0);
        }
        if (tab->camera_view) {
            const int width = std::max(1, static_cast<int>(content.size.x * sx));
            const int height = std::max(1, static_cast<int>(content.size.y * sy));
            CameraViewportManager::instance().bind_surface(content.tab_id, surface, x, y, width, height);
            CameraViewportManager::instance().update_layout(content.tab_id, x, y, width, height);
        }
        if (tab->web_camera_handle && tab->web_camera_width > 0) {
            // Main camera is the native surface background, never a dock-node
            // viewport. Web still controls its render-resolution budget.
            const bool main = tab->docking_pos == "main";
            SharedDataHub::instance().enqueue_camera_viewport_update({
                .camera_handle = tab->web_camera_handle, .surface = surface, .view_open = false,
                .x = main ? 0 : x + static_cast<int>(tab->web_camera_x * sx),
                .y = main ? 0 : y + static_cast<int>(tab->web_camera_y * sy),
                .width = main ? std::max(1, pixel_w) : std::max(1, static_cast<int>(tab->web_camera_width * sx)),
                .height = main ? std::max(1, pixel_h) : std::max(1, static_cast<int>(tab->web_camera_height * sy)),
                .render_width = std::max(1, static_cast<int>(tab->web_camera_render_width * sx)),
                .render_height = std::max(1, static_cast<int>(tab->web_camera_render_height * sy)),
            });
        }
    }
    if (auto* hover = manager.get_tab(hovered_tab_); hover && hover->hide_system_cursor.load()) SDL_HideCursor();
    render_imgui_platform_windows(*context.vulkan_backend);
    for (auto [tab_id, tab] : manager.tabs_snapshot()) {
        if (tab && !tab->open) {
            if (*context.active_tab_id == tab_id) *context.active_tab_id = -1;
            if (captured_tab_ == tab_id) captured_tab_ = -1;
            manager.remove_tab(tab_id);
        }
    }
}
}
