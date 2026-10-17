#pragma once

#include "gpu_rigid_state.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Corona::Systems {

// GPU floor response keeps candidate impulses separate until the CPU has
// successfully acquired the corresponding transform write lock.
class GpuFloorSleepPipeline {
   public:
    static constexpr std::uint32_t max_bodies = 1u << 20;
    struct Velocity { std::array<float, 3> velocity{}, angular_velocity{}; };
    struct FloorInput {
        std::uint32_t body{};
        std::array<float, 3> center{}, inertia_inv{}, lowest{};
        // Column-major body-to-world rotation, matching KTM.
        std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
        float mass{1.0f}, bottom{};
        std::uint8_t linear_lock{}, angular_lock{};
        bool skinned{}, enabled{true};
    };
    struct FloorParams {
        float floor_y{}, floor_eps{0.01f}, restitution{0.6f}, gravity_y{-9.8f}, dt{};
        float low_velocity{0.05f}, zero_velocity{0.01f}, friction{0.35f}, static_friction{0.55f};
    };
    struct FloorContact { float delta_y{}; bool contact{}; };
    struct FloorVelocity {
        std::array<float, 3> velocity{}, angular_velocity{};
        bool reset_sleep_timer{};
    };
    struct SleepInput { float sleep_timer{}; bool sleeping{}, dynamic{true}; };
    struct SleepParams { float dt{}, velocity_threshold{0.05f}, time_threshold{0.4f}; };
    struct SleepResult {
        std::array<float, 3> velocity{}, angular_velocity{};
        float sleep_timer{};
        bool sleeping{}, was_awake{};
    };
    struct ResidentSleepInput { bool sleeping{}, dynamic{true}, reset_timer{}; };
    struct SleepFlags { bool sleeping{}, was_awake{}; };

    GpuFloorSleepPipeline();
    ~GpuFloorSleepPipeline();
    GpuFloorSleepPipeline(const GpuFloorSleepPipeline&) = delete;
    GpuFloorSleepPipeline& operator=(const GpuFloorSleepPipeline&) = delete;

    // Validate the whole batch before replacing the current step. The initial
    // snapshot and writable velocity target are independent device buffers.
    void begin(std::span<const Velocity> velocities);
    // Remap the previous step's device state by unique nonzero identity. New
    // identities start at rest; omitted identities are discarded. Callers that
    // pause the entire simulation must skip this call to retain its state.
    void begin_resident(std::span<const std::uint64_t> identities, GpuSubmissionBatch* batch = nullptr);
    // Install the device external-force result without replacing resident timers.
    void set_velocities(const GpuRigidVelocityView& source, GpuSubmissionBatch* batch = nullptr);
    [[nodiscard]] GpuRigidVelocityView initial_velocities() const noexcept;
    [[nodiscard]] GpuRigidVelocityTarget velocity_target() const noexcept;
    // Unique body indices only. Evaluation returns compact position/contact
    // metadata and never changes working velocities. Commit accepts exactly
    // one 0/1 byte per evaluated row. A pending evaluation must be committed
    // before another evaluation or finish_sleep; begin may reset the step.
    [[nodiscard]] std::vector<FloorContact> evaluate_floor(std::span<const FloorInput>, const FloorParams&);
    void commit_floor(std::span<const std::uint8_t> accepted, GpuSubmissionBatch* batch = nullptr);
    [[nodiscard]] std::vector<FloorVelocity> readback_velocities();
    // Inputs cover all bodies in begin order. Floor rebound timer resets are
    // applied before the sleep test. Newly sleeping bodies have was_awake=true
    // so the existing final movement callback still executes this step.
    [[nodiscard]] std::vector<SleepResult> finish_sleep(std::span<const SleepInput>, const SleepParams&);
    // Only one uint32 status word per body returns; velocities and timers remain
    // authoritative on device for the next begin_resident call.
    [[nodiscard]] std::vector<SleepFlags> finish_sleep_resident(std::span<const ResidentSleepInput>, const SleepParams&,
                                                            GpuSubmissionBatch* batch = nullptr);
    // Explicit diagnostic snapshot; not used by the simulation path.
    [[nodiscard]] std::vector<SleepResult> readback_resident_state();
    // Device-only batch overloads record without submitting. A nonempty resident
    // sleep readback flushes; otherwise explicitly flush before another consumer.
    // Standalone submissions only; shared batches expose their own counters.
    [[nodiscard]] std::uint64_t submission_count() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace Corona::Systems
