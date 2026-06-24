#pragma once

#include "horizon.h"
#include <corona/shader_include.h>
// clang-format off
#include GLSL(../../../assets/shaders/imgui.vert.glsl)
#include GLSL(../../../assets/shaders/imgui.frag.glsl)
// clang-format on
#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Corona::Systems {

// ============================================================================
// Per-viewport rendering resources (render target, executor, geometry buffers).
// Used by both the main viewport and secondary (dragged-out) viewports.
// ============================================================================
struct ViewportRenderResources {
    Horizon::HardwareImage render_target;
    Horizon::HardwareExecutor executor;
    Horizon::HardwareBuffer vertex_buffer;
    Horizon::HardwareBuffer index_buffer;
    size_t vertex_buffer_capacity = 0;
    size_t index_buffer_capacity = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool frame_ready = false;
};

class VulkanBackend {
   public:
    explicit VulkanBackend(SDL_Window* window);
    ~VulkanBackend() = default;

    bool initialize();
    void shutdown();

    // Must be called AFTER ImGui::CreateContext() + ImGui_ImplSDL3_InitForOther().
    // Registers renderer viewport callbacks and enables RendererHasViewports.
    void register_viewport_callbacks();

    void new_frame();
    void render_frame(ImDrawData* draw_data);
    void present_frame();

    [[nodiscard]] bool is_rebuild_needed() const noexcept { return rebuild_needed_; }
    void request_rebuild() noexcept { rebuild_needed_ = true; }
    void rebuild(int width, int height);

    // Render ImDrawData into arbitrary per-viewport resources.
    // Shared pipeline and font atlas are provided externally.
    // Returns true if draw commands were recorded and submitted.
    static bool render_draw_data(
        ImDrawData* draw_data,
        ViewportRenderResources& resources,
        Horizon::RasterizerPipeline<imgui_vert_glsl_t, imgui_frag_glsl_t>& pipeline,
        const Horizon::HardwareImage& font_atlas,
        uint32_t target_width,
        uint32_t target_height,
        Horizon::ImageUsageFlags render_target_usage = Horizon::ImageUsageFlags::Sampled);

    // Accessors for shared resources (used by viewport callbacks)
    [[nodiscard]] Horizon::RasterizerPipeline<imgui_vert_glsl_t, imgui_frag_glsl_t>& pipeline() { return *imgui_pipeline_; }
    [[nodiscard]] const Horizon::HardwareImage& font_atlas() const { return font_atlas_image_; }

   private:
    bool ensure_imgui_pipeline();
    bool ensure_font_texture();

    static bool ensure_render_target(ViewportRenderResources& resources, uint32_t width, uint32_t height,
                                     Horizon::ImageUsageFlags usage = Horizon::ImageUsageFlags::Sampled);

    // --- Multi-Viewport renderer callbacks (static, access shared state via s_instance_) ---
    static void renderer_create_window(ImGuiViewport* vp);
    static void renderer_destroy_window(ImGuiViewport* vp);
    static void renderer_set_window_size(ImGuiViewport* vp, ImVec2 size);
    static void renderer_render_window(ImGuiViewport* vp, void* render_arg);
    static void renderer_swap_buffers(ImGuiViewport* vp, void* render_arg);

   private:
    static VulkanBackend* s_instance_;

    bool initialized_ = false;
    bool rebuild_needed_ = false;
    bool imgui_pipeline_ready_ = false;
    bool font_ready_ = false;

    SDL_Window* window_ = nullptr;
    void* surface_ = nullptr;

    // Main viewport rendering resources
    ViewportRenderResources main_resources_;

    // Shared across all viewports
    Horizon::HardwareImage font_atlas_image_;
    Horizon::HardwareExecutor font_upload_executor_;
    Horizon::SubmitReceipt font_upload_receipt_;
    std::optional<Horizon::RasterizerPipeline<imgui_vert_glsl_t, imgui_frag_glsl_t>> imgui_pipeline_;

    // Main viewport presentation (SharedDataHub path)
    uint64_t frame_index_ = 0;
    std::uintptr_t image_handle_ = 0;
};

}  // namespace Corona::Systems
