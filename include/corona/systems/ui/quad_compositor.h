#pragma once

// Phase 2 of the ImGui-removal plan: a minimal textured-quad compositor that
// replaced the ImDrawData renderer. It reuses the existing ui_quad.vert/frag GLSL
// pipeline and ViewportRenderResources, but instead of merging ImGui draw lists it
// renders an explicit list of QuadDraw entries (one CEF panel texture or one solid
// chrome bar per quad) into a per-window UI render target.
//
// This file is purely additive in Phase 2 — nothing references it yet. It is wired
// into the frame loop in Phase 6.

// This is the self-contained base header for the UI quad rendering layer. It owns the
// GLSL pipeline includes, the ViewportRenderResources struct, QuadDraw, and QuadCompositor.
// vulkan_backend.h includes THIS header (not the other way around) to avoid a circular
// include — QuadCompositor must not depend on VulkanBackend's definition.
#include "horizon.h"
#include "Codegen/ControlFlows.h"  // Horizon: GLSL()/HLSL() include macros
// clang-format off
#include GLSL(../../../assets/shaders/ui_quad.vert.glsl)
#include GLSL(../../../assets/shaders/ui_quad.frag.glsl)
// clang-format on

#include <cstddef>
#include <cstdint>
#include <span>

namespace Corona::Systems {

// ============================================================================
// Per-window rendering resources (render target, executor, geometry buffers).
// Used by the main window and (Phase 7) secondary windows.
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
    // Horizon 移除了 HardwareExecutor::last_receipt()，生产者自己记住最后一次提交：
    // 既用于 publish 时交给 Display 做跨线程 GPU 排序，也用于释放资源前排空。
    Horizon::SubmitReceipt last_receipt;
};

// One textured (or solid) quad to draw into the UI render target.
//   - texture == nullptr  -> a solid quad tinted by `color` (1x1 white texture is used)
//   - texture != nullptr  -> sample `texture` (e.g. a CEF panel); keep color = white for passthrough
//   - texture_ready carries the most recent upload receipt for sampled textures.
// Coordinates are in target pixels. The vertex color is gamma-corrected (pow 2.2) in
// the fragment shader, matching the existing ui_quad.frag behaviour.
struct QuadDraw {
    const Horizon::HardwareImage* texture = nullptr;
    Horizon::SubmitReceipt texture_ready;
    ktm::fvec2 dest_min = ktm::fvec2(0.0f, 0.0f);
    ktm::fvec2 dest_max = ktm::fvec2(0.0f, 0.0f);
    ktm::fvec2 uv_min = ktm::fvec2(0.0f, 0.0f);
    ktm::fvec2 uv_max = ktm::fvec2(1.0f, 1.0f);
    ktm::fvec4 color = ktm::fvec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool has_clip = false;
    ktm::fvec4 clip_rect = ktm::fvec4(0.0f, 0.0f, 0.0f, 0.0f);  // x0,y0,x1,y1 target px (used when has_clip)
};

class QuadCompositor {
   public:
    QuadCompositor() = default;

    // Render `quads` into `res`'s render target (created/resized as needed) using the
    // supplied UI quad pipeline, then submit. Returns true if any draw was recorded.
    // Mirrors VulkanBackend's submit machinery.
    bool composite(std::span<const QuadDraw> quads,
                   ViewportRenderResources& res,
                   Horizon::RasterizerPipeline<ui_quad_vert_glsl_t, ui_quad_frag_glsl_t>& pipeline,
                   uint32_t target_width,
                   uint32_t target_height,
                   Horizon::ImageUsageFlags render_target_usage = Horizon::ImageUsage_Sampled);

   private:
    // Lazily create + upload the 1x1 white texture used for solid (non-textured) quads.
    bool ensure_white_texture();

    Horizon::HardwareImage white_image_;
    Horizon::HardwareExecutor white_upload_executor_;
    Horizon::SubmitReceipt white_upload_receipt_;
    bool white_ready_ = false;
};

}  // namespace Corona::Systems
