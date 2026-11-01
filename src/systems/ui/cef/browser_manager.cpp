#include "browser_manager.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <thread>

#include "cef_client.h"
#include "cef_paths.h"
#include "cef_bridge_helpers.h"
#include "cef_osr_mode.h"
#include "horizon/core/logging.h"
#include "corona/systems/ui/camera_viewport_manager.h"

namespace Corona::Systems::UI {

BrowserManager& BrowserManager::instance() {
    static BrowserManager instance;
    return instance;
}

bool BrowserManager::hide_tab(int tab_id, bool if_close) {
    auto it = tabs_.find(tab_id);
    if (it == tabs_.end()) {
        CFW_LOG_WARNING("Attempted to hide non-existent tab: ID={}", tab_id);
        return false;
    }

    BrowserTab* tab = it->second.get();
    tab->minimized = true;
    if (if_close) {
        tab->open = true;
    }

    CFW_LOG_INFO("Browser tab hidden: ID={}", tab_id);
    return true;
}

bool BrowserManager::show_tab(int tab_id) {
    auto it = tabs_.find(tab_id);
    if (it == tabs_.end()) {
        CFW_LOG_WARNING("Attempted to show non-existent tab: ID={}", tab_id);
        return false;
    }

    BrowserTab* tab = it->second.get();

    if (tab->minimized) {
        tab->client->GetBrowser()->Reload();
    }

    tab->minimized = false;

    // 触发重绘
    if (tab->client && tab->client->GetBrowser()) {
        tab->client->GetBrowser()->GetHost()->WasResized();
    }

    CFW_LOG_INFO("Browser tab shown: ID={}", tab_id);
    return true;
}

// 修改主 create_tab 方法
int BrowserManager::create_tab(const std::string& url, const std::string& path,
                               const std::string& docking_pos,
                               int dock_width, int dock_height,
                               bool dock_fixed) {
    return create_tab(url, path, docking_pos, dock_width, dock_height,
                      dock_fixed, false, 120, 120);
}

int BrowserManager::create_tab(const std::string& url, const std::string& path,
                               const std::string& docking_pos,
                               int dock_width, int dock_height,
                               bool dock_fixed, bool camera_view,
                               int initial_x, int initial_y) {
    auto tab = std::make_unique<BrowserTab>();

    int id = 0;
    {
        std::lock_guard lock(tabs_mutex_);
        id = ++tab_counter_;
    }
    const auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());

    // 设置 docking 属性
    tab->tab_id = id;
    tab->docking_pos = docking_pos;
    tab->dock_width = dock_width;
    tab->dock_height = dock_height;
    tab->dock_fixed = dock_fixed;
    tab->dock_initialized = false;
    tab->minimized = false;
    tab->open = true;  // 新标签页默认显示
    tab->camera_view = camera_view;
    tab->initial_x = initial_x;
    tab->initial_y = initial_y;

    // 如果有指定的dock大小，使用它，否则使用默认大小
    if (dock_width > 0 && dock_height > 0) {
        tab->width = dock_width;
        tab->height = dock_height;
    } else {
        tab->width = 1600;
        tab->height = 900;
    }

    tab->name = "Browser " + path;

    CFW_LOG_INFO("create_tab: id={}, camera_view={}, thread={}, path={}", id, camera_view,
                 thread_id, path);

    // URL Processing
    std::string full_url = local_path_to_url(url);
    if (!path.empty()) {
        std::string clean_fragment = path;
        if (clean_fragment.starts_with("#")) {
            clean_fragment = clean_fragment.substr(1);
        }
        size_t hash_pos = full_url.find('#');
        if (hash_pos != std::string::npos) {
            full_url = full_url.substr(0, hash_pos);
        }
        full_url += "#" + clean_fragment;
    }
    CFW_LOG_INFO("create_tab: id={}, URL={}", id, full_url);

    tab->url = full_url;
    strncpy(tab->url_buffer, full_url.c_str(), sizeof(tab->url_buffer) - 1);

    tab->client = new OffscreenCefClient();
    tab->client->SetTab(tab.get());

    // Create browser texture
    tab->texture_id = create_browser_texture(tab->width, tab->height);

    // Create Offscreen Browser
    CefWindowInfo window_info;
    HWND hwnd = NULL;
    if (main_window_) {
        // SDL3 获取原生 Windows 句柄的标准方法
        hwnd = (HWND)SDL_GetPointerProperty(
            SDL_GetWindowProperties(main_window_),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER,
            NULL);
    }
    // 如果拿到了 HWND 就用 HWND，否则退回到桌面（不推荐）
    if (hwnd) {
        window_info.SetAsWindowless(hwnd);
    } else {
        CFW_LOG_WARNING("Main window handle not found, falling back to Desktop Window.");
        window_info.SetAsWindowless(GetDesktopWindow());
    }

    if (cef_osr_mode() == CefOsrMode::SharedTexture) {
        window_info.shared_texture_enabled = 1;
        CFW_LOG_NOTICE("create_tab: id={}, shared texture OSR enabled (CORONA_CEF_ACCELERATED_OSR=1)", id);
    }

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate = resolve_windowless_frame_rate();
    browser_settings.background_color = CefColorSetARGB(0, 0, 0, 0);
    browser_settings.javascript = STATE_ENABLED;
    browser_settings.local_storage = STATE_ENABLED;
    browser_settings.webgl = STATE_ENABLED;
    const bool transparent_overlay =
        camera_view ||
        docking_pos == "main" ||
        docking_pos == "right_top" ||
        docking_pos == "right_bottom" ||
        docking_pos == "left_bottom" ||
        docking_pos == "center";
    tab->transparent_overlay = transparent_overlay;
    browser_settings.background_color =
        CefColorSetARGB(transparent_overlay ? 0 : 255, 16, 19, 16);

