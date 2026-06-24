#include <corona/events/display_system_events.h>
#include <corona/events/engine_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/display/display_system.h>

#include <algorithm>
#include <array>
#include <ranges>
#include <span>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
struct PixelExtent {
    uint32_t width = 0;
    uint32_t height = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return width != 0 && height != 0;
    }
};

[[nodiscard]] PixelExtent hardware_image_extent(const Corona::Horizon::HardwareImage& image) {
    if (!image) {
        return {};
    }
    const auto extent = image.extent();
    return {extent.width, extent.height};
}

[[nodiscard]] PixelExtent max_extent(PixelExtent lhs, PixelExtent rhs) {
    return {std::max(lhs.width, rhs.width), std::max(lhs.height, rhs.height)};
}

[[nodiscard]] PixelExtent surface_client_extent(void* surface) {
#ifdef _WIN32
    if (surface == nullptr) {
        return {};
    }

    RECT rect{};
    if (!GetClientRect(reinterpret_cast<HWND>(surface), &rect)) {
        return {};
    }

    const auto width = rect.right - rect.left;
    const auto height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        return {};
    }
    return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
#else
    (void)surface;
    return {};
#endif
}
}  // namespace

namespace Corona::Systems {
bool DisplaySystem::initialize(Kernel::ISystemContext* ctx) {
    auto* event_bus = ctx->event_bus();
    if (event_bus == nullptr) {
        CFW_LOG_WARNING("DisplaySystem: No event bus available");
        return true;
    }

    surface_changed_sub_id_ = event_bus->subscribe<Events::DisplaySurfaceChangedEvent>(
        [this](const Events::DisplaySurfaceChangedEvent& event) {
            if (event.surface == nullptr) {
                return;
            }

            const auto surface_id = reinterpret_cast<uint64_t>(event.surface);
            std::lock_guard<std::mutex> lock(frame_mutex_);
            removed_surfaces_.erase(surface_id);
            surfaces_[surface_id] = event.surface;
            pending_surfaces_.push_back(event.surface);
        });

    // Published synchronously on the MAIN thread when an ImGui secondary viewport window
    // is being destroyed. We only buffer the request (+ promise) here and return; the
    // actual GPU-idle + displayer teardown happens in update() on the Display thread,
    // which then fulfills the promise. The publisher (main thread) blocks on that promise
    // so the OS window is not destroyed until our swapchain is gone. Must NOT block here:
    // this handler runs on the main thread, and blocking while holding frame_mutex_ would
    // deadlock against update()'s own frame_mutex_ acquisition.
    surface_removed_sub_id_ = event_bus->subscribe<Events::DisplaySurfaceRemovedEvent>(
        [this](const Events::DisplaySurfaceRemovedEvent& event) {
            if (event.surface == nullptr) {
                // Nothing to tear down; fulfill immediately so the publisher does not hang.
                if (event.done) {
                    event.done->set_value();
                }
                return;
            }

            const auto surface_id = reinterpret_cast<uint64_t>(event.surface);
            std::lock_guard<std::mutex> lock(frame_mutex_);
            removed_surfaces_.insert(surface_id);
            surfaces_.erase(surface_id);
            surface_states_.erase(surface_id);
            pending_surfaces_.erase(
                std::remove_if(pending_surfaces_.begin(), pending_surfaces_.end(),
                               [surface_id](void* s) {
                                   return reinterpret_cast<uint64_t>(s) == surface_id;
                               }),
                pending_surfaces_.end());
            pending_removals_.push_back({event.surface, event.done});
        });

    optics_frame_sub_id_ = event_bus->subscribe<Events::OpticsFrameReadyEvent>(
        [this](const Events::OpticsFrameReadyEvent& event) {
            if (event.surface == nullptr ||
                event.image_handle == 0) {
                return;
            }

            const auto surface_id = reinterpret_cast<uint64_t>(event.surface);
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (removed_surfaces_.contains(surface_id)) {
                return;
            }
            auto& layer = surface_states_[surface_id].optics;
            if (event.frame_index >= layer.frame_index) {
                layer.image_handle = event.image_handle;
                layer.frame_index = event.frame_index;
                layer.width = event.width;
                layer.height = event.height;
                layer.viewport_x = event.viewport_x;
                layer.viewport_y = event.viewport_y;
                layer.viewport_width = event.viewport_width;
                layer.viewport_height = event.viewport_height;
            }
        });

    ui_frame_sub_id_ = event_bus->subscribe<Events::UIFrameReadyEvent>(
        [this](const Events::UIFrameReadyEvent& event) {
            if (event.surface == nullptr ||
                event.image_handle == 0) {
                return;
            }

            const auto surface_id = reinterpret_cast<uint64_t>(event.surface);
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (removed_surfaces_.contains(surface_id)) {
                return;
            }
            auto& layer = surface_states_[surface_id].ui;
            if (event.frame_index >= layer.frame_index) {
                layer.image_handle = event.image_handle;
                layer.frame_index = event.frame_index;
                layer.width = event.width;
                layer.height = event.height;
            }
        });

    // Create 1x1 transparent fallback images for single-layer compositing.
    // Porter-Duff Source Over with a transparent layer is an identity operation.
    auto transparent_storage_desc = Horizon::HardwareImageDesc::texture_2d(
        1,
        1,
        Horizon::Format::RGBA16_FLOAT,
        Horizon::ImageUsageFlags::Storage | Horizon::ImageUsageFlags::TransferDst,
        "display.transparent_storage");
    transparent_storage_desc.cpu_access = Horizon::CpuAccessMode::Write;
    transparent_storage_ = Horizon::HardwareImage(transparent_storage_desc);

    if (transparent_storage_) {
        const std::array<std::uint16_t, 4> zero_rgba16f = {0, 0, 0, 0};
        (void)transparent_storage_.write(std::span<const std::uint16_t>(zero_rgba16f));
    }

    return true;
}

void DisplaySystem::update() {
    // Snapshot shared state and process pending displayer creation under lock,
    // then release before GPU work. displayers_ is only modified here, so
    // iterating it after the lock is safe.
    std::unordered_map<uint64_t, SurfaceState> states_snapshot;
    std::unordered_map<uint64_t, void*> surfaces_snapshot;
    std::vector<PendingRemoval> removals;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);

