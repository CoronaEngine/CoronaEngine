#include "gpu_skinning.h"
#include "gpu_performance.h"
#include "gpu_compute_pipeline.h"

#include <Codegen/BuiltinVariate.h>
#include <Codegen/ControlFlows.h>
#include <Codegen/CustomLibrary.h>
#include <Codegen/TypeAlias.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Corona::Systems {
namespace {

namespace H = Corona::Horizon;
using namespace EmbeddedShader;

constexpr std::uint32_t kThreads = 64;
constexpr std::uint32_t kVertexFloats = 8;
constexpr std::uint32_t kBoundsFloats = 6;
constexpr std::uint32_t kBoundsFanIn = 8;
static_assert(sizeof(Resource::Vertex) == kVertexFloats * sizeof(float));
static_assert(offsetof(Resource::Vertex, position) == 0);
static_assert(offsetof(Resource::Vertex, normal) == 3 * sizeof(float));
static_assert(offsetof(Resource::Vertex, tex_coords) == 6 * sizeof(float));
static_assert(Resource::MAX_BONE_INFLUENCE == 4);

H::HardwareBuffer make_buffer(std::uint64_t bytes, H::BufferUsageFlags usage,
                              H::CpuAccessMode access, const char* name) {
    H::HardwareBufferDesc desc;
    desc.element_count = bytes / sizeof(float);
    desc.element_size = sizeof(float);
    desc.usage = usage;
    desc.cpu_access = access;
    desc.debug_name = name;
    return H::HardwareBuffer(desc);
}

void write_buffer(const H::HardwareBuffer& buffer, std::span<const std::byte> bytes) {
    if (!buffer.write_bytes(bytes)) {
        throw std::runtime_error("GPU skinning upload staging write failed");
    }
}

// These proxies hold back-pointers used by auto-binding. Keep this object at a
// stable address, and keep its HardwareBuffer members alive through recording.
struct SkinKernel {
    H::HardwareBuffer bind_buffer, id_buffer, weight_buffer, palette_buffer;
    H::HardwareBuffer output_buffer, bounds_buffer;
    EmbeddedShader::Array<float> bind_vertices;
    EmbeddedShader::Array<std::uint32_t> bone_ids;
    EmbeddedShader::Array<float> bone_weights;
    EmbeddedShader::Array<float> palette;
    EmbeddedShader::Array<float> output;
    EmbeddedShader::Array<float> bounds;
    Uint vertex_count;
    Uint bone_count;
    Uint palette_base;
    H::ComputePipeline<> pipeline;

