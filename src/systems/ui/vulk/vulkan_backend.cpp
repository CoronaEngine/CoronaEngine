#include <corona/systems/ui/vulkan_backend.h>

#include <corona/events/display_system_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/shared_data_hub.h>
#include <corona/engine/engine_runtime_api.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace {

[[nodiscard]] Corona::Horizon::RasterizerPipelineDesc make_ui_pipeline_desc() {
    Corona::Horizon::RasterizerPipelineDesc desc;
    desc.debug_name = "corona.ui_quad";
    desc.rasterizer.cull_mode = Corona::Horizon::CullMode::None;
    desc.depth_stencil.depth_test_enabled = false;
    desc.depth_stencil.depth_write_enabled = false;
    desc.depth_stencil.stencil_test_enabled = false;
    return desc;
}

constexpr auto kUiRenderTargetUsage =
    Corona::Horizon::ImageUsageFlags::Sampled | Corona::Horizon::ImageUsageFlags::Storage;

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

VulkanBackend::VulkanBackend(SDL_Window* window)
    : window_(window) {
}

VulkanBackend::PerSurfaceRender* VulkanBackend::find_surface(void* surface) {
    const auto it = surfaces_.find(surface);
    return it != surfaces_.end() ? it->second.get() : nullptr;
}

bool VulkanBackend::has_surface(void* surface) const {
    return surfaces_.contains(surface);
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

    // Register the main window's surface (allocates its image handle, creates initial RT).
    if (!register_surface(surface_, window_)) {
        CFW_LOG_ERROR("VulkanBackend: failed to register main surface");
        return false;
    }

    initialized_ = true;
    CFW_LOG_INFO("VulkanBackend: initialized");
    return true;
}

void VulkanBackend::shutdown() {
    if (!initialized_) {
        return;
    }

    // Release every surface's image handle and GPU resources.
    for (auto& [surface, render] : surfaces_) {
        if (render && render->image_handle != 0) {
            if (auto image = SharedDataHub::instance().image_storage().acquire_write(render->image_handle)) {
                render->resources.executor.wait(image->consumed_receipt);
            }
            render->resources.executor.wait_idle(render->resources.last_receipt);
            SharedDataHub::instance().image_storage().deallocate(render->image_handle);
            render->image_handle = 0;
        }
    }
    surfaces_.clear();

    pipeline_ready_ = false;
    pipeline_.reset();
    rebuild_needed_ = false;
    surface_ = nullptr;

    initialized_ = false;
    CFW_LOG_INFO("VulkanBackend: shutdown");
}

bool VulkanBackend::register_surface(void* surface, SDL_Window* window) {
    if (surface == nullptr) {
        return false;
    }
    if (surfaces_.contains(surface)) {
        return true;  // idempotent
    }

    auto render = std::make_unique<PerSurfaceRender>();
    render->image_handle = SharedDataHub::instance().image_storage().allocate();
    if (auto image_device =
            SharedDataHub::instance().image_storage().acquire_write(render->image_handle)) {
        // Keep storage entry alive; per-frame values updated in present_surface().
    } else {
        CFW_LOG_ERROR("VulkanBackend: failed to acquire image storage handle for surface {}", surface);
        SharedDataHub::instance().image_storage().deallocate(render->image_handle);
        return false;
    }

    // Best-effort initial render target from the window's current pixel size.
    if (window != nullptr) {
        int w = 0;
        int h = 0;
        if (SDL_GetWindowSizeInPixels(window, &w, &h) && w > 0 && h > 0) {
            ensure_render_target(render->resources, static_cast<uint32_t>(w),
                                 static_cast<uint32_t>(h), kUiRenderTargetUsage);
        }
    }

    surfaces_.emplace(surface, std::move(render));
    CFW_LOG_INFO("VulkanBackend: registered surface {}", surface);
    return true;
}

void VulkanBackend::unregister_surface(void* surface) {
    auto it = surfaces_.find(surface);
    if (it == surfaces_.end()) {
        return;
    }

    // Drain both Display's consumed receipt and our producer executor before releasing the
    // image handle. This is the same producer/consumer ordering used by resize/rebuild.
    auto& render = *it->second;
    if (render.image_handle != 0) {
        if (auto image = SharedDataHub::instance().image_storage().acquire_write(render.image_handle)) {
            render.resources.executor.wait(image->consumed_receipt);
        }
    }
    render.resources.executor.wait_idle(render.resources.last_receipt);

    if (render.image_handle != 0) {
        SharedDataHub::instance().image_storage().deallocate(render.image_handle);
        render.image_handle = 0;
    }
    surfaces_.erase(it);
    CFW_LOG_INFO("VulkanBackend: unregistered surface {}", surface);
}

void VulkanBackend::new_frame(void* surface) {
    if (!initialized_) {
        return;
    }
    auto* render = find_surface(surface);
    if (render == nullptr || render->image_handle == 0) {
        return;
    }

    // GPU sync: wait for Display to finish consuming our image before we render new content.
    if (auto image_device =
            SharedDataHub::instance().image_storage().acquire_write(render->image_handle)) {
        render->resources.executor.wait(image_device->consumed_receipt);
    }
}

