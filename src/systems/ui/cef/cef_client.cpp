#include "cef_client.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include <horizon/core/logging.h>

#include "browser_manager.h"
#include "cef_app.h"
#include "cef_bridge_helpers.h"
#include "cef_editor_api.h"
#include "cef_shared_texture_probe.h"
#include "request_response_broker.h"

namespace Corona::Systems::UI {

namespace {

bool is_main_editor_route(const std::string& url) {
    const auto hash_pos = url.find('#');
    if (hash_pos == std::string::npos) {
        return false;
    }

    std::string route = url.substr(hash_pos + 1);
    if (const auto query_pos = route.find('?'); query_pos != std::string::npos) {
        route = route.substr(0, query_pos);
    }

    return route == "/" || route == "/MainPage";
}

bool should_preserve_alpha(BrowserTab* tab, CefRefPtr<CefBrowser> browser) {
    if (!tab) {
        return false;
    }
    if (tab->camera_view) {
        return true;
    }
    if (tab->docking_pos != "main") {
        return tab->transparent_overlay;
    }

    std::string url = tab->url;
    if (browser && browser->GetMainFrame()) {
        url = browser->GetMainFrame()->GetURL().ToString();
    }
    return is_main_editor_route(url);
}

}  // namespace

// ============================================================================
// OffscreenRenderHandler 实现
// ============================================================================

void OffscreenRenderHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    std::lock_guard lock(tab_mutex_);
    BrowserTab* t = tab_;
    if (t) {
        rect = CefRect(0, 0, t->width, t->height);
    } else {
        rect = CefRect(0, 0, 800, 600);
    }
}

void OffscreenRenderHandler::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                     const RectList& dirty_rects, const void* buffer,
                                     int width, int height) {
    std::lock_guard tab_lock(tab_mutex_);
    record_dirty_stats(type, dirty_rects, width, height);
    BrowserTab* t = tab_;
    if (t == nullptr) {
        return;
    }

    if (type == PET_POPUP) {
        if (buffer != nullptr && width > 0 && height > 0) {
            std::lock_guard<std::mutex> lock(t->mutex);
            t->popup.update_pixels(buffer, width, height);
            t->buffer_dirty = true;
        }
        return;
    }

    if (type == PET_VIEW && buffer && width > 0 && height > 0) {
        const bool preserve_alpha = should_preserve_alpha(t, browser);
        size_t bufferSize = static_cast<size_t>(width) * height * 4;
        std::lock_guard<std::mutex> lock(t->mutex);
        t->pixel_buffer.resize(bufferSize);
        std::memcpy(t->pixel_buffer.data(), buffer, bufferSize);

        // CEF outputs BGRA on Windows; convert to RGBA for Vulkan RGBA8 textures.
        auto* pixels = t->pixel_buffer.data();
        for (size_t i = 0; i < bufferSize; i += 4) {
            std::swap(pixels[i], pixels[i + 2]);
            if (!preserve_alpha) {
                pixels[i + 3] = 255;
            }
        }

        t->buffer_dirty = true;
    }
}

void OffscreenRenderHandler::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                                const RectList& dirty_rects,
                                                const CefAcceleratedPaintInfo& info) {
    (void)browser;
    (void)dirty_rects;

    const auto handle = reinterpret_cast<std::uintptr_t>(info.shared_texture_handle);

    if (type == PET_POPUP) {
        static std::atomic<std::uint64_t> popup_frames{0};
        const std::uint64_t popup_frame = popup_frames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (popup_frame == 1 || popup_frame % 600 == 0) {
            CFW_LOG_INFO(
                "[CEF/OSR] OnAcceleratedPaint PET_POPUP frame={} handle=0x{:X} coded={}x{}"
                "（加速路径未消费弹层纹理）",
                popup_frame, handle, info.extra.coded_size.width, info.extra.coded_size.height);
        }
        return;
    }
    if (type != PET_VIEW) {
        return;
    }

    static std::atomic<std::uint64_t> frame_counter{0};
    const std::uint64_t frame = frame_counter.fetch_add(1, std::memory_order_relaxed);

    if (frame < 3 || frame % 600 == 0) {
        CFW_LOG_INFO("[CEF/OSR] OnAcceleratedPaint frame={} handle=0x{:X} coded={}x{} visible={}x{} format={}",
                     frame, handle,
                     info.extra.coded_size.width, info.extra.coded_size.height,
                     info.extra.visible_rect.width, info.extra.visible_rect.height,
                     static_cast<int>(info.format));
    }

    probe_cef_shared_texture(handle, info.extra.coded_size.width, info.extra.coded_size.height);
}

