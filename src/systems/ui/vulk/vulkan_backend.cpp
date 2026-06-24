#include <corona/events/display_system_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/script/corona_engine_api.h>
#include <corona/systems/ui/vulkan_backend.h>

#include "../cef/browser_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace {
struct ImGuiGpuVertex {
    float pos[2]{};
    float uv[2]{};
    float color[4]{};
};

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

[[nodiscard]] FVec2Upload upload_value(const ktm::fvec2& value) {
    return {value.x, value.y};
}

[[nodiscard]] FVec4Upload upload_value(const ktm::fvec4& value) {
    return {value.x, value.y, value.z, value.w};
}

[[nodiscard]] Corona::Horizon::RasterizerPipelineDesc make_imgui_pipeline_desc() {
    Corona::Horizon::RasterizerPipelineDesc desc;
    desc.debug_name = "corona.imgui";
    desc.rasterizer.cull_mode = Corona::Horizon::CullMode::None;
    desc.depth_stencil.depth_test_enabled = false;
    desc.depth_stencil.depth_write_enabled = false;
    desc.depth_stencil.stencil_test_enabled = false;
    return desc;
}

inline ktm::fvec4 unpack_imgui_color(const ImU32 color) {
    constexpr float kInv255 = 1.0f / 255.0f;
    const float r = static_cast<float>((color >> IM_COL32_R_SHIFT) & 0xFFu) * kInv255;
    const float g = static_cast<float>((color >> IM_COL32_G_SHIFT) & 0xFFu) * kInv255;
    const float b = static_cast<float>((color >> IM_COL32_B_SHIFT) & 0xFFu) * kInv255;
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFu) * kInv255;
    return ktm::fvec4(r, g, b, a);
}

inline uint32_t texture_id_to_descriptor(ImTextureID tex_id) {
    const ImU64 raw = static_cast<ImU64>(tex_id);
    if (raw == 0) {
        CFW_LOG_ERROR("VulkanBackend: ImTextureID is null");
        return 0;
    }
    const ImU64 descriptor = raw - 1u;
    if (descriptor > static_cast<ImU64>(std::numeric_limits<uint32_t>::max())) {
        CFW_LOG_ERROR("VulkanBackend: ImTextureID out of uint32 range: {}", raw);
    }
    return static_cast<uint32_t>(descriptor);
}

inline ImTextureID descriptor_to_texture_id(uint32_t descriptor) {
    return static_cast<ImTextureID>(static_cast<ImU64>(descriptor) + 1u);
}

constexpr auto kUiRenderTargetUsage =
    Corona::Horizon::ImageUsageFlags::Sampled | Corona::Horizon::ImageUsageFlags::Storage;

struct PixelExtent {
    uint32_t width = 0;
    uint32_t height = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return width != 0 && height != 0;
    }
};

[[nodiscard]] PixelExtent window_pixel_extent(SDL_Window* window) {
    if (window == nullptr) {
        return {};
    }

    int width = 0;
    int height = 0;
    if (SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0) {
        return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }

    width = 0;
    height = 0;
    if (SDL_GetWindowSize(window, &width, &height) && width > 0 && height > 0) {
        return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }

    return {};
}

