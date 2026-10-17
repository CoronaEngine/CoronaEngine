#include <corona/systems/ui/cef_runtime.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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

// Switches this app appends. `allow-file-access` and `disable-web-security` used to sit here;
// both were removed deliberately (see expect_exact_switch_set and the P0-2 record in
// docs/development/cef-ui-risk-optimization-tasks.md).
constexpr std::array<std::string_view, 13> kCommonSwitches{
    // Load-bearing. Do not drop these without an equivalent replacement:
    "allow-file-access-from-files",  // the Vue bundle is an ES-module graph served over file://
    "no-sandbox",                    // the CEF package ships no cef_sandbox.lib, so no sandbox is compiled in
    // Footprint / hygiene:
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

// Pins the *exact* set of Chromium switches we append, so neither an accidental addition nor an
// accidental removal can slip through unnoticed. `disable-web-security` in particular must never
// come back without a deliberate, documented reason: it disables the same-origin policy outright,
// and the app does not need it (all native calls go through the cefQuery manifest contract).
void expect_exact_switch_set(CefRefPtr<CefCommandLine> command_line, bool gpu_enabled) {
    std::set<std::string> expected;
    for (const auto switch_name : kCommonSwitches) {
        expected.insert(std::string(switch_name));
    }
    if (gpu_enabled) {
        expected.insert("enable-gpu-rasterization");
        expected.insert("enable-zero-copy");
    } else {
        expected.insert("disable-gpu");
        expected.insert("disable-gpu-compositing");
    }

    CefCommandLine::SwitchMap switches;
    command_line->GetSwitches(switches);
    std::set<std::string> actual;
    for (const auto& entry : switches) {
        actual.insert(entry.first.ToString());
    }

    std::vector<std::string> unexpected;
    std::vector<std::string> missing;
    std::set_difference(actual.begin(), actual.end(), expected.begin(), expected.end(),
                        std::back_inserter(unexpected));
    std::set_difference(expected.begin(), expected.end(), actual.begin(), actual.end(),
                        std::back_inserter(missing));

    for (const auto& name : unexpected) {
        std::cerr << "CefAppTests: unexpected Chromium switch appended: " << name << '\n';
    }
    for (const auto& name : missing) {
        std::cerr << "CefAppTests: expected Chromium switch is missing: " << name << '\n';
    }
    require(unexpected.empty() && missing.empty(),
            "the appended Chromium switch set must match the documented list exactly");
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
    expect_exact_switch_set(command_line, Corona::Systems::UI::cef_gpu_enabled());

    auto forced_accelerated = CefCommandLine::CreateCommandLine();
    forced_accelerated->InitFromString("corona_cef_app_tests.exe");
    Corona::Systems::UI::append_cef_command_line_switches(forced_accelerated, true);
    expect_common_switches(forced_accelerated);
    expect_gpu_enabled_switches(forced_accelerated);
    expect_exact_switch_set(forced_accelerated, true);

    auto forced_software = CefCommandLine::CreateCommandLine();
    forced_software->InitFromString("corona_cef_app_tests.exe");
    Corona::Systems::UI::append_cef_command_line_switches(forced_software, false);
    expect_common_switches(forced_software);
    expect_gpu_disabled_switches(forced_software);
    expect_exact_switch_set(forced_software, false);
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

void test_remote_debugging_port_policy() {
    using Corona::Systems::UI::resolve_remote_debugging_port;

    // The caller passes its build's default; both branches are asserted here so the
    // release/debug policy stays testable from a single build configuration.
    constexpr int kReleaseDefault = 0;
    constexpr int kDebugDefault = 9222;

    _putenv_s("CORONA_CEF_REMOTE_DEBUG_PORT", "");
    require(resolve_remote_debugging_port(kReleaseDefault) == 0,
            "release builds must not open a DevTools port by default");
    require(resolve_remote_debugging_port(kDebugDefault) == kDebugDefault,
            "debug builds must keep the historical 9222 default");

    _putenv_s("CORONA_CEF_REMOTE_DEBUG_PORT", "9333");
    require(resolve_remote_debugging_port(kReleaseDefault) == 9333,
            "an explicit port must be honoured in release builds too");
    require(resolve_remote_debugging_port(kDebugDefault) == 9333,
            "an explicit port must override the debug default");

    for (const char* disabled : {"0", "off", "false", "no", "OFF"}) {
        _putenv_s("CORONA_CEF_REMOTE_DEBUG_PORT", disabled);
        require(resolve_remote_debugging_port(kDebugDefault) == 0,
                "an explicit disable value must close the port");
    }

    for (const char* invalid : {"abc", "-1", "70000", "9222x", "65536", " "}) {
        _putenv_s("CORONA_CEF_REMOTE_DEBUG_PORT", invalid);
        require(resolve_remote_debugging_port(kDebugDefault) == 0,
                "an invalid value must fail closed instead of falling back to a port");
    }

    _putenv_s("CORONA_CEF_REMOTE_DEBUG_PORT", "65535");
    require(resolve_remote_debugging_port(kDebugDefault) == 65535,
            "the highest valid port must be accepted");

    _putenv_s("CORONA_CEF_REMOTE_DEBUG_PORT", "");
}

void test_remote_debugging_port_wiring() {
    using Corona::Systems::UI::apply_remote_debugging_port;
    using Corona::Systems::UI::cef_remote_debugging_port;

    // A correct policy is worthless if it never reaches CefSettings, so assert the wiring
    // rather than only the resolver.
    _putenv_s("CORONA_CEF_REMOTE_DEBUG_PORT", "9777");
    CefSettings explicit_settings;
    explicit_settings.remote_debugging_port = 12345;
    apply_remote_debugging_port(explicit_settings);
    require(explicit_settings.remote_debugging_port == 9777,
            "CefSettings must take the explicitly configured debugging port");

    _putenv_s("CORONA_CEF_REMOTE_DEBUG_PORT", "");
    CefSettings default_settings;
    default_settings.remote_debugging_port = 12345;
    apply_remote_debugging_port(default_settings);
    require(default_settings.remote_debugging_port == cef_remote_debugging_port(),
            "CefSettings must take the build default when no port is configured");
}

void test_osr_mode_resolution() {
    using Corona::Systems::UI::CefOsrMode;
    using Corona::Systems::UI::resolve_cef_osr_mode;

    // The default was flipped to GPU compositing after that path was measured (gpu-process CPU
    // ~5.77 -> ~0.64 cores) and visually verified. Resolution is a pure function so every case
    // stays assertable from one build, unlike the cached cef_osr_mode().
    require(resolve_cef_osr_mode(nullptr) == CefOsrMode::GpuSoftware,
            "an unset OSR mode must default to GPU compositing");
    require(resolve_cef_osr_mode("") == CefOsrMode::GpuSoftware,
            "a blank OSR mode must default to GPU compositing");

    require(resolve_cef_osr_mode("gpu") == CefOsrMode::GpuSoftware,
            "gpu must select GPU compositing");
    require(resolve_cef_osr_mode("1") == CefOsrMode::SharedTexture,
            "1 must select the shared-texture path");
    require(resolve_cef_osr_mode("software") == CefOsrMode::Software,
            "software must remain explicitly selectable so the previous path stays reachable");

    for (const char* disabled : {"0", "false", "off", "no", "FALSE", "Off"}) {
        require(resolve_cef_osr_mode(disabled) == CefOsrMode::Software,
                "an explicit disable value must select software compositing");
    }

    // Now that GPU is the default, an unrecognized value must fall back to the default rather
    // than silently dropping the user onto the slow path.
    require(resolve_cef_osr_mode("gpuu") == CefOsrMode::GpuSoftware,
            "an unrecognized value must fall back to the default, not to software");
    require(resolve_cef_osr_mode(" GPU ") == CefOsrMode::GpuSoftware,
            "surrounding whitespace must not change the selected mode");
}

int main(int argc, char* argv[]) {
    test_browser_process_dispatch(argc, argv);
    test_popup_wiring();
    test_message_router_config();
    test_process_per_site_toggle();
    test_app_contract();
    test_windowless_frame_rate_resolution();
    test_remote_debugging_port_policy();
    test_remote_debugging_port_wiring();
    test_osr_mode_resolution();
    return 0;
}
