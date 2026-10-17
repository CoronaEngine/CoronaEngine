#pragma once

#include <horizon.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Corona::Systems::MechanicsInternal {
struct CollisionMesh;
struct TriangleOctree;
}  // namespace Corona::Systems::MechanicsInternal

namespace Corona::Systems {

// GPU-resident deformed triangle geometry. CPU topology is uploaded once; only
// compact contact/query records cross back to the existing impulse/IK solvers.
class GpuCollisionPipeline {
   public:
    static constexpr float contact_distance = 0.02f;
    using Matrix = std::array<float, 16>;
    using Point = std::array<float, 3>;
    struct VertexSource {
        Horizon::HardwareBuffer buffer;  // packed Resource::Vertex, stride 32
        std::uint32_t vertex_base{};
        std::uint32_t vertex_count{};
        Matrix transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };
    struct VertexUpdate {
        std::uint64_t key{};
        std::span<const VertexSource> sources;
        float pose_dt{};
    };
    struct Instance {
        std::uint64_t key{};
        Matrix transform{};
    };
    struct Pair {
        Instance a, b;
        float threshold{contact_distance};
    };
    struct Contact {
        bool has_contact{};
        Point normal{}, point{};
        float penetration{}; // Positive interior distance; negative separated gap.
        int triangle_a{-1}, triangle_b{-1};
    };
    struct VertexQuery {
        Instance instance;
        Point contact_model{};
    };
    struct VertexResult {
        Point lowest_world{}, surface_velocity{};
        std::uint32_t lowest_vertex{};
    };
    struct GroundQuery {
        Instance instance;
        Point probe{};
        float max_drop{};
        bool use_snapshot{};
    };
    struct GroundResult {
        bool found{};
        float height{};
        Point normal{};
        // Device traversal counters; geometry remains resident on the GPU.
        std::uint32_t nodes_visited{}, triangles_tested{};
    };

    GpuCollisionPipeline();
    ~GpuCollisionPipeline();
    GpuCollisionPipeline(const GpuCollisionPipeline&) = delete;
    GpuCollisionPipeline& operator=(const GpuCollisionPipeline&) = delete;

    // revision invalidates topology/bind geometry. set_mesh is a no-op for the
    // same key/revision. Skin sources overwrite only their own collision segment.
    void set_mesh(std::uint64_t key, std::uint64_t revision,
                  const MechanicsInternal::CollisionMesh& mesh,
                  const MechanicsInternal::TriangleOctree& tree);
    void update_vertices(std::uint64_t key, std::span<const VertexSource> sources, float pose_dt);
    // Validate the complete batch before changing any pose. Source buffers are
    // captured by one device submission, including when physics is paused.
    void update_vertices(std::span<const VertexUpdate> updates);
    // Cumulative successful submissions, useful for checking batch boundaries.
    [[nodiscard]] std::uint64_t submission_count() const noexcept;
    // Capture animated supports before any character updates this animation
    // frame. Foot IK then sees one consistent previous pose for every support.
    void snapshot_ground(std::span<const std::uint64_t> keys);
    [[nodiscard]] bool contains(std::uint64_t key) const;
    [[nodiscard]] bool contains(std::uint64_t key, std::uint64_t revision) const;
    // Upload and refit resident geometry before its first query, without any
    // contact generation or readback. The caller supplies current transforms.
    void prepare_instances(std::span<const Instance> instances);
    [[nodiscard]] std::vector<Contact> contacts(std::span<const Pair> pairs);
    [[nodiscard]] std::vector<VertexResult> sample_vertices(std::span<const VertexQuery> queries);
    [[nodiscard]] std::vector<GroundResult> ground(std::span<const GroundQuery> queries);
    void retain(std::span<const std::uint64_t> active_keys);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace Corona::Systems
