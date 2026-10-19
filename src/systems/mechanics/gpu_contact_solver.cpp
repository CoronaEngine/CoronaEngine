#include "gpu_contact_solver.h"
#include "gpu_performance.h"
#include "gpu_compute_pipeline.h"

#include <horizon.h>
#include <Codegen/BuiltinVariate.h>
#include <Codegen/ControlFlows.h>
#include <Codegen/CustomLibrary.h>
#include <Codegen/TypeAlias.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace Corona::Systems {
namespace {
namespace H = Corona::Horizon;
using namespace EmbeddedShader;
using Vec = std::array<Float, 3>;
constexpr std::uint32_t threads = 64, reduction_width = 64;
constexpr std::uint32_t body_words = 17, contact_words = 13, result_words = 6;
constexpr auto device_usage = H::BufferUsage_Storage | H::BufferUsage_TransferSrc | H::BufferUsage_TransferDst;
static_assert(GpuContactSolverPipeline::max_contacts <= threads * 65535u);
static_assert(std::uint64_t(GpuContactSolverPipeline::max_bodies) * body_words * sizeof(float) <= 128u * 1024u * 1024u);
static_assert(sizeof(GpuContactSolverPipeline::Velocity) == 6 * sizeof(float));
static_assert(sizeof(GpuContactSolverPipeline::ContactResult) == result_words * sizeof(float));

template <class T>
struct Binding {
    H::HardwareBuffer handle;
    EmbeddedShader::Array<T> values;
    void bind(const H::HardwareBuffer& value) { handle = value; values = handle; }
};
using Floats = Binding<float>;
using UInts = Binding<std::uint32_t>;

Vec load3(Floats& buffer, const Uint& base) {
    Vec result;
    for (std::uint32_t a = 0; a < 3; ++a) result[a] = buffer.values[base + Uint(a)];
    return result;
}
Vec add(const Vec& a, const Vec& b) {
    Vec r;
    for (std::uint32_t k = 0; k < 3; ++k) r[k] = a[k] + b[k];
    return r;
}
Vec sub(const Vec& a, const Vec& b) {
    Vec r;
    for (std::uint32_t k = 0; k < 3; ++k) r[k] = a[k] - b[k];
    return r;
}
Vec mul(const Vec& a, const Float& scale) {
    Vec r;
    for (std::uint32_t k = 0; k < 3; ++k) r[k] = a[k] * scale;
    return r;
}
Vec neg(const Vec& a) {
    Vec r;
    for (std::uint32_t k = 0; k < 3; ++k) r[k] = Float(0.0f) - a[k];
    return r;
}
Float dot3(const Vec& a, const Vec& b) {
    Float r;
    r = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return r;
}
Vec cross3(const Vec& a, const Vec& b) {
    Vec r;
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
    return r;
}
void add_in_place(Vec& a, const Vec& b) {
    for (std::uint32_t k = 0; k < 3; ++k) a[k] = a[k] + b[k];
}
void clamp_speed(Vec& velocity, const Float& limit) {
    Float squared;
    squared = dot3(velocity, velocity);
    $IF(squared > limit * limit) {
        Float scale;
        scale = limit / sqrt(squared);
        for (auto& component : velocity) component = component * scale;
    }
}
Vec inertia_apply(Floats& bodies, const Uint& base, const Vec& world) {
    // R * (diag(I^-1) * (transpose(R) * world)); packed R is column-major.
    Vec local, result;
    for (std::uint32_t a = 0; a < 3; ++a) {
        local[a] = bodies.values[base + Uint(6u + 3u * a)] * world[0] +
                   bodies.values[base + Uint(7u + 3u * a)] * world[1] +
                   bodies.values[base + Uint(8u + 3u * a)] * world[2];
        local[a] = local[a] * bodies.values[base + Uint(3u + a)];
    }
    for (std::uint32_t a = 0; a < 3; ++a)
        result[a] = bodies.values[base + Uint(6u + a)] * local[0] +
                    bodies.values[base + Uint(9u + a)] * local[1] +
                    bodies.values[base + Uint(12u + a)] * local[2];
    return result;
}

Vec project_axes(const Vec& v, const Uint& bits, unsigned shift) {
    Vec result;
    for (unsigned i = 0; i < 3; ++i) {
        result[i] = v[i];
        $IF((bits & Uint(1u << (shift + i))) != Uint(0u)) { result[i] = 0.0f; }
    }
    return result;
}

Vec constrained_inertia(Floats& bodies, const Uint& base, const Vec& v, const Uint& bits) {
    // P I^-1 P is symmetric and respects world-axis angular locks both in the
    // effective mass and in the velocity update.
    return project_axes(inertia_apply(bodies, base, project_axes(v, bits, 5)), bits, 5);
}

struct GatherKernel {
    Floats source, velocities;
    UInts indices;
    Uint count;
    H::ComputePipeline<> pipeline;
    GatherKernel() {
        count.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint body;
            body = dispatchThreadID()->x;
            $IF(body < count) {
                Uint input_base, output_base;
                input_base = indices.values[body] * Uint(6u);
                output_base = body * Uint(6u);
                for (std::uint32_t axis = 0; axis < 6; ++axis)
                    velocities.values[output_base + Uint(axis)] = source.values[input_base + Uint(axis)];
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

struct SolveKernel {
    Floats bodies, contacts, velocities, results, maxima;
    UInts flags, endpoints, order, offsets, control;
    Uint islands, iteration;
    Float max_linear, max_angular, max_impulse, friction, static_friction, inverse_dt, restitution_threshold;
    H::ComputePipeline<> pipeline;

    void apply_impulse(Vec& va, Vec& vb, Vec& wa, Vec& wb, const Vec& impulse,
                       const Float& inv_a, const Float& inv_b, const Uint& bits_a, const Uint& bits_b,
                       const Uint& ba, const Uint& bb, const Vec& ra, const Vec& rb) {
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            $IF((bits_a & Uint(1u << (axis + 2u))) == Uint(0u)) { va[axis] = va[axis] + impulse[axis] * inv_a; }
            $IF((bits_b & Uint(1u << (axis + 2u))) == Uint(0u)) { vb[axis] = vb[axis] - impulse[axis] * inv_b; }
        }
        $IF((bits_a & Uint(3u)) == Uint(1u)) { add_in_place(wa, constrained_inertia(bodies, ba, cross3(ra, impulse), bits_a)); }
        $IF((bits_b & Uint(3u)) == Uint(1u)) { add_in_place(wb, constrained_inertia(bodies, bb, cross3(rb, neg(impulse)), bits_b)); }
    }
    void clamp_pair(Vec& va, Vec& vb, Vec& wa, Vec& wb, const Uint& bits_a, const Uint& bits_b) {
        $IF((bits_a & Uint(3u)) == Uint(1u)) { clamp_speed(va, max_linear); clamp_speed(wa, max_angular); }
        $IF((bits_b & Uint(3u)) == Uint(1u)) { clamp_speed(vb, max_linear); clamp_speed(wb, max_angular); }
    }
    void solve_contact(const Uint& index, Float& max_delta) {
        Uint a, b, ba, bb, ca, out, bits_a, bits_b;
        a = endpoints.values[index * Uint(2u)];
        b = endpoints.values[index * Uint(2u) + Uint(1u)];
        ba = a * Uint(body_words); bb = b * Uint(body_words);
        ca = index * Uint(contact_words); out = index * Uint(result_words);
        bits_a = flags.values[a]; bits_b = flags.values[b];
        results.values[out + Uint(2u)] = 0.0f;
        $IF(((bits_a & Uint(2u)) == Uint(0u)) || ((bits_b & Uint(2u)) == Uint(0u))) {
            Float inv_a, inv_b;
            inv_a = 0.0f; inv_b = 0.0f;
            $IF((bits_a & Uint(3u)) == Uint(1u)) { inv_a = Float(1.0f) / bodies.values[ba + Uint(15u)]; }
            $IF((bits_b & Uint(3u)) == Uint(1u)) { inv_b = Float(1.0f) / bodies.values[bb + Uint(15u)]; }
            results.values[out] = inv_a;
            results.values[out + Uint(1u)] = inv_b;
            auto n = load3(contacts, ca);
            auto point = load3(contacts, ca + Uint(3u));
            auto ra = sub(point, load3(bodies, ba));
            auto rb = sub(point, load3(bodies, bb));
            auto va = load3(velocities, a * Uint(6u));
            auto wa = load3(velocities, a * Uint(6u) + Uint(3u));
            auto vb = load3(velocities, b * Uint(6u));
            auto wb = load3(velocities, b * Uint(6u) + Uint(3u));
            va = project_axes(va, bits_a, 2); vb = project_axes(vb, bits_b, 2);
            wa = project_axes(wa, bits_a, 5); wb = project_axes(wb, bits_b, 5);
            Vec before_wa, before_wb;
            for (std::uint32_t axis = 0; axis < 3; ++axis) {
                before_wa[axis] = wa[axis]; before_wb[axis] = wb[axis];
            }
            Float accumulated, accumulated_t, initial;
            accumulated = results.values[out + Uint(3u)];
            accumulated_t = results.values[out + Uint(4u)];
            initial = results.values[out + Uint(5u)];
            auto pa = add(va, cross3(wa, ra));
            auto pb = add(vb, cross3(wb, rb));
            add_in_place(pa, load3(contacts, ca + Uint(6u)));
            add_in_place(pb, load3(contacts, ca + Uint(9u)));
            $IF(iteration == Uint(0u)) {
                // Restitution uses the incoming speed, before warm starting.
                initial = dot3(sub(pa, pb), n);
                results.values[out + Uint(5u)] = initial;
                $IF(accumulated > Float(0.0f)) {
                    apply_impulse(va, vb, wa, wb, mul(n, Float(0.0f) - accumulated), inv_a, inv_b, bits_a, bits_b, ba, bb, ra, rb);
                }
            }
            pa = add(add(va, cross3(wa, ra)), load3(contacts, ca + Uint(6u)));
            pb = add(add(vb, cross3(wb, rb)), load3(contacts, ca + Uint(9u)));
            Float vn;
            vn = dot3(sub(pa, pb), n);
            // n points A -> B, so positive vn is approach. A positive normal
            // multiplier pushes A along -n and B along +n. Separating contacts
            // must still solve so an excessive warm start can be removed.
            {
                auto ran = cross3(ra, n);
                auto rbn = cross3(rb, n);
                Float angular_a, angular_b, denominator;
                angular_a = 0.0f; angular_b = 0.0f;
                $IF((bits_a & Uint(3u)) == Uint(1u)) { angular_a = dot3(ran, constrained_inertia(bodies, ba, ran, bits_a)); }
                $IF((bits_b & Uint(3u)) == Uint(1u)) { angular_b = dot3(rbn, constrained_inertia(bodies, bb, rbn, bits_b)); }
                denominator = inv_a * dot3(n, project_axes(n, bits_a, 2)) +
                              inv_b * dot3(n, project_axes(n, bits_b, 2)) + angular_a + angular_b;
                $IF(denominator > Float(1e-12f)) {
                    Float target, raw, next, impulse;
                    target = 0.0f;
                    Float restitution;
                    restitution = (bodies.values[ba + Uint(16u)] + bodies.values[bb + Uint(16u)]) * Float(0.5f);
                    $IF(initial > restitution_threshold) { target = (Float(0.0f) - restitution) * initial; }
                    $IF(contacts.values[ca + Uint(12u)] < Float(0.0f)) {
                        // A contact margin is not overlap: allow approach up to
                        // the actual surface instead of stopping at the margin.
                        target = (Float(0.0f) - contacts.values[ca + Uint(12u)]) * inverse_dt;
                    }
                    raw = (vn - target) / denominator;
                    next = max(Float(0.0f), accumulated + raw);
                    impulse = max(Float(0.0f) - max_impulse, min(max_impulse, next - accumulated));
                    accumulated = accumulated + impulse;
                    results.values[out + Uint(2u)] = impulse;
                    apply_impulse(va, vb, wa, wb, mul(n, Float(0.0f) - impulse), inv_a, inv_b, bits_a, bits_b, ba, bb, ra, rb);
                    clamp_pair(va, vb, wa, wb, bits_a, bits_b);
                    Float dv;
                    dv = max(abs(impulse) * inv_a, abs(impulse) * inv_b);
                    max_delta = max(max_delta, dv * dv);

                    // Tangential velocity intentionally excludes deformation
                    // surface velocity, matching the original CPU loop.
                    auto rel = sub(add(va, cross3(wa, ra)), add(vb, cross3(wb, rb)));
                    Float normal_speed, tangent_length, jt_abs;
                    normal_speed = dot3(rel, n);
                    auto tangent = sub(rel, mul(n, normal_speed));
                    tangent_length = sqrt(dot3(tangent, tangent));
                    jt_abs = 0.0f;
                    $IF(tangent_length > Float(1e-8f)) {
                        Vec direction;
                        for (std::uint32_t axis = 0; axis < 3; ++axis) direction[axis] = tangent[axis] / tangent_length;
                        auto rat = cross3(ra, direction);
                        auto rbt = cross3(rb, direction);
                        Float tangent_a, tangent_b, tangent_denominator;
                        tangent_a = 0.0f; tangent_b = 0.0f;
                        $IF((bits_a & Uint(3u)) == Uint(1u)) { tangent_a = dot3(rat, constrained_inertia(bodies, ba, rat, bits_a)); }
                        $IF((bits_b & Uint(3u)) == Uint(1u)) { tangent_b = dot3(rbt, constrained_inertia(bodies, bb, rbt, bits_b)); }
                        tangent_denominator = inv_a * dot3(direction, project_axes(direction, bits_a, 2)) +
                                              inv_b * dot3(direction, project_axes(direction, bits_b, 2)) + tangent_a + tangent_b;
                        $IF(tangent_denominator > Float(1e-12f)) {
                            Float slip, free_impulse, coefficient, cap, next_t, impulse_t;
                            slip = dot3(rel, direction);
                            free_impulse = (Float(0.0f) - slip) / tangent_denominator;
                            coefficient = friction;
                            $IF(tangent_length < Float(0.02f)) { coefficient = static_friction; }
                            cap = coefficient * accumulated;
                            next_t = max(Float(0.0f) - cap, min(cap, accumulated_t + free_impulse));
                            impulse_t = next_t - accumulated_t;
                            accumulated_t = next_t;
                            jt_abs = abs(impulse_t);
                            apply_impulse(va, vb, wa, wb, mul(direction, impulse_t), inv_a, inv_b, bits_a, bits_b, ba, bb, ra, rb);
                        }
                    }
                    clamp_pair(va, vb, wa, wb, bits_a, bits_b);
                    $IF(jt_abs > Float(0.0f)) {
                        dv = max(jt_abs * inv_a, jt_abs * inv_b);
                        max_delta = max(max_delta, dv * dv);
                    }
                }
            }
            // Angular impulses can move the contact substantially even when
            // inverse mass is tiny. Use contact-speed units for convergence.
            auto angular_delta_a = cross3(sub(wa, before_wa), ra);
            auto angular_delta_b = cross3(sub(wb, before_wb), rb);
            max_delta = max(max_delta, max(dot3(angular_delta_a, angular_delta_a), dot3(angular_delta_b, angular_delta_b)));
            // Fixed/kinematic/sleeping endpoints are read-only and may be shared
            // by independent islands. Never race on their velocity records.
            for (std::uint32_t axis = 0; axis < 3; ++axis) {
                $IF((bits_a & Uint(3u)) == Uint(1u)) {
                    velocities.values[a * Uint(6u) + Uint(axis)] = va[axis];
                    velocities.values[a * Uint(6u) + Uint(axis + 3u)] = wa[axis];
                }
                $IF((bits_b & Uint(3u)) == Uint(1u)) {
                    velocities.values[b * Uint(6u) + Uint(axis)] = vb[axis];
                    velocities.values[b * Uint(6u) + Uint(axis + 3u)] = wb[axis];
                }
            }
            results.values[out + Uint(3u)] = accumulated;
            results.values[out + Uint(4u)] = accumulated_t;
        }
    }
    SolveKernel() {
        islands.as_push_constant(); iteration.as_push_constant();
        max_linear.as_push_constant(); max_angular.as_push_constant(); max_impulse.as_push_constant();
        friction.as_push_constant(); static_friction.as_push_constant();
        inverse_dt.as_push_constant(); restitution_threshold.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint island;
            island = dispatchThreadID()->x;
            $IF((island < islands) && (control.values[Uint(0u)] != Uint(0u))) {
                Uint k, end;
                k = offsets.values[island]; end = offsets.values[island + Uint(1u)];
                Float max_delta;
                max_delta = 0.0f;
                $WHILE(k < end) {
                    Uint index;
                    index = order.values[k];
                    solve_contact(index, max_delta);
                    k = k + Uint(1u);
                }
                maxima.values[island] = max_delta;
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

// Hierarchical reduction avoids one serial scan over every island. Only the
// final invocation updates global continuation; all islands share this gate.
struct ReduceKernel {
    Floats input, output;
    UInts control;
    Uint count, final_level;
    Float threshold_squared;
    H::ComputePipeline<> pipeline;
    ReduceKernel() {
        count.as_push_constant(); final_level.as_push_constant(); threshold_squared.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint lane, begin;
            lane = dispatchThreadID()->x; begin = lane * Uint(reduction_width);
            $IF((begin < count) && (control.values[Uint(0u)] != Uint(0u))) {
                Float maximum;
                maximum = 0.0f;
                Uint k, end;
                k = begin; end = count;
                $IF(begin + Uint(reduction_width) < count) { end = begin + Uint(reduction_width); }
                $WHILE(k < end) {
                    maximum = max(maximum, input.values[k]);
                    k = k + Uint(1u);
                }
                output.values[lane] = maximum;
                $IF(final_level != Uint(0u)) {
                    control.values[Uint(1u)] = control.values[Uint(1u)] + Uint(1u);
                    $IF(maximum < threshold_squared) { control.values[Uint(0u)] = 0u; }
                }
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

struct Workspace {
    std::size_t capacity{};
    H::HardwareBuffer data;
    void ensure(std::size_t words, H::BufferUsageFlags usage, H::CpuAccessMode cpu = H::CpuAccessMode::None) {
        words = std::max<std::size_t>(1, words);
        if (capacity >= words && capacity <= std::max<std::size_t>(4096, words * 4)) return;
        H::HardwareBufferDesc desc;
        desc.element_count = words; desc.element_size = sizeof(float);
        desc.usage = usage; desc.cpu_access = cpu;
        desc.debug_name = "mechanics.contact_solver";
        data = H::HardwareBuffer(desc); capacity = words;
    }
};
template <std::size_t N>
void finite(const std::array<float, N>& values) {
    for (float value : values)
        if (!std::isfinite(value)) throw std::invalid_argument("GPU contact solver requires finite state");
}
void nonnegative(float value) {
    if (!std::isfinite(value) || value < 0.0f) throw std::invalid_argument("Invalid GPU contact solver parameter");
}
template <std::size_t N>
void append(std::vector<float>& target, const std::array<float, N>& value) {
    target.insert(target.end(), value.begin(), value.end());
}
void validate(std::span<const GpuContactSolverPipeline::Body> bodies,
              std::span<const GpuContactSolverPipeline::Contact> contacts, const GpuContactSolverPipeline::Params& p,
              bool resident_velocities) {
    if (bodies.size() > GpuContactSolverPipeline::max_bodies || contacts.size() > GpuContactSolverPipeline::max_contacts)
        throw std::length_error("GPU contact solver batch exceeds storage, dispatch or indexing limit");
    nonnegative(p.max_linear_speed); nonnegative(p.max_angular_speed); nonnegative(p.max_impulse);
    nonnegative(p.friction); nonnegative(p.static_friction); nonnegative(p.early_exit_velocity);
    nonnegative(p.restitution_threshold);
    if (!std::isfinite(p.dt) || p.dt <= 0 || !std::isfinite(1.0f / p.dt))
        throw std::invalid_argument("GPU contact solver requires a positive finite timestep");
    if (p.iterations == 0 || p.iterations > 8) throw std::invalid_argument("GPU contact solver requires 1 to 8 iterations");
    for (const auto& body : bodies) {
        if (!resident_velocities) { finite(body.velocity); finite(body.angular_velocity); }
        finite(body.center); finite(body.inertia_inv); finite(body.rotation);
        if (!std::isfinite(body.mass) || !std::isfinite(body.restitution) ||
            (body.dynamic && !body.sleeping && body.mass <= 0.0f))
            throw std::invalid_argument("Invalid GPU contact solver body mass or restitution");
    }
    for (const auto& contact : contacts) {
        if (contact.a >= bodies.size() || contact.b >= bodies.size() || contact.a == contact.b)
            throw std::invalid_argument("Invalid GPU contact solver endpoints");
        finite(contact.normal); finite(contact.point); finite(contact.surface_a); finite(contact.surface_b);
        if (!std::isfinite(contact.penetration) || !std::isfinite(contact.accumulated_j) || !std::isfinite(contact.accumulated_jt))
            throw std::invalid_argument("Invalid GPU contact solver contact state");
    }
}

struct Islands {
    std::vector<std::uint32_t> order, offsets;
    Islands(std::span<const GpuContactSolverPipeline::Body> bodies, std::span<const GpuContactSolverPipeline::Contact> contacts) {
        const auto body_count = bodies.size();
        const auto writable = [&](std::uint32_t i) { return bodies[i].dynamic && !bodies[i].sleeping; };
        std::vector<std::uint32_t> parent(body_count), sizes(body_count, 1);
        std::iota(parent.begin(), parent.end(), 0u);
        const auto find = [&](std::uint32_t index) {
            while (parent[index] != index) { parent[index] = parent[parent[index]]; index = parent[index]; }
            return index;
        };
        for (const auto& contact : contacts) {
            // Sharing an immutable ground/platform is not a dependency between
            // dynamic bodies. Only writable endpoints join an island.
            if (!writable(contact.a) || !writable(contact.b)) continue;
            auto a = find(contact.a), b = find(contact.b);
            if (a != b) {
                if (sizes[a] < sizes[b]) std::swap(a, b);
                parent[b] = a; sizes[a] += sizes[b];
            }
        }
        constexpr auto missing = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::uint32_t> root_to_island(body_count, missing), contact_islands(contacts.size());
        std::vector<std::uint32_t> counts;
        for (std::size_t i = 0; i < contacts.size(); ++i) {
            const auto& contact = contacts[i];
            if (!writable(contact.a) && !writable(contact.b)) {
                contact_islands[i] = static_cast<std::uint32_t>(counts.size());
                counts.push_back(1);
                continue;
            }
            auto& island = root_to_island[find(writable(contact.a) ? contact.a : contact.b)];
            if (island == missing) { island = static_cast<std::uint32_t>(counts.size()); counts.push_back(0); }
            contact_islands[i] = island; ++counts[island];
        }
        offsets.resize(counts.size() + 1);
        for (std::size_t i = 0; i < counts.size(); ++i) offsets[i + 1] = offsets[i] + counts[i];
        auto cursor = offsets;
        order.resize(contacts.size());
        // Stable insertion preserves original contact ordering within each island.
        for (std::uint32_t i = 0; i < contacts.size(); ++i) order[cursor[contact_islands[i]]++] = i;
    }
};
}  // namespace

struct GpuContactSolverPipeline::Impl {
    H::HardwareExecutor executor;
    GatherKernel gather;
    SolveKernel solve_kernel;
    ReduceKernel reduce;
    Workspace bodies, contacts, velocities, results, flags, endpoints, order, offsets, control, maxima, source_indices;
    std::array<Workspace, 2> reductions;
    Workspace upload_floats, upload_uints, readback, velocity_readback;
    bool poisoned{};
    std::uint32_t valid_velocity_count{};
    std::uint64_t submissions{};
    H::SubmitReceipt last_receipt;
    struct Recording {
        Impl& owner;
        int exceptions{std::uncaught_exceptions()};
        ~Recording() { if (std::uncaught_exceptions() > exceptions) owner.poisoned = true; }
    };
    ~Impl() {
        if (poisoned && last_receipt.data) {
            try { executor.wait_completed(last_receipt); } catch (...) {}
        }
    }
    void usable() const {
        if (poisoned) throw std::runtime_error("GPU contact solver submission is unconfirmed");
    }
    void finish(H::HardwareStream& stream) {
        poisoned = true; last_receipt = {};
        {
            GpuPerformance::OperationScope timing{GpuPerformance::Operation::Commit};
            last_receipt = stream << H::commit();
        }
        ++submissions;
        {
            GpuPerformance::OperationScope timing{GpuPerformance::Operation::Wait};
            executor.wait_completed(last_receipt);
        }
        last_receipt = {}; poisoned = false;
    }
    Result run(std::span<const Body> input_bodies, std::span<const Contact> input_contacts, const Params& p, bool device_only,
               const GpuRigidVelocityView* source = nullptr, std::span<const std::uint32_t> indices = {}) {
        GpuPerformance::Scope performance{"solver"};
        GpuPerformance::record_work_items(input_contacts.size());
        valid_velocity_count = 0;
        usable();
        validate(input_bodies, input_contacts, p, source != nullptr);
        if (source) {
            if (source->readiness) source->readiness->check();
            if (indices.size() != input_bodies.size())
                throw std::invalid_argument("GPU contact solver requires one source index per body");
            if (source->count > max_bodies)
                throw std::length_error("GPU contact solver source exceeds storage or indexing limit");
            if ((!input_bodies.empty() || source->count != 0u) &&
                (!source->buffer || source->buffer.get_byte_size() < std::uint64_t(source->count) * 6u * sizeof(float)))
                throw std::invalid_argument("GPU contact solver requires a valid velocity source");
            // Check before ensure: resizing our workspace must not make a
            // retained view of its old storage appear to be an independent source.
            if (source->buffer && source->buffer.same_resource(velocities.data))
                throw std::invalid_argument("GPU contact solver source must not alias its velocity buffer");
            for (const auto index : indices)
                if (index >= source->count)
                    throw std::out_of_range("GPU contact solver source index is out of range");
        }
        Result result;
        if (input_contacts.empty() && !device_only) {
            result.bodies.reserve(input_bodies.size());
            for (const auto& body : input_bodies) result.bodies.push_back({body.velocity, body.angular_velocity});
            return result;
        }
        if (input_bodies.empty()) return result;

        std::vector<float> float_upload;
        float_upload.reserve(input_bodies.size() * (body_words + (source ? 0u : 6u)) + input_contacts.size() * (contact_words + result_words));
        if (!source)
            for (const auto& body : input_bodies) { append(float_upload, body.velocity); append(float_upload, body.angular_velocity); }
        const auto body_offset = float_upload.size();
        for (const auto& body : input_bodies) {
            append(float_upload, body.center); append(float_upload, body.inertia_inv); append(float_upload, body.rotation);
            float_upload.push_back(body.mass); float_upload.push_back(body.restitution);
        }
        const auto contact_offset = float_upload.size();
        for (const auto& contact : input_contacts) {
            append(float_upload, contact.normal); append(float_upload, contact.point);
            append(float_upload, contact.surface_a); append(float_upload, contact.surface_b);
            float_upload.push_back(contact.penetration);
        }
        const auto result_offset = float_upload.size();
        for (const auto& contact : input_contacts) {
            float_upload.insert(float_upload.end(), {0, 0, 0, contact.accumulated_j, contact.accumulated_jt, 0});
        }
        Islands islands(input_bodies, input_contacts);
        const auto island_count = static_cast<std::uint32_t>(islands.offsets.size() - 1u);
        std::vector<std::uint32_t> uint_upload;
        uint_upload.reserve(input_bodies.size() + input_contacts.size() * 3u + islands.offsets.size() + 2u + indices.size());
        for (const auto& body : input_bodies) uint_upload.push_back(std::uint32_t(body.dynamic) | (std::uint32_t(body.sleeping) << 1u) |
            (std::uint32_t(body.linear_lock & 7u) << 2u) | (std::uint32_t(body.angular_lock & 7u) << 5u));
        const auto endpoints_offset = uint_upload.size();
        for (const auto& contact : input_contacts) { uint_upload.push_back(contact.a); uint_upload.push_back(contact.b); }
        const auto order_offset = uint_upload.size();
        uint_upload.insert(uint_upload.end(), islands.order.begin(), islands.order.end());
        const auto offsets_offset = uint_upload.size();
        uint_upload.insert(uint_upload.end(), islands.offsets.begin(), islands.offsets.end());
        const auto control_offset = uint_upload.size();
        uint_upload.insert(uint_upload.end(), {1u, 0u});
        const auto source_indices_offset = uint_upload.size();
        uint_upload.insert(uint_upload.end(), indices.begin(), indices.end());

        velocities.ensure(input_bodies.size() * 6u, device_usage);
        if (source) source_indices.ensure(indices.size(), device_usage);
        bodies.ensure(input_bodies.size() * body_words, device_usage);
        contacts.ensure(input_contacts.size() * contact_words, device_usage);
        results.ensure(input_contacts.size() * result_words, device_usage);
        flags.ensure(input_bodies.size(), device_usage);
        endpoints.ensure(input_contacts.size() * 2u, device_usage);
        order.ensure(input_contacts.size(), device_usage); offsets.ensure(islands.offsets.size(), device_usage);
        control.ensure(2, device_usage); maxima.ensure(island_count, device_usage);
        for (auto& workspace : reductions) workspace.ensure((island_count + reduction_width - 1u) / reduction_width, device_usage);
        upload_floats.ensure(float_upload.size(), H::BufferUsage_TransferSrc, H::CpuAccessMode::Write);
        upload_uints.ensure(uint_upload.size(), H::BufferUsage_TransferSrc, H::CpuAccessMode::Write);
        const auto contact_result_words = input_contacts.size() * result_words;
        const auto velocity_result_words = device_only ? 0u : input_bodies.size() * 6u;
        readback.ensure(contact_result_words + velocity_result_words + 1u, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
        if (!upload_floats.data.write_bytes(std::as_bytes(std::span(float_upload))) ||
            !upload_uints.data.write_bytes(std::as_bytes(std::span(uint_upload))))
            throw std::runtime_error("GPU contact solver upload failed");
        {
            Recording recording{*this};
            auto stream = executor.stream();
            const auto copy = [&](const Workspace& from, const Workspace& to, std::size_t begin, std::size_t count) {
                if (count) {
                    GpuPerformance::record_upload(count * sizeof(float));
                    stream << H::CopyBufferCommand{from.data, to.data, begin * sizeof(float), 0, count * sizeof(float)};
                }
            };
            if (source) {
                copy(upload_uints, source_indices, source_indices_offset, indices.size());
                gather.source.bind(source->buffer); gather.indices.bind(source_indices.data); gather.velocities.bind(velocities.data);
                gather.count = static_cast<std::uint32_t>(input_bodies.size());
                GpuPerformance::record_dispatch();
                stream << gather.pipeline.groups((static_cast<std::uint32_t>(input_bodies.size()) + threads - 1u) / threads, 1);
            } else {
                copy(upload_floats, velocities, 0, input_bodies.size() * 6u);
            }
            if (!input_contacts.empty()) {
                copy(upload_floats, bodies, body_offset, input_bodies.size() * body_words);
                copy(upload_floats, contacts, contact_offset, input_contacts.size() * contact_words);
                copy(upload_floats, results, result_offset, contact_result_words);
                copy(upload_uints, flags, 0, input_bodies.size());
                copy(upload_uints, endpoints, endpoints_offset, input_contacts.size() * 2u);
                copy(upload_uints, order, order_offset, input_contacts.size());
                copy(upload_uints, offsets, offsets_offset, islands.offsets.size());
                copy(upload_uints, control, control_offset, 2u);
                auto& kernel = solve_kernel;
                kernel.bodies.bind(bodies.data); kernel.contacts.bind(contacts.data); kernel.velocities.bind(velocities.data);
                kernel.results.bind(results.data); kernel.maxima.bind(maxima.data); kernel.flags.bind(flags.data);
                kernel.endpoints.bind(endpoints.data); kernel.order.bind(order.data); kernel.offsets.bind(offsets.data); kernel.control.bind(control.data);
                kernel.islands = island_count;
                kernel.max_linear = p.max_linear_speed; kernel.max_angular = p.max_angular_speed;
                kernel.max_impulse = p.max_impulse; kernel.friction = p.friction; kernel.static_friction = p.static_friction;
                kernel.inverse_dt = 1.0f / p.dt; kernel.restitution_threshold = p.restitution_threshold;
                reduce.control.bind(control.data); reduce.threshold_squared = p.early_exit_velocity * p.early_exit_velocity;
                for (std::uint32_t iteration = 0; iteration < p.iterations; ++iteration) {
                    kernel.iteration = iteration;
                    GpuPerformance::record_dispatch();
                    stream << kernel.pipeline.groups((island_count + threads - 1u) / threads, 1);
                    std::uint32_t reduction_count = island_count, destination = 0;
                    H::HardwareBuffer source = maxima.data;
                    do {
                        const auto groups = (reduction_count + reduction_width - 1u) / reduction_width;
                        reduce.input.bind(source); reduce.output.bind(reductions[destination].data);
                        reduce.count = reduction_count; reduce.final_level = groups == 1 ? 1u : 0u;
                        GpuPerformance::record_dispatch();
                        stream << reduce.pipeline.groups((groups + threads - 1u) / threads, 1);
                        source = reductions[destination].data; destination ^= 1u; reduction_count = groups;
                    } while (reduction_count > 1u);
                }
                GpuPerformance::record_readback((contact_result_words + velocity_result_words) * sizeof(float) + sizeof(std::uint32_t));
                stream << H::CopyBufferCommand{results.data, readback.data, 0, 0, contact_result_words * sizeof(float)};
                if (!device_only)
                    stream << H::CopyBufferCommand{velocities.data, readback.data, 0, contact_result_words * sizeof(float), velocity_result_words * sizeof(float)};
                stream << H::CopyBufferCommand{control.data, readback.data, sizeof(std::uint32_t),
                                              (contact_result_words + velocity_result_words) * sizeof(float), sizeof(std::uint32_t)};
            }
            finish(stream);
        }
        if (!input_contacts.empty()) {
            std::vector<std::byte> bytes((contact_result_words + velocity_result_words + 1u) * sizeof(float));
            if (!readback.data.read_bytes(bytes)) throw std::runtime_error("GPU contact solver readback failed");
            result.contacts.resize(input_contacts.size());
            std::memcpy(result.contacts.data(), bytes.data(), contact_result_words * sizeof(float));
            if (!device_only) {
                result.bodies.resize(input_bodies.size());
                std::memcpy(result.bodies.data(), bytes.data() + contact_result_words * sizeof(float), velocity_result_words * sizeof(float));
            }
            std::memcpy(&result.iterations, bytes.data() + (contact_result_words + velocity_result_words) * sizeof(float), sizeof(std::uint32_t));
        }
        valid_velocity_count = static_cast<std::uint32_t>(input_bodies.size());
        return result;
    }
};

GpuContactSolverPipeline::GpuContactSolverPipeline() : impl_(std::make_unique<Impl>()) {}
GpuContactSolverPipeline::~GpuContactSolverPipeline() = default;
GpuContactSolverPipeline::Result GpuContactSolverPipeline::solve(std::span<const Body> bodies, std::span<const Contact> contacts, const Params& p) {
    return impl_->run(bodies, contacts, p, false);
}
GpuContactSolverPipeline::Result GpuContactSolverPipeline::solve_device(std::span<const Body> bodies, std::span<const Contact> contacts, const Params& p) {
    return impl_->run(bodies, contacts, p, true);
}
GpuContactSolverPipeline::Result GpuContactSolverPipeline::solve_from_velocities(
    std::span<const Body> metadata, std::span<const Contact> contacts, const Params& p,
    const GpuRigidVelocityView& source, std::span<const std::uint32_t> source_indices) {
    return impl_->run(metadata, contacts, p, true, &source, source_indices);
}
GpuRigidVelocityView GpuContactSolverPipeline::velocity_view() const noexcept {
    return impl_->valid_velocity_count ? GpuRigidVelocityView{impl_->velocities.data, impl_->valid_velocity_count} : GpuRigidVelocityView{};
}
std::vector<GpuContactSolverPipeline::Velocity> GpuContactSolverPipeline::readback_velocities() {
    GpuPerformance::Scope performance{"solver.velocities_readback"};
    impl_->usable();
    std::vector<Velocity> result(impl_->valid_velocity_count);
    if (result.empty()) return result;
    impl_->velocity_readback.ensure(result.size() * 6u, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    {
        Impl::Recording recording{*impl_};
        auto stream = impl_->executor.stream();
        GpuPerformance::record_readback(result.size() * sizeof(Velocity));
        stream << H::CopyBufferCommand{impl_->velocities.data, impl_->velocity_readback.data, 0, 0, result.size() * sizeof(Velocity)};
        impl_->finish(stream);
    }
    if (!impl_->velocity_readback.data.read_bytes(std::as_writable_bytes(std::span(result))))
        throw std::runtime_error("GPU contact solver velocity readback failed");
    return result;
}
std::uint64_t GpuContactSolverPipeline::submission_count() const noexcept { return impl_->submissions; }
}  // namespace Corona::Systems
