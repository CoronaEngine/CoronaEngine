#include "gpu_collision.h"
#include "gpu_performance.h"
#include "gpu_compute_pipeline.h"

#include <Codegen/BuiltinVariate.h>
#include <Codegen/ControlFlows.h>
#include <Codegen/CustomLibrary.h>
#include <Codegen/TypeAlias.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "gpu_collision_math.h"
#include "mechanics_internal.h"

namespace Corona::Systems {
namespace {
namespace H = Corona::Horizon;
namespace M = GpuCollisionMath;
using namespace EmbeddedShader;
constexpr std::uint32_t threads = 64, fan = 8, record_size = 12, node_size = 14, bounds_size = 7;
constexpr std::uint32_t max_items = threads * 65535u;
constexpr std::uint32_t contact_page_size = 65536;
constexpr std::uint32_t surface_walkers = 1024;
// Slang diagnoses the rounded decimal spelling of FLT_MAX as overflow. Half
// that value is a finite sentinel with ample range for engine coordinates.
constexpr float huge = std::numeric_limits<float>::max() * 0.5f;

H::HardwareBuffer buffer(std::size_t words, H::BufferUsageFlags usage,
                         H::CpuAccessMode cpu = H::CpuAccessMode::None) {
    H::HardwareBufferDesc d;
    d.element_count = std::max<std::size_t>(words, 1);
    d.element_size = 4;
    d.usage = usage;
    d.cpu_access = cpu;
    d.debug_name = "mechanics.collision";
    return H::HardwareBuffer(d);
}
constexpr auto device_usage = H::BufferUsage_Storage | H::BufferUsage_TransferSrc | H::BufferUsage_TransferDst;
template <class T>
H::HardwareBuffer upload(std::span<const T> data) {
    auto b = buffer(data.size_bytes() / 4, H::BufferUsage_TransferSrc, H::CpuAccessMode::Write);
    if (!data.empty() && !b.write_bytes(std::as_bytes(data))) throw std::runtime_error("GPU collision upload failed");
    return b;
}
template <class T>
H::HardwareBuffer upload(const std::vector<T>& data) { return upload(std::span<const T>(data)); }
void launch(H::HardwareStream& stream, H::ComputePipeline<>& pipeline, std::uint32_t n) {
    if (n) {
        GpuPerformance::record_dispatch();
        stream << pipeline.groups((n + threads - 1u) / threads, 1);
    }
}

// Proxies and the HardwareBuffer objects they point at have stable addresses.
template <class T>
struct Binding {
    H::HardwareBuffer handle;
    EmbeddedShader::Array<T> values;
    void bind(const H::HardwareBuffer& b) {
        handle = b;
        values = handle;
    }
};
using Floats = Binding<float>;
using UInts = Binding<std::uint32_t>;

M::Vec3 load3(EmbeddedShader::Array<float>& data, const Uint& base) {
    M::Vec3 p;
    p.x = data[base];
    p.y = data[base + Uint(1u)];
    p.z = data[base + Uint(2u)];
    return p;
}
M::Vec3 subtract3(const M::Vec3& a, const M::Vec3& b) {
    M::Vec3 p;
    p.x = a.x - b.x;
    p.y = a.y - b.y;
    p.z = a.z - b.z;
    return p;
}
Float dot3(const M::Vec3& a, const M::Vec3& b) {
    Float v;
    v = a.x * b.x + a.y * b.y + a.z * b.z;
    return v;
}
M::Vec3 cross3(const M::Vec3& a, const M::Vec3& b) {
    M::Vec3 p;
    p.x = a.y * b.z - a.z * b.y;
    p.y = a.z * b.x - a.x * b.z;
    p.z = a.x * b.y - a.y * b.x;
    return p;
}
struct MatrixUniform {
    std::array<Float, 16> m;
    MatrixUniform() {
        for (auto& x : m) x.as_push_constant();
    }
    void bind(const GpuCollisionPipeline::Matrix& matrix) {
        for (std::size_t i = 0; i < 16; ++i) m[i] = matrix[i];
    }
    M::Vec3 transform(const M::Vec3& p, bool direction = false) {
        M::Vec3 r;
        r.x = m[0] * p.x + m[4] * p.y + m[8] * p.z;
        r.y = m[1] * p.x + m[5] * p.y + m[9] * p.z;
        r.z = m[2] * p.x + m[6] * p.y + m[10] * p.z;
        if (!direction) {
            r.x = r.x + m[12];
            r.y = r.y + m[13];
            r.z = r.z + m[14];
        }
        return r;
    }
};

// Both importing a skin segment and preparing world vertices use the same EDSL
// shader. Only positions are required by collision; raster normals/UV stay in
// the original skin output.
struct TransformKernel {
    Floats source, destination;
    Uint count, base, source_stride;
    MatrixUniform matrix;
    H::ComputePipeline<> pipeline;
    TransformKernel() {
        count.as_push_constant();
        base.as_push_constant();
        source_stride.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint v;
            v = dispatchThreadID()->x;
            $IF(v < count) {
                auto p = load3(source.values, v * source_stride);
                auto w = matrix.transform(p);
                Uint o;
                // Skin input has position/normal/UV, but every collision pose
                // stores only positions (including history and IK snapshots).
                o = (v + base) * Uint(3u);
                destination.values[o] = w.x;
                destination.values[o + Uint(1u)] = w.y;
                destination.values[o + Uint(2u)] = w.z;
            }
        },
                                        ktm::uvec3(threads, 1, 1));
    }
};

// Metadata per node: leaf begin/count, DFS escape, first child, is_leaf,
// child_count, eight explicit direct child indices. Refit deepest level first.
struct RefitKernel {
    Floats vertices, bounds;
    UInts indices, nodes, leaves, order;
    Uint offset, count;
    H::ComputePipeline<> pipeline;
    RefitKernel() {
        offset.as_push_constant();
        count.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint lane;
            lane = dispatchThreadID()->x;
            $IF(lane < count) {
                Uint node, meta;
                node = order.values[offset + lane];
                meta = node * Uint(node_size);
                std::array<Float, 3> lo, hi;
                Float ground_uncertain;
                ground_uncertain = 0.0f;
                for (int a = 0; a < 3; ++a) {
                    lo[a] = huge;
                    hi[a] = -huge;
                }
                $IF(nodes.values[meta + Uint(4u)] == Uint(1u)) {
                    Uint k, end;
                    k = nodes.values[meta];
                    end = k + nodes.values[meta + Uint(1u)];
                    $WHILE(k < end) {
                        Uint tri;
                        tri = leaves.values[k] * Uint(3u);
                        std::array<M::Vec3, 3> p;
                        for (std::uint32_t c = 0; c < 3; ++c) {
                            Uint v;
                            v = indices.values[tri + Uint(c)] * Uint(3u);
                            p[c] = load3(vertices.values, v);
                            for (std::uint32_t a = 0; a < 3; ++a) {
                                Float x;
                                x = vertices.values[v + Uint(a)];
                                lo[a] = min(lo[a], x);
                                hi[a] = max(hi[a], x);
                            }
                        }
                        // Preserve the existing float ground predicate. Its
                        // dot-product barycentrics can accept points beyond
                        // the geometric slop for ill-conditioned XZ triangles.
                        // Those leaves (and their ancestors) must not be pruned.
                        auto e1 = subtract3(p[1], p[0]);
                        auto e2 = subtract3(p[2], p[0]);
                        Float d00, d01, d11, product, correlation;
                        d00 = e2.x * e2.x + e2.z * e2.z;
                        d01 = e2.x * e1.x + e2.z * e1.z;
                        d11 = e1.x * e1.x + e1.z * e1.z;
                        product = d00 * d11;
                        correlation = d01 * d01;
                        $IF(abs(product - correlation) <= Float(.01f) * (product + correlation)) { ground_uncertain = 1.0f; }
                        k = k + Uint(1u);
                    }
                }
                $ELSE {
                    for (std::uint32_t c = 0; c < 8; ++c) {
                        $IF(Uint(c) < nodes.values[meta + Uint(5u)]) {
                            Uint child;
                            child = nodes.values[meta + Uint(6u + c)] * Uint(bounds_size);
                            ground_uncertain = max(ground_uncertain, bounds.values[child + Uint(6u)]);
                            for (std::uint32_t a = 0; a < 3; ++a) {
                                Float l, h;
                                l = bounds.values[child + Uint(a)];
                                h = bounds.values[child + Uint(a + 3u)];
                                lo[a] = min(lo[a], l);
                                hi[a] = max(hi[a], h);
                            }
                        }
                    }
                }
                for (std::uint32_t a = 0; a < 3; ++a) {
                    bounds.values[node * Uint(bounds_size) + Uint(a)] = lo[a];
                    bounds.values[node * Uint(bounds_size) + Uint(a + 3u)] = hi[a];
                }
                bounds.values[node * Uint(bounds_size) + Uint(6u)] = ground_uncertain;
            }
        },
                                        ktm::uvec3(threads, 1, 1));
    }
};

