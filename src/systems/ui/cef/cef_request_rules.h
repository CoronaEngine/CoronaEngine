#pragma once

namespace Corona::Systems::UI::RequestRules {

/// close(): exactly one identity kind. `panelId` excludes the scene forms, a camera
/// identity needs both ids, and a bare scene is the plural (close every camera there).
/// The V8 reader owns the strings; this decides which shape the object described.
enum class CloseIdentity { None, Panel, Camera, Scene };

inline CloseIdentity close_identity(bool has_panel, bool has_scene, bool has_camera) {
    if (has_panel) return (has_scene || has_camera) ? CloseIdentity::None : CloseIdentity::Panel;
    if (!has_scene) return CloseIdentity::None;
    return has_camera ? CloseIdentity::Camera : CloseIdentity::Scene;
}

/// setWindowPresentation(): a state is either a mode or a size, never both. The main
/// window owns an OS window, not a camera surface, so it accepts a mode only and never
/// `maximized`.
enum class PresentationKind { None, Mode, Size };

inline PresentationKind presentation_kind(bool has_mode, bool has_size, bool main_target) {
    if (has_mode == has_size) return PresentationKind::None;
    const auto kind = has_mode ? PresentationKind::Mode : PresentationKind::Size;
    if (main_target && kind == PresentationKind::Size) return PresentationKind::None;
    return kind;
}

inline bool main_accepts_mode(bool maximized) { return !maximized; }

}  // namespace Corona::Systems::UI::RequestRules