    // Publish before invoking CEF: lifecycle callbacks may arrive before CreateBrowser returns.
    BrowserTab* registered_tab = nullptr;
    {
        std::lock_guard lock(tabs_mutex_);
        tabs_[id] = std::move(tab);
        registered_tab = tabs_[id].get();
    }
    // CreateBrowser runs unlocked: callbacks it triggers may look tabs up again.
    const bool browser_created = CefBrowserHost::CreateBrowser(
        window_info, CefRefPtr<CefClient>(registered_tab->client), full_url, browser_settings,
        nullptr, nullptr);
    CFW_LOG_INFO("create_tab: id={}, CreateBrowser returned {}, thread={}", id,
                 browser_created, thread_id);
    if (!browser_created) {
        registered_tab->cef_creation_failed = true;
        registered_tab->client->MarkBrowserCreationFailed();
        CFW_LOG_ERROR("Failed to create browser tab: ID={}, camera_view={}", id, camera_view);
    }
    return id;
}

BrowserTab* BrowserManager::get_tab(int tab_id) {
    std::lock_guard lock(tabs_mutex_);
    auto it = tabs_.find(tab_id);
    return it != tabs_.end() ? it->second.get() : nullptr;
}

std::vector<BrowserManager::TabSnapshotEntry> BrowserManager::tabs_snapshot() const {
    std::lock_guard lock(tabs_mutex_);
    std::vector<TabSnapshotEntry> snapshot;
    snapshot.reserve(tabs_.size());
    for (const auto& [tab_id, tab] : tabs_) {
        snapshot.push_back({tab_id, tab.get()});
    }
    return snapshot;
}

void BrowserManager::remove_tab(int tab_id) {
    // Take ownership out of the map first: after this no other thread can reach the
    // tab through tabs_, and the GPU texture teardown / CEF close below run without
    // holding the map lock.
    std::unique_ptr<BrowserTab> removed;
    {
        std::lock_guard lock(tabs_mutex_);
        auto it = tabs_.find(tab_id);
        if (it == tabs_.end()) return;
        removed = std::move(it->second);
        tabs_.erase(it);
    }
    BrowserTab* tab = removed.get();
    if (!tab) return;

    CFW_LOG_INFO("[CEF/Tab] remove_tab id={} camera_view={} floating={} open={}", tab_id,
                 tab->camera_view, tab->floating, tab->open);
    if (tab->camera_view) {
        CameraViewportManager::instance().unregister_view(
            tab_id, tab->preserve_camera_open_on_close);
    }
    // 先断开 render handler 对 BrowserTab 的引用，避免 CloseBrowser 之后
    // 仍有在途的 OnPaint 回调访问已释放的 tab（use-after-free 崩溃）。
    if (tab->client) {
        tab->client->SetTab(nullptr);
        tab->client->RequestClose();
    }

    destroy_tab_texture(tab);
    // `removed` releases the tab here, once every reference above is dropped.
}

void BrowserManager::set_tab_drag_regions(int tab_id, const std::vector<DragRegion>& regions) {
    BrowserTab* tab = nullptr;
    {
        std::lock_guard lock(tabs_mutex_);
        auto it = tabs_.find(tab_id);
        if (it == tabs_.end()) return;
        tab = it->second.get();
    }
    std::lock_guard<std::mutex> lock(tab->drag_mutex);
    tab->drag_regions = regions;
}

void BrowserManager::update() {
    ++frame_index_;
    retire_deferred_tab_textures();

    std::function<void()> task;
    {
        std::lock_guard lock(pending_tasks_mutex_);
        if (!pending_tasks_.empty()) {
            task = std::move(pending_tasks_.front());
            pending_tasks_.erase(pending_tasks_.begin());
        }
    }
    if (task) {
        try {
            task();
        } catch (const std::exception& exception) {
            CFW_LOG_ERROR("Browser main-thread task failed: {}", exception.what());
        } catch (...) {
            CFW_LOG_ERROR("Browser main-thread task failed with an unknown exception");
        }
    }

    // GPU actor-pick completions are produced by OpticsSystem on another thread;
    // broadcast them only from this CEF/UI-thread tick.
    drain_actor_pick_completion_events();

    // Texture uploads are driven by UiFrameRunner after this frame's SDL/CEF input has
    // already been routed. Keeping uploads out of this queue drain prevents a GPU receipt
    // wait for one detached surface from delaying mouse events destined for the main window.
}

void BrowserManager::enqueue_main_thread_task(std::function<void()> task) {
    std::lock_guard lock(pending_tasks_mutex_);
    pending_tasks_.push_back(std::move(task));
}

void BrowserManager::close_all_tabs() {
    std::vector<int> ids;
    std::vector<CefRefPtr<OffscreenCefClient>> clients;
    const auto snapshot = tabs_snapshot();
    ids.reserve(snapshot.size());
    clients.reserve(snapshot.size());
    for (const auto& entry : snapshot) {
        ids.push_back(entry.tab_id);
    }

    for (int id : ids) {
        if (auto* tab = get_tab(id); tab && tab->client) {
            clients.emplace_back(tab->client);
        }
        remove_tab(id);
    }

    for (const auto& client : clients) {
        if (!client->WaitForClose(std::chrono::seconds(5))) {
            CFW_LOG_WARNING("Timed out waiting for a CEF browser to close");
        }
    }
    retire_deferred_tab_textures(true);
}

}  // namespace Corona::Systems::UI