struct ContactKernel {
    Floats va, vb, bounds, output;
    UInts ia, ib, nodes, leaves;
    Uint count, first, node_count, reverse;
    Float threshold;
    H::ComputePipeline<> pipeline;
    ContactKernel() {
        count.as_push_constant();
        first.as_push_constant();
        node_count.as_push_constant();
        reverse.as_push_constant();
        threshold.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint lane, ta;
            lane = dispatchThreadID()->x;
            ta = first + lane;
            $IF(lane < count) {
                std::array<M::Vec3, 3> a;
                for (std::uint32_t c = 0; c < 3; ++c) a[c] = load3(va.values, ia.values[ta * Uint(3u) + Uint(c)] * Uint(3u));
                std::array<Float, 3> lo, hi;
                lo[0] = min(min(a[0].x, a[1].x), a[2].x);
                hi[0] = max(max(a[0].x, a[1].x), a[2].x);
                lo[1] = min(min(a[0].y, a[1].y), a[2].y);
                hi[1] = max(max(a[0].y, a[1].y), a[2].y);
                lo[2] = min(min(a[0].z, a[1].z), a[2].z);
                hi[2] = max(max(a[0].z, a[1].z), a[2].z);
                Float depth, nx, ny, nz, sx, sy, sz, hits, best_b;
                depth = -threshold;
                nx = 0.0f;
                ny = 1.0f;
                nz = 0.0f;
                sx = 0.0f;
                sy = 0.0f;
                sz = 0.0f;
                hits = 0.0f;
                best_b = -1.0f;
                Uint node;
                node = 0u;
                $WHILE(node < node_count) {
                    Bool overlap;
                    overlap = true;
                    for (std::uint32_t axis = 0; axis < 3; ++axis) {
                        $IF((lo[axis] - threshold > bounds.values[node * Uint(bounds_size) + Uint(axis + 3u)]) ||
                            (bounds.values[node * Uint(bounds_size) + Uint(axis)] - threshold > hi[axis])) { overlap = false; }
                    }
                    Uint meta;
                    meta = node * Uint(node_size);
                    $IF(overlap) {
                        $IF(nodes.values[meta + Uint(4u)] == Uint(1u)) {
                            Uint k, end;
                            k = nodes.values[meta];
                            end = k + nodes.values[meta + Uint(1u)];
                            $WHILE(k < end) {
                                Uint tb;
                                tb = leaves.values[k];
                                std::array<M::Vec3, 3> b;
                                for (std::uint32_t c = 0; c < 3; ++c) b[c] = load3(vb.values, ib.values[tb * Uint(3u) + Uint(c)] * Uint(3u));
                                Bool triangle_overlap;
                                triangle_overlap =
                                    (min(min(b[0].x,b[1].x),b[2].x) <= hi[0]+threshold) &&
                                    (max(max(b[0].x,b[1].x),b[2].x) >= lo[0]-threshold) &&
                                    (min(min(b[0].y,b[1].y),b[2].y) <= hi[1]+threshold) &&
                                    (max(max(b[0].y,b[1].y),b[2].y) >= lo[1]-threshold) &&
                                    (min(min(b[0].z,b[1].z),b[2].z) <= hi[2]+threshold) &&
                                    (max(max(b[0].z,b[1].z),b[2].z) >= lo[2]-threshold);
                                $IF(triangle_overlap) {
                                    M::Contact contact;
                                    M::triangle_contact(a, b, threshold, contact);
                                    Float contact_count, tbf;
                                    contact_count = Float(contact.count);
                                    tbf = Float(tb);
                                    $IF((contact.count > Uint(0u)) && (contact.depth > Float(0.0f) - threshold) && ((best_b < Float(0.0f)) || (contact.depth > depth) || ((contact.depth == depth) && (tbf < best_b)))) {
                                        hits = contact_count;
                                        sx = contact.sx; sy = contact.sy; sz = contact.sz;
                                        depth = contact.depth;
                                        nx = contact.nx;
                                        ny = contact.ny;
                                        nz = contact.nz;
                                        best_b = tbf;
                                    }
                                }
                                k = k + Uint(1u);
                            }
                            node = nodes.values[meta + Uint(2u)];
                        }
                        $ELSE { node = nodes.values[meta + Uint(3u)]; }
                    }
                    $ELSE { node = nodes.values[meta + Uint(2u)]; }
                }
                $IF(reverse != Uint(0u)) { nx = -nx; ny = -ny; nz = -nz; }
                Uint o;
                o = lane * Uint(record_size);
                output.values[o] = depth;
                output.values[o + Uint(1u)] = nx;
                output.values[o + Uint(2u)] = ny;
                output.values[o + Uint(3u)] = nz;
                output.values[o + Uint(4u)] = sx;
                output.values[o + Uint(5u)] = sy;
                output.values[o + Uint(6u)] = sz;
                output.values[o + Uint(7u)] = hits;
                Float taf;
                taf = Float(ta);
                output.values[o + Uint(8u)] = taf;
                output.values[o + Uint(9u)] = best_b;
                $IF(reverse != Uint(0u)) {
                    output.values[o + Uint(8u)] = best_b;
                    output.values[o + Uint(9u)] = taf;
                }
                output.values[o + Uint(10u)] = 0.0f;
                output.values[o + Uint(11u)] = 0.0f;
            }
        },
                                        ktm::uvec3(threads, 1, 1));
    }
};

void merge_contact_record(std::array<Float, record_size>& r, std::array<Float, record_size>& s) {
    // Average only one coherent contact patch. Points
    // with another normal/depth cannot share its impulse.
    $IF((s[7] > Float(0.0f)) && (r[7] > Float(0.0f)) &&
        (s[11] == r[11]) &&
        (abs(s[0] - r[0]) < Float(1e-5f)) &&
        (s[1] * r[1] + s[2] * r[2] + s[3] * r[3] > Float(0.999f))) {
        for (int i = 4; i < 8; ++i) r[i] = r[i] + s[i];
    } $ELSE {
        // Equal-depth face interiors take precedence
        // over duplicated edge/vertex samples and the
        // intersection witness. Their area-weighted
        // centroid avoids artificial torque on flat supports.
        $IF((s[7] > Float(0.0f)) && ((r[7] == Float(0.0f)) || (s[0] > r[0] + Float(1e-5f)) ||
            ((abs(s[0] - r[0]) < Float(1e-5f)) && ((s[11] > r[11]) || ((s[11] == r[11]) &&
            ((s[10] > r[10]) || ((s[10] == r[10]) && ((s[8] < r[8]) || ((s[8] == r[8]) && (s[9] < r[9])))))))))) {
            for (int i = 0; i < record_size; ++i) r[i] = s[i];
        }
    }
}

