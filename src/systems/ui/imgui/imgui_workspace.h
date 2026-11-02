#pragma once

#include <imgui.h>

namespace Corona::Systems::UI {

inline constexpr const char* kMainWorkspaceName = "Workspace###Corona.Workspace";

// The dockspace is an overlay. Its host must not fill the scene with WindowBg,
// even when a saved layout contains a window in the central node.
void draw_editor_dockspace(ImGuiID id, const ImGuiViewport* viewport);

// A full-surface, transparent CEF host behind all docked/floating panels. It is
// deliberately outside the docking tree, including when restoring an old ini.
bool begin_main_workspace();
void end_main_workspace();

}  // namespace Corona::Systems::UI
