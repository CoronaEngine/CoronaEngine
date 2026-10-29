#include "cef/cef_client.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include <include/cef_command_line.h>

namespace {

class ShutdownTestClient final : public Corona::Systems::UI::OffscreenCefClient {
   public:
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect = CefRect(0, 0, 64, 64);
    }

    void OnLoadEnd(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   int status) override {
        OffscreenCefClient::OnLoadEnd(browser, frame, status);
        if (frame->IsMain()) {
            loaded.store(true);
        }
    }

    std::atomic<bool> loaded{false};
};

}  // namespace

// A real CEF integration test: exercises the production settings, main-thread
// pump and WaitForClose, then requires CefShutdown to return normally. Each
// invocation gets its own process/cache because CEF cannot be reinitialized.
int run_cef_lifecycle_test(bool close_before_created) {
    using namespace Corona::Systems::UI;
    if (!initialize_cef()) {
        std::cerr << "CEF lifecycle: initialization failed\n";
        return 1;
    }
    const bool actual_single = CefCommandLine::GetGlobalCommandLine()->HasSwitch("single-process");
    bool passed = actual_single && CefCurrentlyOn(TID_UI);
    if (!passed) {
        std::cerr << "CEF lifecycle: C++ single-process/main-thread policy was not applied\n";
    }

    CefWindowInfo window;
    window.SetAsWindowless(GetDesktopWindow());
    CefBrowserSettings settings;
    settings.windowless_frame_rate = 10;
    CefRefPtr<ShutdownTestClient> clients[2] = {new ShutdownTestClient, new ShutdownTestClient};
    for (const auto& client : clients) {
        if (!CefBrowserHost::CreateBrowser(window, client,
                "data:text/html,<html><body>shutdown test<script>document.title='ready'</script></body></html>",
                settings, nullptr, nullptr)) {
            client->MarkBrowserCreationFailed();
            passed = false;
        }
    }
    if (close_before_created) {
        clients[0]->RequestClose();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (passed && (!clients[1]->loaded.load() ||
                     (!close_before_created && !clients[0]->loaded.load())) &&
           std::chrono::steady_clock::now() < deadline) {
        do_cef_message_loop_work();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    passed = passed && clients[1]->loaded.load() &&
             (close_before_created || clients[0]->loaded.load());
    if (!passed) {
        std::cerr << "CEF lifecycle: browser loading failed\n";
    }
    for (const auto& client : clients) {
        client->RequestClose();
    }
    for (auto& client : clients) {
        if (!client->WaitForClose(std::chrono::seconds(5))) {
            std::cerr << "CEF lifecycle: OnBeforeClose timed out\n";
            passed = false;
        }
        client = nullptr;
    }
    std::cout << "CEF lifecycle: CefShutdown begin" << std::endl;
    shutdown_cef();
    std::cout << "CEF lifecycle: CefShutdown complete" << std::endl;
    return passed ? 0 : 1;
}