void OffscreenRenderHandler::record_dirty_stats(PaintElementType type, const RectList& dirty_rects,
                                                int width, int height) {
    if (type != PET_VIEW) {
        ++popup_paints_;
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }

    ++paint_frames_;
    dirty_rect_total_ += dirty_rects.size();
    if (dirty_rects.empty()) {
        ++zero_dirty_frames_;
    }

    constexpr int kGridWidth = 64;
    constexpr int kGridHeight = 32;
    constexpr int kGridCells = kGridWidth * kGridHeight;

    std::fill(dirty_cells_.begin(), dirty_cells_.end(), std::uint8_t{0});
    for (const auto& rect : dirty_rects) {
        const int x0 = std::clamp(rect.x, 0, width);
        const int y0 = std::clamp(rect.y, 0, height);
        const int x1 = std::clamp(rect.x + rect.width, 0, width);
        const int y1 = std::clamp(rect.y + rect.height, 0, height);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        const int cx0 = x0 * kGridWidth / width;
        const int cy0 = y0 * kGridHeight / height;
        const int cx1 = std::min((x1 * kGridWidth + width - 1) / width, kGridWidth - 1);
        const int cy1 = std::min((y1 * kGridHeight + height - 1) / height, kGridHeight - 1);
        for (int cy = cy0; cy <= cy1; ++cy) {
            for (int cx = cx0; cx <= cx1; ++cx) {
                dirty_cells_[static_cast<std::size_t>(cy) * kGridWidth + cx] = 1;
            }
        }
    }

    const std::uint64_t marked = static_cast<std::uint64_t>(
        std::count(dirty_cells_.begin(), dirty_cells_.end(), std::uint8_t{1}));
    covered_cells_total_ += marked;
    if (marked * 10 >= static_cast<std::uint64_t>(kGridCells) * 9) {
        ++full_dirty_frames_;
    }

    constexpr std::uint64_t kStatsIntervalFrames = 600;
    if (paint_frames_ % kStatsIntervalFrames == 0) {
        flush_dirty_stats(width, height);
    }
}

void OffscreenRenderHandler::flush_dirty_stats(int width, int height) {
    constexpr double kGridCells = 64.0 * 32.0;
    const double frames = static_cast<double>(paint_frames_);
    const double avg_rects = frames > 0.0 ? static_cast<double>(dirty_rect_total_) / frames : 0.0;
    const double avg_cover = frames > 0.0
                                 ? 100.0 * static_cast<double>(covered_cells_total_) / (frames * kGridCells)
                                 : 0.0;
    CFW_LOG_INFO(
        "[CEF/Paint] tab={} frames={} view={}x{} avg_dirty_rects={:.2f} avg_dirty_area={:.1f}% "
        "zero_dirty_frames={} full_dirty_frames={} popup_paints={}",
        tab_ != nullptr ? tab_->tab_id : -1, paint_frames_, width, height, avg_rects, avg_cover,
        zero_dirty_frames_, full_dirty_frames_, popup_paints_);

    paint_frames_ = 0;
    dirty_rect_total_ = 0;
    zero_dirty_frames_ = 0;
    full_dirty_frames_ = 0;
    covered_cells_total_ = 0;
    popup_paints_ = 0;
}