[[nodiscard]] PixelExtent draw_data_pixel_extent(const ImDrawData* draw_data) {
    if (draw_data == nullptr) {
        return {};
    }

    const float scale_x = draw_data->FramebufferScale.x > 0.0f
                              ? draw_data->FramebufferScale.x
                              : 1.0f;
    const float scale_y = draw_data->FramebufferScale.y > 0.0f
                              ? draw_data->FramebufferScale.y
                              : 1.0f;
    const auto width = static_cast<int>(std::lround(draw_data->DisplaySize.x * scale_x));
    const auto height = static_cast<int>(std::lround(draw_data->DisplaySize.y * scale_y));
    if (width <= 0 || height <= 0) {
        return {};
    }
    return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

[[nodiscard]] ktm::fvec2 framebuffer_scale_for(const ImDrawData& draw_data,
                                               PixelExtent extent) {
    if (draw_data.DisplaySize.x <= 0.0f || draw_data.DisplaySize.y <= 0.0f) {
        return ktm::fvec2(1.0f, 1.0f);
    }
    return ktm::fvec2(static_cast<float>(extent.width) / draw_data.DisplaySize.x,
                      static_cast<float>(extent.height) / draw_data.DisplaySize.y);
}

[[nodiscard]] SDL_Window* sdl_window_from_viewport(ImGuiViewport* viewport) {
    if (viewport == nullptr || viewport->PlatformHandle == nullptr) {
        return nullptr;
    }
    const auto window_id =
        static_cast<SDL_WindowID>(reinterpret_cast<std::intptr_t>(viewport->PlatformHandle));
    return SDL_GetWindowFromID(window_id);
}

std::uintptr_t select_main_camera_handle(const Corona::SceneDevice& scene) {
    if (scene.active_camera_handle != 0 &&
        std::find(scene.camera_handles.begin(),
                  scene.camera_handles.end(),
                  scene.active_camera_handle) != scene.camera_handles.end()) {
        return scene.active_camera_handle;
    }
    return scene.camera_handles.empty() ? 0 : scene.camera_handles.front();
}

void sync_default_surface_camera_size(void* surface, uint32_t width, uint32_t height) {
    if (surface == nullptr || width == 0 || height == 0) {
        return;
    }

    auto& hub = Corona::SharedDataHub::instance();
    for (const auto& scene : hub.scene_storage()) {
        if (!scene.enabled) {
            continue;
        }
        const auto camera_handle = select_main_camera_handle(scene);
        if (camera_handle == 0) {
            continue;
        }
        if (auto camera = hub.camera_storage().try_acquire_write(camera_handle)) {
            if (!camera->follows_default_surface) {
                continue;
            }
            if (camera->surface != nullptr && camera->surface != surface) {
                continue;
            }
            camera->surface = surface;
            camera->width = width;
            camera->height = height;
            camera->aspect = static_cast<float>(width) / static_cast<float>(height);
        }
    }
}
}  // namespace

namespace Corona::Systems {

// Per-secondary-viewport data stored in ImGuiViewport::RendererUserData.
//
// 改造2: secondary viewports no longer own a swapchain. DisplaySystem owns the
// per-surface HardwareDisplayer and composites Optics+UI before presenting, exactly
// like the main window. Here we only render the ImGui UI into a render target and
// publish it as a UIFrameReadyEvent keyed by this window's native handle (surface).
struct ViewportData {
    ViewportRenderResources resources;                              // per-window render target + geometry buffers
    std::optional<Horizon::RasterizerPipeline<imgui_vert_glsl_t, imgui_frag_glsl_t>> pipeline;  // independent pipeline instance
    bool pipeline_ready = false;
    bool pending_show = false;          // deferred show: wait until first frame is published
    void* surface = nullptr;            // native window handle (HWND/NSWindow*); the DisplaySystem surface key
    std::uintptr_t image_handle = 0;    // image_storage handle carrying this viewport's UI layer
    uint64_t frame_index = 0;           // monotonic UI frame counter for this viewport
};

// Deferred Platform_ShowWindow: intercept to delay OS window visibility until after the
// first Renderer_SwapBuffers, eliminating the 1-frame white/black flash on dock-out.
static void (*s_original_platform_show_window)(ImGuiViewport*) = nullptr;

static void deferred_platform_show_window(ImGuiViewport* vp) {
    if (auto* vd = static_cast<ViewportData*>(vp->RendererUserData)) {
        vd->pending_show = true;  // defer until after first swap
    } else if (s_original_platform_show_window) {
        s_original_platform_show_window(vp);  // main viewport: show immediately
    }
}

VulkanBackend* VulkanBackend::s_instance_ = nullptr;

VulkanBackend::VulkanBackend(SDL_Window* window)
    : window_(window) {
}

bool VulkanBackend::initialize() {
    if (initialized_) {
        return true;
    }

    if (window_ == nullptr) {
        CFW_LOG_ERROR("VulkanBackend: initialize failed, window is null");
        return false;
    }

    void* native_handle = nullptr;
#if defined(_WIN32)
    native_handle = SDL_GetPointerProperty(
        SDL_GetWindowProperties(window_),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER,
        nullptr);
#elif defined(__APPLE__)
    native_handle = SDL_GetPointerProperty(
        SDL_GetWindowProperties(window_),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
        nullptr);
#endif

    if (native_handle == nullptr) {
        CFW_LOG_ERROR("VulkanBackend: failed to get native window handle from SDL");
        return false;
    }

    surface_ = native_handle;
    Corona::API::set_default_surface(surface_);
    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        event_bus->publish<Events::DisplaySurfaceChangedEvent>({surface_});
    }

