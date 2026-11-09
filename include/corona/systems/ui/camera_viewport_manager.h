#pragma once

#include <corona/kernel/entity_id.h>
#include <corona/camera_command_authority.h>

#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Corona::Systems::UI {

struct CameraWindowState;

enum class DetachedWindowCloseAction {
    CloseTab,
    Redock,
};

[[nodiscard]] constexpr DetachedWindowCloseAction detached_window_close_action(
    bool camera_view) noexcept {
    return camera_view ? DetachedWindowCloseAction::CloseTab
                       : DetachedWindowCloseAction::Redock;
}

struct CameraViewportRecord {
    std::string scene_id;
    std::string camera_id;
    EntityId camera_handle{};
    int tab_id{-1};
    void* surface{};
    int x{120};
    int y{120};
    int width{960};
    int height{540};
    int render_width{960};
    int render_height{540};
    std::shared_ptr<CameraWindowState> window_state;
    std::shared_ptr<CameraCommandAuthority> camera_authority;
};

class CameraViewportManager {
   public:
    static CameraViewportManager& instance();

    bool register_view(std::string scene_id, std::string camera_id,
                       EntityId camera_handle, int tab_id,
                       std::shared_ptr<CameraWindowState> window_state = {},
                       std::shared_ptr<CameraCommandAuthority> camera_authority = {});
    bool bind_surface(int tab_id, void* surface, int x, int y, int width, int height);
    bool update_layout(int tab_id, int x, int y, int width, int height);
    bool unregister_view(int tab_id, bool preserve_open);

    [[nodiscard]] bool is_camera_view(int tab_id) const;
    [[nodiscard]] std::optional<CameraViewportRecord> find_by_tab(int tab_id) const;
    [[nodiscard]] std::optional<CameraViewportRecord> find_by_camera(
        const std::string& scene_id, const std::string& camera_id) const;
    [[nodiscard]] std::vector<int> tabs_for_scene(const std::string& scene_id) const;

   private:
    CameraViewportManager() = default;

    mutable std::mutex mutex_;
    std::unordered_map<int, CameraViewportRecord> views_;
};

}  // namespace Corona::Systems::UI