// Signed distances are evaluated against the current deformed triangles, not
// a bind-pose SDF or an AABB proxy. The exact triangle intersection pass remains
// active for edge crossings which do not contain a sampled surface feature.
struct SurfaceContactKernel {
    Floats source, target, bounds, source_bounds, output;
    UInts source_indices, target_indices, source_nodes, source_leaves, nodes, leaves;
    Uint count, node_count, source_node_count, closed, source_closed, reverse;
    Float threshold, winding, source_winding;
    H::ComputePipeline<> pipeline;
    SurfaceContactKernel() {
        count.as_push_constant(); node_count.as_push_constant(); source_node_count.as_push_constant();
        closed.as_push_constant(); reverse.as_push_constant(); threshold.as_push_constant(); winding.as_push_constant();
        source_closed.as_push_constant(); source_winding.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint lane;
            lane = dispatchThreadID()->x;
            $IF(lane < count) {
                // Traverse only source leaves overlapping the target root. Each
                // walker processes a disjoint stride of actual triangle features;
                // there is no candidate cap and no full-map feature scratch.
                std::array<Float, record_size> accumulated;
                for (auto& value : accumulated) value = 0.0f;
                accumulated[0] = Float(0.0f)-threshold;
                accumulated[8] = -1.0f; accumulated[9] = -1.0f;
                Uint source_node;
                source_node = 0u;
                $WHILE(source_node < source_node_count) {
                    Bool overlap;
                    overlap = true;
                    for (std::uint32_t axis=0;axis<3;++axis) {
                        $IF((source_bounds.values[source_node*Uint(bounds_size)+Uint(axis)] > bounds.values[Uint(axis+3u)]+threshold) ||
                            (source_bounds.values[source_node*Uint(bounds_size)+Uint(axis+3u)] < bounds.values[Uint(axis)]-threshold)) { overlap = false; }
                    }
                    Uint source_meta;
                    source_meta = source_node*Uint(node_size);
                    $IF(overlap) {
                        $IF(source_nodes.values[source_meta+Uint(4u)] == Uint(1u)) {
                            Uint sample, sample_end, sample_begin;
                            sample_begin = source_nodes.values[source_meta]*Uint(7u);
                            // Keep a global lane assignment across leaves. A
                            // leaf-local stride concentrates all work in the
                            // first few warps when most leaves are small.
                            sample = sample_begin + (lane + count - sample_begin % count) % count;
                            sample_end = (source_nodes.values[source_meta]+source_nodes.values[source_meta+Uint(1u)])*Uint(7u);
                            $WHILE(sample < sample_end) {
                                Uint tri, feature;
                                tri = source_leaves.values[sample/Uint(7u)]; feature = sample%Uint(7u);
                                std::array<M::Vec3, 3> a;
                                for (std::uint32_t i = 0; i < 3; ++i)
                                    a[i] = load3(source.values, source_indices.values[tri * Uint(3u) + Uint(i)] * Uint(3u));
                                M::Vec3 p;
                                p = M::mul(M::add(M::add(a[0], a[1]), a[2]), Float(1.0f / 3.0f));
                                for (std::uint32_t i = 0; i < 3; ++i) {
                                    $IF(feature == Uint(i)) { p = a[i]; }
                                    $IF(feature == Uint(i + 3u)) { p = M::mul(M::add(a[i], a[(i + 1u) % 3u]), Float(0.5f)); }
                                }
                                M::Vec3 center_delta;
                                center_delta.x = (bounds.values[Uint(0u)] + bounds.values[Uint(3u)] - source_bounds.values[Uint(0u)] - source_bounds.values[Uint(3u)]) * Float(0.5f);
                                center_delta.y = (bounds.values[Uint(1u)] + bounds.values[Uint(4u)] - source_bounds.values[Uint(1u)] - source_bounds.values[Uint(4u)]) * Float(0.5f);
                                center_delta.z = (bounds.values[Uint(2u)] + bounds.values[Uint(5u)] - source_bounds.values[Uint(2u)] - source_bounds.values[Uint(5u)]) * Float(0.5f);
                                Float best, best_tri, best_preference;
                                Bool can_be_inside;
                                can_be_inside = closed != Uint(0u) &&
                                    (p.x >= bounds.values[Uint(0u)]) && (p.x <= bounds.values[Uint(3u)]) &&
                                    (p.y >= bounds.values[Uint(1u)]) && (p.y <= bounds.values[Uint(4u)]) &&
                                    (p.z >= bounds.values[Uint(2u)]) && (p.z <= bounds.values[Uint(5u)]);
                                best = threshold*threshold; best_tri = -1.0f; best_preference = -huge;
                                $IF(can_be_inside) { best = huge; }
                                M::Vec3 closest, face;
                                closest = p; face.x = 0.0f; face.y = 1.0f; face.z = 0.0f;
                                Uint node, crossings;
                                node = 0u; crossings = 0u;
                                $WHILE(node < node_count) {
                                    std::array<Float, 3> pos;
                                    pos[0] = p.x; pos[1] = p.y; pos[2] = p.z;
                                    Float box_distance, enter, leave;
                                    box_distance = 0.0f; enter = 0.0f; leave = huge;
                                    constexpr std::array<float, 3> direction{1.0f, 0.37139067f, 0.52917331f};
                                    for (std::uint32_t i = 0; i < 3; ++i) {
                                        Float low, high, delta;
                                        low = bounds.values[node * Uint(bounds_size) + Uint(i)];
                                        high = bounds.values[node * Uint(bounds_size) + Uint(i + 3u)];
                                        delta = max(Float(0.0f), max(low - pos[i], pos[i] - high));
                                        box_distance = box_distance + delta * delta;
                                        enter = max(enter, (low - pos[i]) / Float(direction[i]));
                                        leave = min(leave, (high - pos[i]) / Float(direction[i]));
                                    }
                                    Uint meta;
                                    meta = node * Uint(node_size);
                                    $IF((box_distance <= best) || (can_be_inside && (leave >= enter))) {
                                        $IF(nodes.values[meta + Uint(4u)] == Uint(1u)) {
                                            Uint k, end;
                                            k = nodes.values[meta]; end = k + nodes.values[meta + Uint(1u)];
                                            $WHILE(k < end) {
                                                Uint other;
                                                other = leaves.values[k];
                                                std::array<M::Vec3, 3> b;
                                                for (std::uint32_t i = 0; i < 3; ++i)
                                                    b[i] = load3(target.values, target_indices.values[other * Uint(3u) + Uint(i)] * Uint(3u));
                                                const auto q = M::closest_triangle_point(p, b);
                                                const auto normal = M::mul(M::normalize_safe(M::cross(M::sub(b[1], b[0]), M::sub(b[2], b[0]))), winding);
                                                Float distance, other_float, preference;
                                                distance = M::dot(M::sub(p, q), M::sub(p, q)); other_float = Float(other);
                                                preference = Float(0.0f) - M::dot(normal, center_delta);
                                                $IF((distance < best) || ((distance == best) && ((preference > best_preference) ||
                                                    ((preference == best_preference) && (other_float < best_tri))))) {
                                                    best = distance; closest = q; best_tri = other_float;
                                                    face = normal; best_preference = preference;
                                                }
                                                $IF(can_be_inside) { crossings = crossings + M::ray_crossing(p, b); }
                                                k = k + Uint(1u);
                                            }
                                            node = nodes.values[meta + Uint(2u)];
                                        } $ELSE { node = nodes.values[meta + Uint(3u)]; }
                                    } $ELSE { node = nodes.values[meta + Uint(2u)]; }
                                }
                                Float distance, depth, hit;
                                distance = sqrt(best); depth = Float(0.0f) - distance; hit = 0.0f;
                                M::Vec3 normal, point;
                                normal = M::mul(face, Float(-1.0f));
                                point = M::mul(M::add(p, closest), Float(0.5f));
                                $IF(distance > Float(1e-7f)) {
                                    normal = M::mul(M::sub(closest, p), Float(1.0f) / distance);
                                    $IF((crossings & Uint(1u)) != Uint(0u)) {
                                        depth = distance; normal = M::mul(normal, Float(-1.0f));
                                    }
                                }
                                // Coincident closed surfaces have zero distance everywhere.
                                // Matching outward faces at a face-interior sample indicate
                                // overlapping interiors; give recovery a bounded initial step.
                                const auto source_normal = M::mul(M::normalize_safe(M::cross(M::sub(a[1], a[0]), M::sub(a[2], a[0]))), source_winding);
                                $IF((closed != Uint(0u)) && (source_closed != Uint(0u)) && (feature == Uint(6u)) &&
                                    (distance <= Float(1e-7f)) && (M::dot(source_normal, face) > Float(0.999f))) { depth = threshold; }
                                Float preference;
                                preference = M::dot(normal, center_delta);
                                $IF((best_tri >= Float(0.0f)) && (depth >= Float(0.0f) - threshold)) { hit = 1.0f; }
                                $IF(reverse != Uint(0u)) { normal = M::mul(normal, Float(-1.0f)); }
                                Float priority, weight;
                                priority = 1.0f; weight = hit;
                                $IF(feature == Uint(6u)) {
                                    priority = 2.0f;
                                    weight = hit * M::length(M::cross(M::sub(a[1], a[0]), M::sub(a[2], a[0]))) * Float(0.5f);
                                }
                                std::array<Float, record_size> candidate;
                                candidate[0] = depth;
                                candidate[1] = normal.x; candidate[2] = normal.y; candidate[3] = normal.z;
                                candidate[4] = point.x * weight; candidate[5] = point.y * weight; candidate[6] = point.z * weight;
                                candidate[7] = weight;
                                Float ta, tb;
                                ta = Float(tri); tb = best_tri;
                                $IF(reverse != Uint(0u)) { ta = best_tri; tb = Float(tri); }
                                candidate[8] = ta; candidate[9] = tb;
                                candidate[10] = preference; candidate[11] = priority;
                                merge_contact_record(accumulated, candidate);
                                sample = sample + count;
                            }
                            source_node = source_nodes.values[source_meta+Uint(2u)];
                        } $ELSE { source_node = source_nodes.values[source_meta+Uint(3u)]; }
                    } $ELSE { source_node = source_nodes.values[source_meta+Uint(2u)]; }
                }
                for (std::uint32_t i=0;i<record_size;++i)
                    output.values[lane*Uint(record_size)+Uint(i)] = accumulated[i];
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

// One fixed-size record per vertex. Minimum world height and the closest local
// vertex are reduced independently, preserving exact floor geometry and the
// existing nearest-vertex approximation for animation surface velocity.
struct VertexKernel {
    Floats local, previous, world, output;
    Uint count;
    Float px, py, pz, inverse_dt;
    MatrixUniform matrix;
    H::ComputePipeline<> pipeline;
    VertexKernel() {
        count.as_push_constant();
        px.as_push_constant();
        py.as_push_constant();
        pz.as_push_constant();
        inverse_dt.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint v;
            v = dispatchThreadID()->x;
            $IF(v < count) {
                auto p = load3(local.values, v * Uint(3u));
                auto old = load3(previous.values, v * Uint(3u));
                auto w = load3(world.values, v * Uint(3u));
                auto delta = subtract3(p, old);
                auto velocity = matrix.transform(delta, true);
                Float dx, dy, dz, d2, vf;
                dx = p.x - px;
                dy = p.y - py;
                dz = p.z - pz;
                d2 = dx * dx + dy * dy + dz * dz;
                vf = Float(v);
                Uint o;
                o = v * Uint(record_size);
                output.values[o] = w.y;
                output.values[o + Uint(1u)] = w.x;
                output.values[o + Uint(2u)] = w.z;
                output.values[o + Uint(3u)] = vf;
                output.values[o + Uint(4u)] = d2;
                output.values[o + Uint(5u)] = velocity.x * inverse_dt;
                output.values[o + Uint(6u)] = velocity.y * inverse_dt;
                output.values[o + Uint(7u)] = velocity.z * inverse_dt;
                output.values[o + Uint(8u)] = vf;
                for (std::uint32_t i = 9; i < record_size; ++i) output.values[o + Uint(i)] = 0.0f;
            }
        },
                                        ktm::uvec3(threads, 1, 1));
    }
};

struct GroundKernel {
    Floats world, bounds, output;
    UInts indices, nodes, leaves;
    Uint node_count, walker_count;
    Float px, py, pz, drop;
    H::ComputePipeline<> pipeline;
    GroundKernel() {
        node_count.as_push_constant();
        walker_count.as_push_constant();
        px.as_push_constant();
        py.as_push_constant();
        pz.as_push_constant();
        drop.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            // Walkers take disjoint strides through each leaf, so even a large
            // unsplittable leaf is processed in parallel without a candidate
            // cap. All lanes take the same bounds-dependent DFS branches.
            Uint lane;
            lane = dispatchThreadID()->x;
            $IF(lane < walker_count) {
                Float y, nx, ny, nz;
                y = -huge;
                nx = 0.0f;
                ny = 1.0f;
                nz = 0.0f;
                Uint node, best_tri, visited, tested;
                node = 0u;
                best_tri = std::numeric_limits<std::uint32_t>::max();
                visited = 0u;
                tested = 0u;
                $WHILE(node < node_count) {
                    visited = visited + Uint(1u);
                    Bool overlap;
                    overlap = true;
                    // Prune only the vertical probe column in X/Z. The old
                    // float plane-height expression can exceed a vertex's Y
                    // bound through cancellation at large world offsets; its
                    // height/drop predicate remains authoritative below.
                    for (std::uint32_t axis = 0; axis < 3; axis += 2) {
                        Float lo, hi, margin, qlo, qhi;
                        lo = bounds.values[node * Uint(bounds_size) + Uint(axis)];
                        hi = bounds.values[node * Uint(bounds_size) + Uint(axis + 3u)];
                        // All three barycentric weights may be as low as
                        // -1e-4. Their accepted projection can therefore extend
                        // up to 2e-4 of a triangle's axis extent beyond its AABB.
                        // Node extent bounds every contained triangle's extent;
                        // add a coordinate-scaled float roundoff allowance too.
                        margin = Float(2e-4f) * (hi - lo) + Float(1e-5f) +
                                 Float(2e-6f) * max(abs(lo), abs(hi));
                        if (axis == 0) { qlo = px; qhi = px; }
                        else { qlo = pz; qhi = pz; }
                        $IF((lo - margin > qhi) || (qlo > hi + margin)) { overlap = false; }
                    }
                    $IF(bounds.values[node * Uint(bounds_size) + Uint(6u)] > Float(0.0f)) { overlap = true; }
                    Uint meta;
                    meta = node * Uint(node_size);
                    $IF(overlap) {
                        $IF(nodes.values[meta + Uint(4u)] == Uint(1u)) {
                            Uint k, end;
                            k = nodes.values[meta] + lane;
                            end = nodes.values[meta] + nodes.values[meta + Uint(1u)];
                            $WHILE(k < end) {
                                Uint tri;
                                tri = leaves.values[k];
                                tested = tested + Uint(1u);
                                std::array<M::Vec3, 3> v;
                                for (std::uint32_t c = 0; c < 3; ++c) v[c] = load3(world.values, indices.values[tri * Uint(3u) + Uint(c)] * Uint(3u));
                                auto e1 = subtract3(v[1], v[0]);
                                auto e2 = subtract3(v[2], v[0]);
                                auto n = cross3(e1, e2);
                                Float len;
                                len = sqrt(dot3(n, n));
                                $IF((len >= Float(1e-8f)) && (n.y >= Float(0.1f) * len)) {
                                    Float h;
                                    h = (dot3(n, v[0]) - n.x * px - n.z * pz) / n.y;
                                    $IF((h <= py + Float(0.05f)) && (h >= py - drop)) {
                                        Float x, z, d00, d01, d02, d11, d12, den;
                                        x = px - v[0].x;
                                        z = pz - v[0].z;
                                        d00 = e2.x * e2.x + e2.z * e2.z;
                                        d01 = e2.x * e1.x + e2.z * e1.z;
                                        d02 = e2.x * x + e2.z * z;
                                        d11 = e1.x * e1.x + e1.z * e1.z;
                                        d12 = e1.x * x + e1.z * z;
                                        den = d00 * d11 - d01 * d01;
                                        $IF(abs(den) >= Float(1e-10f)) {
                                            Float u, t;
                                            u = (d11 * d02 - d01 * d12) / den;
                                            t = (d00 * d12 - d01 * d02) / den;
                                            $IF((u >= Float(-1e-4f)) && (t >= Float(-1e-4f)) && (u + t <= Float(1.0001f)) &&
                                                ((h > y) || ((h == y) && (tri < best_tri)))) {
                                                y = h;
                                                nx = n.x / len;
                                                ny = n.y / len;
                                                nz = n.z / len;
                                                best_tri = tri;
                                            }
                                        }
                                    }
                                }
                                k = k + walker_count;
                            }
                            node = nodes.values[meta + Uint(2u)];
                        }
                        $ELSE { node = nodes.values[meta + Uint(3u)]; }
                    }
                    $ELSE { node = nodes.values[meta + Uint(2u)]; }
                }
                Uint o;
                o = lane * Uint(record_size);
                output.values[o] = y;
                output.values[o + Uint(1u)] = nx;
                output.values[o + Uint(2u)] = ny;
                output.values[o + Uint(3u)] = nz;
                Float vf, tf, best;
                vf = 0.0f;
                $IF(lane == Uint(0u)) { vf = Float(visited); }
                tf = Float(tested);
                best = Float(best_tri);
                output.values[o + Uint(4u)] = vf;
                output.values[o + Uint(5u)] = tf;
                output.values[o + Uint(6u)] = best;
                for (std::uint32_t i = 7; i < record_size; ++i) output.values[o + Uint(i)] = 0.0f;
            }
        },
                                        ktm::uvec3(threads, 1, 1));
    }
};

