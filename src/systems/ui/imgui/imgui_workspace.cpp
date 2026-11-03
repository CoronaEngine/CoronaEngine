#include "imgui_workspace.h"

#include <imgui_internal.h>
#include <limits>

namespace Corona::Systems::UI {

ImGuiID resolve_default_panel_dock(ImGuiID dockspace, std::string_view zone) {
    auto* root = ImGui::DockBuilderGetNode(dockspace);
    if (!root) return dockspace;
    ImGuiDockNode* central = nullptr;
    ImGuiDockNode* fallback = nullptr;
    ImGuiDockNode* side = nullptr;
    float best = std::numeric_limits<float>::max();
    const auto visit = [&](auto&& self, ImGuiDockNode* node) -> void {
        if (!node) return;
        if (node->IsSplitNode()) {
            self(self, node->ChildNodes[0]);
            self(self, node->ChildNodes[1]);
            return;
        }
        if (!fallback || node->Size.x * node->Size.y > fallback->Size.x * fallback->Size.y) fallback = node;
        if (node->IsCentralNode()) { central = node; return; }
        float score = std::numeric_limits<float>::max();
        if (zone == "left" && node->Pos.x + node->Size.x <= root->Pos.x + root->Size.x * 0.55f)
            score = node->Pos.x + node->Size.x * 0.5f - root->Pos.x;
        else if (zone == "right" && node->Pos.x >= root->Pos.x + root->Size.x * 0.45f)
            score = root->Pos.x + root->Size.x - node->Pos.x - node->Size.x * 0.5f;
        else if (zone == "bottom" && node->Pos.y >= root->Pos.y + root->Size.y * 0.5f)
            score = root->Pos.y + root->Size.y - node->Pos.y - node->Size.y * 0.5f;
        if (score < best) { best = score; side = node; }
    };
    visit(visit, root);
    if (side) return side->ID;
    auto* target = central ? central : fallback;
    if (!target) return dockspace;
    const ImGuiDir direction = zone == "left" ? ImGuiDir_Left : zone == "right" ? ImGuiDir_Right
        : zone == "bottom" ? ImGuiDir_Down : ImGuiDir_None;
    if (direction == ImGuiDir_None) return target->ID;
    const auto result = ImGui::DockBuilderSplitNode(target->ID, direction, zone == "left" ? 0.22f : 0.28f, nullptr, nullptr);
    ImGui::DockBuilderFinish(dockspace);
    return result;
}

void draw_editor_dockspace(ImGuiID id, const ImGuiViewport* viewport) {
    // PassthruCentralNode alone is insufficient: an occupied central node makes
    // ImGui paint the entire dockspace host, even if that window has NoBackground.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::DockSpaceOverViewport(id, viewport, ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::PopStyleColor();
}

bool begin_main_workspace() {
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowDockID(0);
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    const bool visible = ImGui::Begin(kMainWorkspaceName, nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoScrollWithMouse);
    // Keep both draw order and hit testing behind the dock host and its panels.
    // Clicking the scene must never raise the full-window CEF image over them.
    ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    return visible;
}

void end_main_workspace() {
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}  // namespace Corona::Systems::UI