void OffscreenRenderHandler::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) {
    (void)browser;
    std::lock_guard tab_lock(tab_mutex_);
    BrowserTab* t = tab_;
    if (t == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(t->mutex);
    t->popup.set_visible(show);
    t->buffer_dirty = true;
    CFW_LOG_INFO("[CEF/Popup] show={} tab={}", show, t->tab_id);
}

void OffscreenRenderHandler::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) {
    (void)browser;
    std::lock_guard tab_lock(tab_mutex_);
    BrowserTab* t = tab_;
    if (t == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(t->mutex);
    t->popup.set_rect(rect.x, rect.y, rect.width, rect.height);
}

bool OffscreenRenderHandler::GetScreenPoint(CefRefPtr<CefBrowser> browser, int viewX, int viewY, int& screenX, int& screenY) {
    std::lock_guard lock(tab_mutex_);
    if (!tab_) return false;

    // 将局部坐标转换成屏幕绝对坐标
    POINT mouse_pt;
    GetCursorPos(&mouse_pt);
    screenX = mouse_pt.x;
    screenY = mouse_pt.y;
    return true;
}

void OffscreenRenderHandler::SetTab(BrowserTab* tab) {
    std::lock_guard lock(tab_mutex_);
    tab_ = tab;
}

// ============================================================================
// OffscreenCefClient 实现
// ============================================================================

OffscreenCefClient::OffscreenCefClient()
    : browser_(nullptr),
      render_handler_(new OffscreenRenderHandler()),
      browser_side_router_(nullptr),
      js_handler_(nullptr) {
}

CefRefPtr<CefRenderHandler> OffscreenCefClient::GetRenderHandler() {
    return render_handler_;
}

void OffscreenCefClient::SetTab(BrowserTab* tab) {
    tab_id_ = tab ? tab->tab_id : -1;
    if (render_handler_) {
        render_handler_->SetTab(tab);
    }
}

void OffscreenCefClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    CFW_LOG_INFO("CEF: OnAfterCreated tab={}, browser={}, thread={}", tab_id_,
                 browser ? browser->GetIdentifier() : -1,
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));

    bool close_requested = false;
    {
        std::lock_guard lock(browser_mutex_);
        if (!browser_) {
            browser_ = browser;
        }
        close_requested = close_requested_;
    }

    if (!browser_side_router_) {
        browser_side_router_ =
            CefMessageRouterBrowserSide::Create(make_cef_message_router_config());

        js_handler_ = new BrowserSideJSHandler();
        browser_side_router_->AddHandler(js_handler_, true);
    }

    if (close_requested) {
        browser->GetHost()->CloseBrowser(true);
        return;
    }

    browser->GetHost()->WasHidden(false);
    browser->GetHost()->WasResized();
    browser->GetHost()->Invalidate(PET_VIEW);
}

void OffscreenCefClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   int httpStatusCode) {
    CEF_REQUIRE_UI_THREAD();
    if (!browser || !frame || !frame->IsMain()) {
        return;
    }

    CFW_LOG_INFO("CEF: OnLoadEnd tab={}, status={}, thread={}", tab_id_, httpStatusCode,
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));
    browser->GetHost()->WasHidden(false);
    browser->GetHost()->WasResized();
    browser->GetHost()->Invalidate(PET_VIEW);
}

void OffscreenCefClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    if (browser) {
        cef_request_response_broker().cancel_browser(browser->GetIdentifier());
        EditorApiCallbackRegistry::instance().clear_cef_callbacks_for_browser(
            browser->GetIdentifier());
    }
    if (browser_side_router_) {
        browser_side_router_->OnBeforeClose(browser);
    }
    {
        std::lock_guard lock(browser_mutex_);
        browser_ = nullptr;
        browser_closed_ = true;
    }
    browser_closed_cv_.notify_all();
}

CefRefPtr<CefBrowser> OffscreenCefClient::GetBrowser() {
    std::lock_guard lock(browser_mutex_);
    return browser_;
}

void OffscreenCefClient::RequestClose() {
    CefRefPtr<CefBrowser> browser;
    {
        std::lock_guard lock(browser_mutex_);
        close_requested_ = true;
        browser = browser_;
    }
    if (browser) {
        browser->GetHost()->CloseBrowser(true);
    }
}

bool OffscreenCefClient::WaitForClose(std::chrono::milliseconds timeout) {
    std::unique_lock lock(browser_mutex_);
    return browser_closed_cv_.wait_for(lock, timeout, [this] {
        return browser_closed_;
    });
}