#include "gpu_mesh_motion.h"

enum class Reduction { Sweep, Contact,
                       Vertex,
                       Ground };
struct ReduceKernel {
    Floats input, output;
    Uint count;
    H::ComputePipeline<> pipeline;
    explicit ReduceKernel(Reduction kind) {
        count.as_push_constant();
        pipeline = make_compute_pipeline([&, kind] {
            Uint lane, first;
            lane = dispatchThreadID()->x;
            first = lane * Uint(fan);
            $IF(first < count) {
                std::array<Float, record_size> r;
                for (std::uint32_t i = 0; i < record_size; ++i) r[i] = input.values[first * Uint(record_size) + Uint(i)];
                for (std::uint32_t child = 1; child < fan; ++child) {
                    $IF(first + Uint(child) < count) {
                        std::array<Float, record_size> s;
                        for (std::uint32_t i = 0; i < record_size; ++i) s[i] = input.values[(first + Uint(child)) * Uint(record_size) + Uint(i)];
                        if (kind == Reduction::Sweep) {
                            $IF(s[0] < r[0]) {
                                for (int i=0;i<record_size;++i) r[i]=s[i];
                            }
                        } else if (kind == Reduction::Contact) {
                            merge_contact_record(r, s);
                        } else if (kind == Reduction::Vertex) {
                            $IF((s[0] < r[0]) || ((s[0] == r[0]) && (s[3] < r[3]))) {
                                for (int i = 0; i < 4; ++i) r[i] = s[i];
                            }
                            $IF((s[4] < r[4]) || ((s[4] == r[4]) && (s[8] < r[8]))) {
                                for (int i = 4; i < 9; ++i) r[i] = s[i];
                            }
                        } else {
                            $IF((s[0] > r[0]) || ((s[0] == r[0]) && (s[6] < r[6]))) {
                                for (int i = 0; i < 4; ++i) r[i] = s[i];
                                r[6] = s[6];
                            }
                            r[4] = r[4] + s[4];
                            r[5] = r[5] + s[5];
                        }
                    }
                }
                for (std::uint32_t i = 0; i < record_size; ++i) output.values[lane * Uint(record_size) + Uint(i)] = r[i];
            }
        },
                                        ktm::uvec3(threads, 1, 1));
    }
};

struct Mesh {
    std::uint64_t revision{};
    std::uint64_t shape_version{};
    std::uint64_t snapshot_shape_version{};
    std::uint32_t vertices{}, triangles{}, node_count{};
    std::uint32_t ground_walkers{1};
    H::HardwareBuffer local, previous, world, indices, nodes, leaves, order, bounds;
    H::HardwareBuffer ground_snapshot, ground_world, ground_bounds;
    std::vector<std::pair<H::HardwareBuffer, H::HardwareBuffer>> uploads;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> levels;
    std::vector<GpuCollisionPipeline::VertexSource> sources, completed_sources;
    GpuCollisionPipeline::Matrix prepared_matrix{}, ground_prepared_matrix{};
    bool world_valid{}, tree_valid{}, pose_initialized{}, pending_pose{};
    bool snapshot_valid{}, ground_world_valid{}, ground_tree_valid{};
    float pose_dt{};
    bool closed{};
    float winding{1.0f};
};
struct Scratch {
    H::HardwareBuffer data[2], readback;
    std::uint32_t capacity{};
    void ensure(std::uint32_t count) {
        if (count > capacity) {
            data[0] = buffer(std::size_t(count) * record_size, device_usage);
            data[1] = buffer(std::size_t((count + fan - 1u) / fan) * record_size, device_usage);
            capacity = count;
        }
        if (!readback) readback = buffer(record_size, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    }
};
void validate_instance(const GpuCollisionPipeline::Instance& instance) {
    for (float x : instance.transform)
        if (!std::isfinite(x)) throw std::invalid_argument("Nonfinite GPU collision transform");
}
void validate_point(const GpuCollisionPipeline::Point& point) {
    for (float x : point)
        if (!std::isfinite(x)) throw std::invalid_argument("Nonfinite GPU collision query point");
}
}  // namespace

struct GpuCollisionPipeline::Impl {
    H::HardwareExecutor executor;
    TransformKernel transform;
    RefitKernel refit;
    ContactKernel contact;
    SurfaceContactKernel surface_contact;
    VertexKernel vertex;
    GroundKernel ground_query;
    std::unique_ptr<MeshSweepKernel> mesh_sweep;
    std::unique_ptr<MeshSweepKernel> mesh_turn;
    std::unique_ptr<MeshSlideKernel> mesh_slide;
    std::unique_ptr<ReduceKernel> reduce_sweep;
    H::HardwareBuffer motion_state, motion_contact, motion_callback;
    ReduceKernel reduce_contact{Reduction::Contact}, reduce_vertex{Reduction::Vertex}, reduce_ground{Reduction::Ground};
    std::unordered_map<std::uint64_t, std::unique_ptr<Mesh>> meshes;
    struct CachedContact {
        Pair pair;
        std::uint64_t shape_a{}, shape_b{};
        Contact contact;
    };
    // Only the immediately preceding query batch is retained, bounding memory
    // by active pairs rather than every pair ever seen in a moving scene.
    std::map<std::pair<std::uint64_t,std::uint64_t>, CachedContact> contact_cache;
    std::uint64_t next_shape_version{};
    std::vector<std::unique_ptr<Scratch>> scratch;
    Scratch working, ground_working, contact_pages;
    bool poisoned{};
    std::uint64_t submissions{};
    H::SubmitReceipt last_receipt;

    // Recording mutates host cache state. If recording/commit fails, prohibit
    // reuse rather than publishing cache flags for commands that never ran.
    struct Recording {
        Impl& owner;
        int exceptions{std::uncaught_exceptions()};
        ~Recording() {
            if (std::uncaught_exceptions() > exceptions) owner.poisoned = true;
        }
    };

