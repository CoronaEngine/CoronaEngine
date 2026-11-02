#include "imgui_draw_packet.h"

#include <algorithm>
#include <cmath>

namespace Corona::Systems::UI {

ImGuiDrawPacket build_imgui_draw_packet(const ImDrawData& data,
                                       std::uint32_t pixel_width,
                                       std::uint32_t pixel_height) {
    static_assert(sizeof(ImDrawIdx) == 2, "Horizon currently binds uint16 indices; use VtxOffset for large meshes.");
    ImGuiDrawPacket packet;
    if (pixel_width == 0 || pixel_height == 0 ||
        !std::isfinite(data.DisplaySize.x) || !std::isfinite(data.DisplaySize.y) ||
        data.DisplaySize.x <= 0 || data.DisplaySize.y <= 0) return packet;

    packet.scale = {2.0f / data.DisplaySize.x, 2.0f / data.DisplaySize.y};
    packet.translate = {-1.0f - data.DisplayPos.x * packet.scale[0],
                        -1.0f - data.DisplayPos.y * packet.scale[1]};
    const float sx = static_cast<float>(pixel_width) / data.DisplaySize.x;
    const float sy = static_cast<float>(pixel_height) / data.DisplaySize.y;
    packet.vertices.reserve(std::max(data.TotalVtxCount, 0));
    packet.indices.reserve(std::max(data.TotalIdxCount, 0));
    for (const ImDrawList* list : data.CmdLists) {
        if (!list) continue;
        const auto vertex_base = static_cast<std::int32_t>(packet.vertices.size());
        const auto index_base = static_cast<std::uint32_t>(packet.indices.size());
        for (const ImDrawVert& v : list->VtxBuffer) {
            constexpr float unit = 1.0f / 255.0f;
            packet.vertices.push_back({{v.pos.x, v.pos.y}, {v.uv.x, v.uv.y},
                {((v.col >> IM_COL32_R_SHIFT) & 255) * unit,
                 ((v.col >> IM_COL32_G_SHIFT) & 255) * unit,
                 ((v.col >> IM_COL32_B_SHIFT) & 255) * unit,
                 ((v.col >> IM_COL32_A_SHIFT) & 255) * unit}});
        }
        if (list->IdxBuffer.Size > 0)
            packet.indices.insert(packet.indices.end(), list->IdxBuffer.begin(), list->IdxBuffer.end());
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback) {
                ImGuiGpuCommand callback;
                callback.source_list = list;
                callback.source_command = &command;
                packet.commands.push_back(callback);
                continue;
            }
            const std::array<float, 4> clip{
                std::clamp((command.ClipRect.x - data.DisplayPos.x) * sx, 0.0f, static_cast<float>(pixel_width)),
                std::clamp((command.ClipRect.y - data.DisplayPos.y) * sy, 0.0f, static_cast<float>(pixel_height)),
                std::clamp((command.ClipRect.z - data.DisplayPos.x) * sx, 0.0f, static_cast<float>(pixel_width)),
                std::clamp((command.ClipRect.w - data.DisplayPos.y) * sy, 0.0f, static_cast<float>(pixel_height))};
            if (command.ElemCount == 0 || clip[2] <= clip[0] || clip[3] <= clip[1]) continue;
            packet.commands.push_back({command.ElemCount, index_base + command.IdxOffset,
                vertex_base + static_cast<std::int32_t>(command.VtxOffset), command.GetTexID(), clip});
        }
    }
    return packet;
}

}  // namespace Corona::Systems::UI
