#include "imgui/imgui_draw_packet.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void callback(const ImDrawList*, const ImDrawCmd*) {}
int main() {
    ImGui::CreateContext();
    try {
        ImDrawList first(ImGui::GetDrawListSharedData()), second(ImGui::GetDrawListSharedData());
        first.VtxBuffer.resize(65540);
        for (auto& vertex : first.VtxBuffer) vertex = {};
        first.IdxBuffer.resize(6);
        first.IdxBuffer[3] = 0; first.IdxBuffer[4] = 1; first.IdxBuffer[5] = 2;
        first.VtxBuffer[65536] = {{105, -45}, {0.25f, 0.75f}, IM_COL32(255, 128, 0, 64)};
        ImDrawCmd draw;
        draw.ElemCount = 3; draw.IdxOffset = 3; draw.VtxOffset = 65536;
        draw.ClipRect = {90, -60, 140, -20}; draw.TexRef = ImTextureRef(ImTextureID(7));
        first.CmdBuffer.push_back(draw);
        second.VtxBuffer.resize(3); second.IdxBuffer.resize(3);
        for (auto& vertex : second.VtxBuffer) vertex = {};
        for (auto& index : first.IdxBuffer) index = 0;
        for (auto& index : second.IdxBuffer) index = 0;
        draw.IdxOffset = draw.VtxOffset = 0;
        draw.TexRef = ImTextureRef(ImTextureID(9)); second.CmdBuffer.push_back(draw);
        draw.UserCallback = callback; second.CmdBuffer.push_back(draw);
        ImDrawData data;
        data.DisplayPos = {100, -50}; data.DisplaySize = {200, 100};
        data.CmdLists.push_back(&first); data.CmdLists.push_back(&second);
        data.CmdListsCount = 2; data.TotalVtxCount = 65543; data.TotalIdxCount = 9;
        auto packet = Corona::Systems::UI::build_imgui_draw_packet(data, 400, 150);
        require(packet.commands.size() == 3, "draw and callback order");
        require(packet.indices[3] == 0, "indices must not be rebased/truncated");
        require(packet.commands[0].vertex_offset == 65536, "large meshes use VtxOffset");
        require(packet.commands[0].first_index == 3, "IdxOffset");
        require(packet.commands[1].vertex_offset == 65540 && packet.commands[1].first_index == 6, "list bases");
        require(packet.commands[1].texture == 9, "texture identity");
        require(packet.commands[0].clip == std::array<float,4>{0,0,80,45}, "desktop offset and nonuniform framebuffer scale");
        require(std::abs(packet.translate[0] + 2.0f) < 0.0001f && std::abs(packet.translate[1]) < 0.0001f, "projection origin");
        require(std::abs(packet.vertices[65536].color[1] - 128.0f/255) < 0.0001f, "packed vertex color");
        require(packet.commands[2].source_command == &second.CmdBuffer[1], "borrow callback intact");
        first.CmdBuffer[0].ClipRect = {0, 0, 1, 1};
        packet = Corona::Systems::UI::build_imgui_draw_packet(data, 400, 150);
        require(packet.commands.size() == 2, "offscreen clipping");
        data.DisplaySize.x = 0;
        require(Corona::Systems::UI::build_imgui_draw_packet(data, 400, 150).commands.empty(), "minimized viewport");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ImGui::DestroyContext(); return 1;
    }
    ImGui::DestroyContext();
    std::cout << "ImGui draw packet tests passed\n";
}