        // Drain teardown requests first. Drop any matching state and any not-yet-created
        // surface so the creation loop below does not resurrect a surface being removed.
        removals.swap(pending_removals_);
        if (!removals.empty()) {
            for (const auto& r : removals) {
                const auto surface_id = reinterpret_cast<uint64_t>(r.surface);
                removed_surfaces_.insert(surface_id);
                surfaces_.erase(surface_id);
                surface_states_.erase(surface_id);
            }
            pending_surfaces_.erase(
                std::remove_if(pending_surfaces_.begin(), pending_surfaces_.end(),
                               [&](void* s) {
                                   const auto sid = reinterpret_cast<uint64_t>(s);
                                   for (const auto& r : removals) {
                                       if (reinterpret_cast<uint64_t>(r.surface) == sid) {
                                           return true;
                                       }
                                   }
                                   return false;
                               }),
                pending_surfaces_.end());
        }

        for (auto* surface : pending_surfaces_) {
            const auto surface_id = reinterpret_cast<uint64_t>(surface);
            surfaces_[surface_id] = surface;
            if (!displayers_.contains(surface_id)) {
                displayers_.emplace(surface_id, Horizon::HardwareDisplayer(surface));
            }
        }
        pending_surfaces_.clear();
        states_snapshot = surface_states_;
        surfaces_snapshot = surfaces_;
    }

    // Destroy displayers OUTSIDE the lock (displayers_ is touched only on this thread).
    // ~HardwareDisplayer → cleanUpDisplayManager() runs vkDeviceWaitIdle before destroying
    // the swapchain + VkSurfaceKHR, so no present is in flight and the surface is gone
    // before the main thread destroys the OS window. Fulfilling the promise unblocks the
    // main thread (the publisher of DisplaySurfaceRemovedEvent) to proceed with that.
    for (auto& r : removals) {
        const auto surface_id = reinterpret_cast<uint64_t>(r.surface);
        displayers_.erase(surface_id);
        composite_resources_.erase(surface_id);
        if (r.done) {
            r.done->set_value();
        }
    }

    for (auto& [surface_id, displayer] : displayers_) {
        auto it = states_snapshot.find(surface_id);
        if (it == states_snapshot.end()) {
            continue;
        }

        auto& state = it->second;
        const bool has_optics = state.optics.image_handle != 0;
        const bool has_ui = state.ui.image_handle != 0;

        if (!has_optics && !has_ui) {
            continue;
        }

        // Acquire write handles for available layers
        SharedDataHub::ImageStorage::WriteHandle optics_frame;
        SharedDataHub::ImageStorage::WriteHandle ui_frame;
        if (has_optics) {
            optics_frame = SharedDataHub::instance().image_storage().acquire_write(state.optics.image_handle);
        }
        if (has_ui) {
            ui_frame = SharedDataHub::instance().image_storage().acquire_write(state.ui.image_handle);
        }

        // Resolve images: use producer image if available, transparent fallback otherwise.
        Horizon::HardwareImage* optics_img_ptr = nullptr;
        const Horizon::SubmitReceipt* optics_receipt_ptr = nullptr;
        if (has_optics && optics_frame) {
            optics_img_ptr = &optics_frame->image;
            optics_receipt_ptr = &optics_frame->submit_receipt;
        }

        Horizon::HardwareImage* ui_img_ptr = nullptr;
        const Horizon::SubmitReceipt* ui_receipt_ptr = nullptr;
        if (has_ui && ui_frame) {
            ui_img_ptr = &ui_frame->image;
            ui_receipt_ptr = &ui_frame->submit_receipt;
        }

        Horizon::HardwareImage& bg_image = (optics_img_ptr && *optics_img_ptr) ? *optics_img_ptr : transparent_storage_;
        Horizon::HardwareImage& fg_image = (ui_img_ptr && *ui_img_ptr) ? *ui_img_ptr : transparent_storage_;

        if (!bg_image || !fg_image) {
            continue;
        }

        auto& composite_resources = composite_resources_[surface_id];
        void* surface = nullptr;
        if (auto surface_it = surfaces_snapshot.find(surface_id);
            surface_it != surfaces_snapshot.end()) {
            surface = surface_it->second;
        }
        if (!compose_and_present(
                displayer,
                surface,
                state,
                composite_resources,
                bg_image,
                (optics_img_ptr && *optics_img_ptr) ? optics_receipt_ptr : nullptr,
                fg_image,
                (ui_img_ptr && *ui_img_ptr) ? ui_receipt_ptr : nullptr)) {
            continue;
        }

        // Write back the consumed signal so producers know when to safely reuse their image.
        const Horizon::SubmitReceipt consumed_receipt = composite_resources.executor.last_receipt();
        if (has_optics && optics_frame) {
            optics_frame->consumed_receipt = consumed_receipt;
        }
        if (has_ui && ui_frame) {
            ui_frame->consumed_receipt = consumed_receipt;
        }
    }
}

