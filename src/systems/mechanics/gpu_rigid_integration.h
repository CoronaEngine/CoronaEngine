#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "gpu_rigid_state.h"

namespace Corona::Systems {

// Batched rigid-body state integration. Mesh vertices never cross this interface.
class GpuRigidIntegrationPipeline {
   public:
    // Fits 32-bit shader indexing, Vulkan's minimum dispatch limit, and a
    // 128 MiB storage-buffer range for both input layouts.
    static constexpr std::uint32_t max_bodies = 1u << 20;
    struct VelocityInput {
        std::array<float, 3> velocity{}, angular_velocity{};
        float damping{1.0f};
        bool dynamic{true}, sleeping{};
        std::uint8_t linear_lock{}, angular_lock{};
        // Clear resident momentum before integration (e.g. externally owned poses).
        bool reset_velocity{};
    };
    struct VelocityParams {
        float dt{};
        std::array<float, 3> gravity{0.0f, -9.81f, 0.0f};
        float rot_damping_factor{0.97f};
        float max_linear_speed{80.0f}, max_angular_speed{30.0f};
    };
    struct VelocityResult {
        std::array<float, 3> velocity{}, angular_velocity{};
    };
    struct PoseInput {
        std::array<float, 3> position{};
        // KTM's component order is (i,j,k,r), i.e. (x,y,z,w).
        std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 3> velocity{}, angular_velocity{}, correction{};
        std::uint8_t linear_lock{}, angular_lock{};
        bool advance{true};
    };
    struct PoseResult {
        std::array<float, 3> position{};
        std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
    };
    struct ResidentPoseInput {
        PoseInput pose;
        std::uint32_t velocity_index{};
        // The solver's axis-lock projection precedes floor-driven wakeup.
        // Keep it independent of pose.advance, which uses the later sleep state.
        bool apply_velocity_locks{true};
    };
    struct PoseVelocityResult {
        PoseResult pose;
        VelocityResult velocities;
    };

    GpuRigidIntegrationPipeline();
    ~GpuRigidIntegrationPipeline();
    GpuRigidIntegrationPipeline(const GpuRigidIntegrationPipeline&) = delete;
    GpuRigidIntegrationPipeline& operator=(const GpuRigidIntegrationPipeline&) = delete;

    // Apply damping, then gravity, then speed limits to awake dynamic bodies;
    // all awake bodies apply axis locks afterwards. Sleeping bodies pass through
    // unless reset_velocity explicitly clears their momentum first.
    // Invalid input is rejected before submission. Empty batches submit nothing.
    [[nodiscard]] std::vector<VelocityResult> update_velocities(
        std::span<const VelocityInput> inputs, const VelocityParams& params);
    // Update resident velocities using only CPU damping/type/sleep/lock metadata.
    // metadata.size() must match source.count; metadata velocity fields are ignored.
    // No velocities are read back. The returned buffer is independent of pose
    // and host-update workspaces and remains valid until the next nonempty
    // update_velocities_device submission or pipeline destruction. It may be
    // supplied as source to that next call. Rejected input/empty calls preserve it.
    [[nodiscard]] GpuRigidVelocityView update_velocities_device(
        std::span<const VelocityInput> metadata, const GpuRigidVelocityView& source, const VelocityParams& params,
        GpuSubmissionBatch* batch = nullptr);

    // Translation precedes positional correction. World angular velocity is
    // left-multiplied: dq/dt = 0.5 * (0,omega) * q, followed by normalization.
    // advance=false is an exact pass-through, without quaternion normalization.
    // No hemisphere canonicalization or Euler conversion is performed here.
    [[nodiscard]] std::vector<PoseResult> integrate_poses(std::span<const PoseInput> inputs, float dt);
    // Consume completed solver velocities without an intermediate CPU roundtrip.
    // pose.velocity/angular_velocity are ignored. Return the projected velocities
    // with the pose when the caller needs a host velocity snapshot.
    [[nodiscard]] std::vector<PoseVelocityResult> integrate_poses_from_velocities(
        std::span<const ResidentPoseInput> inputs, const GpuRigidVelocityView& velocities, float dt);
    // Read resident velocities without modifying them and return only poses.
    // No host velocity fields are uploaded and no velocity snapshot is read back.
    [[nodiscard]] std::vector<PoseResult> predict_poses_from_velocities(
        std::span<const ResidentPoseInput> inputs, const GpuRigidVelocityView& velocities, float dt,
        GpuSubmissionBatch* batch = nullptr);
    // Read back only poses; scatter projected velocities into a stable buffer
    // for floor/sleep. Destination indices must be unique in this batch and the
    // destination buffer must not alias the source. Repeated source indices are
    // allowed, including later single-body position rebases.
    [[nodiscard]] std::vector<PoseResult> integrate_poses_to_velocities(
        std::span<const ResidentPoseInput> inputs, const GpuRigidVelocityView& source,
        const GpuRigidVelocityTarget& destination, std::span<const std::uint32_t> destination_indices, float dt,
        GpuSubmissionBatch* batch = nullptr);
    // With batch != nullptr, device-only updates record without submitting;
    // nonempty pose readbacks flush the batch. Flush explicitly for empty poses.
    // This counter covers standalone submissions; batch owns its own counters.
    [[nodiscard]] std::uint64_t submission_count() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace Corona::Systems