void OffscreenCefClient::MarkBrowserCreationFailed() {
    CFW_LOG_ERROR("CEF: MarkBrowserCreationFailed tab={}, thread={}", tab_id_,
                  std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::lock_guard lock(browser_mutex_);
        browser_closed_ = true;
    }
    browser_closed_cv_.notify_all();
}

void OffscreenCefClient::Resize(int width, int height) {
    if (auto browser = GetBrowser()) {
        browser->GetHost()->WasResized();
        browser->GetHost()->Invalidate(PET_VIEW);
    }
}

bool OffscreenCefClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        CefRefPtr<CefRequest> request,
                                        bool user_gesture,
                                        bool is_redirect) {
    CEF_REQUIRE_UI_THREAD();
    if (browser_side_router_) {
        browser_side_router_->OnBeforeBrowse(browser, frame);
    }
    return false;
}

void OffscreenCefClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    if (render_handler_) {
        render_handler_->GetViewRect(browser, rect);
    }
}

void OffscreenCefClient::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                 const RectList& dirtyRects, const void* buffer,
                                 int width, int height) {
    if (render_handler_) {
        render_handler_->OnPaint(browser, type, dirtyRects, buffer, width, height);
    }
}

void OffscreenCefClient::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                            const RectList& dirtyRects,
                                            const CefAcceleratedPaintInfo& info) {
    if (render_handler_) {
        render_handler_->OnAcceleratedPaint(browser, type, dirtyRects, info);
    }
}

void OffscreenCefClient::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) {
    if (render_handler_) {
        render_handler_->OnPopupShow(browser, show);
    }
}

void OffscreenCefClient::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) {
    if (render_handler_) {
        render_handler_->OnPopupSize(browser, rect);
    }
}

bool OffscreenCefClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                          cef_log_severity_t level,
                                          const CefString& message,
                                          const CefString& source,
                                          int line) {
    const char* levelStr = "LOG";
    switch (level) {
        case LOGSEVERITY_DEBUG:
            levelStr = "DEBUG";
            break;
        case LOGSEVERITY_INFO:
            levelStr = "INFO";
            break;
        case LOGSEVERITY_WARNING:
            levelStr = "WARNING";
            break;
        case LOGSEVERITY_ERROR:
            levelStr = "ERROR";
            break;
        default:
            break;
    }

    const auto msg = message.ToString();
    if (msg.find("ActorTransformFast") != std::string::npos || msg.find("coronaBridge") != std::string::npos) {
        VUE_LOG_INFO("[{}] {}", levelStr, msg.c_str());
    }
    return true;
}

void OffscreenCefClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                                   TerminationStatus status,
                                                   int error_code,
                                                   const CefString& error_string) {
    CEF_REQUIRE_UI_THREAD();
    if (browser_side_router_) {
        browser_side_router_->OnRenderProcessTerminated(browser);
    }
}

bool OffscreenCefClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                                  CefRefPtr<CefFrame> frame,
                                                  CefProcessId source_process,
                                                  CefRefPtr<CefProcessMessage> message) {
    CEF_REQUIRE_UI_THREAD();
    if (handle_realtime_process_message(browser, frame, message)) {
        return true;
    }
    if (message->GetName() == "RendererMessage") {
        std::string msg = message->GetArgumentList()->GetString(0);
        CFW_LOG_INFO("CEF: Received message from Renderer: {}", msg);
        return true;
    }

    return forward_process_message_to_router(browser_side_router_, browser, frame, source_process, message);
}

// ============================================================================
// CefContextMenuHandler 实现
// ============================================================================

void OffscreenCefClient::OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                                             CefRefPtr<CefFrame> frame,
                                             CefRefPtr<CefContextMenuParams> params,
                                             CefRefPtr<CefMenuModel> model) {
    CEF_REQUIRE_UI_THREAD();

    if (!model || !frame) {
        return;
    }

    // 清空现有菜单项（可选，如果只想保留自定义菜单）
    model->Clear();

    // 添加刷新菜单项
    model->AddItem(MENU_ID_REFRESH, "刷新页面");
}

