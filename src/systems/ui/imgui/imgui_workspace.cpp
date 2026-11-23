#include "imgui_workspace.h"

#include <imgui_internal.h>

namespace Corona::Systems::UI {

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