    const PixelExtent initial_extent = window_pixel_extent(window_);
    if (initial_extent) {
        if (!ensure_render_target(main_resources_, initial_extent.width, initial_extent.height,
                                  kUiRenderTargetUsage)) {
            CFW_LOG_ERROR("VulkanBackend: failed to create initial render target");
            return false;
        }
    } else {
        rebuild_needed_ = true;
    }

    image_handle_ = SharedDataHub::instance().image_storage().allocate();
    if (auto image_device = SharedDataHub::instance().image_storage().acquire_write(image_handle_)) {
        // Keep storage entry alive; per-frame values are updated in present_frame().
    } else {
        CFW_LOG_ERROR("VulkanBackend: failed to acquire shared image storage handle");
        SharedDataHub::instance().image_storage().deallocate(image_handle_);
        image_handle_ = 0;
        return false;
    }

    initialized_ = true;
    CFW_LOG_INFO("VulkanBackend: initialized");
    return true;
}

void VulkanBackend::register_viewport_callbacks() {
    if (!initialized_) {
        CFW_LOG_ERROR("VulkanBackend: register_viewport_callbacks called before initialize");
        return;
    }

    if (ImGui::GetCurrentContext() == nullptr) {
        CFW_LOG_ERROR("VulkanBackend: register_viewport_callbacks called without ImGui context");
        return;
    }

    s_instance_ = this;

    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Renderer_CreateWindow = renderer_create_window;
    platform_io.Renderer_DestroyWindow = renderer_destroy_window;
    platform_io.Renderer_SetWindowSize = renderer_set_window_size;
    platform_io.Renderer_RenderWindow = renderer_render_window;
    platform_io.Renderer_SwapBuffers = renderer_swap_buffers;

    // Intercept Platform_ShowWindow to defer OS window visibility until first frame is presented.
    s_original_platform_show_window = platform_io.Platform_ShowWindow;
    platform_io.Platform_ShowWindow = deferred_platform_show_window;

    // Main viewport is rendered via render_frame()/present_frame(), not via callbacks.
    // Set RendererUserData to nullptr so callbacks skip it (they check for null).
    ImGuiViewport* main_vp = ImGui::GetMainViewport();
    if (main_vp != nullptr) {
        main_vp->RendererUserData = nullptr;
    }

    CFW_LOG_INFO("VulkanBackend: viewport callbacks registered (RendererHasViewports enabled)");
}

void VulkanBackend::shutdown() {
    if (!initialized_) {
        return;
    }

    // Destroy all secondary viewport data before releasing shared resources.
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::DestroyPlatformWindows();
    }

    main_resources_.frame_ready = false;
    imgui_pipeline_ready_ = false;
    font_ready_ = false;
    font_upload_receipt_ = Horizon::SubmitReceipt();
    rebuild_needed_ = false;

    main_resources_.render_target = Horizon::HardwareImage();
    font_atlas_image_ = Horizon::HardwareImage();
    imgui_pipeline_.reset();

    main_resources_.vertex_buffer = Horizon::HardwareBuffer();
    main_resources_.index_buffer = Horizon::HardwareBuffer();
    main_resources_.vertex_buffer_capacity = 0;
    main_resources_.index_buffer_capacity = 0;

    main_resources_.width = 0;
    main_resources_.height = 0;

    surface_ = nullptr;
    frame_index_ = 0;

    if (image_handle_ != 0) {
        SharedDataHub::instance().image_storage().deallocate(image_handle_);
        image_handle_ = 0;
    }

    initialized_ = false;
    s_instance_ = nullptr;
    CFW_LOG_INFO("VulkanBackend: shutdown");
}

void VulkanBackend::new_frame() {
    if (!initialized_) {
        return;
    }

    // GPU sync: wait for Display to finish consuming our image
    // before we clear and render new UI content into it.
    if (image_handle_ != 0) {
        if (auto image_device = SharedDataHub::instance().image_storage().acquire_write(image_handle_)) {
            main_resources_.executor.wait(image_device->consumed_receipt);
        }
    }
}

void VulkanBackend::render_frame(ImDrawData* draw_data) {
    if (!initialized_ || draw_data == nullptr) {
        return;
    }

    if (!ensure_imgui_pipeline() || !ensure_font_texture()) {
        return;
    }

    main_resources_.executor.wait(font_upload_receipt_);
    PixelExtent target_extent = draw_data_pixel_extent(draw_data);
    if (!target_extent) {
        target_extent = window_pixel_extent(window_);
    }
    if (!target_extent) {
        return;
    }

    if (render_draw_data(draw_data,
                         main_resources_,
                         *imgui_pipeline_,
                         font_atlas_image_,
                         target_extent.width,
                         target_extent.height,
                         kUiRenderTargetUsage)) {
        main_resources_.frame_ready = true;
    }
}

