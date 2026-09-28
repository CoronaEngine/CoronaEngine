#pragma once

// CEF headers
#include <include/cef_app.h>
#include <cef_browser.h>
#include <cef_render_handler.h>
#include <wrapper/cef_helpers.h>
#include <wrapper/cef_message_router.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

#include "horizon/core/logging.h"

namespace Corona::Systems::UI {

struct BrowserTab;  // 前向声明

// ============================================================================
// 离屏渲染处理器
// ============================================================================

class OffscreenRenderHandler : public CefRenderHandler {
   public:
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                 const RectList& dirty_rects, const void* buffer,
                 int width, int height) override;
    bool GetScreenPoint(CefRefPtr<CefBrowser> browser, int viewX, int viewY, int& screenX, int& screenY);
    void SetTab(BrowserTab* tab);

    IMPLEMENT_REFCOUNTING(OffscreenRenderHandler);

   private:
    std::mutex tab_mutex_;
    BrowserTab* tab_ = nullptr;
};

// ============================================================================
// JS 请求处理器（处理来自浏览器的 cefQuery 调用）
// ============================================================================

class BrowserSideJSHandler : public CefMessageRouterBrowserSide::Handler,
                             public CefBaseRefCounted {
   public:
    BrowserSideJSHandler() = default;
    ~BrowserSideJSHandler() override = default;

    bool OnQuery(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int64_t query_id,
                 const CefString& request,
                 bool persistent,
                 CefRefPtr<Callback> callback) override;

    void OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         int64_t query_id) override {
        CEF_REQUIRE_UI_THREAD();
        std::cout << "[Browser] Query canceled: " << query_id << std::endl;
    }

   private:
    IMPLEMENT_REFCOUNTING(BrowserSideJSHandler);
    DISALLOW_COPY_AND_ASSIGN(BrowserSideJSHandler);
};

// ============================================================================
// 离屏 CEF 客户端（主要的浏览器集成类）
// ============================================================================

class OffscreenCefClient : public CefClient,
                           public CefLifeSpanHandler,
                           public CefLoadHandler,
                           public CefRequestHandler,
                           public CefRenderHandler,
                           public CefDisplayHandler,
                           public CefContextMenuHandler {
   public:
    OffscreenCefClient();

    void SetTab(BrowserTab* tab);
    void Resize(int width, int height);
    CefRefPtr<CefBrowser> GetBrowser();
    void RequestClose();
    bool WaitForClose(std::chrono::milliseconds timeout);
    void MarkBrowserCreationFailed();

    // CefClient 接口
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override;
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }

    // CefRequestHandler
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request,
                        bool user_gesture,
                        bool is_redirect) override;

    // CefRenderHandler
    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                 const RectList& dirtyRects, const void* buffer,
                 int width, int height) override;

    // CefDisplayHandler
    bool OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                          cef_log_severity_t level,
                          const CefString& message,
                          const CefString& source,
                          int line) override;

    // CefLoadHandler
    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   int httpStatusCode) override;

    // CefLifeSpanHandler
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                   TerminationStatus status,
                                   int error_code,
                                   const CefString& error_string) override;

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) override;

    bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                              CefRefPtr<CefFrame> frame,
                              CefRefPtr<CefContextMenuParams> params,
                              int command_id,
                              CefContextMenuHandler::EventFlags event_flags) override;

    void OnContextMenuDismissed(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame) override;

   private:
    int tab_id_ = -1;
    std::mutex browser_mutex_;
    std::condition_variable browser_closed_cv_;
    CefRefPtr<CefBrowser> browser_;
    bool close_requested_ = false;
    bool browser_closed_ = false;
    CefRefPtr<OffscreenRenderHandler> render_handler_;
    CefRefPtr<CefMessageRouterBrowserSide> browser_side_router_;
    BrowserSideJSHandler* js_handler_;

    static constexpr int MENU_ID_REFRESH = 1001;

    IMPLEMENT_REFCOUNTING(OffscreenCefClient);
};

// ============================================================================
// CEF 生命周期管理
// ============================================================================

/**
 * @brief 初始化 CEF 框架
 * @return 成功返回 true
 */
bool initialize_cef();

/**
 * @brief 关闭 CEF 框架
 */
void shutdown_cef();

}  // namespace Corona::Systems::UI
