#include <corona/shared_data_hub.h>
#include <horizon/core/logging.h>
#include <corona/systems/ui/camera_viewport_manager.h>

#include <algorithm>

namespace Corona::Systems::UI {

namespace {

void enqueue_update(const CameraViewportRecord& record, bool view_open) {
    SharedDataHub::instance().enqueue_camera_viewport_update({
        .camera_handle = record.camera_handle,
        .surface = record.surface,
        .view_open = view_open,
        .x = record.x,
        .y = record.y,
        .width = record.width,
        .height = record.height,
        .render_width = record.render_width,
        .render_height = record.render_height,
    });
}

}  // namespace

CameraViewportManager& CameraViewportManager::instance() {
    static CameraViewportManager manager;
    return manager;
}

bool CameraViewportManager::register_view(std::string scene_id, std::string camera_id,
                                          std::uintptr_t camera_handle, int tab_id) {
    if (scene_id.empty() || camera_id.empty() || camera_handle == 0 || tab_id < 0) {
        return false;
    }

    CameraViewportRecord record{
        .scene_id = std::move(scene_id),
        .camera_id = std::move(camera_id),
        .camera_handle = camera_handle,
        .tab_id = tab_id,
    };
    {
        std::lock_guard lock(mutex_);
        views_[tab_id] = record;
    }
    CFW_LOG_INFO("CameraViewport: registered tab={} camera={} scene={} handle={}",
                 tab_id, camera_id, record.scene_id, camera_handle);
    return true;
}

bool CameraViewportManager::bind_surface(int tab_id, void* surface, int x, int y,
                                         int width, int height) {
    CameraViewportRecord record;
    const int safe_width = std::max(width, 1);
    const int safe_height = std::max(height, 1);
    {
        std::lock_guard lock(mutex_);
        auto it = views_.find(tab_id);
        if (it == views_.end()) {
            return false;
        }
        if (it->second.surface == surface &&
            it->second.x == x && it->second.y == y &&
            it->second.render_width == safe_width && it->second.render_height == safe_height) {
            return true;
        }
        it->second.surface = surface;
        it->second.x = x;
        it->second.y = y;
        it->second.render_width = safe_width;
        it->second.render_height = safe_height;
        record = it->second;
    }

    if (record.surface) {
        enqueue_update(record, true);
        CFW_LOG_INFO("CameraViewport: bound tab={} handle={} surface={} rect={}x{}+{},{} render={}x{}",
                     tab_id, record.camera_handle, record.surface, record.width, record.height,
                     record.x, record.y, record.render_width, record.render_height);
    } else {
        enqueue_update(record, false);
        CFW_LOG_INFO("CameraViewport: unbound tab={} handle={} (surface retired)",
                     tab_id, record.camera_handle);
    }
    return true;
}

bool CameraViewportManager::update_layout(int tab_id, int x, int y, int width, int height) {
    CameraViewportRecord record;
    const int safe_width = std::max(width, 1);
    const int safe_height = std::max(height, 1);
    {
        std::lock_guard lock(mutex_);
        auto it = views_.find(tab_id);
        if (it == views_.end()) {
            return false;
        }
        if (it->second.x == x && it->second.y == y &&
            it->second.width == safe_width && it->second.height == safe_height) {
            return true;
        }
        it->second.x = x;
        it->second.y = y;
        it->second.width = safe_width;
        it->second.height = safe_height;
        record = it->second;
    }

    if (record.surface) {
        enqueue_update(record, true);
    }
    return true;
}

bool CameraViewportManager::unregister_view(int tab_id, bool preserve_open) {
    CameraViewportRecord record;
    {
        std::lock_guard lock(mutex_);
        auto it = views_.find(tab_id);
        if (it == views_.end()) {
            return false;
        }
        record = std::move(it->second);
        views_.erase(it);
    }

    record.surface = nullptr;
    enqueue_update(record, preserve_open);
    return true;
}

bool CameraViewportManager::is_camera_view(int tab_id) const {
    std::lock_guard lock(mutex_);
    return views_.contains(tab_id);
}

std::optional<CameraViewportRecord> CameraViewportManager::find_by_tab(int tab_id) const {
    std::lock_guard lock(mutex_);
    auto it = views_.find(tab_id);
    return it == views_.end() ? std::nullopt : std::optional{it->second};
}

std::optional<CameraViewportRecord> CameraViewportManager::find_by_camera(
    const std::string& scene_id, const std::string& camera_id) const {
    std::lock_guard lock(mutex_);
    for (const auto& [_, view] : views_) {
        if (view.scene_id == scene_id && view.camera_id == camera_id) {
            return view;
        }
    }
    return std::nullopt;
}

std::vector<int> CameraViewportManager::tabs_for_scene(const std::string& scene_id) const {
    std::vector<int> tabs;
    std::lock_guard lock(mutex_);
    for (const auto& [tab_id, view] : views_) {
        if (view.scene_id == scene_id) {
            tabs.push_back(tab_id);
        }
    }
    return tabs;
}

}  // namespace Corona::Systems::UI
