#pragma once

#include "horizon.h"
#include "corona/systems/display/display_callback_gate.h"
#include "corona/systems/display/surface_frame_coordinator.h"
#include "corona/systems/display/surface_lifecycle_acks.h"
#include <corona/events/display_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>
#include "Codegen/ControlFlows.h"  // Horizon: GLSL()/HLSL() include macros
// clang-format off
#include GLSL(../../../assets/shaders/composite.comp.glsl)
// clang-format on

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Corona::Systems {
/**
 * @brief Display system
 *
 * Manages windows, input events, and display devices.
 * Runs on a dedicated thread at 120 FPS for responsive input handling.
 * Receives Optics and UI layers, composites them before presenting.
 */
class DisplaySystem : public Kernel::SystemBase {
   public:
    DisplaySystem() {
        set_target_fps(120);
    }

    ~DisplaySystem() override;

    // ========================================
    // ISystem interface
    // ========================================

    std::string_view get_name() const override {
        return "Display";
    }

    int get_priority() const override {
        return 100;
    }

    bool initialize(Kernel::ISystemContext* ctx) override;
    void update() override;
    void stop() override;
    void shutdown() override;

   private:
    using CallbackGate = Detail::OwnerCallbackGate<DisplaySystem>;

    struct PendingLayer : Detail::ImageFrameMetadata {
        std::uintptr_t image_handle = 0;
        Detail::PublishedImage published_image;
        Detail::SurfaceLifecycleAcks::FirstPresentBoundary
            first_present_boundary = 0;
    };

    struct SurfaceState {
        PendingLayer optics;
        PendingLayer ui;
    };

    struct CompositeResources {
        Horizon::HardwareExecutor executor;
        Horizon::HardwareImage output;
        uint32_t width = 0;
        uint32_t height = 0;
        // Horizon no longer exposes HardwareExecutor::last_receipt(). Display
        // tracks its final composite/present submission and writes it back as
        // consumed_receipt so producers know when their images are safe to reuse.
        Horizon::SubmitReceipt last_receipt;
    };

    Detail::PresentOutcome compose_and_present(
        Horizon::HardwareDisplayer& displayer,
        void* surface,
        SurfaceState& state,
        CompositeResources& resources,
        Horizon::HardwareImage& optics_image,
        const Horizon::SubmitReceipt* optics_receipt,
        Horizon::HardwareImage& ui_image,
        const Horizon::SubmitReceipt* ui_receipt);
    bool ensure_composite_resources(CompositeResources& resources,
                                    uint32_t width,
                                    uint32_t height);
    void handle_surface_changed(const Events::DisplaySurfaceChangedEvent& event);
    void handle_surface_removed(const Events::DisplaySurfaceRemovedEvent& event);
    void handle_optics_frame(const Events::OpticsFrameReadyEvent& event);
    void handle_ui_frame(const Events::UIFrameReadyEvent& event);
    void on_thread_stopped() override;

    std::optional<Kernel::EventId> surface_changed_sub_id_;
    std::optional<Kernel::EventId> surface_removed_sub_id_;
    std::optional<Kernel::EventId> optics_frame_sub_id_;
    std::optional<Kernel::EventId> ui_frame_sub_id_;
    std::shared_ptr<CallbackGate> callback_gate_;
    std::shared_ptr<Detail::ForwardCompletionFence>
        forward_completion_fence_;

    // Protects displayers_ and surface_states_ against concurrent access
    // from EventBus handlers (Optics thread, main thread) and update() (Display thread)
    std::mutex frame_mutex_;

    std::unordered_map<uint64_t, Horizon::HardwareDisplayer> displayers_;
    std::unordered_map<uint64_t, void*> surfaces_;
    std::unordered_map<uint64_t, SurfaceState> surface_states_;
    std::unordered_map<uint64_t, CompositeResources> composite_resources_;
    std::unordered_map<uint64_t, std::shared_ptr<Detail::SurfaceLifecycleAcks>>
        surface_acknowledgements_;
    std::unordered_set<uint64_t> removed_surfaces_;
    std::vector<void*> pending_surfaces_;  ///< Surfaces awaiting displayer creation (deferred to update thread)

    // Surfaces awaiting teardown (ImGui secondary viewport closed). The removal event
    // is published synchronously from the main thread; its handler only buffers the
    // request here (+ a promise) and returns, then update() on the Display thread
    // GPU-idles and destroys the displayer/state before fulfilling the promise so the
    // main thread can safely destroy the OS window. See DisplaySurfaceRemovedEvent.
    struct PendingRemoval {
        void* surface = nullptr;
        Detail::SurfaceFrameCoordinator::Retirement retirement;
        std::shared_ptr<Detail::SurfaceLifecycleAcks> acknowledgements;
    };
    std::vector<PendingRemoval> pending_removals_;
    Detail::SurfaceFrameCoordinator surface_frame_coordinator_;

    // Compositing resources
    std::optional<Horizon::ComputePipeline<composite_comp_glsl_t>> composite_pipeline_;
    Horizon::HardwareImage transparent_storage_;  ///< 1x1 transparent StorageImage fallback for missing layers
    bool composite_pipeline_ready_ = false;
    std::atomic<bool> device_lost_ = false;
    std::atomic<bool> frame_submission_enabled_ = false;
};
}  // namespace Corona::Systems