// ============================================================================
// Static reusable render function — works with any ViewportRenderResources
// ============================================================================

bool VulkanBackend::render_draw_data(
    ImDrawData* draw_data,
    ViewportRenderResources& res,
    Horizon::RasterizerPipeline<imgui_vert_glsl_t, imgui_frag_glsl_t>& pipeline,
    const Horizon::HardwareImage& font_atlas,
    uint32_t target_width,
    uint32_t target_height,
    Horizon::ImageUsageFlags render_target_usage) {
    if (draw_data == nullptr) {
        return false;
    }
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) {
        return false;
    }

    PixelExtent target_extent{target_width, target_height};
    if (!target_extent) {
        target_extent = draw_data_pixel_extent(draw_data);
    }
    if (!target_extent) {
        return false;
    }

    const uint32_t fb_width = target_extent.width;
    const uint32_t fb_height = target_extent.height;
    const ktm::fvec2 framebuffer_scale = framebuffer_scale_for(*draw_data, target_extent);

    // --- Ensure render target ---
    if (!ensure_render_target(res, fb_width, fb_height, render_target_usage)) {
        return false;
    }

    if (draw_data->TotalVtxCount <= 0) {
        return false;
    }

    // --- Set pipeline output ---
    pipeline.out_color = res.render_target;
    pipeline.bind_render_target(0, res.render_target);
    pipeline.clear_records();

    // --- Merge all draw lists into single vertex/index arrays ---
    const auto total_vtx = static_cast<size_t>(draw_data->TotalVtxCount);
    const auto total_idx = static_cast<size_t>(draw_data->TotalIdxCount);

    std::vector<ImGuiGpuVertex> merged_vertices;
    merged_vertices.reserve(total_vtx);
    std::vector<ImDrawIdx> merged_indices;
    merged_indices.reserve(total_idx);

    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        if (cmd_list == nullptr) {
            continue;
        }

        for (const ImDrawVert& v : cmd_list->VtxBuffer) {
            ImGuiGpuVertex gv{};
            gv.pos[0] = v.pos.x;
            gv.pos[1] = v.pos.y;
            gv.uv[0] = v.uv.x;
            gv.uv[1] = v.uv.y;
            const ktm::fvec4 color = unpack_imgui_color(v.col);
            gv.color[0] = color.x;
            gv.color[1] = color.y;
            gv.color[2] = color.z;
            gv.color[3] = color.w;
            merged_vertices.push_back(gv);
        }

        const auto* idx_src = cmd_list->IdxBuffer.Data;
        merged_indices.insert(merged_indices.end(), idx_src, idx_src + cmd_list->IdxBuffer.Size);
    }

    if (merged_vertices.empty() || merged_indices.empty()) {
        return false;
    }

    // --- Ensure buffer capacity, only reallocate when needed ---
    const size_t vtx_bytes = merged_vertices.size() * sizeof(ImGuiGpuVertex);
    const size_t idx_bytes = merged_indices.size() * sizeof(ImDrawIdx);

    if (!res.vertex_buffer || res.vertex_buffer_capacity < vtx_bytes) {
        const size_t new_count = merged_vertices.size() + 5000;
        Horizon::HardwareBufferDesc desc;
        desc.element_count = new_count;
        desc.element_size = static_cast<uint32_t>(sizeof(ImGuiGpuVertex));
        desc.usage = Horizon::BufferUsageFlags::TransferDst | Horizon::BufferUsageFlags::Vertex;
        desc.debug_name = "imgui.vertex";
        res.vertex_buffer = Horizon::HardwareBuffer(desc);
        res.vertex_buffer_capacity = desc.byte_size();
        if (!res.vertex_buffer) {
            CFW_LOG_ERROR("VulkanBackend: failed to allocate vertex buffer ({} bytes)", res.vertex_buffer_capacity);
            return false;
        }
    }

    if (!res.index_buffer || res.index_buffer_capacity < idx_bytes) {
        const size_t new_count = merged_indices.size() + 10000;
        Horizon::HardwareBufferDesc desc;
        desc.element_count = new_count;
        desc.element_size = static_cast<uint32_t>(sizeof(ImDrawIdx));
        desc.usage = Horizon::BufferUsageFlags::TransferDst | Horizon::BufferUsageFlags::Index;
        desc.debug_name = "imgui.index";
        res.index_buffer = Horizon::HardwareBuffer(desc);
        res.index_buffer_capacity = desc.byte_size();
        if (!res.index_buffer) {
            CFW_LOG_ERROR("VulkanBackend: failed to allocate index buffer ({} bytes)", res.index_buffer_capacity);
            return false;
        }
    }

    const bool vertex_write_ok = res.vertex_buffer.write_bytes(
        std::as_bytes(std::span<const ImGuiGpuVertex>(merged_vertices.data(), merged_vertices.size())));
    const bool index_write_ok = res.index_buffer.write_bytes(
        std::as_bytes(std::span<const ImDrawIdx>(merged_indices.data(), merged_indices.size())));
    if (!vertex_write_ok || !index_write_ok) {
        CFW_LOG_ERROR("VulkanBackend: ImGui geometry upload failed vertex_ok={} index_ok={} vtx_bytes={} idx_bytes={}",
                      vertex_write_ok,
                      index_write_ok,
                      vtx_bytes,
                      idx_bytes);
        return false;
    }

    // --- Record draw commands with global offsets ---
    const float sx = 2.0f / draw_data->DisplaySize.x;
    const float sy = 2.0f / draw_data->DisplaySize.y;
    const ktm::fvec2 scale(sx, sy);
    const ktm::fvec2 translate(-1.0f - draw_data->DisplayPos.x * sx,
                               -1.0f - draw_data->DisplayPos.y * sy);

    int global_vtx_offset = 0;
    int global_idx_offset = 0;
    int recorded_draw_cmds = 0;

    for (int n = 0; n < draw_data->CmdListsCount; ++n) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        if (cmd_list == nullptr) {
            continue;
        }

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; ++cmd_i) {
            const ImDrawCmd& pcmd = cmd_list->CmdBuffer[cmd_i];

            if (pcmd.UserCallback != nullptr) {
                if (pcmd.UserCallback == ImDrawCallback_ResetRenderState) {
                    pipeline.out_color = res.render_target;
                    pipeline.bind_render_target(0, res.render_target);
                    continue;
                }
                pcmd.UserCallback(cmd_list, &pcmd);
                continue;
            }

            ImVec2 clip_min((pcmd.ClipRect.x - draw_data->DisplayPos.x) * framebuffer_scale.x,
                            (pcmd.ClipRect.y - draw_data->DisplayPos.y) * framebuffer_scale.y);
            ImVec2 clip_max((pcmd.ClipRect.z - draw_data->DisplayPos.x) * framebuffer_scale.x,
                            (pcmd.ClipRect.w - draw_data->DisplayPos.y) * framebuffer_scale.y);

            clip_min.x = std::max(clip_min.x, 0.0f);
            clip_min.y = std::max(clip_min.y, 0.0f);
            clip_max.x = std::min(clip_max.x, static_cast<float>(fb_width));
            clip_max.y = std::min(clip_max.y, static_cast<float>(fb_height));

            if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y) {
                continue;
            }

            const ImTextureID tex_id = pcmd.GetTexID();
            uint32_t texture_index = texture_id_to_descriptor(tex_id);
            if (const auto* browser_texture = UI::BrowserManager::instance().get_texture_image(tex_id)) {
                UI::BrowserManager::instance().wait_for_texture_upload(tex_id);
                texture_index = browser_texture->storeSampledDescriptor();
            } else if (font_atlas) {
                const uint32_t font_descriptor = font_atlas.storeSampledDescriptor();
                if (texture_index == 0 || texture_index == font_descriptor) {
                    texture_index = font_descriptor;
                }
            }

            pipeline[imgui_vert_glsl_t::pushConsts::scale] = upload_value(scale);
            pipeline[imgui_vert_glsl_t::pushConsts::translate] = upload_value(translate);
            pipeline[imgui_frag_glsl_t::pushConsts::clip_rect] = upload_value(
                ktm::fvec4(clip_min.x, clip_min.y, clip_max.x, clip_max.y));
            pipeline[imgui_frag_glsl_t::pushConsts::texture_index] = texture_index;

            const int32_t scissor_x = static_cast<int32_t>(std::floor(clip_min.x));
            const int32_t scissor_y = static_cast<int32_t>(std::floor(clip_min.y));
            const int32_t scissor_w = static_cast<int32_t>(std::ceil(clip_max.x)) - scissor_x;
            const int32_t scissor_h = static_cast<int32_t>(std::ceil(clip_max.y)) - scissor_y;
            if (scissor_w <= 0 || scissor_h <= 0) {
                continue;
            }

            Horizon::DrawIndexedParams draw_params;
            draw_params.index_count = static_cast<uint32_t>(pcmd.ElemCount);
            draw_params.first_index = static_cast<uint32_t>(pcmd.IdxOffset + global_idx_offset);
            draw_params.vertex_offset = static_cast<int32_t>(pcmd.VtxOffset + global_vtx_offset);
            draw_params.index_type = sizeof(ImDrawIdx) == sizeof(uint16_t) ? Horizon::IndexType::UInt16 : Horizon::IndexType::UInt32;
            draw_params.enable_scissor = true;
            draw_params.scissor = Horizon::ScissorRect{
                scissor_x,
                scissor_y,
                static_cast<uint32_t>(scissor_w),
                static_cast<uint32_t>(scissor_h)};

            pipeline.record(res.index_buffer, res.vertex_buffer, draw_params);
            ++recorded_draw_cmds;
        }

        global_idx_offset += cmd_list->IdxBuffer.Size;
        global_vtx_offset += cmd_list->VtxBuffer.Size;
    }

    if (recorded_draw_cmds == 0) {
        return false;
    }

    // --- Submit ---
    (void)(res.executor.stream()
           << pipeline(static_cast<uint16_t>(fb_width), static_cast<uint16_t>(fb_height))
           << Horizon::commit());

    return true;
}

