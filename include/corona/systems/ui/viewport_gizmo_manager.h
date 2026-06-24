#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

namespace Corona::Systems::UI {

class ViewportGizmoManager {
   public:
    struct SelectionEvent {
        std::string scene_id;
        std::uintptr_t camera_handle{};
        std::uintptr_t actor_handle{};
    };

    struct TransformEvent {
        std::string scene_id;
        std::uintptr_t camera_handle{};
        std::uintptr_t actor_handle{};
        float position[3]{};
        float rotation[3]{};
        float scale[3]{1.0f, 1.0f, 1.0f};
    };

    enum class Mode {
        Move,
        Scale,
        Rotate,
    };

    static ViewportGizmoManager& instance();

    void set_mode(const std::string& mode);
    void clear_selection();
    void clear_camera(std::uintptr_t camera_handle);
    void set_selection(std::string scene_id,
                       std::uintptr_t camera_handle,
                       std::uintptr_t actor_handle);

    [[nodiscard]] std::uintptr_t selected_camera_handle() const;
    [[nodiscard]] std::vector<SelectionEvent> drain_selection_events();
    [[nodiscard]] std::vector<TransformEvent> drain_transform_events();

    [[nodiscard]] bool render(const std::string& scene_id,
                              std::uintptr_t camera_handle,
                              const ImVec2& origin,
                              const ImVec2& size,
                              ImDrawList* draw_list);

   private:
    struct Selection {
        std::string scene_id;
        std::uintptr_t camera_handle{};
        std::uintptr_t actor_handle{};
    };

    struct PendingPick {
        std::string request_id;
        std::string scene_id;
        std::uintptr_t camera_handle{};
        std::uintptr_t pick_handle{};
        bool active{false};
    };

    ViewportGizmoManager() = default;

    [[nodiscard]] bool start_pick_locked(const std::string& scene_id,
                                         std::uintptr_t camera_handle,
                                         const ImVec2& origin,
                                         const ImVec2& size,
                                         const ImVec2& mouse_pos);
    void consume_pick_result_locked();

    mutable std::mutex mutex_;
    Mode mode_{Mode::Move};
    std::uintptr_t primary_camera_handle_{};
    std::unordered_map<std::uintptr_t, Selection> selections_;
    std::unordered_map<std::uintptr_t, PendingPick> pending_picks_;
    std::uint64_t next_pick_sequence_{0};
    std::vector<SelectionEvent> selection_events_;
    std::vector<TransformEvent> transform_events_;
};

}  // namespace Corona::Systems::UI