bool DisplaySystem::ensure_composite_resources(CompositeResources& resources,
                                               uint32_t width,
                                               uint32_t height) {
    if (!composite_pipeline_ready_) {
        if (!composite_pipeline_) {
            composite_pipeline_.emplace(composite_comp_glsl, ktm::uvec3(8, 8, 1));
        }
        composite_pipeline_ready_ = composite_pipeline_->getComputePipelineID() != 0;
        if (!composite_pipeline_ready_) {
            CFW_LOG_ERROR("DisplaySystem: Failed to create typed composite pipeline");
            return false;
        }
    }

    if (resources.width != width || resources.height != height || !resources.output) {
        resources.executor.wait_idle(resources.executor.last_receipt());
        resources.output = Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(
            width,
            height,
            Horizon::Format::RGBA16_FLOAT,
            Horizon::ImageUsageFlags::Storage | Horizon::ImageUsageFlags::ColorAttachment |
                Horizon::ImageUsageFlags::Sampled | Horizon::ImageUsageFlags::TransferSrc |
                Horizon::ImageUsageFlags::TransferDst,
            "display.composite_output"));
        if (!resources.output) {
            CFW_LOG_ERROR("DisplaySystem: Failed to create composite output ({}x{})", width, height);
            return false;
        }
        resources.width = width;
        resources.height = height;
    }

    return true;
}

