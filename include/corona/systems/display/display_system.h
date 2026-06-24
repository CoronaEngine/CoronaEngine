#pragma once

#include "horizon.h"
#include <corona/events/display_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>
#include <corona/shader_include.h>
// clang-format off
#include GLSL(../../../assets/shaders/composite.comp.glsl)
// clang-format on

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

    ~DisplaySystem() override = default;

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
    void shutdown() override;

   private:
    struct PendingLayer {
        std::uintptr_t image_handle = 0;
        uint64_t frame_index = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t viewport_x = 0;
        uint32_t viewport_y = 0;
        uint32_t viewport_width = 0;
        uint32_t viewport_height = 0;
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
    };

    bool compose_and_present(Horizon::HardwareDisplayer& displayer,
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

    Kernel::EventId surface_changed_sub_id_ = 0;
    Kernel::EventId surface_removed_sub_id_ = 0;
    Kernel::EventId optics_frame_sub_id_ = 0;
    Kernel::EventId ui_frame_sub_id_ = 0;

    // Protects displayers_ and surface_states_ against concurrent access
    // from EventBus handlers (Optics thread, main thread) and update() (Display thread)
    std::mutex frame_mutex_;

    std::unordered_map<uint64_t, Horizon::HardwareDisplayer> displayers_;
    std::unordered_map<uint64_t, void*> surfaces_;
    std::unordered_map<uint64_t, SurfaceState> surface_states_;
    std::unordered_map<uint64_t, CompositeResources> composite_resources_;
    std::unordered_set<uint64_t> removed_surfaces_;
    std::vector<void*> pending_surfaces_;  ///< Surfaces awaiting displayer creation (deferred to update thread)

    // Surfaces awaiting teardown (ImGui secondary viewport closed). The removal event
    // is published synchronously from the main thread; its handler only buffers the
    // request here (+ a promise) and returns, then update() on the Display thread
    // GPU-idles and destroys the displayer/state before fulfilling the promise so the
    // main thread can safely destroy the OS window. See DisplaySurfaceRemovedEvent.
    struct PendingRemoval {
        void* surface = nullptr;
        std::shared_ptr<std::promise<void>> done;
    };
    std::vector<PendingRemoval> pending_removals_;

    // Compositing resources
    std::optional<Horizon::ComputePipeline<composite_comp_glsl_t>> composite_pipeline_;
    Horizon::HardwareImage transparent_storage_;  ///< 1x1 transparent StorageImage fallback for missing layers
    bool composite_pipeline_ready_ = false;
};
}  // namespace Corona::Systems