    ~Impl() {
        if (last_receipt.data) {
            try {
                executor.wait_completed(last_receipt);
            } catch (...) {
            }
        }
    }
    void usable() const {
        if (poisoned) throw std::runtime_error("GPU collision submission is unconfirmed");
    }
    Mesh& mesh(std::uint64_t key) {
        usable();
        auto it = meshes.find(key);
        if (it == meshes.end()) throw std::invalid_argument("GPU collision mesh is not resident");
        return *it->second;
    }
    // Queries execute sequentially in a batch. Share the large reduction
    // buffers; only each query's 48-byte readback must remain independent.
    void reserve_outputs(std::size_t slots, std::uint32_t count, bool for_ground = false) {
        if (!count) return;
        // Ground uses at most 64 records and must not shrink the large contact
        // workspace between physics steps, which would force reallocation.
        auto& workspace = for_ground ? ground_working : working;
        if (workspace.capacity > std::max<std::uint64_t>(4096u, std::uint64_t(count) * 4u)) workspace = {};
        workspace.ensure(count);
        while (scratch.size() < slots) scratch.push_back(std::make_unique<Scratch>());
        if (scratch.size() > slots) scratch.resize(slots);
        for (auto& s : scratch)
            if (!s->readback) s->readback = buffer(record_size, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    }
    Scratch& output(std::size_t slot, std::uint32_t count, bool for_ground = false) {
        const auto& workspace = for_ground ? ground_working : working;
        if (count > workspace.capacity || slot >= scratch.size()) throw std::logic_error("GPU collision scratch was not reserved");
        auto& s = *scratch[slot];
        s.data[0] = workspace.data[0];
        s.data[1] = workspace.data[1];
        return s;
    }
    void finish(H::HardwareStream& stream, bool wait_for_completion = true) {
        poisoned = true;
        // Chain device work, including disjoint meshes, without a CPU fence
        // between pose capture and its consumer. Queue submissions retain the
        // recorded buffers until completion; staging is never rewritten.
        if (last_receipt.data) executor.wait(last_receipt);
        {
            GpuPerformance::OperationScope timing{GpuPerformance::Operation::Commit};
            last_receipt = stream << H::commit();
        }
        ++submissions;
        if (wait_for_completion) {
            GpuPerformance::OperationScope timing{GpuPerformance::Operation::Wait};
            executor.wait_completed(last_receipt);
            last_receipt = {};
        }
        poisoned = false;
        for (auto& s : scratch) {
            s->data[0] = {};
            s->data[1] = {};
        }
    }
    void prepare(H::HardwareStream& stream, const Instance& instance, bool tree, bool world = true) {
        auto& m = mesh(instance.key);
        for (const auto& [src, dst] : m.uploads) {
            GpuPerformance::record_upload(dst.get_byte_size());
            stream << H::CopyBufferCommand{src, dst, 0, 0, dst.get_byte_size()};
        }
        // Move staging into the batch lifetime; repeated prepare must not replay
        // bind uploads after skin import. Recording failures poison the backend.
        auto staged = std::move(m.uploads);
        m.uploads.clear();
        for (auto& pair : staged) in_flight_uploads.push_back(std::move(pair.first));
        if (m.pending_pose) {
            m.shape_version = ++next_shape_version;
            GpuPerformance::record_device_copy(m.local.get_byte_size());
            if (m.pose_initialized) stream << H::CopyBufferCommand{m.local, m.previous, 0, 0, m.local.get_byte_size()};
            for (const auto& s : m.sources) {
                transform.source.bind(s.buffer);
                transform.destination.bind(m.local);
                transform.count = s.vertex_count;
                transform.base = s.vertex_base;
                transform.source_stride = 8u;
                transform.matrix.bind(s.transform);
                launch(stream, transform.pipeline, s.vertex_count);
            }
            if (!m.pose_initialized) stream << H::CopyBufferCommand{m.local, m.previous, 0, 0, m.local.get_byte_size()};
            m.pose_initialized = true;
            m.pending_pose = false;
            m.sources.clear();
            m.world_valid = false;
            m.tree_valid = false;
        }
        if (!world) return;
        if (!m.world_valid || m.prepared_matrix != instance.transform) {
            transform.source.bind(m.local);
            transform.destination.bind(m.world);
            transform.count = m.vertices;
            transform.base = 0u;
            transform.source_stride = 3u;
            transform.matrix.bind(instance.transform);
            launch(stream, transform.pipeline, m.vertices);
            m.prepared_matrix = instance.transform;
            m.world_valid = true;
            m.tree_valid = false;
        }
        if (tree && !m.tree_valid) {
            refit_tree(stream, m, m.world, m.bounds);
            m.tree_valid = true;
        }
    }
    void refit_tree(H::HardwareStream& stream, const Mesh& m, const H::HardwareBuffer& vertices, const H::HardwareBuffer& bounds) {
        refit.vertices.bind(vertices);
        refit.bounds.bind(bounds);
        refit.indices.bind(m.indices);
        refit.nodes.bind(m.nodes);
        refit.leaves.bind(m.leaves);
        refit.order.bind(m.order);
        for (const auto& [offset, count] : m.levels) {
            refit.offset = offset;
            refit.count = count;
            launch(stream, refit.pipeline, count);
        }
    }
    void prepare_ground_snapshot(H::HardwareStream& stream, const Instance& instance) {
        auto& m = mesh(instance.key);
        // snapshot_ground already consumed bind uploads and captured the local
        // pose. Preparing a snapshot must not transform or refit the live pose.
        if (!m.ground_world_valid || m.ground_prepared_matrix != instance.transform) {
            transform.source.bind(m.ground_snapshot);
            transform.destination.bind(m.ground_world);
            transform.count = m.vertices;
            transform.base = 0u;
            transform.source_stride = 3u;
            transform.matrix.bind(instance.transform);
            launch(stream, transform.pipeline, m.vertices);
            m.ground_prepared_matrix = instance.transform;
            m.ground_world_valid = true;
            m.ground_tree_valid = false;
        }
        if (!m.ground_tree_valid) {
            refit_tree(stream, m, m.ground_world, m.ground_bounds);
            m.ground_tree_valid = true;
        }
    }
    std::vector<H::HardwareBuffer> in_flight_uploads;
    void reduce(H::HardwareStream& stream, Scratch& s, std::uint32_t count, ReduceKernel& kernel,
                const H::HardwareBuffer* destination = nullptr, std::uint32_t destination_record = 0) {
        unsigned source = 0;
        while (count > 1u) {
            kernel.input.bind(s.data[source]);
            kernel.output.bind(s.data[1u - source]);
            kernel.count = count;
            count = (count + fan - 1u) / fan;
            launch(stream, kernel.pipeline, count);
            source = 1u - source;
        }
        if (destination) {
            GpuPerformance::record_device_copy(record_size * sizeof(float));
            stream << H::CopyBufferCommand{s.data[source], *destination, 0,
                std::size_t(destination_record) * record_size * sizeof(float), record_size * sizeof(float)};
        } else {
            GpuPerformance::record_readback(record_size * sizeof(float));
            stream << H::CopyBufferCommand{s.data[source], s.readback, 0, 0, record_size * sizeof(float)};
        }
    }
    std::array<float, record_size> read(std::size_t slot) {
        std::array<float, record_size> r{};
        if (!scratch[slot]->readback.read_bytes(std::as_writable_bytes(std::span(r)))) throw std::runtime_error("GPU collision result readback failed");
        return r;
    }
};

GpuCollisionPipeline::GpuCollisionPipeline() : impl_(std::make_unique<Impl>()) {}
GpuCollisionPipeline::~GpuCollisionPipeline() = default;

void GpuCollisionPipeline::set_mesh(std::uint64_t key, std::uint64_t revision,
                                    const MechanicsInternal::CollisionMesh& input, const MechanicsInternal::TriangleOctree& tree) {
    GpuPerformance::Scope performance{"collision.set_mesh"};
    impl_->usable();
    if (auto it = impl_->meshes.find(key); it != impl_->meshes.end() && it->second->revision == revision) return;
    if (input.vertices.empty() || input.vertices.size() > max_items || input.triangles.size() > max_items || tree.nodes.size() > max_items)
        throw std::invalid_argument("GPU collision mesh exceeds supported dispatch size or has no vertices");
    auto m = std::make_unique<Mesh>();
    m->revision = revision;
    m->shape_version = ++impl_->next_shape_version;
    m->vertices = static_cast<std::uint32_t>(input.vertices.size());
    m->triangles = static_cast<std::uint32_t>(input.triangles.size());
    m->node_count = static_cast<std::uint32_t>(tree.nodes.size());
    if (m->triangles && !m->node_count) throw std::invalid_argument("GPU collision triangle mesh has no octree");
    std::vector<float> positions(std::size_t(m->vertices) * 3u);
    for (std::size_t i = 0; i < input.vertices.size(); ++i) {
        validate_point({input.vertices[i].x, input.vertices[i].y, input.vertices[i].z});
        positions[i * 3] = input.vertices[i].x;
        positions[i * 3 + 1] = input.vertices[i].y;
        positions[i * 3 + 2] = input.vertices[i].z;
    }
    std::vector<std::uint32_t> indices(std::size_t(m->triangles) * 3u);
    if (!std::is_sorted(input.submeshes.begin(), input.submeshes.end(),
        [](const auto& a, const auto& b) { return a.triangle_base < b.triangle_base; }))
        throw std::invalid_argument("Collision segments are not in triangle order");
    std::size_t segment = 0;
    // Resolve every uint16 corner through its segment without the legacy
    // uint16(vertex_count) assertion (65536 is a valid segment size).
    for (std::uint32_t t = 0; t < m->triangles; ++t) {
        while (segment < input.submeshes.size() &&
               t >= std::uint64_t(input.submeshes[segment].triangle_base) + input.submeshes[segment].triangle_count)
            ++segment;
        for (int c = 0; c < 3; ++c) {
            std::uint32_t v = input.triangles[t][c];
            bool found = input.submeshes.empty();
            if (segment < input.submeshes.size()) {
                const auto& s = input.submeshes[segment];
                if (t >= s.triangle_base && t - s.triangle_base < s.triangle_count) {
                    if (v >= s.vertex_count) throw std::invalid_argument("GPU collision segment index out of range");
                    v += s.vertex_base;
                    found = true;
                }
            }
            if (!found || v >= m->vertices) throw std::invalid_argument("GPU collision vertex index out of range");
            indices[t * 3u + c] = v;
        }
    }
    // Import seams duplicate vertices. Weld positions only for the topology
    // check; rendering/skinning indices and deformation buffers stay intact.
    std::map<std::array<float, 3>, std::uint32_t> welded;
    std::vector<std::uint32_t> vertex_ids(m->vertices);
    for (std::uint32_t v = 0; v < m->vertices; ++v) {
        const auto& p = input.vertices[v];
        vertex_ids[v] = welded.try_emplace({p.x, p.y, p.z}, static_cast<std::uint32_t>(welded.size())).first->second;
    }
    struct EdgeUse { unsigned count{}; int orientation{}; };
    std::map<std::pair<std::uint32_t, std::uint32_t>, EdgeUse> edges;
    double volume = 0;
    const auto origin = input.vertices.front();
    for (std::uint32_t t = 0; t < m->triangles; ++t) {
        for (std::uint32_t c = 0; c < 3; ++c) {
            const auto a = vertex_ids[indices[t * 3 + c]], b = vertex_ids[indices[t * 3 + (c + 1) % 3]];
            auto& use = edges[{std::min(a, b), std::max(a, b)}];
            ++use.count; use.orientation += a < b ? 1 : -1;
        }
        const auto a = input.vertices[indices[t * 3]] - origin;
        const auto b = input.vertices[indices[t * 3 + 1]] - origin;
        const auto c = input.vertices[indices[t * 3 + 2]] - origin;
        volume += double(a.x) * (double(b.y) * c.z - double(b.z) * c.y) +
                  double(a.y) * (double(b.z) * c.x - double(b.x) * c.z) +
                  double(a.z) * (double(b.x) * c.y - double(b.y) * c.x);
    }
    m->closed = !edges.empty() && volume != 0 && std::all_of(edges.begin(), edges.end(), [](const auto& edge) {
        return edge.first.first != edge.first.second && edge.second.count == 2 && edge.second.orientation == 0;
    });
    m->winding = volume < 0 ? -1.0f : 1.0f;
    std::vector<std::uint32_t> nodes(std::size_t(m->node_count) * node_size), order;
    std::vector<std::vector<std::uint32_t>> levels;
    std::vector<bool> seen(m->node_count), seen_tris(m->triangles);
    auto visit = [&](auto&& self, std::uint32_t n, std::size_t depth) -> std::uint32_t {
        if (n >= m->node_count || seen[n] || depth > 21) throw std::invalid_argument("Invalid GPU collision octree");
        seen[n] = true;
        if (levels.size() <= depth) levels.resize(depth + 1);
        levels[depth].push_back(n);
        const auto& src = tree.nodes[n];
        auto* dst = nodes.data() + std::size_t(n) * node_size;
        dst[0] = src.tri_begin;
        dst[1] = src.tri_count;
        dst[3] = src.child_begin;
        dst[4] = src.child_count == 0;
        dst[5] = src.child_count;
        std::uint32_t end = n + 1;
        if (src.child_count) {
            if (src.child_count > 8 || src.child_begin != n + 1) throw std::invalid_argument("Invalid GPU octree children");
            end = src.child_begin;
            for (std::uint32_t c = 0; c < src.child_count; ++c) {
                dst[6 + c] = end;
                end = self(self, end, depth + 1);
            }
        } else {
            if (std::uint64_t(src.tri_begin) + src.tri_count > tree.leaf_tris.size()) throw std::invalid_argument("Invalid GPU octree leaf range");
            m->ground_walkers = std::max(m->ground_walkers, std::min(src.tri_count, threads));
            for (std::uint32_t k = 0; k < src.tri_count; ++k) {
                const auto t = tree.leaf_tris[src.tri_begin + k];
                if (t >= m->triangles || seen_tris[t]) throw std::invalid_argument("Invalid GPU octree leaf triangle");
                seen_tris[t] = true;
            }
        }
        dst[2] = end;
        return end;
    };
    if (m->node_count && (visit(visit, 0, 0) != m->node_count || std::find(seen_tris.begin(), seen_tris.end(), false) != seen_tris.end()))
        throw std::invalid_argument("Incomplete GPU collision octree");
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
        m->levels.emplace_back(static_cast<std::uint32_t>(order.size()), static_cast<std::uint32_t>(it->size()));
        order.insert(order.end(), it->begin(), it->end());
    }
    auto init = [&](auto& target, const auto& data) {target=buffer(std::max<std::size_t>(data.size(),1u),device_usage);if(!data.empty())m->uploads.emplace_back(upload(data),target); };
    init(m->local, positions);
    init(m->previous, positions);
    init(m->indices, indices);
    init(m->nodes, nodes);
    init(m->leaves, tree.leaf_tris);
    init(m->order, order);
    m->world = buffer(std::size_t(m->vertices) * 3u, device_usage);
    m->bounds = buffer(std::size_t(m->node_count) * bounds_size, device_usage);
    impl_->meshes[key] = std::move(m);
}
bool GpuCollisionPipeline::contains(std::uint64_t key) const { return impl_->meshes.contains(key); }
bool GpuCollisionPipeline::contains(std::uint64_t key, std::uint64_t revision) const {
    const auto it = impl_->meshes.find(key);
    return it != impl_->meshes.end() && it->second->revision == revision;
}
void GpuCollisionPipeline::prepare_instances(std::span<const Instance> instances) {
    GpuPerformance::Scope performance{"collision.prepare"};
    GpuPerformance::record_work_items(instances.size());
    impl_->usable();
    if (instances.empty()) return;
    bool needs_prepare = false;
    for (const auto& instance : instances) {
        const auto& m = impl_->mesh(instance.key);
        for (float value : instance.transform)
            if (!std::isfinite(value)) throw std::invalid_argument("Invalid GPU collision transform");
        needs_prepare |= !m.uploads.empty() || m.pending_pose || !m.world_valid ||
            !m.tree_valid || m.prepared_matrix != instance.transform;
    }
    if (!needs_prepare) return;
    Impl::Recording recording{*impl_};
    auto stream = impl_->executor.stream();
    for (const auto& instance : instances) impl_->prepare(stream, instance, true);
    impl_->finish(stream);
    impl_->in_flight_uploads.clear();
}
void GpuCollisionPipeline::update_vertices(std::uint64_t key, std::span<const VertexSource> sources, float pose_dt) {
    const VertexUpdate update{key, sources, pose_dt};
    update_vertices(std::span(&update, 1));
}
void GpuCollisionPipeline::update_vertices(std::span<const VertexUpdate> updates, bool wait_for_completion) {
    GpuPerformance::Scope performance{"collision.update"};
    GpuPerformance::record_work_items(updates.size());
    impl_->usable();
    if (updates.empty()) return;
    std::unordered_set<std::uint64_t> keys;
    keys.reserve(updates.size());
    std::vector<const VertexUpdate*> changed;
    // Reject the entire batch before changing source lists, pose history, or
    // cache flags. In particular, an invalid later entry must not advance an
    // earlier mesh's history or consume its pending bind uploads.
    for (const auto& update : updates) {
        auto& m = impl_->mesh(update.key);
        if (!keys.insert(update.key).second) throw std::invalid_argument("Duplicate GPU collision vertex update");
        if (!std::isfinite(update.pose_dt) || update.pose_dt < 0.0f) throw std::invalid_argument("Invalid GPU collision pose time");
        if (m.pending_pose) throw std::logic_error("GPU collision pose must be consumed before another update");
        for (const auto& s : update.sources) {
            if (!s.buffer || std::uint64_t(s.vertex_base) + s.vertex_count > m.vertices || s.buffer.get_byte_size() < std::uint64_t(s.vertex_count) * 32u)
                throw std::invalid_argument("Invalid GPU collision skin source");
            for (float v : s.transform)
                if (!std::isfinite(v)) throw std::invalid_argument("Invalid GPU collision source transform");
        }
        const bool reuse = m.pose_initialized && std::equal(update.sources.begin(), update.sources.end(),
            m.completed_sources.begin(), m.completed_sources.end(), [](const auto& a, const auto& b) {
                return a.revision != 0 && a.revision == b.revision && a.buffer.same_resource(b.buffer) &&
                    a.vertex_base == b.vertex_base && a.vertex_count == b.vertex_count && a.transform == b.transform;
            });
        if (!reuse) changed.push_back(&update);
    }
    // A stationary pose has zero surface velocity even if its previous pose
    // differs. No history copy, world transform, or refit is needed to express it.
    for (const auto& update : updates) impl_->mesh(update.key).pose_dt = 0.0f;
    if (changed.empty()) {
        if (wait_for_completion && impl_->last_receipt.data) {
            impl_->poisoned = true;
            GpuPerformance::OperationScope timing{GpuPerformance::Operation::Wait};
            impl_->executor.wait_completed(impl_->last_receipt);
            impl_->last_receipt = {};
            impl_->poisoned = false;
        }
        return;
    }
    // Capture source contents now: skin outputs are reused on the next animation
    // tick, including when physics is paused. This is a device-only transfer.
    Impl::Recording recording{*impl_};
    auto stream = impl_->executor.stream();
    Instance instance{};
    instance.transform[0] = instance.transform[5] = instance.transform[10] = instance.transform[15] = 1.0f;
    for (const auto* entry : changed) {
        const auto& update = *entry;
        auto& m = impl_->mesh(update.key);
        m.sources.assign(update.sources.begin(), update.sources.end());
        m.pose_dt = update.pose_dt;
        m.pending_pose = true;
        instance.key = update.key;
        // The consumer will transform directly to the actual instance pose.
        // Transforming to identity here only invalidates a useful world cache.
        impl_->prepare(stream, instance, false, false);
    }
    impl_->finish(stream, wait_for_completion);
    impl_->in_flight_uploads.clear();
    for (const auto* update : changed)
        impl_->mesh(update->key).completed_sources.assign(update->sources.begin(), update->sources.end());
}
std::uint64_t GpuCollisionPipeline::submission_count() const noexcept { return impl_->submissions; }