    SkinKernel() {
        vertex_count.as_push_constant();
        bone_count.as_push_constant();
        palette_base.as_push_constant();
        auto shader = [&] {
            Uint vertex;
            vertex = dispatchThreadID()->x;
            $IF(vertex < vertex_count) {
                Uint vbase;
                vbase = vertex * Uint(kVertexFloats);
                Float bx, by, bz, bnx, bny, bnz;
                bx = bind_vertices[vbase];
                by = bind_vertices[vbase + Uint(1u)];
                bz = bind_vertices[vbase + Uint(2u)];
                bnx = bind_vertices[vbase + Uint(3u)];
                bny = bind_vertices[vbase + Uint(4u)];
                bnz = bind_vertices[vbase + Uint(5u)];
                Float px, py, pz, nx, ny, nz, total;
                px = 0.0f; py = 0.0f; pz = 0.0f;
                nx = 0.0f; ny = 0.0f; nz = 0.0f; total = 0.0f;

                // A C++ loop intentionally emits four independent AST blocks.
                // Helicon's unfinished runtime-loop macros are not used.
                for (std::uint32_t influence = 0; influence < 4; ++influence) {
                    Uint wi, id;
                    wi = vertex * Uint(4u) + Uint(influence);
                    id = bone_ids[wi];
                    Float weight;
                    weight = bone_weights[wi];
                    // Negative CPU IDs are stored as UINT_MAX (or another
                    // out-of-range uint), so this also rejects negative IDs.
                    $IF((id < bone_count) && !(weight <= Float(0.0f))) {
                        Uint m;
                        m = (palette_base + id) * Uint(16u);
                        Float m0, m1, m2, m4, m5, m6, m8, m9, m10;
                        Float m12, m13, m14;
                        m0 = palette[m]; m1 = palette[m + Uint(1u)];
                        m2 = palette[m + Uint(2u)]; m4 = palette[m + Uint(4u)];
                        m5 = palette[m + Uint(5u)]; m6 = palette[m + Uint(6u)];
                        m8 = palette[m + Uint(8u)]; m9 = palette[m + Uint(9u)];
                        m10 = palette[m + Uint(10u)];
                        m12 = palette[m + Uint(12u)];
                        m13 = palette[m + Uint(13u)];
                        m14 = palette[m + Uint(14u)];
                        px = px + weight * (m0 * bx + m4 * by + m8 * bz + m12);
                        py = py + weight * (m1 * bx + m5 * by + m9 * bz + m13);
                        pz = pz + weight * (m2 * bx + m6 * by + m10 * bz + m14);
                        nx = nx + weight * (m0 * bnx + m4 * bny + m8 * bnz);
                        ny = ny + weight * (m1 * bnx + m5 * bny + m9 * bnz);
                        nz = nz + weight * (m2 * bnx + m6 * bny + m10 * bnz);
                        total = total + weight;
                    }
                }
                Float ox, oy, oz, onx, ony, onz;
                ox = bx; oy = by; oz = bz;
                onx = bnx; ony = bny; onz = bnz;
                $IF(total > Float(0.0f)) {
                    ox = px; oy = py; oz = pz;
                    Float length;
                    length = sqrt(nx * nx + ny * ny + nz * nz);
                    $IF(length > Float(1.0e-8f)) {
                        Float inverse;
                        inverse = Float(1.0f) / length;
                        onx = nx * inverse; ony = ny * inverse; onz = nz * inverse;
                    }
                }
                output[vbase] = ox;
                output[vbase + Uint(1u)] = oy;
                output[vbase + Uint(2u)] = oz;
                output[vbase + Uint(3u)] = onx;
                output[vbase + Uint(4u)] = ony;
                output[vbase + Uint(5u)] = onz;
                output[vbase + Uint(6u)] = bind_vertices[vbase + Uint(6u)];
                output[vbase + Uint(7u)] = bind_vertices[vbase + Uint(7u)];
                Uint bb;
                bb = vertex * Uint(kBoundsFloats);
                bounds[bb] = ox; bounds[bb + Uint(1u)] = oy;
                bounds[bb + Uint(2u)] = oz; bounds[bb + Uint(3u)] = ox;
                bounds[bb + Uint(4u)] = oy; bounds[bb + Uint(5u)] = oz;
            }
        };
        pipeline = make_compute_pipeline(shader, ktm::uvec3(kThreads, 1, 1));
    }

    void bind(const H::HardwareBuffer& vertices, const H::HardwareBuffer& ids,
              const H::HardwareBuffer& weights, const H::HardwareBuffer& matrices,
              const H::HardwareBuffer& destination, const H::HardwareBuffer& aabbs,
              std::uint32_t vertices_size, std::uint32_t matrices_size,
              std::uint32_t matrices_base) {
        bind_buffer = vertices; id_buffer = ids; weight_buffer = weights;
        palette_buffer = matrices; output_buffer = destination; bounds_buffer = aabbs;
        bind_vertices = bind_buffer; bone_ids = id_buffer; bone_weights = weight_buffer;
        palette = palette_buffer; output = output_buffer; bounds = bounds_buffer;
        vertex_count = vertices_size; bone_count = matrices_size;
        palette_base = matrices_base;
    }
};

// Fixed eight-way multi-dispatch reduction avoids shared-memory/barrier/atomic
// features missing from this EDSL, while reducing both launch count and scratch
// traffic compared with a binary reduction. The host loop only generates AST.
struct BoundsKernel {
    H::HardwareBuffer source_buffer, destination_buffer;
    EmbeddedShader::Array<float> source;
    EmbeddedShader::Array<float> destination;
    Uint count;
    H::ComputePipeline<> pipeline;

