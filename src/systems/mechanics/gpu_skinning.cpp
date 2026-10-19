#include "gpu_skinning.h"
#include "gpu_performance.h"
#include "gpu_compute_pipeline.h"

#include <Codegen/BuiltinVariate.h>
#include <Codegen/ControlFlows.h>
#include <Codegen/CustomLibrary.h>
#include <Codegen/TypeAlias.h>

#include <algorithm>
#include <cmath>
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
    H::HardwareBuffer output_buffer;
    EmbeddedShader::Array<float> bind_vertices;
    EmbeddedShader::Array<std::uint32_t> bone_ids;
    EmbeddedShader::Array<float> bone_weights;
    EmbeddedShader::Array<float> palette;
    EmbeddedShader::Array<float> output;
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
                Float px, py, pz, total;
                px = 0.0f; py = 0.0f; pz = 0.0f;
                total = 0.0f;
                std::array<Float, 9> linear;
                for (auto& component : linear) component = 0.0f;

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
                    $IF((id < bone_count) && (weight > Float(0.0f))) {
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
                        px = px + weight * m12;
                        py = py + weight * m13;
                        pz = pz + weight * m14;
                        linear[0] = linear[0] + weight * m0;
                        linear[1] = linear[1] + weight * m1;
                        linear[2] = linear[2] + weight * m2;
                        linear[3] = linear[3] + weight * m4;
                        linear[4] = linear[4] + weight * m5;
                        linear[5] = linear[5] + weight * m6;
                        linear[6] = linear[6] + weight * m8;
                        linear[7] = linear[7] + weight * m9;
                        linear[8] = linear[8] + weight * m10;
                        total = total + weight;
                    }
                }
                Float ox, oy, oz, onx, ony, onz;
                ox = bx; oy = by; oz = bz;
                onx = bnx; ony = bny; onz = bnz;
                $IF(total > Float(0.0f)) {
                    // Reuse the blended linear transform already needed by
                    // normals instead of transforming the position four times.
                    ox = (linear[0]*bx + linear[3]*by + linear[6]*bz + px) / total;
                    oy = (linear[1]*bx + linear[4]*by + linear[7]*bz + py) / total;
                    oz = (linear[2]*bx + linear[5]*by + linear[8]*bz + pz) / total;
                    // The inverse transpose of the blended affine transform
                    // preserves perpendicular normals under nonuniform scaling
                    // and shear. Normalization cancels the determinant magnitude.
                    Float c0, c1, c2, c3, c4, c5, c6, c7, c8, determinant;
                    c0 = linear[4]*linear[8]-linear[5]*linear[7];
                    c1 = linear[5]*linear[6]-linear[3]*linear[8];
                    c2 = linear[3]*linear[7]-linear[4]*linear[6];
                    c3 = linear[7]*linear[2]-linear[8]*linear[1];
                    c4 = linear[8]*linear[0]-linear[6]*linear[2];
                    c5 = linear[6]*linear[1]-linear[7]*linear[0];
                    c6 = linear[1]*linear[5]-linear[2]*linear[4];
                    c7 = linear[2]*linear[3]-linear[0]*linear[5];
                    c8 = linear[0]*linear[4]-linear[1]*linear[3];
                    determinant = linear[0]*c0 + linear[1]*c1 + linear[2]*c2;
                    Float nx, ny, nz;
                    nx = c0*bnx+c3*bny+c6*bnz;
                    ny = c1*bnx+c4*bny+c7*bnz;
                    nz = c2*bnx+c5*bny+c8*bnz;
                    $IF(determinant < Float(0.0f)) { nx = -nx; ny = -ny; nz = -nz; }
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
            }
        };
        pipeline = make_compute_pipeline(shader, ktm::uvec3(kThreads, 1, 1));
    }

    void bind(const H::HardwareBuffer& vertices, const H::HardwareBuffer& ids,
              const H::HardwareBuffer& weights, const H::HardwareBuffer& matrices,
              const H::HardwareBuffer& destination,
              std::uint32_t vertices_size, std::uint32_t matrices_size,
              std::uint32_t matrices_base) {
        bind_buffer = vertices; id_buffer = ids; weight_buffer = weights;
        palette_buffer = matrices; output_buffer = destination;
        bind_vertices = bind_buffer; bone_ids = id_buffer; bone_weights = weight_buffer;
        palette = palette_buffer; output = output_buffer;
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

    explicit BoundsKernel(bool vertices = false) {
        count.as_push_constant();
        auto shader = [&] {
            Uint index;
            index = dispatchThreadID()->x;
            Uint first;
            first = index * Uint(kBoundsFanIn);
            $IF(first < count) {
                Uint a, out;
                const auto stride = vertices ? 8u : kBoundsFloats;
                a = first * Uint(stride);
                out = index * Uint(kBoundsFloats);
                for (std::uint32_t axis = 0; axis < 3; ++axis) {
                    Float low, high;
                    low = source[a + Uint(axis)];
                    high = source[a + Uint(axis + (vertices ? 0u : 3u))];
                    for (std::uint32_t lane = 1; lane < kBoundsFanIn; ++lane) {
                        $IF(first + Uint(lane) < count) {
                            Uint b;
                            b = a + Uint(lane * stride);
                            Float other_low, other_high;
                            other_low = source[b + Uint(axis)];
                            other_high = source[b + Uint(axis + (vertices ? 0u : 3u))];
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
    bool pose_valid{}, readback_valid{};
    std::uint64_t pose_revision{};
    std::vector<GpuSkinningPipeline::Matrix> finals;
    std::array<float, kBoundsFloats> reduced_bounds{};
    H::HardwareBuffer vertex_target, storage_target;

    explicit MeshCache(const GpuSkinningPipeline::Request& request)
        : revision(request.revision), source_vertices(request.vertices.data()),
          source_weights(request.weights.data()),
          vertex_count(static_cast<std::uint32_t>(request.vertices.size())) {
        const auto vertex_bytes = request.vertices.size_bytes();
        const auto influence_bytes = request.vertices.size() * 4u * sizeof(float);
        constexpr auto input_usage = H::BufferUsage_Storage | H::BufferUsage_TransferDst;
        bind = make_buffer(vertex_bytes, input_usage, H::CpuAccessMode::None, "mechanics.skin.bind");
        ids = make_buffer(influence_bytes, input_usage, H::CpuAccessMode::None, "mechanics.skin.ids");
        weights = make_buffer(influence_bytes, input_usage, H::CpuAccessMode::None, "mechanics.skin.weights");
        output = make_buffer(vertex_bytes, H::BufferUsage_Storage | H::BufferUsage_TransferSrc,
                             H::CpuAccessMode::None, "mechanics.skin.output");
        // First reduction reads positions directly from the skin output. No
        // duplicated per-vertex min/max image of the pose is necessary.
        const auto reduced_count = (request.vertices.size() + kBoundsFanIn - 1u) / kBoundsFanIn;
        bounds[0] = make_buffer(reduced_count * kBoundsFloats * sizeof(float), H::BufferUsage_Storage | H::BufferUsage_TransferSrc,
                                H::CpuAccessMode::None, "mechanics.skin.bounds");
        bounds[1] = make_buffer(((reduced_count + kBoundsFanIn - 1u) / kBoundsFanIn) * kBoundsFloats * sizeof(float),
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
            float largest = 0.0f;
            for (float weight : request.weights[v].weights)
                if (std::isfinite(weight)) largest = std::max(largest, weight);
            for (std::size_t influence = 0; influence < 4; ++influence) {
                packed_ids[v * 4u + influence] = static_cast<std::uint32_t>(request.weights[v].ids[influence]);
                const auto weight = request.weights[v].weights[influence];
                // Scale before summation to avoid overflow, then normalize the
                // valid influences in the kernel (out-of-range IDs are ignored).
                packed_weights[v * 4u + influence] = std::isfinite(weight) && weight > 0.0f
                    ? weight / largest : 0.0f;
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
    BoundsKernel vertex_bounds{true};
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
        std::span<const Matrix> finals;
        bool skin{}, copy_vertex{}, copy_storage{}, copy_readback{};
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
        for (const auto& matrix : batch.finals)
            for (float value : matrix)
                if (!std::isfinite(value)) throw std::invalid_argument("GPU skinning palette is not finite");
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
                            static_cast<std::uint32_t>(batch.finals.size()), batch.finals});
        }
        palette_size += batch.finals.size();
    }
    std::vector<Result> results(work.size());
    for (std::size_t i = 0; i < work.size(); ++i) results[i].key = work[i].request->key;
    if (!has_work) return results;

    bool has_skin = false, has_commands = false;
    for (auto& item : work) {
        const auto& request = *item.request;
        if (request.vertices.empty()) continue;
        auto& cached = impl_->meshes[request.key];
        if (!cached || !cached->matches(request)) cached = std::make_unique<MeshCache>(request);
        item.skin = !request.reuse_unchanged || !cached->pose_valid ||
            !std::equal(item.finals.begin(), item.finals.end(), cached->finals.begin(), cached->finals.end());
        item.copy_vertex = request.vertex_buffer &&
            (item.skin || !request.vertex_buffer.same_resource(cached->vertex_target));
        item.copy_storage = request.storage_buffer &&
            (item.skin || !request.storage_buffer.same_resource(cached->storage_target)) &&
            !(item.copy_vertex && request.storage_buffer.same_resource(request.vertex_buffer));
        item.copy_readback = request.readback && (item.skin || !cached->readback_valid);
        has_skin |= item.skin;
        has_commands |= item.skin || item.copy_vertex || item.copy_storage || item.copy_readback;
        if (request.readback && !cached->readback) {
            cached->readback = make_buffer(request.vertices.size_bytes(), H::BufferUsage_TransferDst,
                                          H::CpuAccessMode::Read, "mechanics.skin.vertices.readback");
        }
    }

    if (has_commands) {
        auto stream = impl_->executor.stream();
        if (has_skin) {
            std::vector<Matrix> finals;
            finals.reserve(palette_size);
            for (const auto& batch : batches) finals.insert(finals.end(), batch.finals.begin(), batch.finals.end());
            impl_->upload_palette(finals);
            const auto palette_bytes = std::max<std::size_t>(finals.size(), 1u) * 16u * sizeof(float);
            GpuPerformance::record_upload(palette_bytes);
            stream << H::CopyBufferCommand{impl_->palette_upload, impl_->palette, 0, 0, palette_bytes};
        }
        for (const auto& item : work) {
            const auto& request = *item.request;
            if (request.vertices.empty()) continue;
            auto& cached = *impl_->meshes.at(request.key);
            const auto bytes = request.vertices.size_bytes();
            if (item.skin) {
                cached.pose_valid = false;
                cached.readback_valid = false;
                GpuPerformance::record_work_items(cached.vertex_count);
                if (cached.pending_upload) {
                    GpuPerformance::record_upload(bytes + cached.ids.get_byte_size() + cached.weights.get_byte_size());
                    stream << H::CopyBufferCommand{cached.upload_bind, cached.bind, 0, 0, bytes};
                    stream << H::CopyBufferCommand{cached.upload_ids, cached.ids, 0, 0, cached.ids.get_byte_size()};
                    stream << H::CopyBufferCommand{cached.upload_weights, cached.weights, 0, 0, cached.weights.get_byte_size()};
                }
                impl_->skin.bind(cached.bind, cached.ids, cached.weights, impl_->palette,
                                 cached.output, cached.vertex_count,
                                 item.bone_count, item.palette_base);
                GpuPerformance::record_dispatch();
                stream << impl_->skin.pipeline.groups((cached.vertex_count + kThreads - 1u) / kThreads, 1);

                impl_->vertex_bounds.bind(cached.output, cached.bounds[0], cached.vertex_count);
                std::uint32_t count = (cached.vertex_count + kBoundsFanIn - 1u) / kBoundsFanIn;
                GpuPerformance::record_dispatch();
                stream << impl_->vertex_bounds.pipeline.groups((count + kThreads - 1u) / kThreads, 1);
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
            }
            if (item.copy_vertex) {
                GpuPerformance::record_device_copy(bytes);
                stream << H::CopyBufferCommand{cached.output, request.vertex_buffer, 0, 0, bytes};
            }
            if (item.copy_storage) {
                GpuPerformance::record_device_copy(bytes);
                stream << H::CopyBufferCommand{cached.output, request.storage_buffer, 0, 0, bytes};
            }
            if (item.copy_readback) {
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
    }

    for (std::size_t i = 0; i < work.size(); ++i) {
        const auto& request = *work[i].request;
        if (request.vertices.empty()) continue;
        auto& cached = *impl_->meshes.at(request.key);
        cached.pending_upload = false;
        // Static staging is no longer needed once its first transfer completed.
        cached.upload_bind = {}; cached.upload_ids = {}; cached.upload_weights = {};
        if (work[i].skin && !cached.bounds_readback.read_bytes(std::as_writable_bytes(std::span(cached.reduced_bounds)))) {
            throw std::runtime_error("GPU skinning bounds readback failed");
        }
        results[i].output = cached.output;
        results[i].pose_changed = work[i].skin;
        if (work[i].skin) ++cached.pose_revision;
        results[i].pose_revision = cached.pose_revision;
        std::copy_n(cached.reduced_bounds.begin(), 3, results[i].min_xyz.begin());
        std::copy_n(cached.reduced_bounds.begin() + 3, 3, results[i].max_xyz.begin());
        if (request.readback) {
            results[i].vertices.resize(request.vertices.size_bytes());
            if (!cached.readback.read_bytes(results[i].vertices)) {
                throw std::runtime_error("GPU skinning vertex readback failed");
            }
            cached.readback_valid = true;
        }
        if (work[i].skin) cached.finals.assign(work[i].finals.begin(), work[i].finals.end());
        cached.vertex_target = request.vertex_buffer;
        cached.storage_target = request.storage_buffer;
        cached.pose_valid = true;
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
