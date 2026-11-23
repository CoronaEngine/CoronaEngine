#pragma once

// Local debug HUD. All presentation stays here; the frame runner only calls draw().
#include "cef/browser_manager.h"
#include "cef/cef_client.h"
#include <corona/shared_data_hub.h>
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string_view>

namespace Corona::Systems::UI::CameraTimingOverlay {

inline void draw(const BrowserTab& tab, ImVec2 origin, ImVec2 size) {
    // The HUD belongs to the editor's main viewport tab only; panels/camera windows
    // have their own surfaces (the docking_pos string was retired with ImGui docking).
    if (!tab.main_workspace || size.x <= 0 || size.y <= 0) return;
    auto camera = tab.web_camera_handle;
    // SPA navigation retains the tab's old camera handle. Only the main scene
    // route owns a viewport; launcher pages must never show that old telemetry.
    if (camera) {
        const auto browser = tab.client ? tab.client->GetBrowser() : nullptr;
        const auto frame = browser ? browser->GetMainFrame() : nullptr;
        const auto url = frame ? frame->GetURL().ToString() : std::string{};
        const auto hash = url.find('#');
        const auto route = hash == std::string::npos ? std::string_view{} :
            std::string_view(url).substr(hash + 1);
        if (route != "/" && !route.starts_with("/?")) camera = 0;
    }

    auto& hub = SharedDataHub::instance();
    auto timing = hub.camera_frame_timing(camera);
    bool supported = false;
    bool has_camera = false;
    bool pt_fallback = false;
    if (const auto* device = Corona::find_camera(camera)) {
        has_camera = true;
        // Native and diagnostic outputs do not publish render/denoise telemetry.
        supported = device->render_backend == CameraRenderBackend::Vision &&
            device->output_mode != CameraOutputMode::Normal &&
            device->output_mode != CameraOutputMode::WorldPosition &&
            device->output_mode != CameraOutputMode::ObjectID &&
            device->output_mode != CameraOutputMode::VisibilityBuffer;
        pt_fallback = supported && timing && device->output_mode == CameraOutputMode::FinalColor &&
            device->vision_render_mode == CameraVisionRenderMode::ReSTIR &&
            timing->mode == CameraVisionRenderMode::PathTracing;
        if (!supported || (timing && timing->mode != device->vision_render_mode && !pt_fallback)) timing.reset();
    } else {
        timing.reset();
    }
    const bool stale = timing && std::chrono::steady_clock::now() - timing->recorded_at >
        std::chrono::seconds(1);
    char values[5][48]{};
    for (auto& value : values) std::snprintf(value, sizeof(value), "--");
    const auto delta_ms = static_cast<double>(ImGui::GetIO().DeltaTime) * 1000.0;
    if (delta_ms > 0) {
        std::snprintf(values[0], sizeof(values[0]), "%.1f", 1000.0 / delta_ms);
        std::snprintf(values[1], sizeof(values[1]), "%.3f ms", delta_ms);
    }
    if (timing) {
        if (timing->frame_ms > 0) {
            std::snprintf(values[2], sizeof(values[2]), "%.3f ms", timing->frame_ms);
        }
        std::snprintf(values[3], sizeof(values[3]), "%.3f ms", timing->render_ms);
        std::snprintf(values[4], sizeof(values[4]), "%.3f ms", timing->denoise_ms);
    }

    constexpr const char* labels[] = {"UI FPS", "UI frame", "Camera frame", "Render total", "Denoise"};
    const char* title = !has_camera ? "TIMING / no camera" : !supported ? "TIMING / unavailable" : !timing ? "TIMING / waiting" :
        stale ? "TIMING / last sample" : pt_fallback ? "TIMING / PT fallback" : "TIMING / live";
    constexpr const char* footer = "Render total includes denoise";
    const float padding = 10.0f;
    const float line = ImGui::GetTextLineHeightWithSpacing();
    float width = ImGui::CalcTextSize(footer).x + padding * 2;
    for (int i = 0; i < 5; ++i)
        width = std::max(width, ImGui::CalcTextSize(labels[i]).x +
            ImGui::CalcTextSize(values[i]).x + padding * 4);
    const ImVec2 start(origin.x + std::max(padding, size.x - width - padding),
                       origin.y + padding + line * 2);
    const ImVec2 end(start.x + width, start.y + line * 7 + padding * 2);
    auto* draw_list = ImGui::GetForegroundDrawList(ImGui::GetMainViewport());
    draw_list->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    draw_list->AddRectFilled(start, end, IM_COL32(12, 16, 22, 215), 6.0f);
    draw_list->AddText(ImVec2(start.x + padding, start.y + padding),
        timing && !stale && !pt_fallback ? IM_COL32(110, 225, 180, 255) : IM_COL32(240, 190, 100, 255), title);
    for (int i = 0; i < 5; ++i) {
        const float y = start.y + padding + line * (i + 1);
        draw_list->AddText(ImVec2(start.x + padding, y), IM_COL32(205, 213, 223, 255), labels[i]);
        draw_list->AddText(ImVec2(end.x - padding - ImGui::CalcTextSize(values[i]).x, y),
            IM_COL32(245, 247, 250, 255), values[i]);
    }
    draw_list->AddText(ImVec2(start.x + padding, start.y + padding + line * 6),
        IM_COL32(150, 162, 178, 255), footer);
    draw_list->PopClipRect();
}

}  // namespace Corona::Systems::UI::CameraTimingOverlay
