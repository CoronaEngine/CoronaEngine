#pragma once

#include <corona/systems/ui/quad_compositor.h>

struct ImDrawData;

namespace Corona::Systems {

// The renderer consumes ImGui draw data. ImGui's Vulkan renderer is never involved.
class ImGuiRenderer {
public:
    bool initialize();
    void shutdown();
    bool render(const ImDrawData& data, ViewportRenderResources& resources,
                Horizon::RasterizerPipeline<ui_quad_vert_glsl_t, ui_quad_frag_glsl_t>& pipeline,
                std::uint32_t width, std::uint32_t height);

private:
    Horizon::HardwareImage font_image_;
    Horizon::HardwareExecutor font_executor_;
    Horizon::SubmitReceipt font_receipt_;
    std::uint64_t font_texture_id_ = 0;
};

}  // namespace Corona::Systems