bool DisplaySystem::compose_and_present(Horizon::HardwareDisplayer& displayer,
                                        void* surface,
                                        SurfaceState& state,
                                        CompositeResources& resources,
                                        Horizon::HardwareImage& optics_image,
                                        const Horizon::SubmitReceipt* optics_receipt,
                                        Horizon::HardwareImage& ui_image,
                                        const Horizon::SubmitReceipt* ui_receipt) {
    const PixelExtent optics_extent = hardware_image_extent(optics_image);
    const PixelExtent ui_extent = hardware_image_extent(ui_image);

    const PixelExtent state_optics_extent{state.optics.width, state.optics.height};
    const PixelExtent state_ui_extent{state.ui.width, state.ui.height};
    PixelExtent output_extent = surface_client_extent(surface);
    if (!output_extent) {
        output_extent = max_extent(optics_extent, ui_extent);
    }
    if (!output_extent) {
        output_extent = max_extent(state_optics_extent, state_ui_extent);
    }
    if (!output_extent) {
        return false;
    }

    const PixelExtent bg_extent = optics_extent ? optics_extent : state_optics_extent;
    const PixelExtent fg_extent = ui_extent ? ui_extent : state_ui_extent;
    const uint32_t output_width = output_extent.width;
    const uint32_t output_height = output_extent.height;

    if (!ensure_composite_resources(resources, output_width, output_height)) {
        return false;
    }

    auto& composite_pipeline = *composite_pipeline_;
    composite_pipeline.pushConsts.bgImage = optics_image.storeStorageDescriptor();
    composite_pipeline.pushConsts.fgImage = ui_image.storeStorageDescriptor();
    composite_pipeline.pushConsts.outputImage = resources.output.storeStorageDescriptor();
    composite_pipeline.pushConsts.outputWidth = output_width;
    composite_pipeline.pushConsts.outputHeight = output_height;
    composite_pipeline.pushConsts.bgWidth = std::max(bg_extent.width, 1u);
    composite_pipeline.pushConsts.bgHeight = std::max(bg_extent.height, 1u);
    composite_pipeline.pushConsts.fgWidth = std::max(fg_extent.width, 1u);
    composite_pipeline.pushConsts.fgHeight = std::max(fg_extent.height, 1u);
    composite_pipeline.pushConsts.bgViewportX = state.optics.viewport_x;
    composite_pipeline.pushConsts.bgViewportY = state.optics.viewport_y;
    composite_pipeline.pushConsts.bgViewportWidth =
        state.optics.viewport_width != 0 ? state.optics.viewport_width : output_width;
    composite_pipeline.pushConsts.bgViewportHeight =
        state.optics.viewport_height != 0 ? state.optics.viewport_height : output_height;
    composite_pipeline.pushConsts.fgOpaque =
        (state.ui.image_handle != 0 && state.optics.image_handle == 0) ? 1u : 0u;
    composite_pipeline.bind_storage_image(0, optics_image);
    composite_pipeline.bind_storage_image(1, ui_image);
    composite_pipeline.bind_storage_image(2, resources.output);

    const uint32_t dispatch_x = output_width;
    const uint32_t dispatch_y = output_height;

    // GPU sync: wait for each producer's rendering to finish before reading their images
    if (optics_receipt != nullptr && !optics_receipt->empty()) {
        resources.executor.wait(*optics_receipt);
    }
    if (ui_receipt != nullptr && !ui_receipt->empty()) {
        resources.executor.wait(*ui_receipt);
    }

    const Horizon::SubmitReceipt composite_receipt =
        resources.executor.stream()
        << composite_pipeline(dispatch_x, dispatch_y, 1)
        << Horizon::commit();

    resources.executor.wait(composite_receipt);
    (void)(resources.executor.stream()
           << Horizon::present(displayer, resources.output)
           << Horizon::commit());
    return true;
}

void DisplaySystem::shutdown() {
    if (auto* event_bus = context()->event_bus()) {
        if (surface_changed_sub_id_ != 0) {
            event_bus->unsubscribe(surface_changed_sub_id_);
        }
        if (surface_removed_sub_id_ != 0) {
            event_bus->unsubscribe(surface_removed_sub_id_);
        }
        if (optics_frame_sub_id_ != 0) {
            event_bus->unsubscribe(optics_frame_sub_id_);
        }
        if (ui_frame_sub_id_ != 0) {
            event_bus->unsubscribe(ui_frame_sub_id_);
        }
    }

    // Fulfill any outstanding teardown promises so a main thread blocked in
    // renderer_destroy_window cannot hang past Display-thread shutdown.
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        for (auto& r : pending_removals_) {
            if (r.done) {
                r.done->set_value();
            }
        }
        pending_removals_.clear();
    }

    composite_pipeline_ready_ = false;
    composite_pipeline_.reset();
    surface_states_.clear();
    removed_surfaces_.clear();
    surfaces_.clear();
    displayers_.clear();
    composite_resources_.clear();
    transparent_storage_ = Horizon::HardwareImage();
}

}  // namespace Corona::Systems
