#include <include/cef_v8.h>
#include <wrapper/cef_helpers.h>

#include "cef_process.h"

#include <unordered_map>

namespace Corona::Systems::UI {

namespace {

class CoronaRenderProcessHandler : public CefRenderProcessHandler {
   public:
    CoronaRenderProcessHandler() = default;

    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        if (auto endpoint = attach_corona_v8_binding(context)) {
            const auto key = frame->GetIdentifier().ToString();
            if (const auto found = endpoints_.find(key); found != endpoints_.end()) release_corona_v8_binding(found->second);
            endpoints_[key] = std::move(endpoint);
        }
    }

    void OnContextReleased(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) override {
        const auto key = frame->GetIdentifier().ToString();
        if (const auto found = endpoints_.find(key); found != endpoints_.end()) {
            release_corona_v8_binding(found->second);
            endpoints_.erase(found);
        }
    }

   private:
    std::unordered_map<std::string, std::shared_ptr<CoronaV8Endpoint>> endpoints_;
    IMPLEMENT_REFCOUNTING(CoronaRenderProcessHandler);
};

}  // namespace

CefRefPtr<CefRenderProcessHandler> create_cef_render_process_handler() {
    return new CoronaRenderProcessHandler();
}

}  // namespace Corona::Systems::UI