void GpuCollisionPipeline::snapshot_ground(std::span<const std::uint64_t> keys) {
    GpuPerformance::Scope performance{"collision.snapshot"};
    GpuPerformance::record_work_items(keys.size());
    impl_->usable();
    if (keys.empty()) return;
    std::vector<std::uint64_t> changed;
    std::unordered_set<std::uint64_t> seen;
    for (auto key : keys) {
        const auto& m = impl_->mesh(key);
        if (seen.insert(key).second &&
            (!m.snapshot_valid || m.pending_pose || m.snapshot_shape_version != m.shape_version))
            changed.push_back(key);
    }
    if (changed.empty()) return;
    Impl::Recording recording{*impl_};
    auto stream = impl_->executor.stream();
    Matrix identity{};
    identity[0] = identity[5] = identity[10] = identity[15] = 1;
    for (auto key : changed) {
        auto& m = impl_->mesh(key);
        // A local snapshot must not overwrite the live world transform cache.
        impl_->prepare(stream, {key, identity}, false, false);
        if (!m.ground_snapshot) {
            m.ground_snapshot = buffer(std::size_t(m.vertices) * 3u, device_usage);
            m.ground_world = buffer(std::size_t(m.vertices) * 3u, device_usage);
            m.ground_bounds = buffer(std::size_t(m.node_count) * bounds_size, device_usage);
        }
        GpuPerformance::record_device_copy(m.local.get_byte_size());
        stream << H::CopyBufferCommand{m.local, m.ground_snapshot, 0, 0, m.local.get_byte_size()};
        m.snapshot_valid = true;
        m.snapshot_shape_version = m.shape_version;
        m.ground_world_valid = false;
        m.ground_tree_valid = false;
    }
    impl_->finish(stream);
    impl_->in_flight_uploads.clear();
}

