#pragma once

#include <corona/kernel/entity_id.h>
#include "camera_window_lifetime.h"

#include "horizon.h"
#include <SDL3/SDL.h>
#include <include/internal/cef_types.h>
#include <include/internal/cef_types_wrappers.h>

#include "popup_overlay.h"

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

// ============================================================================
// 浏览器标签页数据结构
// ============================================================================

struct BrowserTab {
    int tab_id = -1;
    std::string name;
    std::string url;
    std::string panel_id; // Stable ImGui identity, independent of the current browser id.
    std::string panel_title;
    bool cef_visible = true;
    // Published on the UI thread, read under mutex by CEF callbacks. CEF renders
    // at logical resolution; screen coordinates are physical desktop pixels.
    int content_screen_x = 0;
    int content_screen_y = 0;
    float content_screen_scale = 1.0f;
    CefRect ime_caret{0, 0, 1, 18};
    // Last Web-local camera rectangle; composed with the ImGui content origin each frame.
    EntityId web_camera_handle = 0;
    std::shared_ptr<Corona::CameraCommandAuthority> web_camera_authority;
    int web_camera_x = 0, web_camera_y = 0;
    int web_camera_width = 0, web_camera_height = 0;
    int web_camera_render_width = 0, web_camera_render_height = 0;

    OffscreenCefClient* client = nullptr;
    // VkDescriptorSet texture_id = VK_NULL_HANDLE;
    UiTextureId texture_id = k_invalid_texture_id;

    int width = 800;
    int height = 600;

    // ImGui owns placement: a panel is one ImGui window, the dockspace remembers where it
    // sits, and the user docks it. Native keeps only what ImGui cannot know:
    //   main_workspace ⇒ the whole-window editor surface (never a dockable panel)
    //   camera_view    ⇒ a camera surface in its own OS window (never merged into a dock)
    //   initial_x/y    ⇒ first-use position for a window that opens floating
    bool main_workspace = false;
    bool camera_view = false;
    // Set by FocusTab, consumed by the next ImGui frame.
    bool native_focus_requested = false;
    int initial_x = 120;
    int initial_y = 120;
    // First-use size/layout already applied once.
    bool laid_out = false;

    bool open = true;
    bool buffer_dirty = false;
    // Dimensions of the buffer currently sitting in `pixel_buffer`, as delivered by CEF's
    // OnPaint. They are NOT always width/height: a paint issued before a resize can still arrive
    // after it. Uploading such a buffer into the resized texture copies it as if its row length
    // were the texture's width, which shifts every row and shears the panel content, so the
    // upload path compares these against the texture and refuses the mismatch.
    int paint_width = 0;
    int paint_height = 0;
    bool has_focus = false;
    bool cef_creation_failed = false;
    bool transparent_overlay = false;
    std::atomic_bool hide_system_cursor{false};
    std::uint64_t viewport_pointer_owner = 0;
    std::uintptr_t viewport_pointer_camera = 0;
    std::weak_ptr<CameraWindowAuthority> viewport_pointer_authority;
    bool preserve_camera_open_on_close = false;
    SDL_WindowID platform_window_id = 0;
    void* platform_handle_raw = nullptr;

    // The surface ImGui renders this tab into:
    //   nullptr  ⇒ docked in the main window (composed into the main viewport)
    //   non-null ⇒ its own OS window (a camera surface, or a panel the user undocked)

    char url_buffer[1024] = "";
    // Last uncomposited PET_VIEW. Retained after upload so popup-only updates
    // and dismissal can publish immediately without waiting for a new view paint.
    std::vector<uint8_t> pixel_buffer;
    PopupOverlay popup;
    std::mutex mutex;
};

// ============================================================================
// 浏览器标签管理器
// ============================================================================

// What a tab is for. ImGui decides placement; this only tells the compositor which kind of
// surface it is.
enum class TabKind { Panel, CameraView, MainWorkspace };

class BrowserManager {
   public:
    static BrowserManager& instance();

    /// Creates one CEF document. Panels open as ImGui windows the user docks; camera views
    /// and the main workspace are the two surfaces native has to place itself.
    int create_tab(const std::string& url, const std::string& path = "",
                   TabKind kind = TabKind::Panel, int width = 0, int height = 0,
                   int initial_x = 120, int initial_y = 120);
    BrowserTab* get_tab(int tab_id);
    /// Stable panel identity (the id createPanel was given), not the transient tab id.
    BrowserTab* find_tab_by_panel_id(const std::string& panel_id);
    void remove_tab(int tab_id);
    void update_texture(int tab_id);
    void resize_tab(int tab_id, int width, int height);
    [[nodiscard]] const Horizon::HardwareImage* get_texture_image(UiTextureId texture_id) const;
    [[nodiscard]] Horizon::SubmitReceipt get_texture_upload_receipt(UiTextureId texture_id) const;
    void wait_for_texture_upload(UiTextureId texture_id);

    // void set_vulkan_backend(VulkanBackend* backend);
    // VulkanBackend* get_vulkan_backend() const;

    /**
     * @brief 标签页快照（id + 裸指针），供调用方在锁外遍历
     *
     * tabs_ 同时被引擎主线程（create_tab / remove_tab / update）与 CEF UI 线程
     * （输入与事件派发里的多处 range-for）访问。此前直接返回 map 的**引用**，等于让
     * CEF 线程拿着迭代器/元素指针去撞主线程的 erase —— 迭代器失效、悬垂的
     * BrowserTab* / OffscreenCefClient*。
     *
     * @note 返回的 BrowserTab* 只在对应标签页未被 remove_tab 期间有效；访问其内容仍
     *       需该 tab 自己的锁（paint 用 tab->mutex）。
     */
    struct TabSnapshotEntry {
        int tab_id = 0;
        BrowserTab* tab = nullptr;
    };
    [[nodiscard]] std::vector<TabSnapshotEntry> tabs_snapshot() const;

    void update();
    void close_all_tabs();
    void enqueue_main_thread_task(std::function<void()> task);

    // 设置主窗口指针
    void set_main_window(SDL_Window* window) { main_window_ = window; }
    [[nodiscard]] SDL_Window* main_window() const { return main_window_; }

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
    /// 保护 tabs_ 与 tab_counter_（见 tabs_snapshot 的说明）。
    /// 锁序：tabs_mutex_ → tab->mutex；绝不反向。
    mutable std::mutex tabs_mutex_;
    std::mutex pending_tasks_mutex_;
    std::vector<std::function<void()>> pending_tasks_;
    std::unordered_map<UiTextureId, OwnedImage> owned_images_;
    Horizon::HardwareExecutor browser_upload_executor_;
    std::vector<DeferredTextureDestroy> deferred_texture_destroys_;
    uint64_t frame_index_ = 0;
    int tab_counter_ = 0;

};

}  // namespace Corona::Systems::UI
