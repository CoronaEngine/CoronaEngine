#include <corona/systems/ui/quad_compositor.h>

#include <corona/systems/ui/vulkan_backend.h>  // VulkanBackend::ensure_render_target
#include <corona/kernel/core/i_logger.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace Corona::Systems {

namespace {

// Vertex layout consumed by ui_quad.vert.glsl: location 0 vec2 pos, 1 vec2 uv, 2 vec4 color.
struct QuadVertex {
    float pos[2]{};
    float uv[2]{};
    float color[4]{};
};

// Push-constant upload helpers (PushConsts is shared by ui_quad.vert/frag.glsl).
struct FVec2Upload {
    float x;
    float y;
};
struct FVec4Upload {
    float x;
    float y;
    float z;
    float w;
};

[[nodiscard]] FVec2Upload up2(const ktm::fvec2& v) {
    return {v.x, v.y};
}
[[nodiscard]] FVec4Upload up4(float x, float y, float z, float w) {
    return {x, y, z, w};
}

}  // namespace

bool QuadCompositor::ensure_white_texture() {
    if (white_ready_) {
        return true;
    }

    white_image_ = Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(
        1, 1,
        Horizon::Format::SRGBA8_UNORM,
        Horizon::ImageUsage_Sampled | Horizon::ImageUsage_TransferDst,
        "ui.white"));
    if (!white_image_) {
        CFW_LOG_ERROR("QuadCompositor: failed to create 1x1 white texture");
        return false;
    }

    const unsigned char pixel[4] = {255, 255, 255, 255};
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(pixel), sizeof(pixel));

    // Horizon 移除了 HardwareImage::upload()：改为 staging buffer + copy_from()。
    // 提交是异步的（receipt 存成员），staging 必须活到 GPU 用完，故挂 keep_alive。
    Horizon::HardwareBufferDesc staging_desc;
    staging_desc.element_count = bytes.size_bytes();
    staging_desc.element_size = 1;
    staging_desc.usage = Horizon::BufferUsage_TransferSrc;
    staging_desc.cpu_access = Horizon::CpuAccessMode::Write;
    auto staging = std::make_shared<Horizon::HardwareBuffer>(staging_desc, bytes);

    white_upload_receipt_ =
        white_upload_executor_.stream()
        << white_image_.copy_from(*staging)
        // keep_alive removed
        << Horizon::commit();

    white_ready_ = true;
    return true;
}