    BoundsKernel() {
        count.as_push_constant();
        auto shader = [&] {
            Uint index;
            index = dispatchThreadID()->x;
            Uint first;
            first = index * Uint(kBoundsFanIn);
            $IF(first < count) {
                Uint a, out;
                a = first * Uint(kBoundsFloats);
                out = index * Uint(kBoundsFloats);
                for (std::uint32_t axis = 0; axis < 3; ++axis) {
                    Float low, high;
                    low = source[a + Uint(axis)];
                    high = source[a + Uint(axis + 3u)];
                    for (std::uint32_t lane = 1; lane < kBoundsFanIn; ++lane) {
                        $IF(first + Uint(lane) < count) {
                            Uint b;
                            b = a + Uint(lane * kBoundsFloats);
                            Float other_low, other_high;
                            other_low = source[b + Uint(axis)];
                            other_high = source[b + Uint(axis + 3u)];
                            low = min(low, other_low);
                            high = max(high, other_high);
                        }
                    }
                    destination[out + Uint(axis)] = low;
                    destination[out + Uint(axis + 3u)] = high;
                }
            }
        };
        pipeline = make_compute_pipeline(shader, ktm::uvec3(kThreads, 1, 1));
    }

    void bind(const H::HardwareBuffer& input, const H::HardwareBuffer& output,
              std::uint32_t input_count) {
        source_buffer = input; destination_buffer = output;
        source = source_buffer; destination = destination_buffer; count = input_count;
    }
};

struct MeshCache {
    std::uint64_t revision{};
    const Resource::Vertex* source_vertices{};
    const Resource::BoneWeights* source_weights{};
    std::uint32_t vertex_count{};
    H::HardwareBuffer bind, ids, weights, output, bounds[2];
    H::HardwareBuffer upload_bind, upload_ids, upload_weights;
    H::HardwareBuffer readback, bounds_readback;
    bool pending_upload{true};

    explicit MeshCache(const GpuSkinningPipeline::Request& request)
        : revision(request.revision), source_vertices(request.vertices.data()),
          source_weights(request.weights.data()),
          vertex_count(static_cast<std::uint32_t>(request.vertices.size())) {
        const auto vertex_bytes = request.vertices.size_bytes();
        const auto influence_bytes = request.vertices.size() * 4u * sizeof(float);
        const auto bounds_bytes = request.vertices.size() * kBoundsFloats * sizeof(float);
        constexpr auto input_usage = H::BufferUsage_Storage | H::BufferUsage_TransferDst;
        bind = make_buffer(vertex_bytes, input_usage, H::CpuAccessMode::None, "mechanics.skin.bind");
        ids = make_buffer(influence_bytes, input_usage, H::CpuAccessMode::None, "mechanics.skin.ids");
        weights = make_buffer(influence_bytes, input_usage, H::CpuAccessMode::None, "mechanics.skin.weights");
        output = make_buffer(vertex_bytes, H::BufferUsage_Storage | H::BufferUsage_TransferSrc,
                             H::CpuAccessMode::None, "mechanics.skin.output");
        bounds[0] = make_buffer(bounds_bytes, H::BufferUsage_Storage | H::BufferUsage_TransferSrc,
                                H::CpuAccessMode::None, "mechanics.skin.bounds");
        const auto reduced_count = (request.vertices.size() + kBoundsFanIn - 1u) / kBoundsFanIn;
        bounds[1] = make_buffer(reduced_count * kBoundsFloats * sizeof(float),
                                H::BufferUsage_Storage | H::BufferUsage_TransferSrc,
                                H::CpuAccessMode::None, "mechanics.skin.bounds.scratch");
        bounds_readback = make_buffer(kBoundsFloats * sizeof(float), H::BufferUsage_TransferDst,
                                      H::CpuAccessMode::Read, "mechanics.skin.bounds.readback");
        upload_bind = make_buffer(vertex_bytes, H::BufferUsage_TransferSrc,
                                  H::CpuAccessMode::Write, "mechanics.skin.bind.upload");
        upload_ids = make_buffer(influence_bytes, H::BufferUsage_TransferSrc,
                                 H::CpuAccessMode::Write, "mechanics.skin.ids.upload");
        upload_weights = make_buffer(influence_bytes, H::BufferUsage_TransferSrc,
                                     H::CpuAccessMode::Write, "mechanics.skin.weights.upload");
        std::vector<std::uint32_t> packed_ids(request.vertices.size() * 4u);
        std::vector<float> packed_weights(packed_ids.size());
        for (std::size_t v = 0; v < request.weights.size(); ++v) {
            for (std::size_t influence = 0; influence < 4; ++influence) {
                packed_ids[v * 4u + influence] = static_cast<std::uint32_t>(request.weights[v].ids[influence]);
                packed_weights[v * 4u + influence] = request.weights[v].weights[influence];
            }
        }
        write_buffer(upload_bind, std::as_bytes(request.vertices));
        write_buffer(upload_ids, std::as_bytes(std::span(packed_ids)));
        write_buffer(upload_weights, std::as_bytes(std::span(packed_weights)));
    }

