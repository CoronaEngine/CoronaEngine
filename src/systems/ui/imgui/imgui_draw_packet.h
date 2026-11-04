#pragma once

#include <imgui.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Corona::Systems::UI {

// CPU translation only. No Vulkan objects or engine state are accessed here.
struct ImGuiGpuVertex {
    float pos[2];
    float uv[2];
    float color[4];
};

struct ImGuiGpuCommand {
    std::uint32_t count = 0;
    std::uint32_t first_index = 0;
    std::int32_t vertex_offset = 0;
    ImTextureID texture = 0;
    std::array<float, 4> clip{};
    const ImDrawList* source_list = nullptr;
    const ImDrawCmd* source_command = nullptr;
};

struct ImGuiDrawPacket {
    std::array<float, 2> scale{};
    std::array<float, 2> translate{};
    std::vector<ImGuiGpuVertex> vertices;
    std::vector<ImDrawIdx> indices;
    std::vector<ImGuiGpuCommand> commands;
};

// Callback pointers are borrowed until the next ImGui NewFrame; consume synchronously.
[[nodiscard]] ImGuiDrawPacket build_imgui_draw_packet(const ImDrawData& data,
                                                     std::uint32_t pixel_width,
                                                     std::uint32_t pixel_height);

}  // namespace Corona::Systems::UI