void VulkanBackend::present_frame() {
    if (!initialized_ || !main_resources_.frame_ready || !main_resources_.render_target || surface_ == nullptr || image_handle_ == 0) {
        return;
    }

    if (auto image_device = SharedDataHub::instance().image_storage().acquire_write(image_handle_)) {
        image_device->image = main_resources_.render_target;
        image_device->submit_receipt = main_resources_.executor.last_receipt();
    } else {
        return;
    }

    sync_default_surface_camera_size(surface_, main_resources_.width, main_resources_.height);

    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        ++frame_index_;
        event_bus->publish<Events::UIFrameReadyEvent>({surface_,
                                                       image_handle_,
                                                       frame_index_,
                                                       main_resources_.width,
                                                       main_resources_.height});
    }

    main_resources_.frame_ready = false;
}

void VulkanBackend::rebuild(int width, int height) {
    if (!initialized_) {
        return;
    }

    if (width <= 0 || height <= 0) {
        return;
    }

    PixelExtent target_extent = window_pixel_extent(window_);
    if (!target_extent) {
        target_extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    }

    const auto target_width = target_extent.width;
    const auto target_height = target_extent.height;
    const bool size_changed = !main_resources_.render_target ||
                              main_resources_.width != target_width ||
                              main_resources_.height != target_height;
    if (size_changed) {
        if (image_handle_ != 0) {
            if (auto image_device = SharedDataHub::instance().image_storage().acquire_write(image_handle_)) {
                main_resources_.executor.wait_idle(image_device->consumed_receipt);
            }
        }
        main_resources_.executor.wait_idle(main_resources_.executor.last_receipt());
    }

    if (!ensure_render_target(main_resources_, target_width, target_height, kUiRenderTargetUsage)) {
        rebuild_needed_ = true;
        return;
    }

    sync_default_surface_camera_size(surface_, target_width, target_height);
    rebuild_needed_ = false;
}

