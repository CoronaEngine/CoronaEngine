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

CefOsrMode cef_osr_mode() {
    static const CefOsrMode mode = [] {
        const char* value = std::getenv("CORONA_CEF_ACCELERATED_OSR");
        if (value == nullptr) {
            return CefOsrMode::Software;
        }
        if (std::strcmp(value, "1") == 0) {
            return CefOsrMode::SharedTexture;
        }
        if (std::strcmp(value, "gpu") == 0) {
            return CefOsrMode::GpuSoftware;
        }
        return CefOsrMode::Software;
    }();
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

}  // namespace Corona::Systems::UI