    bool matches(const GpuSkinningPipeline::Request& request) const {
        return revision == request.revision && source_vertices == request.vertices.data() &&
               source_weights == request.weights.data() && vertex_count == request.vertices.size();
    }
};

}  // namespace

struct GpuSkinningPipeline::Impl {
    H::HardwareExecutor executor;
    SkinKernel skin;
    BoundsKernel reduce;
    H::HardwareBuffer palette, palette_upload;
    std::size_t palette_capacity{};
    std::unordered_map<std::uint64_t, std::unique_ptr<MeshCache>> meshes;
    H::SubmitReceipt last_receipt;
    bool completion_unknown{};
    std::uint64_t submissions{};

    ~Impl() {
        // Best effort cleanup only; callers must propagate completion errors.
        if (completion_unknown && last_receipt.data) {
            try { executor.wait_completed(last_receipt); } catch (...) {}
        }
    }

    void ensure_usable() const {
        if (completion_unknown) {
            throw GpuSkinningCompletionError("GPU skinning backend has an unconfirmed submission");
        }
    }

    void upload_palette(std::span<const std::array<float, 16>> finals) {
        const std::size_t required = std::max<std::size_t>(finals.size(), 1u);
        if (required > palette_capacity) {
            palette = make_buffer(required * 16u * sizeof(float),
                                  H::BufferUsage_Storage | H::BufferUsage_TransferDst,
                                  H::CpuAccessMode::None, "mechanics.skin.palette");
            palette_upload = make_buffer(required * 16u * sizeof(float), H::BufferUsage_TransferSrc,
                                         H::CpuAccessMode::Write, "mechanics.skin.palette.upload");
            palette_capacity = required;
        }
        if (!finals.empty()) {
            write_buffer(palette_upload, std::as_bytes(finals));
        } else {
            const std::array<float, 16> dummy{};
            write_buffer(palette_upload, std::as_bytes(std::span(&dummy, 1u)));
        }
    }
};

GpuSkinningPipeline::GpuSkinningPipeline() : impl_(std::make_unique<Impl>()) {}
GpuSkinningPipeline::~GpuSkinningPipeline() = default;

std::vector<GpuSkinningPipeline::Result> GpuSkinningPipeline::update(
    std::span<const Request> requests, std::span<const Matrix> finals) {
    const Batch batch{requests, finals};
    return update_batch(std::span(&batch, 1));
}