bool VulkanBackend::ensure_render_target(ViewportRenderResources& resources, uint32_t width, uint32_t height,
                                         Horizon::ImageUsageFlags usage) {
    if (width == 0 || height == 0) {
        return false;
    }

    if (resources.render_target && resources.width == width && resources.height == height) {
        return true;
    }

    // SRGBA8_UNORM does not support VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT on many GPUs.
    // When the render target is used as a storage image (secondary viewports), use a
    // storage-capable format (RGBA16_FLOAT) to avoid VK_ERROR_FORMAT_NOT_SUPPORTED.
    const Horizon::ImageUsageFlags target_usage = usage | Horizon::ImageUsageFlags::ColorAttachment;
    const Horizon::Format target_format =
        Horizon::has_flag(target_usage, Horizon::ImageUsageFlags::Storage) ? Horizon::Format::RGBA16_FLOAT : Horizon::Format::SRGBA8_UNORM;

    Horizon::HardwareImage new_target(Horizon::HardwareImageDesc::texture_2d(
        width,
        height,
        target_format,
        target_usage,
        "imgui.render_target"));
    if (!new_target) {
        CFW_LOG_ERROR("VulkanBackend: create render target failed ({}x{})", width, height);
        return false;
    }
    new_target.set_clear_color(0.0f, 0.0f, 0.0f, 0.0f);
    resources.render_target = std::move(new_target);
    resources.width = width;
    resources.height = height;
    resources.frame_ready = false;

    return true;
}

