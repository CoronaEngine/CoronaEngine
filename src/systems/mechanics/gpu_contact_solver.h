#pragma once

#include "gpu_rigid_state.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Corona::Systems {

// Ordered Gauss-Seidel contacts run in parallel only across disjoint islands.
// CPU construction retains contact order for connected writable bodies. Fixed,
// kinematic and sleeping endpoints are shared read-only between islands.
class GpuContactSolverPipeline {
   public:
    static constexpr std::uint32_t max_bodies = 1u << 20;
    static constexpr std::uint32_t max_contacts = 1u << 20;
    struct Body {
        std::array<float, 3> velocity{}, angular_velocity{}, center{}, inertia_inv{};
        // Column-major body-to-world rotation, matching KTM.
        std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
        float mass{1.0f}, restitution{0.6f};
        bool dynamic{true}, sleeping{};
        std::uint8_t linear_lock{}, angular_lock{};
    };
    struct Contact {
        std::uint32_t a{}, b{};
        // Unit normal from A toward B; positive accumulated_j separates them.
        std::array<float, 3> normal{}, point{}, surface_a{}, surface_b{};
        float penetration{}, accumulated_j{}, accumulated_jt{};
    };
    struct Params {
        float max_linear_speed{80.0f}, max_angular_speed{30.0f}, max_impulse{500.0f};
        float friction{0.35f}, static_friction{0.55f}, early_exit_velocity{0.002f};
        std::uint32_t iterations{8};
        float dt{1.0f / 60.0f}, restitution_threshold{0.5f};
    };
    struct Velocity {
        std::array<float, 3> velocity{}, angular_velocity{};
    };
    struct ContactResult {
        float inv_ma{}, inv_mb{}, last_j{}, accumulated_j{}, accumulated_jt{}, v_n_initial{};
    };
    struct Result {
        std::vector<Velocity> bodies;
        std::vector<ContactResult> contacts;
        std::uint32_t iterations{};
    };

    GpuContactSolverPipeline();
    ~GpuContactSolverPipeline();
    GpuContactSolverPipeline(const GpuContactSolverPipeline&) = delete;
    GpuContactSolverPipeline& operator=(const GpuContactSolverPipeline&) = delete;

    // Entry points validate the entire batch before submitting. With no
    // contacts solve passes bodies through without a GPU submission.
    [[nodiscard]] Result solve(std::span<const Body> bodies, std::span<const Contact> contacts, const Params& params);
    // Only contacts and the iteration count return. Velocity remains on GPU;
    // even a contact-free nonempty batch uploads its body velocities once.
    [[nodiscard]] Result solve_device(std::span<const Body> bodies, std::span<const Contact> contacts, const Params& params);
    // Gather velocity on GPU using one source index per body (duplicates are
    // allowed), then solve in independent storage. Body velocity fields are
    // ignored. Only contacts/iterations return, including a contact-free batch;
    // the source must not alias this solver's velocity buffer.
    [[nodiscard]] Result solve_from_velocities(std::span<const Body> metadata, std::span<const Contact> contacts,
                                              const Params& params, const GpuRigidVelocityView& source,
                                              std::span<const std::uint32_t> source_indices);
    // Valid until the next solve attempt, including failed validation. Empty or
    // failed solves expose an empty view rather than stale body velocities.
    [[nodiscard]] GpuRigidVelocityView velocity_view() const noexcept;
    [[nodiscard]] std::vector<Velocity> readback_velocities();
    [[nodiscard]] std::uint64_t submission_count() const noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace Corona::Systems