bool QuadCompositor::composite(
    std::span<const QuadDraw> quads,
    ViewportRenderResources& res,
    Horizon::RasterizerPipeline<ui_quad_vert_glsl_t, ui_quad_frag_glsl_t>& pipeline,
    uint32_t target_width,
    uint32_t target_height,
    Horizon::ImageUsageFlags render_target_usage) {
    if (target_width == 0 || target_height == 0 || quads.empty()) {
        return false;
    }

    if (!ensure_white_texture()) {
        return false;
    }
    res.executor.wait(white_upload_receipt_);
    for (const QuadDraw& q : quads) {
        if (q.texture != nullptr && q.texture_ready.serial != 0) {
            res.executor.wait(q.texture_ready);
        }
    }

    if (!VulkanBackend::ensure_render_target(res, target_width, target_height, render_target_usage)) {
        return false;
    }

    // --- Build merged vertex/index arrays (4 verts + 6 indices per quad) ---
    std::vector<QuadVertex> vertices;
    vertices.reserve(quads.size() * 4);
    // Horizon's indexed draw path consumes 16-bit indices. Each quad uses local
    // indices, with vertex_offset selecting its vertices in the merged buffer.
    std::vector<uint16_t> indices;
    indices.reserve(quads.size() * 6);

    for (const QuadDraw& q : quads) {
        const float x0 = q.dest_min.x;
        const float y0 = q.dest_min.y;
        const float x1 = q.dest_max.x;
        const float y1 = q.dest_max.y;
        const float u0 = q.uv_min.x;
        const float v0 = q.uv_min.y;
        const float u1 = q.uv_max.x;
        const float v1 = q.uv_max.y;
        const float cr = q.color.x;
        const float cg = q.color.y;
        const float cb = q.color.z;
        const float ca = q.color.w;

        auto push_vertex = [&](float px, float py, float pu, float pv) {
            QuadVertex gv{};
            gv.pos[0] = px;
            gv.pos[1] = py;
            gv.uv[0] = pu;
            gv.uv[1] = pv;
            gv.color[0] = cr;
            gv.color[1] = cg;
            gv.color[2] = cb;
            gv.color[3] = ca;
            vertices.push_back(gv);
        };

        push_vertex(x0, y0, u0, v0);  // top-left
        push_vertex(x1, y0, u1, v0);  // top-right
        push_vertex(x1, y1, u1, v1);  // bottom-right
        push_vertex(x0, y1, u0, v1);  // bottom-left

        // Local indices (vertex_offset is applied per-draw below).
        indices.insert(indices.end(), {0u, 1u, 2u, 0u, 2u, 3u});
    }

    // --- Ensure buffer capacity, reallocate only when needed ---
    const size_t vtx_bytes = vertices.size() * sizeof(QuadVertex);
    const size_t idx_bytes = indices.size() * sizeof(uint16_t);

    if (!res.vertex_buffer || res.vertex_buffer_capacity < vtx_bytes) {
        Horizon::HardwareBufferDesc desc;
        desc.element_count = vertices.size() + 256;
        desc.element_size = static_cast<uint32_t>(sizeof(QuadVertex));
        desc.usage = Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Vertex;
        desc.debug_name = "ui_quad.vertex";
        res.vertex_buffer = Horizon::HardwareBuffer(desc);
        res.vertex_buffer_capacity = desc.byte_size();
        if (!res.vertex_buffer) {
            CFW_LOG_ERROR("QuadCompositor: failed to allocate vertex buffer ({} bytes)", res.vertex_buffer_capacity);
            return false;
        }
    }

    if (!res.index_buffer || res.index_buffer_capacity < idx_bytes) {
        Horizon::HardwareBufferDesc desc;
        desc.element_count = indices.size() + 512;
        desc.element_size = static_cast<uint32_t>(sizeof(uint16_t));
        desc.usage = Horizon::BufferUsage_TransferDst | Horizon::BufferUsage_Index;
        desc.debug_name = "ui_quad.index";
        res.index_buffer = Horizon::HardwareBuffer(desc);
        res.index_buffer_capacity = desc.byte_size();
        if (!res.index_buffer) {
            CFW_LOG_ERROR("QuadCompositor: failed to allocate index buffer ({} bytes)", res.index_buffer_capacity);
            return false;
        }
    }

    const bool vertex_write_ok = res.vertex_buffer.write_bytes(
        std::as_bytes(std::span<const QuadVertex>(vertices.data(), vertices.size())));
    const bool index_write_ok = res.index_buffer.write_bytes(
        std::as_bytes(std::span<const uint16_t>(indices.data(), indices.size())));
    if (!vertex_write_ok || !index_write_ok) {
        CFW_LOG_ERROR("QuadCompositor: geometry upload failed vertex_ok={} index_ok={}",
                      vertex_write_ok, index_write_ok);
        return false;
    }

    // --- Set pipeline output ---
    pipeline.out_color = res.render_target;
    // 新版 Horizon: bind_render_target 已移除，直接赋值 out_color 即可
    pipeline.clear_records();

    const float fb_w = static_cast<float>(target_width);
    const float fb_h = static_cast<float>(target_height);
    const ktm::fvec2 scale(2.0f / fb_w, 2.0f / fb_h);
    const ktm::fvec2 translate(-1.0f, -1.0f);

    // CRITICAL FIX: Collect all indirect buffers to keep them alive until GPU finishes.
    // Local buffers were being destroyed at end of loop iteration, but GPU uses them
    // after commit(). This caused use-after-free → black screen/crash.
    std::vector<Horizon::HardwareBuffer> indirect_buffers;
    indirect_buffers.reserve(quads.size());

    int recorded = 0;
    for (size_t i = 0; i < quads.size(); ++i) {
        const QuadDraw& q = quads[i];

        // Clip rect in target pixels (full target when unset).
        float cx0 = 0.0f, cy0 = 0.0f, cx1 = fb_w, cy1 = fb_h;
        if (q.has_clip) {
            cx0 = std::clamp(q.clip_rect.x, 0.0f, fb_w);
            cy0 = std::clamp(q.clip_rect.y, 0.0f, fb_h);
            cx1 = std::clamp(q.clip_rect.z, 0.0f, fb_w);
            cy1 = std::clamp(q.clip_rect.w, 0.0f, fb_h);
        }

        const int32_t scissor_x = static_cast<int32_t>(std::floor(cx0));
        const int32_t scissor_y = static_cast<int32_t>(std::floor(cy0));
        const int32_t scissor_w = static_cast<int32_t>(std::ceil(cx1)) - scissor_x;
        const int32_t scissor_h = static_cast<int32_t>(std::ceil(cy1)) - scissor_y;
        if (scissor_w <= 0 || scissor_h <= 0) {
            continue;
        }

        // storeSampledDescriptor() 已移除；store_descriptor() 依 usage 自动选表，
        // 这些纹理只有 Sampled，故取到的仍是 combined-image-sampler 索引。
        const uint32_t texture_index =
            q.texture ? q.texture->store_descriptor() : white_image_.store_descriptor();

        // 新版 Horizon: 使用 VertexResourceBindings 访问 vertex shader 的 pushConsts
        using Pipeline = Horizon::RasterizerPipeline<ui_quad_vert_glsl_t, ui_quad_frag_glsl_t>;
        auto& pc = static_cast<Pipeline::VertexResourceBindings&>(pipeline).pushConsts;
        pc.scale = up2(scale);
        pc.translate = up2(translate);
        pc.clip_rect = up4(cx0, cy0, cx1, cy1);
        pc.texture_index = texture_index;

        // 新版 Horizon: 使用 indirect draw
        Horizon::DrawIndexedIndirectCommand cmd{};
        cmd.index_count = 6;
        cmd.first_index = static_cast<uint32_t>(i * 6);
        cmd.vertex_offset = static_cast<int32_t>(i * 4);
        cmd.instance_count = 1;
        cmd.first_instance = 0;

        auto indirect_buffer = Horizon::HardwareBuffer::from_bytes(
            std::as_bytes(std::span(&cmd, 1)),
            sizeof(cmd),
            Horizon::BufferUsage_Indirect,
            "ui_quad_indirect");

        Horizon::DrawIndexedIndirectParams draw_params;
        draw_params.draw_count = 1;
        draw_params.indirect_offset = 0;
        draw_params.stride = sizeof(Horizon::DrawIndexedIndirectCommand);

        pipeline.record_indirect(res.index_buffer, res.vertex_buffer, indirect_buffer, draw_params);

        // Keep buffer alive until after commit()
        indirect_buffers.push_back(std::move(indirect_buffer));
        ++recorded;
    }

    if (recorded == 0) {
        return false;
    }

    // 记住这次提交：Horizon 已移除 executor.last_receipt()，publish 与资源释放都靠它。
    // The indirect_buffers vector stays alive until this function returns, which is
    // after commit(), ensuring GPU has valid data when it executes the commands.
    res.last_receipt = res.executor.stream()
        << pipeline.extent(target_width, target_height)
        << Horizon::commit();

    return true;
}

}  // namespace Corona::Systems