std::vector<GpuCollisionPipeline::Contact> GpuCollisionPipeline::contacts(std::span<const Pair> pairs) {
    GpuPerformance::Scope performance{"collision.contacts"};
    GpuPerformance::record_work_items(pairs.size());
    impl_->usable();
    std::vector<Contact> result(pairs.size());
    if (pairs.empty()) { impl_->contact_cache.clear(); return result; }
    std::vector<bool> cached(pairs.size(), false);
    std::uint32_t max_count = 0, max_pages = 0;
    const auto pages = [](std::uint32_t n) { return (n + contact_page_size - 1u) / contact_page_size; };
    for (const auto& p : pairs) {
        impl_->mesh(p.a.key);
        impl_->mesh(p.b.key);
        validate_instance(p.a);
        validate_instance(p.b);
        if (p.a.key == p.b.key) throw std::invalid_argument("GPU collision self pair");
        if (!std::isfinite(p.threshold) || p.threshold < 0.0f) throw std::invalid_argument("Invalid GPU collision contact threshold");
        const auto a = impl_->mesh(p.a.key).triangles, b = impl_->mesh(p.b.key).triangles;
        max_pages = std::max(max_pages, pages(std::min(a,b)) + 2u);
        max_count = std::max(max_count, std::max(surface_walkers, std::min(contact_page_size, std::min(a,b))));
    }
    for (std::size_t i=0;i<pairs.size();++i) {
        const auto& p = pairs[i];
        const auto entry = impl_->contact_cache.find({p.a.key,p.b.key});
        if (entry == impl_->contact_cache.end()) continue;
        const auto& old = entry->second;
        if (old.shape_a == impl_->mesh(p.a.key).shape_version && old.shape_b == impl_->mesh(p.b.key).shape_version &&
            old.pair.a.transform == p.a.transform && old.pair.b.transform == p.b.transform && old.pair.threshold == p.threshold) {
            result[i] = old.contact;
            cached[i] = true;
        }
    }
    const auto remember_results = [&] {
        decltype(impl_->contact_cache) next_cache;
        for (std::size_t i=0;i<pairs.size();++i) {
            const auto& p = pairs[i];
            next_cache.insert_or_assign({p.a.key,p.b.key}, Impl::CachedContact{
                p, impl_->mesh(p.a.key).shape_version, impl_->mesh(p.b.key).shape_version, result[i]});
        }
        impl_->contact_cache = std::move(next_cache);
    };
    if (std::all_of(cached.begin(),cached.end(),[](bool value){return value;})) {
        remember_results();
        return result;
    }
    impl_->reserve_outputs(pairs.size(), max_count);
    impl_->contact_pages.ensure(max_pages);
    Impl::Recording recording{*impl_};
    auto stream = impl_->executor.stream();
    bool work = false;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (cached[i]) continue;
        const auto& p = pairs[i];
        auto& a = impl_->mesh(p.a.key);
        auto& b = impl_->mesh(p.b.key);
        if (!a.triangles || !b.triangles) continue;
        work = true;
        impl_->prepare(stream, p.a, true);
        impl_->prepare(stream, p.b, true);
        auto& s = impl_->output(i, max_count);
        std::uint32_t page_index = 0;
        const bool reverse = a.triangles > b.triangles;
        auto& from = reverse ? b : a;
        auto& to = reverse ? a : b;
        auto& k = impl_->contact;
        k.va.bind(from.world);
        k.vb.bind(to.world);
        k.ia.bind(from.indices);
        k.ib.bind(to.indices);
        k.bounds.bind(to.bounds);
        k.nodes.bind(to.nodes);
        k.leaves.bind(to.leaves);
        k.output.bind(s.data[0]);
        k.node_count = to.node_count;
        k.threshold = p.threshold;
        k.reverse = reverse ? 1u : 0u;
        for (std::uint32_t first = 0; first < from.triangles; first += contact_page_size) {
            const auto count = std::min(contact_page_size, from.triangles - first);
            k.first = first; k.count = count;
            launch(stream, k.pipeline, count);
            impl_->reduce(stream, s, count, impl_->reduce_contact, &impl_->contact_pages.data[0], page_index++);
        }
        const auto determinant = [](const Matrix& m) {
            return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
        };
        const auto surface_pass = [&](Mesh& from, Mesh& to, float from_det, float to_det, bool reverse) {
            auto& kernel = impl_->surface_contact;
            kernel.source.bind(from.world); kernel.target.bind(to.world);
            kernel.source_indices.bind(from.indices); kernel.target_indices.bind(to.indices);
            kernel.bounds.bind(to.bounds); kernel.nodes.bind(to.nodes); kernel.leaves.bind(to.leaves);
            kernel.source_bounds.bind(from.bounds);
            kernel.source_nodes.bind(from.nodes); kernel.source_leaves.bind(from.leaves);
            kernel.source_node_count = from.node_count;
            kernel.output.bind(s.data[0]);
            kernel.node_count = to.node_count; kernel.reverse = reverse ? 1u : 0u;
            kernel.closed = to.closed && std::abs(to_det) > 1e-12f ? 1u : 0u;
            kernel.winding = to_det < 0 ? -to.winding : to.winding;
            kernel.source_closed = from.closed && std::abs(from_det) > 1e-12f ? 1u : 0u;
            kernel.source_winding = from_det < 0 ? -from.winding : from.winding;
            kernel.threshold = p.threshold;
            const auto count = std::min(surface_walkers, from.triangles * 7u);
            kernel.count = count;
            launch(stream, kernel.pipeline, count);
            impl_->reduce(stream, s, count, impl_->reduce_contact, &impl_->contact_pages.data[0], page_index++);
        };
        surface_pass(a, b, determinant(p.a.transform), determinant(p.b.transform), false);
        surface_pass(b, a, determinant(p.b.transform), determinant(p.a.transform), true);
        impl_->contact_pages.readback = s.readback;
        impl_->reduce(stream, impl_->contact_pages, page_index, impl_->reduce_contact);
    }
    if (work) {
        impl_->finish(stream);
        impl_->in_flight_uploads.clear();
    }
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        if (cached[i]) continue;
        if (!impl_->mesh(pairs[i].a.key).triangles || !impl_->mesh(pairs[i].b.key).triangles) continue;
        const auto r = impl_->read(i);
        auto& out = result[i];
        out.has_contact = r[7] > 0;
        out.penetration = out.has_contact ? r[0] : 0.0f;
        out.normal = {r[1], r[2], r[3]};
        if (out.has_contact) {
            out.point = {r[4] / r[7], r[5] / r[7], r[6] / r[7]};
            if (r[9] >= 0) {
                out.triangle_a = static_cast<int>(r[8]);
                out.triangle_b = static_cast<int>(r[9]);
            }
        }
    }
    remember_results();
    return result;
}
std::vector<GpuCollisionPipeline::VertexResult> GpuCollisionPipeline::sample_vertices(std::span<const VertexQuery> queries) {
    GpuPerformance::Scope performance{"collision.vertices"};
    GpuPerformance::record_work_items(queries.size());
    impl_->usable();
    std::vector<VertexResult> result(queries.size());
    if (queries.empty()) return result;
    std::uint32_t max_count = 0;
    for (const auto& q : queries) {
        validate_instance(q.instance);
        validate_point(q.contact_model);
        max_count = std::max(max_count, impl_->mesh(q.instance.key).vertices);
    }
    impl_->reserve_outputs(queries.size(), max_count);
    Impl::Recording recording{*impl_};
    auto stream = impl_->executor.stream();
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto& q = queries[i];
        auto& m = impl_->mesh(q.instance.key);
        impl_->prepare(stream, q.instance, false);
        auto& s = impl_->output(i, m.vertices);
        auto& k = impl_->vertex;
        k.local.bind(m.local);
        k.previous.bind(m.previous);
        k.world.bind(m.world);
        k.output.bind(s.data[0]);
        k.count = m.vertices;
        k.px = q.contact_model[0];
        k.py = q.contact_model[1];
        k.pz = q.contact_model[2];
        k.inverse_dt = m.pose_dt > 1e-10f ? 1.0f / m.pose_dt : 0.0f;
        k.matrix.bind(q.instance.transform);
        launch(stream, k.pipeline, m.vertices);
        impl_->reduce(stream, s, m.vertices, impl_->reduce_vertex);
    }
    impl_->finish(stream);
    impl_->in_flight_uploads.clear();
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto r = impl_->read(i);
        result[i] = {{r[1], r[0], r[2]}, {r[5], r[6], r[7]}, static_cast<std::uint32_t>(r[3])};
    }
    return result;
}
std::vector<GpuCollisionPipeline::GroundResult> GpuCollisionPipeline::ground(std::span<const GroundQuery> queries) {
    GpuPerformance::Scope performance{"collision.ground"};
    GpuPerformance::record_work_items(queries.size());
    impl_->usable();
    std::vector<GroundResult> result(queries.size());
    if (queries.empty()) return result;
    std::uint32_t max_count = 0;
    for (const auto& q : queries) {
        auto& m = impl_->mesh(q.instance.key);
        validate_instance(q.instance);
        validate_point(q.probe);
        if (!std::isfinite(q.max_drop) || q.max_drop < 0) throw std::invalid_argument("Invalid GPU collision ground drop");
        if (q.use_snapshot && !m.snapshot_valid) throw std::invalid_argument("GPU collision ground snapshot is not available");
        if (m.triangles) max_count = std::max(max_count, m.ground_walkers);
    }
    impl_->reserve_outputs(queries.size(), max_count, true);
    Impl::Recording recording{*impl_};
    auto stream = impl_->executor.stream();
    bool work = false;
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const auto& q = queries[i];
        auto& m = impl_->mesh(q.instance.key);
        if (!m.triangles) continue;
        work = true;
        if (q.use_snapshot) impl_->prepare_ground_snapshot(stream, q.instance);
        else impl_->prepare(stream, q.instance, true);
        auto& s = impl_->output(i, m.ground_walkers, true);
        auto& k = impl_->ground_query;
        k.world.bind(q.use_snapshot ? m.ground_world : m.world);
        k.bounds.bind(q.use_snapshot ? m.ground_bounds : m.bounds);
        k.indices.bind(m.indices);
        k.nodes.bind(m.nodes);
        k.leaves.bind(m.leaves);
        k.output.bind(s.data[0]);
        k.node_count = m.node_count;
        k.walker_count = m.ground_walkers;
        k.px = q.probe[0];
        k.py = q.probe[1];
        k.pz = q.probe[2];
        k.drop = q.max_drop;
        launch(stream, k.pipeline, m.ground_walkers);
        impl_->reduce(stream, s, m.ground_walkers, impl_->reduce_ground);
    }
    if (work) {
        impl_->finish(stream);
        impl_->in_flight_uploads.clear();
    }
    for (std::size_t i = 0; i < queries.size(); ++i) {
        if (!impl_->mesh(queries[i].instance.key).triangles) continue;
        const auto r = impl_->read(i);
        result[i] = {r[0] > -huge, r[0], {r[1], r[2], r[3]}, static_cast<std::uint32_t>(r[4]), static_cast<std::uint32_t>(r[5])};
    }
    return result;
}
GpuCollisionPipeline::MeshMotionResult GpuCollisionPipeline::move_mesh(
    const Instance& moving, std::span<const Instance> obstacles, Point displacement, bool snap_to_ground,
    float turn, Point pivot, Point turn_axis) {
    GpuPerformance::Scope performance{"collision.mesh_motion"};
    impl_->usable(); validate_instance(moving); validate_point(displacement);
    validate_point(pivot); validate_point(turn_axis);
    const float axis_length=std::sqrt(turn_axis[0]*turn_axis[0]+turn_axis[1]*turn_axis[1]+turn_axis[2]*turn_axis[2]);
    if(!std::isfinite(turn) || std::abs(turn)>3.142f || std::abs(axis_length-1.f)>.0001f)
        throw std::invalid_argument("Invalid mesh turn");
    auto& a=impl_->mesh(moving.key);
    if(!a.triangles) throw std::invalid_argument("Moving mesh has no triangles");
    if(obstacles.empty()) { MeshMotionResult result{displacement}; result.turn=turn; return result; }
    for(const auto& obstacle:obstacles) {
        validate_instance(obstacle); (void)impl_->mesh(obstacle.key);
        if(obstacle.key==moving.key) throw std::invalid_argument("Mesh motion self collision");
    }
    if(!impl_->mesh_sweep) impl_->mesh_sweep=std::make_unique<MeshSweepKernel>();
    if(turn!=0 && !impl_->mesh_turn) impl_->mesh_turn=std::make_unique<MeshSweepKernel>(true);
    if(!impl_->mesh_slide) impl_->mesh_slide=std::make_unique<MeshSlideKernel>();
    if(!impl_->reduce_sweep) impl_->reduce_sweep=std::make_unique<ReduceKernel>(Reduction::Sweep);
    if(!impl_->motion_state) {
        impl_->motion_state=buffer(48,device_usage);
        impl_->motion_contact=buffer(record_size,device_usage);
        impl_->motion_callback=buffer(24,H::BufferUsage_TransferDst,H::CpuAccessMode::Read);
    }
    const auto pages=(a.triangles+contact_page_size-1)/contact_page_size;
    if(obstacles.size()>4096 || std::uint64_t(pages)*obstacles.size()>max_items)
        throw std::invalid_argument("Too many mesh motion candidates");
    impl_->reserve_outputs(1,std::min(a.triangles,contact_page_size));
    impl_->contact_pages.ensure(static_cast<std::uint32_t>(pages*obstacles.size()));
    std::array<float,48> initial{};
    std::copy(displacement.begin(),displacement.end(),initial.begin()+3);
    std::copy(turn_axis.begin(),turn_axis.end(),initial.begin()+6);
    initial[19]=turn;
    std::copy(pivot.begin(),pivot.end(),initial.begin()+20);
    initial[15]=initial[16]=initial[17]=-1;
    initial[18]=displacement[1]<=0 ? (snap_to_ground ? -.12f : -.006f) : 0.f;
    auto staging=upload(std::span<const float>(initial));
    Impl::Recording recording{*impl_};
    auto stream=impl_->executor.stream();
    stream << H::CopyBufferCommand{staging,impl_->motion_state,0,0,sizeof(initial)};
    impl_->prepare(stream,moving,true);
    for(const auto& obstacle:obstacles) impl_->prepare(stream,obstacle,true);
    for(int pass=turn!=0 ? -1 : 0;pass<7;++pass) {
        const bool turning=pass<0;
        const auto iteration=static_cast<std::uint32_t>(std::max(0,pass));
        auto& s=impl_->output(0,std::min(a.triangles,contact_page_size));
        std::uint32_t page=0;
        for(std::size_t oi=0;oi<obstacles.size();++oi) {
            auto& b=impl_->mesh(obstacles[oi].key);
            auto& k=turning ? *impl_->mesh_turn : *impl_->mesh_sweep;
            k.va.bind(a.world); k.ia.bind(a.indices); k.vb.bind(b.world); k.ib.bind(b.indices);
            k.bounds.bind(b.bounds); k.nodes.bind(b.nodes); k.leaves.bind(b.leaves);
            k.state.bind(impl_->motion_state); k.output.bind(s.data[0]);
            k.node_count=b.node_count; k.obstacle=static_cast<std::uint32_t>(oi);
            for(std::uint32_t first=0;first<a.triangles;first+=contact_page_size) {
                const auto count=std::min(contact_page_size,a.triangles-first);
                k.first=first; k.count=count;
                launch(stream,k.pipeline,count);
                impl_->reduce(stream,s,count,*impl_->reduce_sweep,&impl_->contact_pages.data[0],page++);
            }
        }
        impl_->reduce(stream,impl_->contact_pages,page,*impl_->reduce_sweep,&impl_->motion_contact);
        auto& slide=*impl_->mesh_slide;
        slide.state.bind(impl_->motion_state); slide.contact.bind(impl_->motion_contact);
        slide.iteration=iteration; slide.snap=turning ? 2u : iteration==6 ? 1u : 0u;
        launch(stream,slide.pipeline,1);
    }
    // No per-contact, per-iteration, vertex, depth or image readbacks. This is
    // the single compact completion record used for the frontend callback.
    GpuPerformance::record_readback(24*sizeof(float));
    stream << H::CopyBufferCommand{impl_->motion_state,impl_->motion_callback,0,0,24*sizeof(float)};
    impl_->in_flight_uploads.push_back(staging);
    impl_->finish(stream); impl_->in_flight_uploads.clear();
    std::array<float,24> r{};
    if(!impl_->motion_callback.read_bytes(std::as_writable_bytes(std::span(r))))
        throw std::runtime_error("Mesh motion callback transfer failed");
    return {{r[0],r[1],r[2]},{r[12],r[13],r[14]},r[9]>0,r[10]>0,r[11]>0,
        static_cast<int>(r[15]),static_cast<int>(r[16]),static_cast<int>(r[17]),r[23]};
}

void GpuCollisionPipeline::retain(std::span<const std::uint64_t> keys) {
    impl_->usable();
    const std::unordered_set<std::uint64_t> active(keys.begin(), keys.end());
    std::erase_if(impl_->meshes, [&](const auto& entry) { return !active.contains(entry.first); });
    std::erase_if(impl_->contact_cache, [&](const auto& entry) {
        return !active.contains(entry.first.first) || !active.contains(entry.first.second);
    });
    if (impl_->meshes.empty()) {
        impl_->scratch.clear();
        impl_->working = {};
        impl_->ground_working = {};
    }
}
}  // namespace Corona::Systems