bool OffscreenCefClient::OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                                              CefRefPtr<CefFrame> frame,
                                              CefRefPtr<CefContextMenuParams> params,
                                              int command_id,
                                              CefContextMenuHandler::EventFlags event_flags) {
    CEF_REQUIRE_UI_THREAD();

    if (!browser || !frame) {
        return false;
    }

    switch (command_id) {
        case MENU_ID_REFRESH:
            // 刷新当前页面
            browser->Reload();
            CFW_LOG_INFO("Browser refresh triggered via context menu");
            return true;
        default:
            return false;
    }
}

void OffscreenCefClient::OnContextMenuDismissed(CefRefPtr<CefBrowser> browser,
                                                CefRefPtr<CefFrame> frame) {
    CEF_REQUIRE_UI_THREAD();
    // 菜单关闭时的清理工作（可选）
}

// ============================================================================
// CEF 生命周期管理
// ============================================================================

bool initialize_cef() {
    if (!was_cef_process_dispatch_completed()) {
        CFW_LOG_ERROR(
            "CEF process dispatch was not completed; call "
            "execute_cef_subprocess_if_needed() at the start of main()");
        return false;
    }

    CefMainArgs main_args(GetModuleHandle(nullptr));
    CefRefPtr<CefApp> app = create_cef_app();

    CefSettings settings;
    settings.multi_threaded_message_loop = true;
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = true;
    settings.remote_debugging_port = 9222;
    settings.log_severity = LOGSEVERITY_FATAL;
    settings.uncaught_exception_stack_size = 10;

    CefString(&settings.locale).FromASCII("zh-CN");

#ifdef _WIN32
    std::filesystem::path root_cache_path;
    if (const char* local_app_data = std::getenv("LOCALAPPDATA")) {
        root_cache_path = std::filesystem::path(local_app_data) / "CoronaEngine" / "CEFRoot";
    } else {
        root_cache_path = std::filesystem::current_path() / "cef_root";
    }
#else
    std::filesystem::path root_cache_path = std::filesystem::current_path() / "cef_root";
#endif
    std::filesystem::path cache_path = root_cache_path / "Default";
    std::error_code cache_ec;
    if (!std::filesystem::exists(cache_path, cache_ec)) {
        std::filesystem::create_directories(cache_path, cache_ec);
    }
    if (cache_ec) {
        CFW_LOG_WARNING("CEF: failed to prepare cache path {}: {}; falling back to local cache",
                        cache_path.string(), cache_ec.message());
        cache_ec.clear();
        root_cache_path = std::filesystem::current_path() / "cef_root";
        cache_path = root_cache_path / "Default";
        if (!std::filesystem::exists(cache_path, cache_ec)) {
            std::filesystem::create_directories(cache_path, cache_ec);
        }
        if (cache_ec) {
            CFW_LOG_WARNING("CEF: local cache path {} is unavailable: {}", cache_path.string(),
                            cache_ec.message());
            root_cache_path.clear();
            cache_path.clear();
        }
    }
    if (!cache_path.empty()) {
        CefString(&settings.root_cache_path).FromString(root_cache_path.string());
        CefString(&settings.cache_path).FromString(cache_path.string());
    }

    wchar_t exe_path[MAX_PATH]{};
    const DWORD exe_path_length = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (exe_path_length == 0 || exe_path_length >= MAX_PATH) {
        CFW_LOG_ERROR("CEF: failed to resolve the current executable path");
        return false;
    }
    CefString(&settings.browser_subprocess_path).FromWString(exe_path);
    CFW_LOG_INFO("CEF: Using main executable for subprocesses: {}",
                 std::filesystem::path(exe_path).string());

    CefString(&settings.user_agent).FromASCII("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    settings.background_color = CefColorSetARGB(0, 0, 0, 0);
    settings.persist_session_cookies = true;

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        CFW_LOG_ERROR("Failed to initialize CEF.");
        return false;
    }

    return true;
}

void shutdown_cef() {
    CFW_LOG_INFO("CEF: Starting shutdown...");
    CefShutdown();
    CFW_LOG_INFO("CEF: Shutdown complete");
}

}  // namespace Corona::Systems::UI
