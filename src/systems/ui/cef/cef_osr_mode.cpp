#include "cef_osr_mode.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace Corona::Systems::UI {

namespace {

bool is_disabled_value(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    std::string text(value);
    for (char& character : text) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return text == "0" || text == "false" || text == "off" || text == "no";
}

}  // namespace

CefOsrMode resolve_cef_osr_mode(const char* value) {
    // GPU compositing is the default as of 2026-10-06. It was measured on the build machine at
    // roughly a tenth of the software path's CPU (gpu-process ~5.77 -> ~0.64 cores, see
    // docs/development/cef-osr-memory-staged-plan.md and the CPU table recorded in
    // docs/development/cef-ui-risk-optimization-tasks.md) and its rendering was verified against
    // the software path, including translucent panels over the StartScreen starfield.
    if (value == nullptr || value[0] == '\0') {
        return CefOsrMode::GpuSoftware;
    }
    if (is_disabled_value(value)) {
        return CefOsrMode::Software;
    }

    // Trim surrounding whitespace so " gpu " behaves like "gpu".
    std::string text(value);
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return CefOsrMode::GpuSoftware;
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1);

    if (text == "1") {
        return CefOsrMode::SharedTexture;
    }
    if (text == "gpu") {
        return CefOsrMode::GpuSoftware;
    }
    if (text == "software" || text == "sw") {
        return CefOsrMode::Software;
    }

    // Unrecognized values fall back to the default rather than silently dropping the user onto
    // the slower software path.
    return CefOsrMode::GpuSoftware;
}

CefOsrMode cef_osr_mode() {
    static const CefOsrMode mode =
        resolve_cef_osr_mode(std::getenv("CORONA_CEF_ACCELERATED_OSR"));
    return mode;
}

bool cef_gpu_enabled() {
    return cef_osr_mode() != CefOsrMode::Software;
}

int resolve_windowless_frame_rate() {
    constexpr int kDefaultFrameRate = 60;
    constexpr int kMaxFrameRate = 240;

    const char* value = std::getenv("CORONA_CEF_FRAME_RATE");
    if (value == nullptr || value[0] == '\0') {
        return kDefaultFrameRate;
    }

    int rate = 0;
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return kDefaultFrameRate;
        }
        rate = rate * 10 + (*p - '0');
        if (rate > kMaxFrameRate) {
            rate = kMaxFrameRate;
        }
    }

    return rate > 0 ? rate : kDefaultFrameRate;
}

bool cef_process_per_site_enabled() {
    return !is_disabled_value(std::getenv("CORONA_CEF_PROCESS_PER_SITE"));
}

int resolve_remote_debugging_port(int build_default_port) {
    const char* value = std::getenv("CORONA_CEF_REMOTE_DEBUG_PORT");
    if (value == nullptr || value[0] == '\0') {
        return build_default_port;
    }
    if (is_disabled_value(value)) {
        return 0;
    }

    constexpr int kMaxPort = 65535;
    int port = 0;
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return 0;  // Fail closed: a malformed value must not open a port.
        }
        port = port * 10 + (*p - '0');
        if (port > kMaxPort) {
            return 0;
        }
    }
    return port > 0 ? port : 0;
}

int cef_remote_debugging_port() {
#ifdef NDEBUG
    // Release: no debugging endpoint unless the env var opts in explicitly.
    constexpr int kBuildDefaultPort = 0;
#else
    // Debug: keep the historical DevTools port.
    constexpr int kBuildDefaultPort = 9222;
#endif
    return resolve_remote_debugging_port(kBuildDefaultPort);
}

}  // namespace Corona::Systems::UI
