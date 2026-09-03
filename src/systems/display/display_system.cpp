#include <corona/events/display_system_events.h>
#include <corona/events/engine_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/display/display_system.h>

#include <algorithm>
#include <array>
#include <exception>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

[[nodiscard]] bool is_vulkan_device_lost_message(std::string_view message) {
    return message.find("VK_ERROR_DEVICE_LOST") != std::string_view::npos ||
           message.find("VkResult=-4") != std::string_view::npos ||
           message.find("Vulkan device is lost") != std::string_view::npos ||
           message.find("Queue acquire skipped because the Vulkan device is lost") != std::string_view::npos ||
           message.find("vkGetSemaphoreCounterValue returned UINT64_MAX") != std::string_view::npos;
}
}  // namespace

namespace Corona::Systems {
DisplaySystem::~DisplaySystem() {
    stop();
    shutdown();
}

void DisplaySystem::handle_surface_changed(
    const Events::DisplaySurfaceChangedEvent& event) {
    if (event.surface == nullptr) {
        if (event.registration_ticket) {
            event.registration_ticket->fail(
                "Display surface registration failed: surface is null");
        }
        return;
    }

    const auto surface_id = reinterpret_cast<uint64_t>(event.surface);
    std::shared_ptr<Detail::SurfaceLifecycleAcks> acknowledgements;
    std::string rejection;
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (device_lost_.load(std::memory_order_acquire)) {
            rejection =
                "Display surface registration failed: Vulkan device is lost";
        } else if (!surface_frame_coordinator_.activate(surface_id)) {
            rejection =
                "Display surface registration cancelled: removal is pending";
            cancelled = true;
        } else {
            const bool begins_new_lifetime =
                removed_surfaces_.erase(surface_id) != 0;
            auto& stored = surface_acknowledgements_[surface_id];
            if (begins_new_lifetime || !stored) {
                stored = std::make_shared<Detail::SurfaceLifecycleAcks>(
                    forward_completion_fence_);
            }
            acknowledgements = stored;
            surfaces_[surface_id] = event.surface;
            pending_surfaces_.push_back(event.surface);
        }
    }

    if (acknowledgements) {
        acknowledgements->add_registration(event.registration_ticket);
    } else if (event.registration_ticket) {
        if (cancelled) {
            event.registration_ticket->cancel(std::move(rejection));
        } else {
            event.registration_ticket->fail(std::move(rejection));
        }
    }
}

void DisplaySystem::handle_surface_removed(
    const Events::DisplaySurfaceRemovedEvent& event) {
    if (event.surface == nullptr) {
        Detail::SurfaceLifecycleAcks acknowledgements{
            forward_completion_fence_};
        acknowledgements.removal_requested(event.removal_ticket, event.done);
        acknowledgements.removal_succeeded();
        return;
    }

    const auto surface_id = reinterpret_cast<uint64_t>(event.surface);
    auto retirement = surface_frame_coordinator_.retire(surface_id);
    std::shared_ptr<Detail::SurfaceLifecycleAcks> acknowledgements;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        auto& stored = surface_acknowledgements_[surface_id];
        if (!stored) {
            stored = std::make_shared<Detail::SurfaceLifecycleAcks>(
                forward_completion_fence_);
        }
        acknowledgements = stored;
        removed_surfaces_.insert(surface_id);
        surfaces_.erase(surface_id);
        surface_states_.erase(surface_id);
        pending_surfaces_.erase(
            std::remove_if(pending_surfaces_.begin(),
                           pending_surfaces_.end(),
                           [surface_id](void* surface) {
                               return reinterpret_cast<uint64_t>(surface) ==
                                      surface_id;
                           }),
            pending_surfaces_.end());
        pending_removals_.push_back(
            {event.surface, std::move(retirement), acknowledgements});
    }
    acknowledgements->removal_requested(event.removal_ticket, event.done);
}

void DisplaySystem::handle_optics_frame(
    const Events::OpticsFrameReadyEvent& event) {
    if (event.surface == nullptr || event.image_handle == 0) {
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
}

void DisplaySystem::handle_ui_frame(const Events::UIFrameReadyEvent& event) {
    if (event.surface == nullptr) {
        if (event.first_present_ticket) {
            event.first_present_ticket->fail(
                "Display first present failed: surface is null");
        }
        return;
    }

    const auto surface_id = reinterpret_cast<uint64_t>(event.surface);
    std::shared_ptr<Detail::SurfaceLifecycleAcks> acknowledgements;
    bool removed = false;
    bool device_lost = false;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        removed = removed_surfaces_.contains(surface_id);
        if (!removed) {
            auto& stored = surface_acknowledgements_[surface_id];
            if (!stored) {
                stored = std::make_shared<Detail::SurfaceLifecycleAcks>(
                    forward_completion_fence_);
            }
            acknowledgements = stored;
            const auto first_present_boundary =
                acknowledgements->add_first_present(
                    event.first_present_ticket);
            device_lost = device_lost_.load(std::memory_order_acquire);
            if (!device_lost && event.image_handle != 0) {
                auto& layer = surface_states_[surface_id].ui;
                if (event.frame_index >= layer.frame_index) {
                    layer.image_handle = event.image_handle;
                    layer.frame_index = event.frame_index;
                    layer.width = event.width;
                    layer.height = event.height;
                    layer.first_present_boundary = first_present_boundary;
                }
            }
        }
    }

    if (removed) {
        if (event.first_present_ticket) {
            event.first_present_ticket->cancel(
                "Display first present cancelled: surface is removed");
        }
        return;
    }
    if (device_lost) {
        acknowledgements->fail_forward(
            "Display first present failed: Vulkan device is lost");
    }
}

