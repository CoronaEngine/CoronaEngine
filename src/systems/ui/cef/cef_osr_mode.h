#pragma once

namespace Corona::Systems::UI {

enum class CefOsrMode {
    Software,
    SharedTexture,
    GpuSoftware,
};

CefOsrMode cef_osr_mode();

bool cef_gpu_enabled();

int resolve_windowless_frame_rate();

bool cef_process_per_site_enabled();

}  // namespace Corona::Systems::UI