std::vector<GpuSkinningPipeline::Result> GpuSkinningPipeline::update_batch(
    std::span<const Batch> batches) {
    GpuPerformance::Scope performance{"skinning"};
    impl_->ensure_usable();
    struct Work {
        const Request* request;
        std::uint32_t palette_base, bone_count;
    };
    std::vector<Work> work;
    std::unordered_set<std::uint64_t> batch_keys;
    std::size_t palette_size = 0;
    constexpr std::size_t max_palette_size = std::numeric_limits<std::uint32_t>::max() / 16u;
    bool has_work = false;
    // Validate every group before allocating/updating cached GPU resources.
    for (const auto& batch : batches) {
        if (batch.finals.size() > max_palette_size - palette_size) {
            throw std::invalid_argument("GPU skinning combined palette exceeds uint32 indexing");
        }
        for (const auto& request : batch.requests) {
            if (!batch_keys.insert(request.key).second) {
                throw std::invalid_argument("GPU skinning batch contains duplicate keys");
            }
            if (request.vertices.size() != request.weights.size()) {
                throw std::invalid_argument("GPU skinning weights must match vertex count");
            }
            if (request.vertices.size() > std::size_t(kThreads) * 65535u) {
                throw std::invalid_argument("GPU skinning vertex count exceeds one-dimensional dispatch limit");
            }
            for (const auto* target : {&request.vertex_buffer, &request.storage_buffer}) {
                if (*target && target->get_byte_size() < request.vertices.size_bytes()) {
                    throw std::invalid_argument("GPU skinning render target is too small");
                }
            }
            has_work |= !request.vertices.empty();
            work.push_back({&request, static_cast<std::uint32_t>(palette_size),
                            static_cast<std::uint32_t>(batch.finals.size())});
        }
        palette_size += batch.finals.size();
    }
    std::vector<Result> results(work.size());
    for (std::size_t i = 0; i < work.size(); ++i) results[i].key = work[i].request->key;
    if (!has_work) return results;

    std::vector<Matrix> finals;
    finals.reserve(palette_size);
    for (const auto& batch : batches) finals.insert(finals.end(), batch.finals.begin(), batch.finals.end());
    impl_->upload_palette(finals);
    for (const auto& item : work) {
        const auto& request = *item.request;
        if (request.vertices.empty()) continue;
        auto& cached = impl_->meshes[request.key];
        if (!cached || !cached->matches(request)) cached = std::make_unique<MeshCache>(request);
        if (request.readback && !cached->readback) {
            cached->readback = make_buffer(request.vertices.size_bytes(), H::BufferUsage_TransferDst,
                                          H::CpuAccessMode::Read, "mechanics.skin.vertices.readback");
        }
    }

    auto stream = impl_->executor.stream();
    const auto palette_bytes = std::max<std::size_t>(finals.size(), 1u) * 16u * sizeof(float);
    GpuPerformance::record_upload(palette_bytes);
    stream << H::CopyBufferCommand{impl_->palette_upload, impl_->palette, 0, 0, palette_bytes};
    for (const auto& item : work) {
        const auto& request = *item.request;
        if (request.vertices.empty()) continue;
        auto& cached = *impl_->meshes.at(request.key);
        const auto bytes = request.vertices.size_bytes();
        GpuPerformance::record_work_items(cached.vertex_count);
        if (cached.pending_upload) {
            GpuPerformance::record_upload(bytes + cached.ids.get_byte_size() + cached.weights.get_byte_size());
            stream << H::CopyBufferCommand{cached.upload_bind, cached.bind, 0, 0, bytes};
            stream << H::CopyBufferCommand{cached.upload_ids, cached.ids, 0, 0, cached.ids.get_byte_size()};
            stream << H::CopyBufferCommand{cached.upload_weights, cached.weights, 0, 0, cached.weights.get_byte_size()};
        }
        impl_->skin.bind(cached.bind, cached.ids, cached.weights, impl_->palette,
                         cached.output, cached.bounds[0], cached.vertex_count,
                         item.bone_count, item.palette_base);
        GpuPerformance::record_dispatch();
        stream << impl_->skin.pipeline.groups((cached.vertex_count + kThreads - 1u) / kThreads, 1);

        std::uint32_t count = cached.vertex_count;
        unsigned source = 0;
        while (count > 1u) {
            impl_->reduce.bind(cached.bounds[source], cached.bounds[1u - source], count);
            const std::uint32_t output_count = (count + kBoundsFanIn - 1u) / kBoundsFanIn;
            GpuPerformance::record_dispatch();
            stream << impl_->reduce.pipeline.groups((output_count + kThreads - 1u) / kThreads, 1);
            source = 1u - source;
            count = output_count;
        }
        GpuPerformance::record_readback(kBoundsFloats * sizeof(float));
        stream << H::CopyBufferCommand{cached.bounds[source], cached.bounds_readback, 0, 0,
                                      kBoundsFloats * sizeof(float)};
        if (request.vertex_buffer) {
            GpuPerformance::record_device_copy(bytes);
            stream << H::CopyBufferCommand{cached.output, request.vertex_buffer, 0, 0, bytes};
        }
        if (request.storage_buffer) {
            GpuPerformance::record_device_copy(bytes);
            stream << H::CopyBufferCommand{cached.output, request.storage_buffer, 0, 0, bytes};
        }
        if (request.readback) {
            GpuPerformance::record_readback(bytes);
            stream << H::CopyBufferCommand{cached.output, cached.readback, 0, 0, bytes};
        }
    }

    // A throwing commit can have submitted a prefix of the command graph. From
    // this point a failure must propagate without reusing the same targets.
    impl_->last_receipt = {};
    impl_->completion_unknown = true;
    try {
        {
            GpuPerformance::OperationScope timing{GpuPerformance::Operation::Commit};
            impl_->last_receipt = stream << H::commit();
        }
        ++impl_->submissions;
        {
            GpuPerformance::OperationScope timing{GpuPerformance::Operation::Wait};
            impl_->executor.wait_completed(impl_->last_receipt);
        }
        impl_->completion_unknown = false;
        // The receipt can retain command/submission resource references. Once
        // its fence has completed, retain() should be able to retire this batch.
        impl_->last_receipt = {};
    } catch (const std::exception& error) {
        throw GpuSkinningCompletionError(std::string("GPU skinning completion could not be confirmed: ") + error.what());
    } catch (...) {
        throw GpuSkinningCompletionError("GPU skinning completion could not be confirmed");
    }

    for (std::size_t i = 0; i < work.size(); ++i) {
        const auto& request = *work[i].request;
        if (request.vertices.empty()) continue;
        auto& cached = *impl_->meshes.at(request.key);
        cached.pending_upload = false;
        // Static staging is no longer needed once its first transfer completed.
        cached.upload_bind = {}; cached.upload_ids = {}; cached.upload_weights = {};
        std::array<float, kBoundsFloats> bounds{};
        if (!cached.bounds_readback.read_bytes(std::as_writable_bytes(std::span(bounds)))) {
            throw std::runtime_error("GPU skinning bounds readback failed");
        }
        results[i].output = cached.output;
        std::copy_n(bounds.begin(), 3, results[i].min_xyz.begin());
        std::copy_n(bounds.begin() + 3, 3, results[i].max_xyz.begin());
        if (request.readback) {
            results[i].vertices.resize(request.vertices.size_bytes());
            if (!cached.readback.read_bytes(results[i].vertices)) {
                throw std::runtime_error("GPU skinning vertex readback failed");
            }
        }
    }
    return results;
}

std::uint64_t GpuSkinningPipeline::submission_count() const noexcept {
    return impl_->submissions;
}

void GpuSkinningPipeline::retain(std::span<const std::uint64_t> active_keys) {
    impl_->ensure_usable();
    const std::unordered_set<std::uint64_t> active(active_keys.begin(), active_keys.end());
    std::erase_if(impl_->meshes, [&](const auto& entry) { return !active.contains(entry.first); });
}

void GpuSkinningPipeline::reset() {
    impl_->ensure_usable();
    impl_->meshes.clear();
    impl_->palette = {}; impl_->palette_upload = {}; impl_->palette_capacity = 0;
}

}  // namespace Corona::Systems
