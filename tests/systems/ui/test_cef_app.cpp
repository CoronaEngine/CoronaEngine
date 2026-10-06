#include <corona/systems/ui/cef_runtime.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "cef/cef_app.h"
#include "cef/cef_osr_mode.h"
#include "cef/browser_manager.h"
#include "cef/cef_client.h"

namespace {

void require(bool condition, std::string_view message) {
    if (condition) {
        return;
    }
    std::cerr << "CefAppTests failed: " << message << '\n';
    std::exit(1);
}

void test_message_router_config() {
    const auto config = Corona::Systems::UI::make_cef_message_router_config();
    require(config.js_query_function == "cefQuery", "unexpected query function");
    require(config.js_cancel_function == "cefQueryCancel", "unexpected cancel function");
}

constexpr std::array<std::string_view, 15> kCommonSwitches{
    "disable-web-security",
    "allow-file-access-from-files",
    "allow-file-access",
    "no-sandbox",
    "disable-extensions",
    "disable-component-extensions-with-background-pages",
    "enable-net-benchmarking",
    "disable-pdf-extension",
    "disable-pdf-viewer",
    "disable-component-update",
    "disable-background-networking",
    "disable-accelerated-video-decode",
    "renderer-process-limit",
    "process-per-site",
    "js-flags",
};

void expect_common_switches(CefRefPtr<CefCommandLine> command_line) {
    for (const auto switch_name : kCommonSwitches) {
        require(command_line->HasSwitch(std::string(switch_name)), switch_name);
    }

    require(command_line->GetSwitchValue("renderer-process-limit").ToString() == "1",
            "renderer-process-limit must cap the renderer count at 1");
    require(command_line->GetSwitchValue("js-flags").ToString().find("--max-old-space-size=") == 0,
            "js-flags must set the V8 old-space ceiling");
}

void expect_gpu_disabled_switches(CefRefPtr<CefCommandLine> command_line) {
    require(command_line->HasSwitch("disable-gpu"), "software OSR must disable the GPU");
    require(command_line->HasSwitch("disable-gpu-compositing"),
            "software OSR must disable GPU compositing");
    require(!command_line->HasSwitch("enable-gpu-rasterization"),
            "software OSR must not enable GPU rasterization");
}

void expect_gpu_enabled_switches(CefRefPtr<CefCommandLine> command_line) {
    require(command_line->HasSwitch("enable-gpu-rasterization"),
            "accelerated OSR must enable GPU rasterization");
    require(command_line->HasSwitch("enable-zero-copy"), "accelerated OSR must enable zero copy");
    require(!command_line->HasSwitch("disable-gpu"), "accelerated OSR must not disable the GPU");
    require(!command_line->HasSwitch("disable-gpu-compositing"),
            "accelerated OSR must not disable GPU compositing");
}

void test_app_contract() {
    auto app = Corona::Systems::UI::create_cef_app();
    require(app != nullptr, "CEF app factory returned null");
    require(app->GetRenderProcessHandler() != nullptr, "renderer handler is missing");

    auto command_line = CefCommandLine::CreateCommandLine();
    command_line->InitFromString("corona_cef_app_tests.exe");
    app->OnBeforeCommandLineProcessing({}, command_line);

    expect_common_switches(command_line);
    if (Corona::Systems::UI::cef_gpu_enabled()) {
        expect_gpu_enabled_switches(command_line);
    } else {
        expect_gpu_disabled_switches(command_line);
    }

    auto forced_accelerated = CefCommandLine::CreateCommandLine();
    forced_accelerated->InitFromString("corona_cef_app_tests.exe");
    Corona::Systems::UI::append_cef_command_line_switches(forced_accelerated, true);
    expect_common_switches(forced_accelerated);
    expect_gpu_enabled_switches(forced_accelerated);

    auto forced_software = CefCommandLine::CreateCommandLine();
    forced_software->InitFromString("corona_cef_app_tests.exe");
    Corona::Systems::UI::append_cef_command_line_switches(forced_software, false);
    expect_common_switches(forced_software);
    expect_gpu_disabled_switches(forced_software);
}

void test_process_per_site_toggle() {
    _putenv_s("CORONA_CEF_PROCESS_PER_SITE", "");
    require(Corona::Systems::UI::cef_process_per_site_enabled(),
            "process-per-site must default to enabled");

    auto default_line = CefCommandLine::CreateCommandLine();
    default_line->InitFromString("corona_cef_app_tests.exe");
    Corona::Systems::UI::append_cef_command_line_switches(default_line, false);
    require(default_line->HasSwitch("process-per-site"),
            "process-per-site must be appended by default");

    for (const char* disabled : {"0", "false", "off", "no", "FALSE", "Off"}) {
        _putenv_s("CORONA_CEF_PROCESS_PER_SITE", disabled);
        require(!Corona::Systems::UI::cef_process_per_site_enabled(), disabled);

        auto disabled_line = CefCommandLine::CreateCommandLine();
        disabled_line->InitFromString("corona_cef_app_tests.exe");
        Corona::Systems::UI::append_cef_command_line_switches(disabled_line, false);
        require(!disabled_line->HasSwitch("process-per-site"),
                "process-per-site must be omitted when disabled");
    }

    for (const char* enabled : {"1", "true", "on", "yes"}) {
        _putenv_s("CORONA_CEF_PROCESS_PER_SITE", enabled);
        require(Corona::Systems::UI::cef_process_per_site_enabled(), enabled);
    }

    _putenv_s("CORONA_CEF_PROCESS_PER_SITE", "");
    auto restored_line = CefCommandLine::CreateCommandLine();
    restored_line->InitFromString("corona_cef_app_tests.exe");
    Corona::Systems::UI::append_cef_command_line_switches(restored_line, false);
    require(restored_line->HasSwitch("process-per-site"),
            "process-per-site must come back after clearing the env var");
}

void test_popup_wiring() {
    CefRefPtr<Corona::Systems::UI::OffscreenRenderHandler> handler =
        new Corona::Systems::UI::OffscreenRenderHandler();
    Corona::Systems::UI::BrowserTab tab;
    handler->SetTab(&tab);

    handler->OnPopupShow(nullptr, true);
    handler->OnPopupSize(nullptr, CefRect(10, 20, 4, 3));
    require(tab.popup.visible(), "popup visibility must be recorded from OnPopupShow");
    require(tab.popup.x() == 10 && tab.popup.y() == 20,
            "popup origin must be recorded from OnPopupSize");

    tab.buffer_dirty = false;
    std::vector<std::uint8_t> popup_bgra(4u * 3u * 4u, 0x22);
    handler->OnPaint(nullptr, PET_POPUP, {}, popup_bgra.data(), 4, 3);
    require(!tab.popup.empty(), "popup pixels must be cached from PET_POPUP paint");
    require(tab.popup.width() == 4 && tab.popup.height() == 3,
            "popup pixel dimensions must be recorded");
    require(tab.buffer_dirty, "popup paint must request a texture re-upload");

    tab.buffer_dirty = false;
    std::vector<std::uint8_t> view_bgra(8u * 8u * 4u, 0x33);
    handler->OnPaint(nullptr, PET_VIEW, {}, view_bgra.data(), 8, 8);
    require(tab.pixel_buffer.size() == 8u * 8u * 4u, "view pixels must still be captured");
    require(tab.buffer_dirty, "view paint must request a texture re-upload");

    tab.buffer_dirty = false;
    handler->OnPopupShow(nullptr, false);
    require(!tab.popup.visible(), "popup must be hidden after OnPopupShow(false)");
    require(tab.buffer_dirty, "hiding the popup must request a re-upload to erase it");

    handler->SetTab(nullptr);
}

void test_browser_process_dispatch(int argc, char* argv[]) {
    const auto exit_code =
        Corona::Systems::UI::execute_cef_subprocess_if_needed(argc, argv);
    require(!exit_code.has_value(), "browser process was treated as a subprocess");
    require(Corona::Systems::UI::was_cef_process_dispatch_completed(),
            "browser process dispatch state was not recorded");
}

}  // namespace

