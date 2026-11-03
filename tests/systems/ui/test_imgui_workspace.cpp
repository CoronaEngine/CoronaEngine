#include "imgui/imgui_workspace.h"

#include <imgui_internal.h>

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace Corona::Systems::UI;

namespace {
constexpr ImTextureID kTransparentBrowser = 2;
constexpr ImGuiID kDockspace = 0x12345678;
ImGuiID right_dock = 0;
bool workspace_hovered = false;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Evaluate actual ImGui triangles at a pixel. The synthetic CEF image is fully
// transparent; font geometry is opaque here, so an unexpected host background
// cannot be hidden by a successful CPU layout assertion.
float alpha_at(const ImDrawData& data, ImVec2 p) {
    float alpha = 0;
    const auto edge = [](ImVec2 a, ImVec2 b, ImVec2 c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    for (const auto* list : data.CmdLists) {
        for (const auto& command : list->CmdBuffer) {
            if (command.UserCallback || command.GetTexID() == kTransparentBrowser ||
                p.x < command.ClipRect.x || p.y < command.ClipRect.y ||
                p.x >= command.ClipRect.z || p.y >= command.ClipRect.w) continue;
            for (unsigned int i = 0; i + 2 < command.ElemCount; i += 3) {
                const auto& a = list->VtxBuffer[command.VtxOffset + list->IdxBuffer[command.IdxOffset + i]];
                const auto& b = list->VtxBuffer[command.VtxOffset + list->IdxBuffer[command.IdxOffset + i + 1]];
                const auto& c = list->VtxBuffer[command.VtxOffset + list->IdxBuffer[command.IdxOffset + i + 2]];
                const float area = edge(a.pos, b.pos, c.pos);
                if (std::abs(area) < 0.0001f) continue;
                const float wa = edge(b.pos, c.pos, p) / area;
                const float wb = edge(c.pos, a.pos, p) / area;
                const float wc = 1 - wa - wb;
                if (wa < 0 || wb < 0 || wc < 0) continue;
                const float source_alpha =
                    (wa * ((a.col >> IM_COL32_A_SHIFT) & 255) +
                     wb * ((b.col >> IM_COL32_A_SHIFT) & 255) +
                     wc * ((c.col >> IM_COL32_A_SHIFT) & 255)) / 255.0f;
                alpha = source_alpha + alpha * (1 - source_alpha);
            }
        }
    }
    return alpha;
}

void frame(bool panel, bool floating = false) {
    ImGui::NewFrame();
    auto* viewport = ImGui::GetMainViewport();
    if (!ImGui::DockBuilderGetNode(kDockspace)) {
        ImGui::DockBuilderAddNode(kDockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(kDockspace, viewport->Size);
        auto center = kDockspace;
        right_dock = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.3f, nullptr, &center);
        // Reproduce the old saved layout: Workspace used to occupy the center.
        ImGui::DockBuilderDockWindow(kMainWorkspaceName, center);
        ImGui::DockBuilderFinish(kDockspace);
    }
    draw_editor_dockspace(kDockspace, viewport);
    const bool visible = begin_main_workspace();
    require(visible, "workspace must stay visible independently of docking");
    require(ImGui::GetWindowDockID() == 0, "saved docking must not capture the main workspace");
    require(ImGui::GetWindowViewport() == viewport, "workspace must stay on the main viewport");
    const auto origin = ImGui::GetCursorScreenPos();
    const auto size = ImGui::GetContentRegionAvail();
    require(origin.x == viewport->Pos.x && origin.y == viewport->Pos.y, "workspace origin must be the surface origin");
    require(size.x == viewport->Size.x && size.y == viewport->Size.y, "docking must not shrink the main browser/camera");
    ImGui::Image(kTransparentBrowser, size);
    workspace_hovered = ImGui::IsItemHovered();
    end_main_workspace();
    if (panel) {
        ImGui::SetNextWindowDockID(floating ? 0 : right_dock);
        if (floating) {
            ImGui::SetNextWindowPos(ImVec2(700, 120));
            ImGui::SetNextWindowSize(ImVec2(300, 400));
        }
        ImGui::Begin("Inspector");
        ImGui::Dummy(ImVec2(60, 60));
        ImGui::End();
    }
    ImGui::Render();
}

void settle(bool panel, bool floating = false) {
    for (int i = 0; i < 4; ++i) frame(panel, floating);
}
}

int main() {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(1);
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1;
    try {
        io.AddMousePosEvent(400, 350);
        settle(false);
        require(alpha_at(*ImGui::GetDrawData(), ImVec2(400, 350)) == 0, "empty workspace must reveal optics");
        require(workspace_hovered, "scene content must receive input through the empty dockspace");

        settle(true);
        require(alpha_at(*ImGui::GetDrawData(), ImVec2(400, 350)) == 0, "docked inspector must not cover the scene background");
        require(alpha_at(*ImGui::GetDrawData(), ImVec2(1100, 350)) > 0.9f, "actual panel backgrounds must remain opaque");
        io.AddMousePosEvent(1100, 350);
        settle(true);
        require(!workspace_hovered, "panel input must not fall through to the full-window browser");

        settle(true, true);
        require(alpha_at(*ImGui::GetDrawData(), ImVec2(400, 350)) == 0, "detaching must preserve transparent scene background");
        io.AddMousePosEvent(800, 350);
        settle(true, true);
        require(!workspace_hovered, "floating panel must remain above the workspace");

        io.DisplaySize = ImVec2(1600, 900);
        settle(true);
        require(alpha_at(*ImGui::GetDrawData(), ImVec2(400, 350)) == 0, "resizing and redocking must not fill the background");
        settle(false);
        require(alpha_at(*ImGui::GetDrawData(), ImVec2(800, 450)) == 0, "closing panels must reveal the full scene again");

        // A restored layout has no in-memory default_docks_ map. New left panels
        // must use a side leaf, including when previous empty side nodes vanished.
        ImGui::NewFrame();
        constexpr ImGuiID restored_dockspace = 0x12345679;
        ImGui::DockBuilderAddNode(restored_dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(restored_dockspace, io.DisplaySize);
        ImGuiID restored_center = restored_dockspace;
        const auto restored_right = ImGui::DockBuilderSplitNode(restored_center, ImGuiDir_Right, 0.3f, nullptr, &restored_center);
        ImGui::DockBuilderGetNode(restored_center)->LocalFlags |= ImGuiDockNodeFlags_CentralNode;
        ImGui::DockBuilderFinish(restored_dockspace);
        const auto recovered_right = resolve_default_panel_dock(restored_dockspace, "right");
        require(recovered_right == restored_right, "restored right dock must be reused");
        const auto left = resolve_default_panel_dock(restored_dockspace, "left");
        auto* left_node = ImGui::DockBuilderGetNode(left);
        require(left != restored_dockspace && left_node && !left_node->IsCentralNode(), "new controls must not fill the root/central dock");
        require(left_node->Size.x < io.DisplaySize.x * 0.5f, "new left dock must leave room for the camera background");
        require(resolve_default_panel_dock(restored_dockspace, "left") == left, "subsequent AI panels must reuse the left dock");
        ImGui::EndFrame();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ImGui::DestroyContext();
        return 1;
    }
    ImGui::DestroyContext();
    std::cout << "ImGui workspace background tests passed\n";
}
