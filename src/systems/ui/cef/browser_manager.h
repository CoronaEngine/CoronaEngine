#pragma once

#include "horizon.h"
#include <SDL3/SDL.h>
#include <include/internal/cef_types.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Corona::Systems {
class VulkanBackend;
}

namespace Corona::Systems::UI {
class OffscreenCefClient;

// ImGui-free texture identifier (Phase 6 of the ImGui-removal plan). This carries the
// same value the ImGui path used to store in ImTextureID: a bindless sampled-image
// descriptor index biased by +1 (so 0 means "no texture"). The encode/decode lives in
// browser_manager_vulkan.cpp; nothing here depends on ImGui anymore.
using UiTextureId = std::uint64_t;

inline constexpr UiTextureId k_invalid_texture_id = 0;

inline bool is_valid_texture_id(const UiTextureId texture_id) {
    return texture_id != k_invalid_texture_id;
}

// Plain 2D point (replaces ImVec2 for drag bookkeeping).
struct PointF {
    float x = 0.0f;
    float y = 0.0f;
};

struct DragRegion {
    float x, y, width, height;
};

// ============================================================================
// 浏览器标签页数据结构
// ============================================================================

struct BrowserTab {
    int tab_id = -1;
    std::string name;
    std::string url;

    OffscreenCefClient* client = nullptr;
    // VkDescriptorSet texture_id = VK_NULL_HANDLE;
    UiTextureId texture_id = k_invalid_texture_id;

    int width = 800;
    int height = 600;

    // Docking 相关属性
    std::string docking_pos;        // 位置: "left", "right", "top", "bottom", "center"
    int dock_width = 0;             // 指定宽度，0 表示自动
    int dock_height = 0;            // 指定高度，0 表示自动
    bool dock_fixed = false;        // 是否固定位置
    bool dock_initialized = false;  // 是否已初始化 docking
    // Phase 10: in-main-window floating panel (popped-out). Positioned by initial_x/y and
    // draggable by its title bar within the main window. Distinct from host_surface detach
    // (own OS window), which is disabled pending the multi-surface fix.
    bool floating = false;
    // Higher values are composed and hit-tested above ordinary floating panels.
    int z_priority = 0;

    bool open = true;
    bool minimized = false;  // 新增：是否最小化
    bool needs_resize = false;
    bool needs_reposition = false;
    bool buffer_dirty = false;
    bool has_focus = false;
    bool camera_view = false;
    bool cef_creation_failed = false;
    bool transparent_overlay = false;
    std::atomic_bool hide_system_cursor{false};
    std::atomic_bool use_custom_system_cursor{false};
    bool preserve_camera_open_on_close = false;
    SDL_WindowID platform_window_id = 0;
    void* platform_handle_raw = nullptr;
    int initial_x = 120;
    int initial_y = 120;

    // Phase 7 (detach / re-dock): the surface this tab is rendered into.
    //   nullptr  ⇒ docked in the main window (composed into the main layout)
    //   non-null ⇒ detached, rendered full-bleed into its own secondary OS window (this is
    //             that window's native surface handle / DisplaySystem key).
    // host_surface is owned/driven by the UI thread only (frame runner + DockCommand tasks).
    void* host_surface = nullptr;
    // Detach lifecycle state. Only the UI thread mutates it; guards against double-detach and
    // detach-during-teardown (ABA). Docked is the initial/redocked state.
    enum class DetachState { Docked, Detaching, Detached, Redocking };
    DetachState detach_state = DetachState::Docked;
    // Desired secondary-window geometry (logical px), filled by detachPanel and consumed by
    // the frame runner's reconcile step when it creates the OS window.
    int detach_x = 120;
    int detach_y = 120;
    int detach_w = 640;
    int detach_h = 480;
    bool detach_maximized = false;

    char url_buffer[1024] = "";
    std::vector<uint8_t> pixel_buffer;
    std::mutex mutex;  // 保护 pixel_buffer 和 buffer_dirty

    std::vector<DragRegion> drag_regions;
    bool drag_pending = false;
    PointF drag_pending_start_pos;
    bool dragging_window = false;
    std::mutex drag_mutex;  // 确保线程安全
};

// ============================================================================
// 浏览器标签管理器
// ============================================================================

class BrowserManager {
   public:
    static BrowserManager& instance();

    int create_tab(const std::string& url, const std::string& path = "",
                   const std::string& docking_pos = "",
                   int dock_width = 0, int dock_height = 0,
                   bool dock_fixed = false);
    int create_tab(const std::string& url, const std::string& path,
                   const std::string& docking_pos, int dock_width, int dock_height,
                   bool dock_fixed, bool camera_view, int initial_x, int initial_y);
    BrowserTab* get_tab(int tab_id);
    void remove_tab(int tab_id);
    void update_texture(int tab_id);
    void resize_tab(int tab_id, int width, int height);
    [[nodiscard]] const Horizon::HardwareImage* get_texture_image(UiTextureId texture_id) const;
    [[nodiscard]] Horizon::SubmitReceipt get_texture_upload_receipt(UiTextureId texture_id) const;
    void wait_for_texture_upload(UiTextureId texture_id);

    // 隐藏标签页（最小化）
    bool hide_tab(int tab_id, bool if_close = false);
    // 显示标签页（恢复）
    bool show_tab(int tab_id);

    // void set_vulkan_backend(VulkanBackend* backend);
    // VulkanBackend* get_vulkan_backend() const;

    [[nodiscard]] const std::unordered_map<int, std::unique_ptr<BrowserTab>>& get_tabs() const;
    std::unordered_map<int, std::unique_ptr<BrowserTab>>& get_tabs();

    void update();
    void close_all_tabs();
    void enqueue_main_thread_task(std::function<void()> task);

    // 设置主窗口指针
    void set_main_window(SDL_Window* window) { main_window_ = window; }
    void set_tab_drag_regions(int tab_id, const std::vector<DragRegion>& regions);

   private:
    BrowserManager() = default;
    SDL_Window* main_window_ = nullptr;
    UiTextureId create_browser_texture(int width, int height);
    void destroy_tab_texture(BrowserTab* tab);
    void retire_deferred_tab_textures(bool force = false);

    struct OwnedImage {
        Horizon::HardwareImage image;
        Horizon::SubmitReceipt upload_receipt;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct DeferredTextureDestroy {
        OwnedImage image;
        uint64_t queued_frame = 0;
    };

    std::unordered_map<int, std::unique_ptr<BrowserTab>> tabs_;
    std::mutex pending_tasks_mutex_;
    std::vector<std::function<void()>> pending_tasks_;
    std::unordered_map<UiTextureId, OwnedImage> owned_images_;
    Horizon::HardwareExecutor browser_upload_executor_;
    std::vector<DeferredTextureDestroy> deferred_texture_destroys_;
    uint64_t frame_index_ = 0;
    int tab_counter_ = 0;

};

}  // namespace Corona::Systems::UI
