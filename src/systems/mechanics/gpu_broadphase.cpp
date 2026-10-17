#include "gpu_broadphase.h"
#include "gpu_performance.h"
#include "gpu_compute_pipeline.h"

#include <horizon.h>
#include <Codegen/BuiltinVariate.h>
#include <Codegen/ControlFlows.h>
#include <Codegen/TypeAlias.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>

namespace Corona::Systems {
namespace {
namespace H = Corona::Horizon;
using namespace EmbeddedShader;
constexpr std::uint32_t threads = 64;
// A padded power of two must fit the Vulkan minimum 65,535 X workgroups.
constexpr std::uint32_t max_members = 1u << 21;
constexpr std::uint32_t invalid = std::numeric_limits<std::uint32_t>::max();
constexpr auto device_usage = H::BufferUsage_Storage | H::BufferUsage_TransferSrc | H::BufferUsage_TransferDst;

H::HardwareBuffer buffer(std::size_t words, H::BufferUsageFlags usage,
                         H::CpuAccessMode cpu = H::CpuAccessMode::None) {
    H::HardwareBufferDesc desc;
    desc.element_count = std::max<std::size_t>(words, 1);
    desc.element_size = sizeof(std::uint32_t);
    desc.usage = usage;
    desc.cpu_access = cpu;
    desc.debug_name = "mechanics.broadphase";
    return H::HardwareBuffer(desc);
}
void launch(H::HardwareStream& stream, H::ComputePipeline<>& pipeline, std::uint32_t n) {
    if (n) {
        GpuPerformance::record_dispatch();
        stream << pipeline.groups((n + threads - 1u) / threads, 1);
    }
}
template <class T>
struct Binding {
    H::HardwareBuffer handle;
    EmbeddedShader::Array<T> values;
    void bind(const H::HardwareBuffer& value) {
        handle = value;
        values = handle;
    }
};
using UInts = Binding<std::uint32_t>;
using Floats = Binding<float>;

// Each invocation writes its own element into a separate buffer. No read/write
// aliasing or cross-workgroup barrier is needed for a bitonic compare stage.
struct SortKernel {
    UInts input, output;
    Floats bounds;
    Uint count, axis, level, stride;
    H::ComputePipeline<> pipeline;
    SortKernel() {
        count.as_push_constant();
        axis.as_push_constant();
        level.as_push_constant();
        stride.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint i;
            i = dispatchThreadID()->x;
            $IF(i < count) {
                Uint other, scene_a, scene_b, body_a, body_b;
                other = i ^ stride;
                scene_a = input.values[i * Uint(2u)];
                body_a = input.values[i * Uint(2u) + Uint(1u)];
                scene_b = input.values[other * Uint(2u)];
                body_b = input.values[other * Uint(2u) + Uint(1u)];
                Bool before;
                before = scene_a < scene_b;
                $IF(scene_a == scene_b) {
                    // Padding sorts after every real scene and never reads an
                    // arbitrary body index from the padded metadata.
                    $IF(scene_a != Uint(invalid)) {
                        Float a, b;
                        a = bounds.values[body_a * Uint(6u) + axis];
                        b = bounds.values[body_b * Uint(6u) + axis];
                        before = (a < b) || ((a == b) && (body_a < body_b));
                    }
                }
                Bool ascending, lower, choose_self;
                ascending = (i & level) == Uint(0u);
                lower = (i & stride) == Uint(0u);
                choose_self = (ascending == lower) == before;
                Uint chosen;
                chosen = other;
                $IF(choose_self) { chosen = i; }
                output.values[i * Uint(2u)] = input.values[chosen * Uint(2u)];
                output.values[i * Uint(2u) + Uint(1u)] = input.values[chosen * Uint(2u) + Uint(1u)];
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

// Count and fill share the exact same predicate and traversal. Sorted minima
// let each body stop as soon as subsequent intervals cannot overlap it.
struct SweepKernel {
    UInts members, prefix, output;
    Floats bounds;
    Uint count, axis, page_begin, page_end;
    H::ComputePipeline<> pipeline;
    explicit SweepKernel(bool fill) {
        count.as_push_constant();
        axis.as_push_constant();
        if (fill) {
            page_begin.as_push_constant();
            page_end.as_push_constant();
        }
        pipeline = make_compute_pipeline([&] {
            Uint i;
            i = dispatchThreadID()->x;
            $IF(i < count) {
                Uint offset;
                offset = Uint(0u);
                Bool active;
                active = true;
                if (fill) {
                    $IF(i > Uint(0u)) { offset = prefix.values[i - Uint(1u)]; }
                    active = (offset < page_end) && (prefix.values[i] > page_begin);
                }
                $IF(active) {
                    Uint scene, body, base, j, found;
                    scene = members.values[i * Uint(2u)];
                    body = members.values[i * Uint(2u) + Uint(1u)];
                    base = body * Uint(6u);
                    j = i + Uint(1u);
                    found = Uint(0u);
                    Bool more;
                    more = true;
                    $WHILE((j < count) && more) {
                        Uint other, other_base;
                        other = members.values[j * Uint(2u) + Uint(1u)];
                        other_base = other * Uint(6u);
                        $IF((members.values[j * Uint(2u)] != scene) ||
                            (bounds.values[other_base + axis] > bounds.values[base + axis + Uint(3u)])) {
                            more = false;
                        }
                        $ELSE {
                            Bool overlap;
                            overlap = body != other;
                            for (std::uint32_t a = 0; a < 3; ++a) {
                                overlap = overlap &&
                                    (bounds.values[base + Uint(a)] <= bounds.values[other_base + Uint(a + 3u)]) &&
                                    (bounds.values[other_base + Uint(a)] <= bounds.values[base + Uint(a + 3u)]);
                            }
                            $IF(overlap) {
                                if (fill) {
                                    Uint destination;
                                    destination = offset + found;
                                    $IF((destination >= page_begin) && (destination < page_end)) {
                                        Uint first, second, word;
                                        first = body;
                                        second = other;
                                        $IF(first > second) {
                                            first = other;
                                            second = body;
                                        }
                                        word = (destination - page_begin) * Uint(2u);
                                        output.values[word] = first;
                                        output.values[word + Uint(1u)] = second;
                                    }
                                }
                                found = found + Uint(1u);
                            }
                        }
                        j = j + Uint(1u);
                        if (fill) {
                            $IF(offset + found >= page_end) { more = false; }
                        }
                    }
                    if (!fill) output.values[i] = found;
                }
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

// Hillis-Steele inclusive scan. Saturating addition reserves UINT_MAX as an
// explicit overflow sentinel; wraparound can never masquerade as a small list.
struct ScanKernel {
    UInts input, output;
    Uint count, stride;
    H::ComputePipeline<> pipeline;
    ScanKernel() {
        count.as_push_constant();
        stride.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint i;
            i = dispatchThreadID()->x;
            $IF(i < count) {
                Uint a;
                a = input.values[i];
                $IF(i >= stride) {
                    Uint b;
                    b = input.values[i - stride];
                    $IF(b > Uint(invalid) - a) { a = Uint(invalid); }
                    $ELSE { a = a + b; }
                }
                output.values[i] = a;
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

// Reuse allocations while avoiding permanent retention after a large scene.
struct Workspace {
    std::size_t capacity{};
    H::HardwareBuffer data;
    void ensure(std::size_t words, H::BufferUsageFlags usage,
                H::CpuAccessMode cpu = H::CpuAccessMode::None) {
        if (capacity >= words && capacity <= std::max<std::size_t>(4096, words * 4)) return;
        data = buffer(words, usage, cpu);
        capacity = words;
    }
};
}  // namespace

struct GpuBroadphasePipeline::Impl {
    Options options;
    H::HardwareExecutor executor;
    SortKernel sort;
    SweepKernel count_pairs{false}, fill_pairs{true};
    ScanKernel scan;
    Workspace bounds, bounds_upload, members_upload, members[2], prefix[2];
    Workspace output, readback, total;
    bool poisoned{};
    std::uint64_t submissions{};
    H::SubmitReceipt last_receipt;

    explicit Impl(Options value) : options(value) {
        // An 8-byte pair page must fit Vulkan's minimum 128 MiB storage range.
        if (!options.pair_page_capacity || options.pair_page_capacity > (1u << 24))
            throw std::invalid_argument("Invalid GPU broadphase pair page capacity");
    }
    struct Recording {
        Impl& owner;
        int exceptions{std::uncaught_exceptions()};
        ~Recording() {
            if (std::uncaught_exceptions() > exceptions) owner.poisoned = true;
        }
    };
    ~Impl() {
        if (poisoned && last_receipt.data) {
            try {
                executor.wait_completed(last_receipt);
            } catch (...) {
            }
        }
    }
    void usable() const {
        if (poisoned) throw std::runtime_error("GPU broadphase submission is unconfirmed");
    }
    void finish(H::HardwareStream& stream) {
        poisoned = true;
        last_receipt = {};
        {
            GpuPerformance::OperationScope timing{GpuPerformance::Operation::Commit};
            last_receipt = stream << H::commit();
        }
        ++submissions;
        {
            GpuPerformance::OperationScope timing{GpuPerformance::Operation::Wait};
            executor.wait_completed(last_receipt);
        }
        last_receipt = {};
        poisoned = false;
    }
};

GpuBroadphasePipeline::GpuBroadphasePipeline() : GpuBroadphasePipeline(Options{}) {}
GpuBroadphasePipeline::GpuBroadphasePipeline(Options options) : impl_(std::make_unique<Impl>(options)) {}
GpuBroadphasePipeline::~GpuBroadphasePipeline() = default;
std::uint64_t GpuBroadphasePipeline::submission_count() const noexcept { return impl_->submissions; }

std::vector<GpuBroadphasePipeline::Pair> GpuBroadphasePipeline::pairs(
    std::span<const Body> bodies, std::span<const Scene> scenes) {
    GpuPerformance::Scope performance{"broadphase"};
    GpuPerformance::record_work_items(bodies.size());
    impl_->usable();
    // Validate every input, even unreferenced bodies and singleton scenes,
    // before allocating/replacing any device cache or submitting commands.
    if (bodies.size() > max_members || scenes.size() >= invalid)
        throw std::length_error("GPU broadphase input exceeds supported body count or scene indexing");
    for (const auto& body : bodies) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(body.min[axis]) || !std::isfinite(body.max[axis]) || body.min[axis] > body.max[axis])
                throw std::invalid_argument("Invalid GPU broadphase bounds");
        }
    }
    std::vector<std::uint32_t> metadata;
    std::array<double, 3> center_min, center_max;
    center_min.fill(std::numeric_limits<double>::infinity());
    center_max.fill(-std::numeric_limits<double>::infinity());
    for (std::size_t scene = 0; scene < scenes.size(); ++scene) {
        const auto indices = scenes[scene].bodies;
        for (const auto index : indices)
            if (index >= bodies.size()) throw std::invalid_argument("GPU broadphase scene body index is out of range");
        std::vector<std::uint32_t> unique(indices.begin(), indices.end());
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        if (unique.size() < 2) continue;
        if (unique.size() > max_members - metadata.size() / 2)
            throw std::length_error("GPU broadphase scene membership exceeds supported dispatch size");
        for (const auto index : unique) {
            metadata.push_back(static_cast<std::uint32_t>(scene));
            metadata.push_back(index);
            for (std::size_t a = 0; a < 3; ++a) {
                const double center = (double(bodies[index].min[a]) + double(bodies[index].max[a])) * .5;
                center_min[a] = std::min(center_min[a], center);
                center_max[a] = std::max(center_max[a], center);
            }
        }
    }
    if (metadata.empty()) return {};
    const auto count = static_cast<std::uint32_t>(metadata.size() / 2);
    std::uint32_t padded = 1, axis = 0;
    while (padded < count) padded *= 2;
    for (std::uint32_t a = 1; a < 3; ++a)
        if (center_max[a] - center_min[a] > center_max[axis] - center_min[axis]) axis = a;
    metadata.resize(std::size_t(padded) * 2, invalid);
    std::vector<float> bounds;
    bounds.reserve(bodies.size() * 6);
    for (const auto& body : bodies) {
        bounds.insert(bounds.end(), body.min.begin(), body.min.end());
        bounds.insert(bounds.end(), body.max.begin(), body.max.end());
    }

    auto& p = *impl_;
    p.bounds.ensure(bounds.size(), device_usage);
    p.bounds_upload.ensure(bounds.size(), H::BufferUsage_TransferSrc, H::CpuAccessMode::Write);
    p.members_upload.ensure(metadata.size(), H::BufferUsage_TransferSrc, H::CpuAccessMode::Write);
    for (auto& workspace : p.members) workspace.ensure(metadata.size(), device_usage);
    for (auto& workspace : p.prefix) workspace.ensure(count, device_usage);
    p.total.ensure(1, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    if (!p.bounds_upload.data.write_bytes(std::as_bytes(std::span(bounds))) ||
        !p.members_upload.data.write_bytes(std::as_bytes(std::span(metadata))))
        throw std::runtime_error("GPU broadphase upload failed");

    unsigned sorted = 0, scanned = 0;
    {
        Impl::Recording recording{p};
        auto stream = p.executor.stream();
        GpuPerformance::record_upload((bounds.size() + metadata.size()) * sizeof(float));
        stream << H::CopyBufferCommand{p.bounds_upload.data, p.bounds.data, 0, 0, bounds.size() * sizeof(float)};
        stream << H::CopyBufferCommand{p.members_upload.data, p.members[0].data, 0, 0, metadata.size() * sizeof(std::uint32_t)};
        p.sort.bounds.bind(p.bounds.data);
        p.sort.axis = axis;
        p.sort.count = padded;
        for (std::uint32_t level = 2; level <= padded; level *= 2) {
            for (std::uint32_t stride = level / 2; stride; stride /= 2) {
                p.sort.input.bind(p.members[sorted].data);
                p.sort.output.bind(p.members[1u - sorted].data);
                p.sort.level = level;
                p.sort.stride = stride;
                launch(stream, p.sort.pipeline, padded);
                sorted = 1u - sorted;
            }
        }
        p.count_pairs.members.bind(p.members[sorted].data);
        p.count_pairs.bounds.bind(p.bounds.data);
        p.count_pairs.output.bind(p.prefix[0].data);
        p.count_pairs.axis = axis;
        p.count_pairs.count = count;
        launch(stream, p.count_pairs.pipeline, count);
        p.scan.count = count;
        for (std::uint32_t stride = 1; stride < count; stride *= 2) {
            p.scan.input.bind(p.prefix[scanned].data);
            p.scan.output.bind(p.prefix[1u - scanned].data);
            p.scan.stride = stride;
            launch(stream, p.scan.pipeline, count);
            scanned = 1u - scanned;
        }
        GpuPerformance::record_readback(sizeof(std::uint32_t));
        stream << H::CopyBufferCommand{p.prefix[scanned].data, p.total.data,
                                       std::size_t(count - 1) * sizeof(std::uint32_t), 0, sizeof(std::uint32_t)};
        p.finish(stream);
    }
    std::uint32_t total{};
    if (!p.total.data.read_bytes(std::as_writable_bytes(std::span(&total, 1))))
        throw std::runtime_error("GPU broadphase pair count readback failed");
    if (total == invalid) throw std::length_error("GPU broadphase pair count exceeds 32-bit output indexing");
    if (!total) return {};
    std::vector<Pair> result;
    if (total > result.max_size()) throw std::length_error("GPU broadphase pair output exceeds host address space");
    result.resize(total);
    const auto capacity = std::min(total, p.options.pair_page_capacity);
    p.output.ensure(std::size_t(capacity) * 2, device_usage);
    p.readback.ensure(std::size_t(capacity) * 2, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    p.fill_pairs.members.bind(p.members[sorted].data);
    p.fill_pairs.bounds.bind(p.bounds.data);
    p.fill_pairs.prefix.bind(p.prefix[scanned].data);
    p.fill_pairs.output.bind(p.output.data);
    p.fill_pairs.axis = axis;
    p.fill_pairs.count = count;
    for (std::uint32_t begin = 0; begin < total;) {
        const auto page_count = std::min(capacity, total - begin);
        {
            Impl::Recording recording{p};
            auto stream = p.executor.stream();
            p.fill_pairs.page_begin = begin;
            p.fill_pairs.page_end = begin + page_count;
            launch(stream, p.fill_pairs.pipeline, count);
            GpuPerformance::record_readback(std::size_t(page_count) * sizeof(Pair));
            stream << H::CopyBufferCommand{p.output.data, p.readback.data, 0, 0, std::size_t(page_count) * sizeof(Pair)};
            p.finish(stream);
        }
        if (!p.readback.data.read_bytes(std::as_writable_bytes(std::span(result).subspan(begin, page_count))))
            throw std::runtime_error("GPU broadphase pair readback failed");
        begin += page_count;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
}  // namespace Corona::Systems
