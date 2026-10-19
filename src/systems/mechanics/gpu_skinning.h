#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <corona/resource/types/scene.h>
#include <horizon.h>

namespace Corona::Systems {

// Submission/completion could not be confirmed. Do not reuse its render targets;
// propagate to the device/runtime error handler.
class GpuSkinningCompletionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One instance owns the immutable mesh inputs for geometry/model keys. Input
// storage must not change in place without changing Request::revision. A changed
// source pointer or vertex count also invalidates the cached input automatically.
// Call retain() after the frame with all resident mesh/LOD keys to release old
// inputs. The class is used from one CPU thread.
class GpuSkinningPipeline {
public:
    using Matrix = std::array<float, 16>;

    struct Request {
        std::uint64_t key{};
        std::uint64_t revision{};
        std::span<const Resource::Vertex> vertices;
        std::span<const Resource::BoneWeights> weights;
        Horizon::HardwareBuffer vertex_buffer;
        Horizon::HardwareBuffer storage_buffer;
        bool readback{true};
        // Opt in only when these render targets are exclusively written by this
        // pipeline. An identical palette can then reuse the completed pose.
        bool reuse_unchanged{};
    };

    struct Result {
        std::uint64_t key{};
        // Device-resident packed vertices, shared directly with collision kernels.
        Horizon::HardwareBuffer output;
        // Packed Resource::Vertex bytes. Empty when Request::readback is false.
        std::vector<std::byte> vertices;
        std::array<float, 3> min_xyz{};
        std::array<float, 3> max_xyz{};
        bool pose_changed{};
        // Version of the completed output, scoped to that buffer's lifetime.
        // Consumers must compare resource identity as well as this counter.
        std::uint64_t pose_revision{};
    };

    struct Batch {
        std::span<const Request> requests;
        std::span<const Matrix> finals;
    };

    GpuSkinningPipeline();
    ~GpuSkinningPipeline();
    GpuSkinningPipeline(const GpuSkinningPipeline&) = delete;
    GpuSkinningPipeline& operator=(const GpuSkinningPipeline&) = delete;
    GpuSkinningPipeline(GpuSkinningPipeline&&) = delete;
    GpuSkinningPipeline& operator=(GpuSkinningPipeline&&) = delete;

    // Runs all requests against one column-major matrix palette, commits once,
    // and waits for that submission before returning CPU readback/results.
    // Empty target buffers are allowed (headless validation/readback).
    // Throws on invalid inputs or GPU failure. Propagate errors to the runtime;
    // do not publish stale readback data or continue physics after a failed batch.
    [[nodiscard]] std::vector<Result> update(
        std::span<const Request> requests,
        std::span<const Matrix> finals);
    // Each group uses its own palette. Inputs must remain alive until this call
    // returns. Results follow group order, then request order, including empty
    // meshes. All groups share one upload/submission/wait; no work submits nothing.
    // Request keys must be unique across all groups, even for empty meshes.
    [[nodiscard]] std::vector<Result> update_batch(std::span<const Batch> batches);
    // Cumulative successful commit calls, including batches whose completion
    // later failed. retain()/reset() do not clear this diagnostic counter.
    [[nodiscard]] std::uint64_t submission_count() const noexcept;
    void retain(std::span<const std::uint64_t> active_keys);
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Systems