bool VulkanBackend::ensure_imgui_pipeline() {
    if (imgui_pipeline_ready_) {
        return true;
    }

    imgui_pipeline_.emplace(imgui_vert_glsl, imgui_frag_glsl, make_imgui_pipeline_desc());
    imgui_pipeline_ready_ = imgui_pipeline_->getRasterizerPipelineID() != 0;

    if (imgui_pipeline_ready_) {
        CFW_LOG_INFO("VulkanBackend: typed imgui pipeline created, pipeline_id={}",
                     imgui_pipeline_->getRasterizerPipelineID());
    } else {
        CFW_LOG_ERROR("VulkanBackend: typed imgui pipeline creation returned invalid pipeline id");
    }

    return imgui_pipeline_ready_;
}

bool VulkanBackend::ensure_font_texture() {
    if (font_ready_) {
        return true;
    }

    if (ImGui::GetCurrentContext() == nullptr) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) {
        return false;
    }

    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    if (pixels == nullptr || width <= 0 || height <= 0) {
        CFW_LOG_ERROR("VulkanBackend: font atlas data invalid");
        return false;
    }

    font_atlas_image_ = Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        Horizon::Format::SRGBA8_UNORM,
        Horizon::ImageUsageFlags::Sampled | Horizon::ImageUsageFlags::TransferDst,
        "imgui.font_atlas"));

    if (!font_atlas_image_) {
        CFW_LOG_ERROR("VulkanBackend: create font atlas image failed");
        return false;
    }

    const auto font_pixels = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(pixels),
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    font_upload_receipt_ =
        font_upload_executor_.stream()
        << font_atlas_image_.upload(font_pixels)
        << Horizon::commit();

    const uint32_t descriptor = font_atlas_image_.storeSampledDescriptor();
    io.Fonts->SetTexID(descriptor_to_texture_id(descriptor));

    font_ready_ = true;
    CFW_LOG_INFO("VulkanBackend: font atlas uploaded ({}x{}), descriptor={}", width, height, descriptor);
    return true;
}

// ============================================================================
// Multi-Viewport renderer callbacks
// ============================================================================

void VulkanBackend::renderer_create_window(ImGuiViewport* vp) {
    if (vp == nullptr || vp->PlatformHandleRaw == nullptr) {
        CFW_LOG_ERROR("VulkanBackend: renderer_create_window called with null viewport or handle");
        return;
    }

    auto* vd = IM_NEW(ViewportData)();
    vd->surface = vp->PlatformHandleRaw;

    // Initialize per-viewport pipeline (shares compiled VkPipeline via global ID,
    // but has independent renderTargets/geomMeshesRecord state).
    vd->pipeline.emplace(imgui_vert_glsl, imgui_frag_glsl, make_imgui_pipeline_desc());
    vd->pipeline_ready = vd->pipeline->getRasterizerPipelineID() != 0;
    if (!vd->pipeline_ready) {
        CFW_LOG_ERROR("VulkanBackend: failed to create pipeline for viewport {}", vp->ID);
        IM_DELETE(vd);
        return;
    }

    // 改造2: this viewport's UI layer flows through DisplaySystem. Allocate a shared
    // image_storage handle for it (mirrors the main window's image_handle_) and register
    // the surface so DisplaySystem creates the per-surface displayer that owns the swapchain.
    vd->image_handle = SharedDataHub::instance().image_storage().allocate();
    if (auto image_device = SharedDataHub::instance().image_storage().acquire_write(vd->image_handle)) {
        // keep-alive only; per-frame image/executor set in renderer_render_window
    }

    vp->RendererUserData = vd;

    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        event_bus->publish<Events::DisplaySurfaceChangedEvent>({vd->surface});
    }

    CFW_LOG_INFO("VulkanBackend: secondary viewport {} created (pipeline_id={}, ui_handle={})",
                 vp->ID, vd->pipeline->getRasterizerPipelineID(), vd->image_handle);
}

