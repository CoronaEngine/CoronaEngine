#include "editor_platform_dialogs.h"

#include "editor_paths.h"
#include "editor_scene_access.h"
#include "native_editor_state.h"

#include <corona/diagnostics/thread_phase.h>

#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>

namespace Corona::Systems::UI {

namespace {

class ScopedComInitialization {
   public:
    ScopedComInitialization() {
#ifdef _WIN32
        result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    }

    ~ScopedComInitialization() {
#ifdef _WIN32
        if (result_ == S_OK || result_ == S_FALSE) {
            CoUninitialize();
        }
#endif
    }

    [[nodiscard]] bool available() const {
#ifdef _WIN32
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
#else
        return false;
#endif
    }

   private:
#ifdef _WIN32
    HRESULT result_{E_FAIL};
#endif
};

std::string compact_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &raw);
#else
    localtime_r(&raw, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d_%H%M%S");
    return out.str();
}

}  // namespace

std::optional<std::filesystem::path> show_native_file_dialog(
    const NativeFileDialogOptions& dialog_options) {
#ifdef _WIN32
    ScopedComInitialization com;
    if (!com.available()) {
        return std::nullopt;
    }

    IFileDialog* dialog = nullptr;
    const CLSID& dialog_class = dialog_options.mode == NativeFileDialogMode::SaveFile
                                    ? CLSID_FileSaveDialog
                                    : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(
            dialog_class,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        return std::nullopt;
    }

    FILEOPENDIALOGOPTIONS options{};
    HRESULT result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (dialog_options.mode == NativeFileDialogMode::PickFolder) {
            options |= FOS_PICKFOLDERS;
        } else if (dialog_options.mode == NativeFileDialogMode::OpenFile) {
            options |= FOS_FILEMUSTEXIST;
        } else {
            options |= FOS_OVERWRITEPROMPT;
        }
        result = dialog->SetOptions(options);
    }
    if (SUCCEEDED(result) && !dialog_options.title.empty()) {
        result = dialog->SetTitle(dialog_options.title.c_str());
    }
    std::vector<COMDLG_FILTERSPEC> native_filters;
    native_filters.reserve(dialog_options.filters.size());
    for (const auto& filter : dialog_options.filters) {
        native_filters.push_back({filter.name.c_str(), filter.pattern.c_str()});
    }
    if (SUCCEEDED(result) && !native_filters.empty() &&
        dialog_options.mode != NativeFileDialogMode::PickFolder) {
        result = dialog->SetFileTypes(
            static_cast<UINT>(native_filters.size()), native_filters.data());
    }
    if (SUCCEEDED(result) && !dialog_options.default_filename.empty()) {
        result = dialog->SetFileName(dialog_options.default_filename.c_str());
    }
    if (SUCCEEDED(result) && !dialog_options.default_extension.empty()) {
        result = dialog->SetDefaultExtension(dialog_options.default_extension.c_str());
    }

    std::error_code ec;
    auto initial_dir = dialog_options.default_path;
    if (std::filesystem::is_regular_file(initial_dir, ec)) {
        initial_dir = initial_dir.parent_path();
    }
    ec.clear();
    if (SUCCEEDED(result) && std::filesystem::is_directory(initial_dir, ec)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                initial_dir.c_str(),
                nullptr,
                IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    result = SUCCEEDED(result) ? dialog->Show(nullptr) : result;
    if (FAILED(result)) {
        dialog->Release();
        return std::nullopt;
    }

    IShellItem* selected = nullptr;
    result = dialog->GetResult(&selected);
    dialog->Release();
    if (FAILED(result) || selected == nullptr) {
        return std::nullopt;
    }

    PWSTR selected_path = nullptr;
    result = selected->GetDisplayName(SIGDN_FILESYSPATH, &selected_path);
    selected->Release();
    if (FAILED(result) || selected_path == nullptr) {
        return std::nullopt;
    }

    std::filesystem::path path{selected_path};
    CoTaskMemFree(selected_path);
    return path;
#else
    (void)dialog_options;
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> select_native_screenshot_path(const std::filesystem::path& project_root) {
    NativeFileDialogOptions options;
    options.mode = NativeFileDialogMode::SaveFile;
    options.default_path = project_root;
    options.title = L"保存截图";
    options.default_filename = L"screenshot_";
    const auto timestamp = compact_timestamp();
    options.default_filename.append(timestamp.begin(), timestamp.end());
    options.default_filename += L".png";
    options.default_extension = L"png";
    options.filters = {{L"PNG 图片", L"*.png"}};
    return show_native_file_dialog(options);
}

bool save_native_camera_screenshot(NativeEditorCamera& camera, const std::string& path) {
    camera.engine_camera->set_size(std::max(camera.width, 1), std::max(camera.height, 1));
    return camera.engine_camera->save_screenshot_sync(path);
}

SceneReply capture_native_screenshot(const SceneRequest& request) {
    SceneReply reply;
    const auto fail = [&](const char* code, const char* message) {
        reply.error_code = code; reply.error_message = message; return reply;
    };
    const auto& target = request.screenshot;
    if (target.scene.empty() || target.camera_id.empty() || !request.archive_session || request.archive_session->empty() ||
        (target.path && (target.path->empty() || target.path->find('\0') != std::string::npos)))
        return fail("INVALID_ARGUMENT", "Screenshot requires scene, camera and archive identity and a valid optional path");
    auto* scene = native_editor_state().scene.get();
    const auto same_scene = [&] {
        return scene && scene->archive_session == *request.archive_session && native_scene_request_matches(*scene, target.scene);
    };
    if (!same_scene()) return fail("PROJECT_CONTEXT_CHANGED", "Screenshot does not identify the active archive");
    const auto find_camera = [&]() -> NativeEditorCamera* {
        const auto found = std::find_if(scene->cameras.begin(), scene->cameras.end(), [&](const auto& camera) {
            return camera.camera_id == target.camera_id;
        });
        return found != scene->cameras.end() && found->engine_camera ? &*found : nullptr;
    };
    auto* camera = find_camera();
    if (!camera) return fail("CAMERA_NOT_FOUND", "Screenshot camera identity is unavailable");
    const auto handle = camera->engine_camera->get_handle();
    auto path = target.path;
    reply.write.screenshot = {"canceled", *request.archive_session, scene->route, target.camera_id, ""};
    if (!path) {
        const auto selected = select_native_screenshot_path(scene->project_root);
        // A modal dialog can pump native events: reacquire all scene/camera state.
        scene = native_editor_state().scene.get();
        if (!same_scene()) return fail("PROJECT_CONTEXT_CHANGED", "Archive changed while choosing a screenshot path");
        camera = find_camera();
        if (!camera || camera->engine_camera->get_handle() != handle)
            return fail("CAMERA_NOT_FOUND", "Screenshot camera changed while choosing a path");
        if (!selected) return reply;
        path = path_to_utf8(absolute_normalized_path(*selected));
    }
    if (!save_native_camera_screenshot(*camera, *path)) return fail("CAPTURE_FAILED", "Screenshot save failed");
    reply.write.screenshot.status = "success";
    reply.write.screenshot.path = *path;
    return reply;
}

}
  // namespace Corona::Systems::UI