#pragma once

// The editor's platform edges: shell file dialogs and frame capture.
//
// Nothing here decides anything about the scene - it opens a native picker, reads a camera's
// frame, or writes a file. Keeping it apart means the scene service never links a dialog
// implementation and this unit never needs scene state beyond the camera it is handed.

#include "cef_scene_operations.h"
#include "native_editor_state.h"

#include <filesystem>
#include <optional>
#include <string>

namespace Corona::Systems::UI {

enum class NativeFileDialogMode {
    OpenFile,
    SaveFile,
    PickFolder,
};

struct NativeFileDialogFilter {
    std::wstring name;
    std::wstring pattern;
};

struct NativeFileDialogOptions {
    NativeFileDialogMode mode{NativeFileDialogMode::OpenFile};
    std::filesystem::path default_path;
    std::wstring title;
    std::wstring default_filename;
    std::wstring default_extension;
    std::vector<NativeFileDialogFilter> filters;
};

/// Native open/save dialog. Returns nothing when the user cancels, which is not an error.
std::optional<std::filesystem::path> show_native_file_dialog(
    const NativeFileDialogOptions& dialog_options);

/// Where a screenshot for this project should be written, or nothing when the user cancels.
std::optional<std::filesystem::path> select_native_screenshot_path(
    const std::filesystem::path& project_root);

/// Captures the camera's current frame into `path`; false when capture failed.
bool save_native_camera_screenshot(NativeEditorCamera& camera, const std::string& path);

/// The Screenshot scene operation: pick a path, capture, report.
SceneReply capture_native_screenshot(const SceneRequest& request);

}  // namespace Corona::Systems::UI
