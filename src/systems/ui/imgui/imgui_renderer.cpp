#include <corona/systems/ui/imgui_renderer.h>
#include <corona/systems/ui/vulkan_backend.h>

#include "imgui_draw_packet.h"
#include "cef/browser_manager.h"
#include <horizon/core/logging.h>

#include <span>
#include <unordered_set>

namespace Corona::Systems {
namespace {
bool upload_geometry(Horizon::HardwareBuffer& buffer, std::span<const std::byte> bytes,
    std::uint32_t stride, Horizon::BufferUsageFlags usage, const char* name) {
    if (!buffer || buffer.get_byte_size() < bytes.size()) {
        Horizon::HardwareBufferDesc desc;
        desc.element_count = bytes.size() / stride + 256;
        desc.element_size = stride;
        desc.usage = usage;
        desc.cpu_access = Horizon::CpuAccessMode::Write;
        desc.debug_name = name;
        buffer = Horizon::HardwareBuffer(desc);
    }
    return buffer && buffer.write_bytes(bytes);
}
}

bool ImGuiRenderer::initialize() {
    auto& io = ImGui::GetIO();
    io.BackendRendererName = "Corona.Horizon";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasViewports;
    io.BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels || width <= 0 || height <= 0) return false;
    font_image_ = Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(
        width, height, Horizon::Format::SRGBA8_UNORM,
        Horizon::ImageUsage_Sampled | Horizon::ImageUsage_TransferDst, "ui.imgui.font"));
    if (!font_image_) return false;
    const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(pixels),
        static_cast<std::size_t>(width) * height * 4);
    auto staging = Horizon::HardwareBuffer::from_bytes(bytes, 1, Horizon::BufferUsage_TransferSrc, "ui.imgui.font.upload");
    font_receipt_ = font_executor_.stream() << font_image_.copy_from(staging) << Horizon::commit();
    font_texture_id_ = static_cast<std::uint64_t>(font_image_.store_descriptor()) + 1;
    io.Fonts->SetTexID(font_texture_id_);
    return true;
}

void ImGuiRenderer::shutdown() {
    if (font_receipt_.serial) font_executor_.wait_completed(font_receipt_);
    if (ImGui::GetCurrentContext()) {
        ImGui::GetIO().Fonts->SetTexID(0);
        ImGui::GetIO().BackendRendererName = nullptr;
        ImGui::GetIO().BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasViewports);
    }
    font_image_ = {};
    font_receipt_ = {};
    font_texture_id_ = 0;
}

bool ImGuiRenderer::render(const ImDrawData& data, ViewportRenderResources& resources,
    Horizon::RasterizerPipeline<ui_quad_vert_glsl_t, ui_quad_frag_glsl_t>& pipeline,
    std::uint32_t width, std::uint32_t height) {
    if (!font_image_ || width == 0 || height == 0 || data.DisplaySize.x <= 0 || data.DisplaySize.y <= 0) return false;
    auto packet = UI::build_imgui_draw_packet(data, width, height);
    auto& frame = resources.imgui_frames[resources.imgui_frame_index++ % resources.imgui_frames.size()];
    // CPU mapped writes require host completion, not HardwareExecutor::wait().
    if (frame.receipt.serial) resources.executor.wait_completed(frame.receipt);
    frame.sampled_images.clear();
    resources.executor.wait(font_receipt_);
    frame.sampled_images.push_back(font_image_);

    // Always record a transparent triangle so even callback-only/retired-texture
    // frames clear the attachment. Preserve all user callbacks in their original order.
    const auto clear_vertex = static_cast<std::int32_t>(packet.vertices.size());
    const auto clear_index = static_cast<std::uint32_t>(packet.indices.size());
    packet.vertices.insert(packet.vertices.end(), {
        {{0, 0}, {0, 0}, {0, 0, 0, 0}}, {{1, 0}, {0, 0}, {0, 0, 0, 0}}, {{0, 1}, {0, 0}, {0, 0, 0, 0}}});
    packet.indices.insert(packet.indices.end(), {0, 1, 2});
    packet.commands.push_back({3, clear_index, clear_vertex, font_texture_id_, {0, 0, static_cast<float>(width), static_cast<float>(height)}});

    const auto vertices = std::as_bytes(std::span(packet.vertices));
    const auto indices = std::as_bytes(std::span(packet.indices));
    // Reuse the completed ring slot, growing only when necessary. No submitted
    // buffer is modified while Horizon can still read it.
    if (!upload_geometry(frame.vertices, vertices, sizeof(UI::ImGuiGpuVertex), Horizon::BufferUsage_Vertex, "ui.imgui.vertices") ||
        !upload_geometry(frame.indices, indices, sizeof(ImDrawIdx), Horizon::BufferUsage_Index, "ui.imgui.indices")) return false;
    std::vector<Horizon::DrawIndexedIndirectCommand> commands;
    commands.reserve(packet.commands.size());
    for (const auto& command : packet.commands)
        commands.push_back({command.count, 1, command.first_index, command.vertex_offset, 0});
    if (!upload_geometry(frame.indirect, std::as_bytes(std::span(commands)),
        sizeof(Horizon::DrawIndexedIndirectCommand), Horizon::BufferUsage_Indirect, "ui.imgui.indirect")) return false;

    pipeline.out_color = resources.render_target;
    pipeline.clear_records();
    std::unordered_set<ImTextureID> retained;
    using Pipeline = Horizon::RasterizerPipeline<ui_quad_vert_glsl_t, ui_quad_frag_glsl_t>;
    struct Float2 { float x, y; };
    struct Float4 { float x, y, z, w; };
    for (std::size_t i = 0; i < packet.commands.size(); ++i) {
        const auto& command = packet.commands[i];
        if (command.source_command) {
            if (command.source_command->UserCallback != ImDrawCallback_ResetRenderState)
                command.source_command->UserCallback(command.source_list, command.source_command);
            pipeline.out_color = resources.render_target;
            continue;
        }
        const Horizon::HardwareImage* texture = nullptr;
        if (command.texture == font_texture_id_) texture = &font_image_;
        else texture = UI::BrowserManager::instance().get_texture_image(command.texture);
        if (!texture) continue;  // A retired/unknown ID must not silently become a font texture.
        if (retained.insert(command.texture).second && command.texture != font_texture_id_) {
            const auto ready = UI::BrowserManager::instance().get_texture_upload_receipt(command.texture);
            if (ready.serial) resources.executor.wait(ready);
            frame.sampled_images.push_back(*texture);
        }
        auto& pc = static_cast<Pipeline::VertexResourceBindings&>(pipeline).pushConsts;
        pc.scale = Float2{packet.scale[0], packet.scale[1]};
        pc.translate = Float2{packet.translate[0], packet.translate[1]};
        pc.clip_rect = Float4{command.clip[0], command.clip[1], command.clip[2], command.clip[3]};
        pc.texture_index = texture->store_descriptor();
        Horizon::DrawIndexedIndirectParams params;
        params.draw_count = 1;
        params.indirect_offset = i * sizeof(Horizon::DrawIndexedIndirectCommand);
        params.stride = sizeof(Horizon::DrawIndexedIndirectCommand);
        pipeline.record_indirect(frame.indices, frame.vertices, frame.indirect, params);
    }
    frame.receipt = resources.executor.stream() << pipeline.extent(width, height) << Horizon::commit();
    resources.last_receipt = frame.receipt;
    return true;
}

}  // namespace Corona::Systems