void VulkanBackend::renderer_destroy_window(ImGuiViewport* vp) {
    auto* vd = static_cast<ViewportData*>(vp->RendererUserData);
    if (vd == nullptr) {
        vp->RendererUserData = nullptr;
        return;
    }

    // Finish any GPU work referencing this viewport's UI render target before teardown.
    // 改造2: DisplaySystem owns the swapchain for this surface. It must destroy that
    // swapchain + VkSurfaceKHR BEFORE we let ImGui/SDL destroy the OS window, or the
    // Display thread could present to a dead window. EventBus dispatch is synchronous and
    // runs the handler on THIS (main) thread, so the handler only buffers the request;
    // the actual teardown + promise fulfillment happens on the Display thread's update().
    // Block here on the promise so destruction is ordered. Bounded wait avoids a hang if
    // the Display thread has already stopped (e.g. during shutdown).
    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        event_bus->publish<Events::DisplaySurfaceRemovedEvent>({vd->surface, done});
        if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            CFW_LOG_WARNING(
                "VulkanBackend: viewport {} surface teardown timed out; proceeding to destroy window",
                vp->ID);
        }
    }

    if (vd->image_handle != 0) {
        SharedDataHub::instance().image_storage().deallocate(vd->image_handle);
        vd->image_handle = 0;
    }

    IM_DELETE(vd);
    vp->RendererUserData = nullptr;
}

void VulkanBackend::renderer_set_window_size(ImGuiViewport* vp, ImVec2 size) {
    auto* vd = static_cast<ViewportData*>(vp->RendererUserData);
    if (vd == nullptr) {
        return;
    }

    (void)size;
    // ImGui passes logical window size here, while renderer_render_window uses
    // framebuffer size. Allocating here causes high-DPI camera viewports to
    // bounce between two sizes during live resize. Let the render path allocate
    // exactly once for the framebuffer dimensions it will actually draw into.
}

void VulkanBackend::renderer_render_window(ImGuiViewport* vp, void* /*render_arg*/) {
    auto* vd = static_cast<ViewportData*>(vp->RendererUserData);
    if (vd == nullptr || s_instance_ == nullptr) {
        return;
    }

    ImDrawData* draw_data = vp->DrawData;
    if (draw_data == nullptr) {
        return;
    }

    PixelExtent target_extent = draw_data_pixel_extent(draw_data);
    if (!target_extent) {
        target_extent = window_pixel_extent(sdl_window_from_viewport(vp));
    }
    if (!target_extent) {
        return;
    }

    if (!s_instance_->ensure_font_texture()) {
        return;
    }
    vd->resources.executor.wait(s_instance_->font_upload_receipt_);

    // Back-pressure: wait for DisplaySystem to finish consuming the previous frame's image
    // before overwriting it (mirrors VulkanBackend::new_frame() for the main window).
    if (vd->image_handle != 0) {
        if (auto image_device =
                SharedDataHub::instance().image_storage().acquire_write(vd->image_handle)) {
            vd->resources.executor.wait(image_device->consumed_receipt);
        }
    }

    if (!vd->pipeline ||
        !render_draw_data(draw_data, vd->resources, *vd->pipeline, s_instance_->font_atlas_image_,
                          target_extent.width, target_extent.height,
                          kUiRenderTargetUsage)) {
        return;
    }

    // Publish this viewport's UI layer to DisplaySystem (keyed by its native surface),
    // mirroring VulkanBackend::present_frame() for the main window.
    if (vd->image_handle != 0) {
        if (auto image_device =
                SharedDataHub::instance().image_storage().acquire_write(vd->image_handle)) {
            image_device->image = vd->resources.render_target;
            image_device->submit_receipt = vd->resources.executor.last_receipt();
        } else {
            return;
        }

        if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
            ++vd->frame_index;
            event_bus->publish<Events::UIFrameReadyEvent>({vd->surface,
                                                           vd->image_handle,
                                                           vd->frame_index,
                                                           vd->resources.width,
                                                           vd->resources.height});
        }
    }

    // Deferred show: make the OS window visible once its first frame has been queued for
    // compositing. DisplaySystem presents asynchronously, so we no longer gate on present.
    if (vd->pending_show) {
        if (s_original_platform_show_window) {
            s_original_platform_show_window(vp);
        }
        vd->pending_show = false;
    }
}

void VulkanBackend::renderer_swap_buffers(ImGuiViewport* vp, void* /*render_arg*/) {
    // 改造2: presentation is owned by DisplaySystem (per-surface composite of Optics+UI).
    // Nothing to present here; the UI layer was published in renderer_render_window.
    (void)vp;
}

}  // namespace Corona::Systems
