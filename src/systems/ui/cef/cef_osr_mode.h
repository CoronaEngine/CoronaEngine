#pragma once

namespace Corona::Systems::UI {

enum class CefOsrMode {
    Software,
    SharedTexture,
    GpuSoftware,
};

CefOsrMode cef_osr_mode();

// Pure resolver behind cef_osr_mode(), so every input case stays unit-testable even though
// cef_osr_mode() caches its answer for the process lifetime.
// Unset/blank -> GPU compositing (the default as of 2026-10-06). "1" -> shared texture.
// "gpu" -> GPU compositing. "software"/"sw" and the disable values (0/false/off/no) ->
// software compositing. Anything unrecognized -> the default.
CefOsrMode resolve_cef_osr_mode(const char* value);

bool cef_gpu_enabled();

int resolve_windowless_frame_rate();

bool cef_process_per_site_enabled();

// DevTools is a local-trust backdoor: it exposes every native Editor API method (including
// arbitrary Python execution) to anything that can reach the port. `build_default_port`
// lets the caller pin its build's policy (0 disables, which is what CEF expects).
// Resolution order: CORONA_CEF_REMOTE_DEBUG_PORT -> build default. An unparseable or
// out-of-range value fails closed (0) rather than falling back to an open port.
int resolve_remote_debugging_port(int build_default_port);

// Same policy, using this build's default: closed in release, 9222 in debug.
int cef_remote_debugging_port();

}  // namespace Corona::Systems::UI
