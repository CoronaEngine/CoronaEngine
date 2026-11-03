#pragma once

#include <imgui.h>
#include <string_view>

namespace Corona::Systems::UI {

inline constexpr const char* kMainWorkspaceName = "Workspace###Corona.Workspace";

// The dockspace is an overlay. Its host must not fill the scene with WindowBg,
// even when a saved layout contains a window in the central node.
void draw_editor_dockspace(ImGuiID id, const ImGuiViewport* viewport);

// Locate a side leaf in a restored layout; never put a new side panel in the
// full-window root/central node merely because the transient default map is empty.
ImGuiID resolve_default_panel_dock(ImGuiID dockspace, std::string_view zone);

// A full-surface, transparent CEF host behind all docked/floating panels. It is
// deliberately outside the docking tree, including when restoring an old ini.
bool begin_main_workspace();
void end_main_workspace();

}  // namespace Corona::Systems::UI