bool DisplaySystem::initialize(Kernel::ISystemContext* ctx) {
    if (callback_gate_) {
        stop();
        shutdown();
    }
    device_lost_.store(false, std::memory_order_release);
    forward_completion_fence_ =
        std::make_shared<Detail::ForwardCompletionFence>();
    const auto forward_completion_fence = forward_completion_fence_;
    callback_gate_ = std::make_shared<CallbackGate>(*this);
    const auto callback_gate = callback_gate_;
    auto* event_bus = ctx->event_bus();
    if (event_bus == nullptr) {
        CFW_LOG_WARNING("DisplaySystem: No event bus available");
        return true;
    }

    surface_changed_sub_id_ = event_bus->subscribe<Events::DisplaySurfaceChangedEvent>(
        [callback_gate, forward_completion_fence](
            const Events::DisplaySurfaceChangedEvent& event) {
            auto access = callback_gate->try_acquire();
            if (!access || !forward_completion_fence->run([&]() {
                    access.owner().handle_surface_changed(event);
                })) {
                callback_gate->defer_registration(event.registration_ticket);
            }
        });
    surface_removed_sub_id_ = event_bus->subscribe<Events::DisplaySurfaceRemovedEvent>(
        [callback_gate, forward_completion_fence](
            const Events::DisplaySurfaceRemovedEvent& event) {
            auto access = callback_gate->try_acquire();
            if (!access || !forward_completion_fence->run([&]() {
                    access.owner().handle_surface_removed(event);
                })) {
                callback_gate->defer_removal(event.removal_ticket, event.done);
            }
        });
    optics_frame_sub_id_ = event_bus->subscribe<Events::OpticsFrameReadyEvent>(
        [callback_gate, forward_completion_fence](
            const Events::OpticsFrameReadyEvent& event) {
            auto access = callback_gate->try_acquire();
            if (access) {
                (void)forward_completion_fence->run([&]() {
                    access.owner().handle_optics_frame(event);
                });
            }
        });
    ui_frame_sub_id_ = event_bus->subscribe<Events::UIFrameReadyEvent>(
        [callback_gate, forward_completion_fence](
            const Events::UIFrameReadyEvent& event) {
            auto access = callback_gate->try_acquire();
            if (!access || !forward_completion_fence->run([&]() {
                    access.owner().handle_ui_frame(event);
                })) {
                callback_gate->defer_first_present(event.first_present_ticket);
            }
        });

    const auto initialization_failed =
        [this,
         event_bus,
         callback_gate,
         forward_completion_fence](std::string message) {
            forward_completion_fence->close();
            callback_gate->close();
            if (surface_changed_sub_id_) {
                event_bus->unsubscribe(*surface_changed_sub_id_);
                surface_changed_sub_id_.reset();
            }
            if (surface_removed_sub_id_) {
                event_bus->unsubscribe(*surface_removed_sub_id_);
                surface_removed_sub_id_.reset();
            }
            if (optics_frame_sub_id_) {
                event_bus->unsubscribe(*optics_frame_sub_id_);
                optics_frame_sub_id_.reset();
            }
            if (ui_frame_sub_id_) {
                event_bus->unsubscribe(*ui_frame_sub_id_);
                ui_frame_sub_id_.reset();
            }
            callback_gate->wait_for_quiescence();

            std::unordered_set<std::uint64_t> surface_ids;
            std::vector<std::shared_ptr<Detail::SurfaceLifecycleAcks>>
                acknowledgements;
            const auto remember_acknowledgements =
                [&acknowledgements](
                    const std::shared_ptr<Detail::SurfaceLifecycleAcks>&
                        pending) {
                    if (pending &&
                        std::find(acknowledgements.begin(),
                                  acknowledgements.end(),
                                  pending) == acknowledgements.end()) {
                        acknowledgements.push_back(pending);
                    }
                };
            {
                std::lock_guard lock(frame_mutex_);
                acknowledgements.reserve(surface_acknowledgements_.size() +
                                         pending_removals_.size());
                for (const auto& [surface_id, pending_acknowledgements] :
                     surface_acknowledgements_) {
                    surface_ids.insert(surface_id);
                    remember_acknowledgements(pending_acknowledgements);
                }
                for (const auto& removal : pending_removals_) {
                    surface_ids.insert(
                        reinterpret_cast<std::uint64_t>(removal.surface));
                    remember_acknowledgements(removal.acknowledgements);
                }
                for (const auto& [surface_id, surface] : surfaces_) {
                    (void)surface;
                    surface_ids.insert(surface_id);
                }
                for (const auto& [surface_id, state] : surface_states_) {
                    (void)state;
                    surface_ids.insert(surface_id);
                }
                for (const auto surface_id : removed_surfaces_) {
                    surface_ids.insert(surface_id);
                }
                for (const auto* surface : pending_surfaces_) {
                    surface_ids.insert(
                        reinterpret_cast<std::uint64_t>(surface));
                }
                pending_surfaces_.clear();
                pending_removals_.clear();
            }

            for (const auto& [surface_id, displayer] : displayers_) {
                (void)displayer;
                surface_ids.insert(surface_id);
            }
            for (const auto& [surface_id, resources] : composite_resources_) {
                (void)resources;
                surface_ids.insert(surface_id);
            }

            std::vector<std::pair<
                std::uint64_t,
                Detail::SurfaceFrameCoordinator::Retirement>>
                retirements;
            retirements.reserve(surface_ids.size());
            for (const auto surface_id : surface_ids) {
                retirements.emplace_back(
                    surface_id,
                    surface_frame_coordinator_.retire(surface_id));
            }
            for (const auto& [surface_id, retirement] : retirements) {
                (void)surface_id;
                retirement.wait();
            }

            displayers_.clear();
            composite_resources_.clear();
            composite_pipeline_.reset();
            composite_pipeline_ready_ = false;
            transparent_storage_ = Horizon::HardwareImage();
            for (const auto& [surface_id, retirement] : retirements) {
                (void)retirement;
                surface_frame_coordinator_.forget(surface_id);
            }
            {
                std::lock_guard lock(frame_mutex_);
                surface_states_.clear();
                surfaces_.clear();
                removed_surfaces_.clear();
                surface_acknowledgements_.clear();
            }

            for (const auto& pending_acknowledgements : acknowledgements) {
                if (pending_acknowledgements) {
                    pending_acknowledgements->fail_forward(message);
                    pending_acknowledgements->removal_succeeded();
                }
            }
            callback_gate->complete_deferred_after_resources_destroyed(
                UI::DisplaySurfaceResult::Status::Failed,
                message);
            CFW_LOG_ERROR("DisplaySystem: {}", message);
            return false;
        };

    // Create 1x1 transparent fallback images for single-layer compositing.
    // Porter-Duff Source Over with a transparent layer is an identity operation.
    try {
        const std::array<std::uint16_t, 4> zero_rgba16f = {0, 0, 0, 0};
        auto transparent_storage_desc = Horizon::HardwareImageDesc::texture_2d(
            1,
            1,
            Horizon::Format::RGBA16_FLOAT,
            Horizon::ImageUsage_Storage |
                Horizon::ImageUsage_TransferDst,
            "display.transparent_storage");
        transparent_storage_desc.cpu_access = Horizon::CpuAccessMode::Write;

        // 新版 Horizon: 在构造时上传初始数据
        transparent_storage_ = Horizon::HardwareImage(
            transparent_storage_desc,
            std::as_bytes(std::span(zero_rgba16f)));

        if (!transparent_storage_) {
            return initialization_failed(
                "initialization failed: transparent fallback image was not created");
        }
    } catch (const std::exception& error) {
        return initialization_failed(
            std::string("initialization failed while creating fallback image: ") +
            error.what());
    } catch (...) {
        return initialization_failed(
            "initialization failed while creating fallback image: unknown exception");
    }

    frame_submission_enabled_.store(true, std::memory_order_release);
    return true;
}