void test_windowless_frame_rate_resolution() {
    using Corona::Systems::UI::resolve_windowless_frame_rate;

    _putenv_s("CORONA_CEF_FRAME_RATE", "");
    require(resolve_windowless_frame_rate() == 60, "unset frame rate must default to 60");

    _putenv_s("CORONA_CEF_FRAME_RATE", "30");
    require(resolve_windowless_frame_rate() == 30, "env must override the frame rate");

    _putenv_s("CORONA_CEF_FRAME_RATE", "1");
    require(resolve_windowless_frame_rate() == 1, "1 must be accepted as a lower bound");

    _putenv_s("CORONA_CEF_FRAME_RATE", "0");
    require(resolve_windowless_frame_rate() == 60, "zero must fall back to the default");

    _putenv_s("CORONA_CEF_FRAME_RATE", "-5");
    require(resolve_windowless_frame_rate() == 60, "negative must fall back to the default");

    _putenv_s("CORONA_CEF_FRAME_RATE", "abc");
    require(resolve_windowless_frame_rate() == 60, "garbage must fall back to the default");

    _putenv_s("CORONA_CEF_FRAME_RATE", "30fps");
    require(resolve_windowless_frame_rate() == 60, "trailing garbage must not be silently parsed");

    _putenv_s("CORONA_CEF_FRAME_RATE", "1000");
    require(resolve_windowless_frame_rate() == 240, "too-large values must clamp to 240");

    _putenv_s("CORONA_CEF_FRAME_RATE", "");
}

int main(int argc, char* argv[]) {
    test_browser_process_dispatch(argc, argv);
    test_popup_wiring();
    test_message_router_config();
    test_process_per_site_toggle();
    test_app_contract();
    test_windowless_frame_rate_resolution();
    return 0;
}
