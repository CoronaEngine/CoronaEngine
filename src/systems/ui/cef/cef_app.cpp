#include "cef_app.h"

#include "cef_osr_mode.h"
#include "cef_renderer_bridge.h"

namespace Corona::Systems::UI {

namespace {

class CoronaCefApp final : public CefApp {
   public:
    CoronaCefApp() : render_handler_(create_cef_render_process_handler()) {}

    void OnBeforeCommandLineProcessing(
        const CefString& process_type,
        CefRefPtr<CefCommandLine> command_line) override {
        append_cef_command_line_switches(command_line, cef_gpu_enabled());
    }

    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return render_handler_;
    }

   private:
    CefRefPtr<CefRenderProcessHandler> render_handler_;

    IMPLEMENT_REFCOUNTING(CoronaCefApp);
};

}  // namespace

CefMessageRouterConfig make_cef_message_router_config() {
    CefMessageRouterConfig config;
    config.js_query_function = "cefQuery";
    config.js_cancel_function = "cefQueryCancel";
    return config;
}

CefRefPtr<CefApp> create_cef_app() {
    return new CoronaCefApp();
}

void append_cef_command_line_switches(CefRefPtr<CefCommandLine> command_line,
                                      bool gpu_enabled) {
    command_line->AppendSwitch("disable-web-security");
    command_line->AppendSwitch("allow-file-access-from-files");
    command_line->AppendSwitch("allow-file-access");
    command_line->AppendSwitch("no-sandbox");
    if (gpu_enabled) {
        command_line->AppendSwitch("enable-gpu-rasterization");
        command_line->AppendSwitch("enable-zero-copy");
    } else {
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
    }
    command_line->AppendSwitch("disable-extensions");
    command_line->AppendSwitch("disable-component-extensions-with-background-pages");
    command_line->AppendSwitch("enable-net-benchmarking");
    command_line->AppendSwitch("disable-pdf-extension");
    command_line->AppendSwitch("disable-pdf-viewer");
    command_line->AppendSwitch("disable-component-update");
    command_line->AppendSwitch("disable-background-networking");
    command_line->AppendSwitch("disable-accelerated-video-decode");

    command_line->AppendSwitchWithValue("renderer-process-limit", "1");

    if (cef_process_per_site_enabled()) {
        command_line->AppendSwitch("process-per-site");
    }

    command_line->AppendSwitchWithValue("js-flags", "--max-old-space-size=1024");
}

}  // namespace Corona::Systems::UI