void DisplaySystem::stop() {
    CFW_LOG_INFO("DisplaySystem: stop requested");
    frame_submission_enabled_.store(false, std::memory_order_release);
    Kernel::SystemBase::stop();
    CFW_LOG_INFO("DisplaySystem: all frame leases retired");
}

void DisplaySystem::on_thread_stopped() {
    CFW_LOG_INFO("DisplaySystem: update loop exited");
}

void DisplaySystem::update() {
    if (!frame_submission_enabled_.load(std::memory_order_acquire)) {
        return;
    }
    // Snapshot shared state and process pending displayer creation under lock,
    // then release before GPU work. displayers_ is only modified here, so
    // iterating it after the lock is safe.
    std::unordered_map<uint64_t, SurfaceState> states_snapshot;
    std::unordered_map<uint64_t, void*> surfaces_snapshot;
    std::unordered_map<uint64_t, Detail::SurfaceFrameCoordinator::Snapshot>
        frame_gate_snapshot;
    std::unordered_map<uint64_t, std::shared_ptr<Detail::SurfaceLifecycleAcks>>
        acknowledgements_snapshot;
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
            const auto acknowledgements_it =
                surface_acknowledgements_.find(surface_id);
            const auto acknowledgements =
                acknowledgements_it != surface_acknowledgements_.end()
                    ? acknowledgements_it->second
                    : nullptr;

            if (removed_surfaces_.contains(surface_id)) {
                if (acknowledgements) {
                    acknowledgements->cancel_forward(
                        "Display surface registration cancelled: surface is removed");
                }
                continue;
            }
            if (device_lost_.load(std::memory_order_acquire)) {
                if (acknowledgements) {
                    acknowledgements->registration_failed(
                        "Display surface registration failed: Vulkan device is lost");
                }
                continue;
            }

            surfaces_[surface_id] = surface;
            try {
                auto displayer_it = displayers_.find(surface_id);
                if (displayer_it == displayers_.end()) {
                    displayer_it =
                        displayers_.try_emplace(surface_id, surface).first;
                }
                if (!displayer_it->second) {
                    displayers_.erase(displayer_it);
                    throw std::runtime_error(
                        "constructor returned an invalid displayer");
                }
                if (acknowledgements) {
                    acknowledgements->registration_succeeded();
                }
            } catch (const std::exception& error) {
                removed_surfaces_.insert(surface_id);
                surfaces_.erase(surface_id);
                surface_states_.erase(surface_id);
                (void)surface_frame_coordinator_.retire(surface_id);
                if (acknowledgements) {
                    acknowledgements->registration_failed(
                        std::string("HardwareDisplayer construction failed: ") +
                        error.what());
                }
                CFW_LOG_ERROR(
                    "DisplaySystem: HardwareDisplayer construction failed "
                    "(surface={}): {}",
                    surface,
                    error.what());
            } catch (...) {
                removed_surfaces_.insert(surface_id);
                surfaces_.erase(surface_id);
                surface_states_.erase(surface_id);
                (void)surface_frame_coordinator_.retire(surface_id);
                if (acknowledgements) {
                    acknowledgements->registration_failed(
                        "HardwareDisplayer construction failed: unknown exception");
                }
                CFW_LOG_ERROR(
                    "DisplaySystem: HardwareDisplayer construction failed with "
                    "unknown exception (surface={})",
                    surface);
            }
        }
        pending_surfaces_.clear();
        states_snapshot = surface_states_;
        surfaces_snapshot = surfaces_;
        acknowledgements_snapshot = surface_acknowledgements_;
        for (const auto& [surface_id, state] : states_snapshot) {
            (void)state;
            frame_gate_snapshot.emplace(
                surface_id, surface_frame_coordinator_.capture(surface_id));
        }
    }

    // Destroy displayers OUTSIDE the lock (displayers_ is touched only on this thread).
    // ~HardwareDisplayer → cleanUpDisplayManager() runs vkDeviceWaitIdle before destroying
    // the swapchain + VkSurfaceKHR, so no present is in flight and the surface is gone
    // before the main thread destroys the OS window. Fulfilling the promise unblocks the
    // main thread (the publisher of DisplaySurfaceRemovedEvent) to proceed with that.
    for (auto& r : removals) {
        const auto surface_id = reinterpret_cast<uint64_t>(r.surface);
        surface_frame_coordinator_.teardown(
            surface_id,
            r.retirement,
            [&]() { displayers_.erase(surface_id); },
            [&]() { composite_resources_.erase(surface_id); },
            [&]() {
                if (r.acknowledgements) {
                    r.acknowledgements->removal_succeeded();
                }
            });
    }

    if (device_lost_.load(std::memory_order_acquire)) {
        return;
    }

    for (auto& [surface_id, displayer] : displayers_) {
        auto it = states_snapshot.find(surface_id);
        if (it == states_snapshot.end()) {
            continue;
        }

        const auto gate_it = frame_gate_snapshot.find(surface_id);
        if (gate_it == frame_gate_snapshot.end()) {
            continue;
        }
        std::shared_ptr<Detail::SurfaceLifecycleAcks> acknowledgements;
        if (const auto acknowledgements_it =
                acknowledgements_snapshot.find(surface_id);
            acknowledgements_it != acknowledgements_snapshot.end()) {
            acknowledgements = acknowledgements_it->second;
        }

        auto& state = it->second;
        const bool has_optics = state.optics.image_handle != 0;
        const bool has_ui = state.ui.image_handle != 0;

        if (!has_optics && !has_ui) {
            continue;
        }

        // The coordinator owns the ordering boundary between the surface lease
        // and image access. It also owns the handles with the lease so an
        // acquisition exception destroys partial results before lease release.
        struct FrameImages {
            SharedDataHub::ImageStorage::WriteHandle optics;
            SharedDataHub::ImageStorage::WriteHandle ui;
        };
        auto frame_access = surface_frame_coordinator_.begin_frame(
            gate_it->second,
            [&]() -> FrameImages {
                FrameImages images;
                if (has_optics) {
                    images.optics = SharedDataHub::instance()
                                        .image_storage()
                                        .acquire_write(
                                            state.optics.image_handle);
                }
                if (has_ui) {
                    images.ui = SharedDataHub::instance()
                                    .image_storage()
                                    .acquire_write(state.ui.image_handle);
                }
                return images;
            });
        if (!frame_access) {
            continue;
        }
        auto& optics_frame = frame_access->images().optics;
        auto& ui_frame = frame_access->images().ui;

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

        void* surface = nullptr;
        if (auto surface_it = surfaces_snapshot.find(surface_id);
            surface_it != surfaces_snapshot.end()) {
            surface = surface_it->second;
        }

        bool use_optics_layer = optics_img_ptr && *optics_img_ptr;
        bool use_ui_layer = ui_img_ptr && *ui_img_ptr;
        if (use_optics_layer && optics_receipt_ptr != nullptr && optics_receipt_ptr->serial == 0) {
            if (state.optics.frame_index <= 1 || state.optics.frame_index % 120 == 0) {
                CFW_LOG_WARNING(
                    "DisplaySystem: skipping optics layer with empty submit receipt "
                    "(surface={}, image_handle={}, frame={}, extent={}x{})",
                    surface,
                    state.optics.image_handle,
                    state.optics.frame_index,
                    state.optics.width,
                    state.optics.height);
            }
            use_optics_layer = false;
            optics_receipt_ptr = nullptr;
        }
        if (use_ui_layer && ui_receipt_ptr != nullptr && ui_receipt_ptr->serial == 0) {
            if (state.ui.frame_index <= 1 || state.ui.frame_index % 120 == 0) {
                CFW_LOG_WARNING(
                    "DisplaySystem: skipping UI layer with empty submit receipt "
                    "(surface={}, image_handle={}, frame={}, extent={}x{})",
                    surface,
                    state.ui.image_handle,
                    state.ui.frame_index,
                    state.ui.width,
                    state.ui.height);
            }
            use_ui_layer = false;
            ui_receipt_ptr = nullptr;
        }

        Horizon::HardwareImage& bg_image = use_optics_layer ? *optics_img_ptr : transparent_storage_;
        Horizon::HardwareImage& fg_image = use_ui_layer ? *ui_img_ptr : transparent_storage_;

        if (!bg_image || !fg_image) {
            if (acknowledgements) {
                acknowledgements->present_completed(
                    Detail::PresentOutcome::Failed,
                    state.ui.first_present_boundary,
                    "Display compose/present failed: transparent fallback "
                    "image is unavailable");
            }
            continue;
        }

        auto& composite_resources = composite_resources_[surface_id];
        auto present_outcome = Detail::PresentOutcome::Skipped;
        try {
            present_outcome = compose_and_present(
                displayer,
                surface,
                state,
                composite_resources,
                bg_image,
                use_optics_layer ? optics_receipt_ptr : nullptr,
                fg_image,
                use_ui_layer ? ui_receipt_ptr : nullptr);
        } catch (const std::exception& error) {
            if (is_vulkan_device_lost_message(error.what())) {
                const std::string failure_message =
                    std::string("Display first present failed: Vulkan device lost: ") +
                    error.what();
                if (!device_lost_.exchange(true, std::memory_order_acq_rel)) {
                    std::vector<std::shared_ptr<
                        Detail::SurfaceLifecycleAcks>>
                        live_acknowledgements;
                    {
                        std::lock_guard lock(frame_mutex_);
                        live_acknowledgements.reserve(
                            surface_acknowledgements_.size());
                        for (const auto& [id, pending_acknowledgements] :
                             surface_acknowledgements_) {
                            (void)id;
                            if (pending_acknowledgements) {
                                live_acknowledgements.push_back(
                                    pending_acknowledgements);
                            }
                        }
                    }
                    for (const auto& pending_acknowledgements :
                         live_acknowledgements) {
                        if (pending_acknowledgements) {
                            pending_acknowledgements->fail_forward(
                                failure_message);
                        }
                    }
                    CFW_LOG_CRITICAL(
                        "DisplaySystem: Vulkan device lost during compose/present; "
                        "disabling further display submits and requesting engine shutdown "
                        "(surface={}, optics_handle={}, optics_frame={}, optics_receipt_empty={}, "
                        "ui_handle={}, ui_frame={}, ui_receipt_empty={}, output={}x{}, error={})",
                        surface,
                        state.optics.image_handle,
                        state.optics.frame_index,
                        optics_receipt_ptr == nullptr || optics_receipt_ptr->serial == 0,
                        state.ui.image_handle,
                        state.ui.frame_index,
                        ui_receipt_ptr == nullptr || ui_receipt_ptr->serial == 0,
                        composite_resources.width,
                        composite_resources.height,
                        error.what());
                    if (auto* stream = context()->event_stream()) {
                        stream->get_stream<Events::EngineShutdownEvent>()->publish(Events::EngineShutdownEvent{});
                    }
                }
                continue;
            }
            if (acknowledgements) {
                acknowledgements->present_failed(
                    std::string("Display compose/present failed: ") +
                    error.what());
            }
            CFW_LOG_ERROR(
                "DisplaySystem: compose/present failed "
                "(surface={}, optics_handle={}, optics_frame={}, optics_receipt_empty={}, "
                "ui_handle={}, ui_frame={}, ui_receipt_empty={}, output={}x{}): {}",
                surface,
                state.optics.image_handle,
                state.optics.frame_index,
                optics_receipt_ptr == nullptr || optics_receipt_ptr->serial == 0,
                state.ui.image_handle,
                state.ui.frame_index,
                ui_receipt_ptr == nullptr || ui_receipt_ptr->serial == 0,
                composite_resources.width,
                composite_resources.height,
                error.what());
            continue;
        } catch (...) {
            if (acknowledgements) {
                acknowledgements->present_failed(
                    "Display compose/present failed: unknown exception");
            }
            CFW_LOG_ERROR(
                "DisplaySystem: compose/present failed with unknown exception "
                "(surface={}, optics_handle={}, ui_handle={})",
                surface,
                state.optics.image_handle,
                state.ui.image_handle);
            continue;
        }
        if (acknowledgements) {
            const auto acknowledgement_outcome =
                present_outcome == Detail::PresentOutcome::Presented &&
                        !use_ui_layer
                    ? Detail::PresentOutcome::Skipped
                    : present_outcome;
            acknowledgements->present_completed(
                acknowledgement_outcome,
                state.ui.first_present_boundary,
                "Display compose/present failed: composite resources "
                "unavailable");
        }
        if (present_outcome != Detail::PresentOutcome::Presented) {
            continue;
        }

        // Write back the consumed signal so producers know when to safely reuse their image.
        const Horizon::SubmitReceipt consumed_receipt = composite_resources.last_receipt;
        if (use_optics_layer && optics_frame) {
            optics_frame->consumed_receipt = consumed_receipt;
            if (optics_receipt_ptr != nullptr && optics_receipt_ptr->serial != 0) {
                if (auto* event_bus = context()->event_bus()) {
                    event_bus->publish(Events::OpticsFrameConsumedEvent{
                        surface,
                        state.optics.frame_index,
                        optics_receipt_ptr->serial,
                    });
                }
            }
        }
        if (use_ui_layer && ui_frame) {
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
        // getComputePipelineID() 已移除；有效性改用 explicit operator bool()。
        composite_pipeline_ready_ = static_cast<bool>(*composite_pipeline_);
        if (!composite_pipeline_ready_) {
            CFW_LOG_ERROR("DisplaySystem: Failed to create typed composite pipeline");
            return false;
        }
    }

    if (resources.width != width || resources.height != height || !resources.output) {
        resources.executor.wait_idle(resources.last_receipt);
        resources.output = Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(
            width,
            height,
            Horizon::Format::RGBA16_FLOAT,
            Horizon::ImageUsage_Storage | Horizon::ImageUsage_ColorAttachment |
                Horizon::ImageUsage_Sampled | Horizon::ImageUsage_TransferSrc |
                Horizon::ImageUsage_TransferDst,
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

Detail::PresentOutcome DisplaySystem::compose_and_present(
    Horizon::HardwareDisplayer& displayer,
    void* surface,
    SurfaceState& state,
    CompositeResources& resources,
    Horizon::HardwareImage& optics_image,
    const Horizon::SubmitReceipt* optics_receipt,
    Horizon::HardwareImage& ui_image,
    const Horizon::SubmitReceipt* ui_receipt) {
    // if (state.optics.image_handle != 0 &&
    //     (state.optics.frame_index <= 3 || state.optics.frame_index % 120 == 0)) {
    //     CFW_LOG_INFO("Display: compose camera surface={} optics_handle={} optics_frame={} optics_extent={}x{} ui_handle={} ui_frame={}",
    //                  surface, state.optics.image_handle, state.optics.frame_index,
    //                  state.optics.width, state.optics.height, state.ui.image_handle,
    //                  state.ui.frame_index);
    // }

    // WORKAROUND for Horizon API change (removed HardwareImage::extent()):
    // Images passed through SharedDataHub have different addresses than when created,
    // so address-based extent cache lookups fail. Use state.width/height directly
    // instead of querying from the image objects.
    const PixelExtent optics_extent{state.optics.width, state.optics.height};
    const PixelExtent ui_extent{state.ui.width, state.ui.height};

    PixelExtent output_extent = surface_client_extent(surface);
    if (!output_extent) {
        output_extent = max_extent(optics_extent, ui_extent);
    }
    if (!output_extent) {
        return Detail::PresentOutcome::Skipped;
    }

    const PixelExtent bg_extent = optics_extent;
    const PixelExtent fg_extent = ui_extent;
    const uint32_t output_width = output_extent.width;
    const uint32_t output_height = output_extent.height;

    if (!ensure_composite_resources(resources, output_width, output_height)) {
        return Detail::PresentOutcome::Failed;
    }

    auto& composite_pipeline = *composite_pipeline_;
    // storeStorageDescriptor() 已移除；store_descriptor() 依 usage 自动选表，
    // 这些图像都有 Storage，故取到 storage-image 索引。
    const uint32_t bg_descriptor = optics_image.store_descriptor();
    const uint32_t fg_descriptor = ui_image.store_descriptor();
    const uint32_t output_descriptor = resources.output.store_descriptor();
    composite_pipeline.pushConsts.bgImage = bg_descriptor;
    composite_pipeline.pushConsts.fgImage = fg_descriptor;
    composite_pipeline.pushConsts.outputImage = output_descriptor;
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
    // bind_storage_image() 已移除。Horizon 现在从 dispatch.bindings（通过 set_resource_direct
    // 注册 bound_images_）自动转换为 GENERAL 布局（execution.cpp:2044-2062 遍历 StorageImage
    // 类型的 binding）。这里 pushConsts.xxxImage 的 bindType 是 0（标量），不触发
    // is_direct_resource_bind()，故必须显式注册 images[] 这个 bindType=8 的 binding。
    // 但 composite.comp.glsl 的 images 是单个 (set=2, binding=0) 槽，三个图像共享，
    // 只能注册一次。实际上三个图像的索引已写入 push constant，shader 从 images[xxxImage]
    // 索引读写，不需要单独的 bind 声明——Horizon 的 example 也是纯 pushConsts 传索引，
    // 从未调用 bind_storage_image。布局转换由 images[] 这个 binding 的任一注册触发，
    // 或者由图像用作 ColorAttachment 时的 begin_rendering 自动处理（这些图像确实是
    // ColorAttachment）。这里删除三个 bind 调用，依赖 images 数组自身的注册（如果有），
    // 或 rendering attachment 的自动转换。

    // 新版 Horizon: 使用 dispatch_extent 替代 dispatch_groups
    // composite_pipeline 使用 8x8 local size，extent 会自动计算 group 数量
    {
        std::ostringstream label;
        label << "Display/composite"
              << " surface=" << surface
              << " bg_desc=" << bg_descriptor
              << " fg_desc=" << fg_descriptor
              << " output_desc=" << output_descriptor
              // get_image_id() 已移除；这些 ID 仅用于日志，改用 descriptor 索引即可。
              << " bg_image=" << bg_descriptor
              << " fg_image=" << fg_descriptor
              << " output_image=" << output_descriptor
              << " bg_extent=" << bg_extent.width << "x" << bg_extent.height
              << " fg_extent=" << fg_extent.width << "x" << fg_extent.height
              << " output_extent=" << output_width << "x" << output_height
              << " optics_frame=" << state.optics.frame_index
              << " optics_receipt_empty="
              << (optics_receipt == nullptr || optics_receipt->serial == 0)
              << " ui_frame=" << state.ui.frame_index
              << " ui_receipt_empty="
              << (ui_receipt == nullptr || ui_receipt->serial == 0);
        // NOTE: set_debug_label() 已在新版 Horizon 中移除
        CFW_LOG_TRACE("{}", label.str());
    }

    // GPU sync: wait for each producer's rendering to finish before reading their images
    if (optics_receipt != nullptr && optics_receipt->serial != 0) {
        resources.executor.wait(*optics_receipt);
    }
    if (ui_receipt != nullptr && ui_receipt->serial != 0) {
        resources.executor.wait(*ui_receipt);
    }

    // 记住这次提交，供调用方回写 consumed_receipt（executor.last_receipt() 已移除）。
    resources.last_receipt = resources.executor.stream()
        << composite_pipeline.dispatch_extent(output_width, output_height)
        << Horizon::present(displayer, resources.output)
        << Horizon::commit();
    return Detail::PresentOutcome::Presented;
}

void DisplaySystem::shutdown() {
    CFW_LOG_INFO("DisplaySystem: destroying display resources");
    frame_submission_enabled_.store(false, std::memory_order_release);
    const auto forward_completion_fence = forward_completion_fence_;
    if (forward_completion_fence) {
        forward_completion_fence->close();
    }
    const auto callback_gate = callback_gate_;
    if (callback_gate) {
        callback_gate->close();
    }

    const bool has_subscriptions = surface_changed_sub_id_ ||
                                   surface_removed_sub_id_ ||
                                   optics_frame_sub_id_ || ui_frame_sub_id_;
    if (has_subscriptions) {
        if (auto* system_context = context(); system_context != nullptr) {
            auto* event_bus = system_context->event_bus();
            if (event_bus != nullptr) {
                if (surface_changed_sub_id_) {
                    event_bus->unsubscribe(*surface_changed_sub_id_);
                }
                if (surface_removed_sub_id_) {
                    event_bus->unsubscribe(*surface_removed_sub_id_);
                }
                if (optics_frame_sub_id_) {
                    event_bus->unsubscribe(*optics_frame_sub_id_);
                }
                if (ui_frame_sub_id_) {
                    event_bus->unsubscribe(*ui_frame_sub_id_);
                }
            }
        }
    }
    surface_changed_sub_id_.reset();
    surface_removed_sub_id_.reset();
    optics_frame_sub_id_.reset();
    ui_frame_sub_id_.reset();

    // Unsubscribe does not stop a handler already copied by EventBus. The
    // owner-independent gate rejects callbacks that have not started and this
    // wait lets callbacks already holding owner access finish. Never wait while
    // holding frame_mutex_: active callbacks may need that mutex to exit.
    if (callback_gate) {
        callback_gate->wait_for_quiescence();
    }

    std::unordered_set<std::uint64_t> surface_ids;
    std::vector<std::shared_ptr<Detail::SurfaceLifecycleAcks>> acknowledgements;
    const auto remember_acknowledgements =
        [&acknowledgements](
            const std::shared_ptr<Detail::SurfaceLifecycleAcks>& pending) {
            if (pending &&
                std::find(acknowledgements.begin(),
                          acknowledgements.end(),
                          pending) == acknowledgements.end()) {
                acknowledgements.push_back(pending);
            }
        };
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        acknowledgements.reserve(surface_acknowledgements_.size() +
                                 pending_removals_.size());
        for (const auto& [surface_id, pending_acknowledgements] :
             surface_acknowledgements_) {
            surface_ids.insert(surface_id);
            remember_acknowledgements(pending_acknowledgements);
        }
        for (const auto& removal : pending_removals_) {
            surface_ids.insert(
                reinterpret_cast<std::uint64_t>(removal.surface));
            remember_acknowledgements(removal.acknowledgements);
        }
        for (const auto& [surface_id, surface] : surfaces_) {
            (void)surface;
            surface_ids.insert(surface_id);
        }
        for (const auto& [surface_id, state] : surface_states_) {
            (void)state;
            surface_ids.insert(surface_id);
        }
        for (const auto surface_id : removed_surfaces_) {
            surface_ids.insert(surface_id);
        }
        for (const auto* surface : pending_surfaces_) {
            surface_ids.insert(reinterpret_cast<std::uint64_t>(surface));
        }

        pending_surfaces_.clear();
        pending_removals_.clear();
    }

    for (const auto& [surface_id, displayer] : displayers_) {
        (void)displayer;
        surface_ids.insert(surface_id);
    }
    for (const auto& [surface_id, resources] : composite_resources_) {
        (void)resources;
        surface_ids.insert(surface_id);
    }

    std::vector<std::pair<
        std::uint64_t,
        Detail::SurfaceFrameCoordinator::Retirement>> retirements;
    retirements.reserve(surface_ids.size());
    for (const auto surface_id : surface_ids) {
        retirements.emplace_back(surface_id,
                                 surface_frame_coordinator_.retire(surface_id));
    }
    for (const auto& [surface_id, retirement] : retirements) {
        (void)surface_id;
        retirement.wait();
    }

    // Lifecycle completions below certify that every frame lease is quiescent
    // and every Display-owned resource has actually been destroyed.
    composite_pipeline_ready_ = false;
    displayers_.clear();
    composite_resources_.clear();
    composite_pipeline_.reset();
    transparent_storage_ = Horizon::HardwareImage();

    for (const auto& [surface_id, retirement] : retirements) {
        (void)retirement;
        surface_frame_coordinator_.forget(surface_id);
    }
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        surface_states_.clear();
        surfaces_.clear();
        removed_surfaces_.clear();
        surface_acknowledgements_.clear();
    }

    constexpr auto shutdown_message =
        "Display shutdown before surface lifecycle completed";
    for (const auto& pending_acknowledgements : acknowledgements) {
        pending_acknowledgements->cancel_forward(shutdown_message);
        pending_acknowledgements->removal_succeeded();
    }
    if (callback_gate) {
        callback_gate->complete_deferred_after_resources_destroyed(
            UI::DisplaySurfaceResult::Status::Cancelled,
            shutdown_message);
    }
    device_lost_.store(false, std::memory_order_release);


    CFW_LOG_INFO("DisplaySystem: display resources destroyed");
}

}  // namespace Corona::Systems