bool VulkanBackend::ensure_pipeline() {
    if (pipeline_ready_) {
        return true;
    }

    pipeline_.emplace(ui_quad_vert_glsl, ui_quad_frag_glsl, make_ui_pipeline_desc());
    // getRasterizerPipelineID() 已移除；有效性改用 explicit operator bool()。
    pipeline_ready_ = static_cast<bool>(*pipeline_);

    if (pipeline_ready_) {
        CFW_LOG_INFO("VulkanBackend: ui quad pipeline created");
    } else {
        CFW_LOG_ERROR("VulkanBackend: ui quad pipeline creation returned invalid pipeline id");
    }

    return pipeline_ready_;
}

void VulkanBackend::render_quads(void* surface, std::span<const QuadDraw> quads,
                                 uint32_t target_pixels_w, uint32_t target_pixels_h) {
    if (!initialized_) {
        return;
    }
    auto* render = find_surface(surface);
    if (render == nullptr) {
        return;
    }
    if (!ensure_pipeline()) {
        return;
    }

    uint32_t tw = target_pixels_w;
    uint32_t th = target_pixels_h;
    if (tw == 0 || th == 0) {
        tw = render->resources.width;
        th = render->resources.height;
    }
    if (tw == 0 || th == 0) {
        return;
    }

    if (compositor_.composite(quads, render->resources, *pipeline_, tw, th, kUiRenderTargetUsage)) {
        render->resources.frame_ready = true;
    }
}

void VulkanBackend::present_surface(void* surface) {
    if (!initialized_) {
        return;
    }
    auto* render = find_surface(surface);
    if (render == nullptr || !render->resources.frame_ready ||
        !render->resources.render_target || render->image_handle == 0) {
        return;
    }

    if (auto image_device =
            SharedDataHub::instance().image_storage().acquire_write(render->image_handle)) {
        const auto submit_receipt = render->resources.last_receipt;
        if (submit_receipt.empty()) {
            CFW_LOG_WARNING(
                "VulkanBackend: publishing UI frame with empty submit receipt "
                "(surface={}, image_handle={}, frame={}, extent={}x{})",
                surface,
                render->image_handle,
                render->frame_index,
                render->resources.width,
                render->resources.height);
        }
        image_device->image = render->resources.render_target;
        image_device->submit_receipt = submit_receipt;
    } else {
        return;
    }

    // Only the main window drives the default-surface camera size.
    if (surface == surface_) {
        sync_default_surface_camera_size(surface, render->resources.width, render->resources.height);
    }

    if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
        ++render->frame_index;
        Events::UIFrameReadyEvent frame{surface,
                                        render->image_handle,
                                        render->frame_index,
                                        render->resources.width,
                                        render->resources.height};
        if (!render->first_present_published) {
            frame.first_present_ticket = render->first_present_ticket;
            render->first_present_published = true;
        }
        event_bus->publish<Events::UIFrameReadyEvent>(frame);
    }

    render->resources.frame_ready = false;
}

bool VulkanBackend::first_present_ready(void* surface) const {
    const auto it = surfaces_.find(surface);
    if (it == surfaces_.end()) return false;
    const auto result = it->second->first_present_ticket.result();
    return result && result->status == UI::DisplaySurfaceResult::Status::Succeeded;
}

void VulkanBackend::rebuild(void* surface, uint32_t pixel_w, uint32_t pixel_h) {
    if (!initialized_ || pixel_w == 0 || pixel_h == 0) {
        return;
    }
    auto* render = find_surface(surface);
    if (render == nullptr) {
        return;
    }

    const bool size_changed = !render->resources.render_target ||
                              render->resources.width != pixel_w ||
                              render->resources.height != pixel_h;
    if (size_changed) {
        if (render->image_handle != 0) {
            if (auto image_device =
                    SharedDataHub::instance().image_storage().acquire_write(render->image_handle)) {
                render->resources.executor.wait_idle(image_device->consumed_receipt);
            }
        }
        render->resources.executor.wait_idle(render->resources.last_receipt);
    }

    if (!ensure_render_target(render->resources, pixel_w, pixel_h, kUiRenderTargetUsage)) {
        if (surface == surface_) {
            rebuild_needed_ = true;
        }
        return;
    }

    if (surface == surface_) {
        sync_default_surface_camera_size(surface, pixel_w, pixel_h);
        rebuild_needed_ = false;
    }
}

uint32_t VulkanBackend::surface_width(void* surface) const {
    const auto it = surfaces_.find(surface);
    return it != surfaces_.end() ? it->second->resources.width : 0;
}

uint32_t VulkanBackend::surface_height(void* surface) const {
    const auto it = surfaces_.find(surface);
    return it != surfaces_.end() ? it->second->resources.height : 0;
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
    // When the render target is used as a storage image, use a storage-capable format
    // (RGBA16_FLOAT) to avoid VK_ERROR_FORMAT_NOT_SUPPORTED.
    const Horizon::ImageUsageFlags target_usage = usage | Horizon::ImageUsageFlags::ColorAttachment;
    const Horizon::Format target_format =
        Horizon::has_flag(target_usage, Horizon::ImageUsageFlags::Storage) ? Horizon::Format::RGBA16_FLOAT : Horizon::Format::SRGBA8_UNORM;

    Horizon::HardwareImage new_target(Horizon::HardwareImageDesc::texture_2d(
        width,
        height,
        target_format,
        target_usage,
        "ui.render_target"));
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

}  // namespace Corona::Systems
