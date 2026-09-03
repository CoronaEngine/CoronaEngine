#pragma once

// quad_compositor.h is the self-contained base: it owns the GLSL pipeline includes,
// ViewportRenderResources, QuadDraw, and QuadCompositor. Include it (not the reverse) to
// avoid a circular dependency.
#include <corona/systems/ui/quad_compositor.h>
#include <corona/systems/ui/ui_surface_lifecycle.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Corona::Systems {

// VulkanBackend (post-ImGui, Phase 7): renders UI quads into one render target per surface
// and publishes each to the DisplaySystem via UIFrameReadyEvent. The main window is the
// surface registered at initialize(); secondary (detached) windows register/unregister at
// runtime. UI geometry is built as textured quads by the UI frame runner — there is no
// ImGui draw-data path, font atlas, or multi-viewport renderer callback.
//
// The QuadCompositor and the rasterizer pipeline are shared across all surfaces: UI
// rendering for every window happens on the same (UI) thread, sequentially, so a single
// pipeline + compositor instance is safe. Per-surface state (render target, geometry
// buffers, executor, image_handle, frame counter) lives in PerSurfaceRender.
class VulkanBackend {
   public:
    explicit VulkanBackend(SDL_Window* window);
    ~VulkanBackend() = default;

    bool initialize();
    void shutdown();

    // Register/unregister a surface (HWND/NSWindow*). The main window's surface is registered
    // automatically by initialize(). Secondary windows call register_surface() on detach and
    // unregister_surface() on re-dock. register_surface() allocates the per-surface image
    // handle; unregister_surface() releases it. Idempotent.
    bool register_surface(void* surface, SDL_Window* window);
    void unregister_surface(void* surface);
    [[nodiscard]] bool has_surface(void* surface) const;

    // GPU back-pressure for one surface: wait for DisplaySystem to finish consuming the
    // previous frame's image before rendering new UI content into it.
    void new_frame(void* surface);

    // Render the given quads into `surface`'s render target (via the shared QuadCompositor)
    // and mark its frame ready for present_surface(). target_pixels_{w,h} is the window's
    // physical client pixel size.
    void render_quads(void* surface, std::span<const QuadDraw> quads,
                      uint32_t target_pixels_w, uint32_t target_pixels_h);

    // Publish `surface`'s render target to DisplaySystem (UIFrameReadyEvent).
    void present_surface(void* surface);
    [[nodiscard]] bool first_present_ready(void* surface) const;

    // Recreate `surface`'s render target at the given pixel size (on window resize).
    void rebuild(void* surface, uint32_t pixel_w, uint32_t pixel_h);

    // Current render-target pixel size for a surface (0 if unknown / not yet created).
    [[nodiscard]] uint32_t surface_width(void* surface) const;
    [[nodiscard]] uint32_t surface_height(void* surface) const;

    // --- Main-window convenience (the surface registered at initialize()). ---
    [[nodiscard]] void* main_surface() const noexcept { return surface_; }
    [[nodiscard]] bool is_rebuild_needed() const noexcept { return rebuild_needed_; }
    void request_rebuild() noexcept { rebuild_needed_ = true; }

    // Ensure `resources` holds a render target of the given size, (re)creating it on
    // size change. Shared with the quad compositor.
    static bool ensure_render_target(ViewportRenderResources& resources, uint32_t width, uint32_t height,
                                     Horizon::ImageUsageFlags usage = Horizon::ImageUsage_Sampled);

   private:
    // Per-surface rendering + presentation state.
    struct PerSurfaceRender {
        ViewportRenderResources resources;
        std::uintptr_t image_handle = 0;
        uint64_t frame_index = 0;
        UI::SurfaceCompletionTicket first_present_ticket;
        bool first_present_published = false;
    };

    // Lazily create the shared quad pipeline (reuses the ui_quad.vert/frag GLSL).
    bool ensure_pipeline();

    [[nodiscard]] PerSurfaceRender* find_surface(void* surface);

    bool initialized_ = false;
    bool rebuild_needed_ = false;
    bool pipeline_ready_ = false;

    SDL_Window* window_ = nullptr;
    void* surface_ = nullptr;  // main window's surface

    // Per-surface state (main + secondary windows), keyed by native surface handle.
    // Held by unique_ptr: ViewportRenderResources owns move-only Horizon GPU resources, so
    // storing it by value in the map (which may move on rehash) is not viable.
    std::unordered_map<void*, std::unique_ptr<PerSurfaceRender>> surfaces_;

    // Shared across all surfaces (single-threaded UI rendering).
    QuadCompositor compositor_;
    std::optional<Horizon::RasterizerPipeline<ui_quad_vert_glsl_t, ui_quad_frag_glsl_t>> pipeline_;
};

}  // namespace Corona::Systems
