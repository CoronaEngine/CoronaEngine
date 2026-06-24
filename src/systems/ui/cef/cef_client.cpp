#include "cef_client.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <string>

#include <corona/kernel/core/i_logger.h>

#include "browser_manager.h"
#include "cef_bridge_helpers.h"

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
    (void)dirty_rects;
    std::lock_guard tab_lock(tab_mutex_);
    BrowserTab* t = tab_;
    if (t && type == PET_VIEW && buffer && width > 0 && height > 0) {
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
    if (render_handler_) {
        render_handler_->SetTab(tab);
    }
}

void OffscreenCefClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();

    bool close_requested = false;
    {
        std::lock_guard lock(browser_mutex_);
        if (!browser_) {
            browser_ = browser;
        }
        close_requested = close_requested_;
    }

    if (!browser_side_router_) {
        CefMessageRouterConfig config;
        config.js_query_function = "cefQuery";
        config.js_cancel_function = "cefQueryCancel";
        browser_side_router_ = CefMessageRouterBrowserSide::Create(config);

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

    CFW_LOG_INFO("CEF: main frame load end status={}", httpStatusCode);
    browser->GetHost()->WasHidden(false);
    browser->GetHost()->WasResized();
    browser->GetHost()->Invalidate(PET_VIEW);
}

void OffscreenCefClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
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
// CefAppConfig 实现
// ============================================================================

void CefAppConfig::OnBeforeCommandLineProcessing(const CefString& process_type,
                                                 CefRefPtr<CefCommandLine> command_line) {
    command_line->AppendSwitch("disable-web-security");
    command_line->AppendSwitch("allow-file-access-from-files");
    command_line->AppendSwitch("allow-file-access");
    command_line->AppendSwitch("no-sandbox");
    command_line->AppendSwitch("disable-gpu");
    command_line->AppendSwitch("disable-gpu-compositing");
    command_line->AppendSwitch("enable-plugins");
    command_line->AppendSwitch("enable-net-benchmarking");
    command_line->AppendSwitch("disable-pdf-extension");
    command_line->AppendSwitch("disable-pdf-viewer");
    command_line->AppendSwitch("disable-component-update");
    command_line->AppendSwitch("disable-background-networking");
    command_line->AppendSwitch("disable-d3d11");
    command_line->AppendSwitch("disable-accelerated-video-decode");
}

// ============================================================================
// CEF 生命周期管理
// ============================================================================

CefMessageRouterConfig message_router_config;

bool initialize_cef() {
    message_router_config.js_query_function = "cefQuery";
    message_router_config.js_cancel_function = "cefQueryCancel";

    CefMainArgs main_args(GetModuleHandle(nullptr));
    CefRefPtr<CefAppConfig> app(new CefAppConfig());

    CefSettings settings;
    settings.multi_threaded_message_loop = true;
    settings.windowless_rendering_enabled = true;
    settings.no_sandbox = true;
    settings.remote_debugging_port = 9222;
    settings.log_severity = LOGSEVERITY_FATAL;
    settings.uncaught_exception_stack_size = 10;

    CefString(&settings.locale).FromASCII("zh-CN");

    std::filesystem::path cache_path = std::filesystem::current_path() / "cache";
    if (!std::filesystem::exists(cache_path)) {
        std::filesystem::create_directories(cache_path);
    }
    CefString(&settings.cache_path).FromString(cache_path.string());

    // 使用单独的子进程可执行文件
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();
    std::filesystem::path subprocess_path = exe_dir / "cef_subprocess.exe";

    if (std::filesystem::exists(subprocess_path)) {
        CefString(&settings.browser_subprocess_path).FromWString(subprocess_path.wstring());
        CFW_LOG_INFO("CEF: Using separate subprocess: {}", subprocess_path.string());
    } else {
        CefString(&settings.browser_subprocess_path).FromWString(exe_path);
        CFW_LOG_WARNING("CEF: cef_subprocess.exe not found, using main executable as subprocess");
    }

    CefString(&settings.user_agent).FromASCII("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    settings.background_color = CefColorSetARGB(0, 0, 0, 0);
    settings.persist_session_cookies = true;

    CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
    command_line->InitFromString(::GetCommandLineW());

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
