#include "gpu_floor_sleep.h"
#include "gpu_performance.h"
#include "gpu_compute_pipeline.h"

#include <horizon.h>
#include <Codegen/BuiltinVariate.h>
#include <Codegen/ControlFlows.h>
#include <Codegen/CustomLibrary.h>
#include <Codegen/TypeAlias.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace Corona::Systems {
namespace {
namespace H = Corona::Horizon;
using namespace EmbeddedShader;
using Vec = std::array<Float, 3>;
constexpr std::uint32_t threads = 64, floor_words = 20;
constexpr auto device_usage = H::BufferUsage_Storage | H::BufferUsage_TransferSrc | H::BufferUsage_TransferDst;
static_assert(GpuFloorSleepPipeline::max_bodies <= threads * 65535u);
static_assert(std::uint64_t(GpuFloorSleepPipeline::max_bodies) * floor_words * sizeof(float) <= 128u * 1024u * 1024u);
static_assert(sizeof(GpuFloorSleepPipeline::Velocity) == 6u * sizeof(float));

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
Vec cross3(const Vec& a, const Vec& b) {
    Vec result;
    result[0] = a[1] * b[2] - a[2] * b[1];
    result[1] = a[2] * b[0] - a[0] * b[2];
    result[2] = a[0] * b[1] - a[1] * b[0];
    return result;
}
Float dot3(const Vec& a, const Vec& b) {
    Float result;
    result = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    return result;
}
Vec inertia_apply(Floats& input, const Uint& base, const Vec& world) {
    Vec local, result;
    for (std::uint32_t a = 0; a < 3; ++a) {
        local[a] = input.values[base + Uint(9u + 3u * a)] * world[0] +
                   input.values[base + Uint(10u + 3u * a)] * world[1] +
                   input.values[base + Uint(11u + 3u * a)] * world[2];
        local[a] = local[a] * input.values[base + Uint(3u + a)];
    }
    for (std::uint32_t a = 0; a < 3; ++a)
        result[a] = input.values[base + Uint(9u + a)] * local[0] +
                    input.values[base + Uint(12u + a)] * local[1] +
                    input.values[base + Uint(15u + a)] * local[2];
    return result;
}
void apply_angular(Vec& angular, const Vec& change, const Uint& bits) {
    for (std::uint32_t a = 0; a < 3; ++a) {
        $IF((bits & Uint(1u << (5u + a))) == Uint(0u)) { angular[a] = angular[a] + change[a]; }
    }
}

struct FloorKernel {
    Floats inputs, velocities, candidates, metadata;
    UInts info;
    Uint count;
    Float floor_y, floor_eps, restitution, gravity_y, dt, low_velocity, zero_velocity, friction, static_friction;
    H::ComputePipeline<> pipeline;
    FloorKernel() {
        count.as_push_constant();
        floor_y.as_push_constant(); floor_eps.as_push_constant(); restitution.as_push_constant();
        gravity_y.as_push_constant(); dt.as_push_constant(); low_velocity.as_push_constant();
        zero_velocity.as_push_constant(); friction.as_push_constant(); static_friction.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint i;
            i = dispatchThreadID()->x;
            $IF(i < count) {
                Uint base, body, bits;
                base = i * Uint(floor_words);
                body = info.values[i * Uint(2u)];
                bits = info.values[i * Uint(2u) + Uint(1u)];
                auto v = load3(velocities, body * Uint(6u));
                auto w = load3(velocities, body * Uint(6u) + Uint(3u));
                Float contact, delta_y, reset_timer, level;
                contact = 0.0f; delta_y = 0.0f; reset_timer = 0.0f;
                level = floor_y + floor_eps;
                $IF(((bits & Uint(1u)) != Uint(0u)) && (inputs.values[base + Uint(19u)] < level)) {
                    contact = 1.0f;
                    $IF((bits & Uint(8u)) == Uint(0u)) { delta_y = level - inputs.values[base + Uint(19u)]; }
                    Float mass, inverse_mass;
                    mass = inputs.values[base + Uint(18u)];
                    inverse_mass = 0.0f;
                    $IF(mass > Float(0.001f)) { inverse_mass = Float(1.0f) / mass; }
                    Vec r;
                    r[0] = 0.0f; r[1] = level - inputs.values[base + Uint(1u)]; r[2] = 0.0f;
                    $IF((bits & Uint(2u)) != Uint(0u)) {
                        r[0] = inputs.values[base + Uint(6u)] - inputs.values[base];
                        r[2] = inputs.values[base + Uint(8u)] - inputs.values[base + Uint(2u)];
                    }
                    auto rotation_velocity = cross3(w, r);
                    Vec point_velocity;
                    for (std::uint32_t a = 0; a < 3; ++a) point_velocity[a] = v[a] + rotation_velocity[a];
                    Float normal_impulse;
                    normal_impulse = 0.0f;
                    $IF((bits & Uint(8u)) == Uint(0u)) {
                        Vec normal;
                        normal[0] = 0.0f; normal[1] = 1.0f; normal[2] = 0.0f;
                        auto rxn = cross3(r, normal);
                        Float denominator;
                        denominator = inverse_mass + dot3(rxn, inertia_apply(inputs, base, rxn)) + Float(1e-8f);
                        $IF(point_velocity[1] < Float(0.0f)) {
                            Float bounce;
                            bounce = 0.0f;
                            // Resting support cancels this step's gravity. It
                            // is not a new impact that should bounce or reset
                            // the sleep timer on every fixed step.
                            $IF(point_velocity[1] < Float(0.0f) -
                                (abs(gravity_y) * dt + max(low_velocity, zero_velocity))) {
                                bounce = restitution;
                                reset_timer = 1.0f;
                            }
                            normal_impulse = (Float(0.0f) - (Float(1.0f) + bounce)) * point_velocity[1] / denominator;
                            v[1] = v[1] + normal_impulse * inverse_mass;
                            Vec impulse;
                            impulse[0] = 0.0f; impulse[1] = normal_impulse; impulse[2] = 0.0f;
                            apply_angular(w, inertia_apply(inputs, base, cross3(r, impulse)), bits);
                        }
                    }
                    // The original floor response uses the contact velocity
                    // captured BEFORE the normal impulse for tangential friction.
                    Float tangent_length;
                    tangent_length = sqrt(point_velocity[0] * point_velocity[0] + point_velocity[2] * point_velocity[2]);
                    $IF(tangent_length > Float(1e-6f)) {
                        Vec tangent;
                        tangent[0] = point_velocity[0] / tangent_length; tangent[1] = 0.0f;
                        tangent[2] = point_velocity[2] / tangent_length;
                        auto rxt = cross3(r, tangent);
                        Float denominator, free_impulse, coefficient, cap, jt;
                        denominator = inverse_mass + dot3(rxt, inertia_apply(inputs, base, rxt)) + Float(1e-8f);
                        free_impulse = (Float(0.0f) - tangent_length) / denominator;
                        coefficient = friction;
                        $IF(tangent_length < Float(0.02f)) { coefficient = static_friction; }
                        cap = coefficient * max(normal_impulse, mass * abs(gravity_y) * dt);
                        jt = max(Float(0.0f) - cap, min(cap, free_impulse));
                        $IF((bits & Uint(4u)) == Uint(0u)) { v[0] = v[0] + tangent[0] * jt * inverse_mass; }
                        $IF((bits & Uint(16u)) == Uint(0u)) { v[2] = v[2] + tangent[2] * jt * inverse_mass; }
                        Vec impulse;
                        impulse[0] = tangent[0] * jt; impulse[1] = 0.0f; impulse[2] = tangent[2] * jt;
                        apply_angular(w, inertia_apply(inputs, base, cross3(r, impulse)), bits);
                    }
                }
                for (std::uint32_t a = 0; a < 3; ++a) {
                    candidates.values[i * Uint(7u) + Uint(a)] = v[a];
                    candidates.values[i * Uint(7u) + Uint(a + 3u)] = w[a];
                }
                candidates.values[i * Uint(7u) + Uint(6u)] = reset_timer;
                metadata.values[i * Uint(2u)] = delta_y;
                metadata.values[i * Uint(2u) + Uint(1u)] = contact;
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

struct CommitKernel {
    Floats velocities, candidates, metadata;
    UInts info, accepted, resets;
    Uint count;
    H::ComputePipeline<> pipeline;
    CommitKernel() {
        count.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint i;
            i = dispatchThreadID()->x;
            $IF(i < count) {
                $IF((accepted.values[i] != Uint(0u)) && (metadata.values[i * Uint(2u) + Uint(1u)] != Float(0.0f))) {
                    Uint body;
                    body = info.values[i * Uint(2u)];
                    for (std::uint32_t a = 0; a < 6; ++a)
                        velocities.values[body * Uint(6u) + Uint(a)] = candidates.values[i * Uint(7u) + Uint(a)];
                    $IF(candidates.values[i * Uint(7u) + Uint(6u)] != Float(0.0f)) { resets.values[body] = Uint(1u); }
                }
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

struct SleepKernel {
    Floats velocities, timers, output;
    UInts flags, resets, status;
    Uint count;
    Float dt, velocity_threshold, time_threshold, timer_limit;
    H::ComputePipeline<> pipeline;
    explicit SleepKernel(bool resident = false) {
        count.as_push_constant(); dt.as_push_constant();
        velocity_threshold.as_push_constant(); time_threshold.as_push_constant();
        if (resident) timer_limit.as_push_constant();
        pipeline = make_compute_pipeline([&, resident] {
            Uint i;
            i = dispatchThreadID()->x;
            $IF(i < count) {
                auto v = load3(velocities, i * Uint(6u));
                auto w = load3(velocities, i * Uint(6u) + Uint(3u));
                Uint bits;
                bits = flags.values[i];
                Float timer, sleeping, was_awake;
                timer = timers.values[i]; sleeping = 1.0f; was_awake = 0.0f;
                $IF(resets.values[i] != Uint(0u)) { timer = 0.0f; }
                if (resident) {
                    $IF((bits & Uint(4u)) != Uint(0u)) { timer = 0.0f; }
                }
                $IF((bits & Uint(1u)) == Uint(0u)) {
                    sleeping = 0.0f; was_awake = 1.0f;
                    $IF((bits & Uint(2u)) != Uint(0u)) {
                        $IF((dot3(v, v) < velocity_threshold * velocity_threshold) &&
                            (dot3(w, w) < velocity_threshold * velocity_threshold)) {
                            timer = timer + dt;
                            if (resident) { timer = min(timer, timer_limit); }
                            $IF(timer >= time_threshold) {
                                sleeping = 1.0f;
                                for (std::uint32_t a = 0; a < 3; ++a) { v[a] = 0.0f; w[a] = 0.0f; }
                            }
                        } $ELSE { timer = 0.0f; }
                    } $ELSE { timer = 0.0f; }
                }
                for (std::uint32_t a = 0; a < 3; ++a) {
                    velocities.values[i * Uint(6u) + Uint(a)] = v[a];
                    velocities.values[i * Uint(6u) + Uint(a + 3u)] = w[a];
                    if (!resident) {
                        output.values[i * Uint(9u) + Uint(a)] = v[a];
                        output.values[i * Uint(9u) + Uint(a + 3u)] = w[a];
                    }
                }
                if (resident) {
                    timers.values[i] = timer;
                    Uint result;
                    result = 0u;
                    $IF(sleeping != Float(0.0f)) { result = result | Uint(1u); }
                    $IF(was_awake != Float(0.0f)) { result = result | Uint(2u); }
                    status.values[i] = result;
                } else {
                    output.values[i * Uint(9u) + Uint(6u)] = timer;
                    output.values[i * Uint(9u) + Uint(7u)] = sleeping;
                    output.values[i * Uint(9u) + Uint(8u)] = was_awake;
                }
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

struct RemapKernel {
    Floats previous, previous_timers, initial, next_timers;
    UInts indices, previous_status, next_status, resets;
    Uint count;
    H::ComputePipeline<> pipeline;
    RemapKernel() {
        count.as_push_constant();
        pipeline = make_compute_pipeline([&] {
            Uint i;
            i = dispatchThreadID()->x;
            $IF(i < count) {
                Uint old, state;
                Float timer;
                old = indices.values[i]; state = 0u; timer = 0.0f;
                Vec v, w;
                for (std::uint32_t a = 0; a < 3; ++a) { v[a] = 0.0f; w[a] = 0.0f; }
                $IF(old != Uint(0xffffffffu)) {
                    for (std::uint32_t a = 0; a < 3; ++a) {
                        v[a] = previous.values[old * Uint(6u) + Uint(a)];
                        w[a] = previous.values[old * Uint(6u) + Uint(a + 3u)];
                    }
                    timer = previous_timers.values[old]; state = previous_status.values[old];
                }
                for (std::uint32_t a = 0; a < 3; ++a) {
                    initial.values[i * Uint(6u) + Uint(a)] = v[a];
                    initial.values[i * Uint(6u) + Uint(a + 3u)] = w[a];
                }
                next_timers.values[i] = timer; next_status.values[i] = state; resets.values[i] = Uint(0u);
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

struct Workspace {
    std::size_t capacity{};
    H::HardwareBuffer data;
    void ensure(std::size_t words, H::BufferUsageFlags usage, H::CpuAccessMode cpu = H::CpuAccessMode::None) {
        if (capacity >= words && capacity <= std::max<std::size_t>(4096, words * 4)) return;
        H::HardwareBufferDesc desc;
        desc.element_count = words; desc.element_size = sizeof(float);
        desc.usage = usage; desc.cpu_access = cpu; desc.debug_name = "mechanics.floor_sleep";
        data = H::HardwareBuffer(desc);
        capacity = words;
    }
};
void finite(float value) {
    if (!std::isfinite(value)) throw std::invalid_argument("GPU floor/sleep requires finite input");
}
template <std::size_t N>
void finite(const std::array<float, N>& values) { for (const auto value : values) finite(value); }
void nonnegative(float value) {
    finite(value);
    if (value < 0.0f) throw std::invalid_argument("GPU floor/sleep requires nonnegative parameters");
}
void validate_count(std::size_t count) {
    if (count > GpuFloorSleepPipeline::max_bodies)
        throw std::length_error("GPU floor/sleep batch exceeds storage, dispatch or indexing limit");
}
template <std::size_t N>
void append(std::vector<float>& packed, const std::array<float, N>& values) {
    packed.insert(packed.end(), values.begin(), values.end());
}
}  // namespace

struct GpuFloorSleepPipeline::Impl {
    H::HardwareExecutor executor;
    FloorKernel floor;
    CommitKernel commit;
    SleepKernel sleep;
    SleepKernel resident_sleep{true};
    RemapKernel remap;
    Workspace initial, working, resets, input, info, candidates, metadata, accepted, output;
    Workspace upload, info_upload, resets_upload, readback, resets_readback;
    Workspace resident_timers, next_timers, resident_status, next_status, remap_indices;
    std::vector<std::uint64_t> identities;
    bool resident_mode{};
    std::uint32_t count{}, pending_count{};
    bool pending{};
    std::shared_ptr<GpuSubmissionState> submission_state = std::make_shared<GpuSubmissionState>();
    std::uint64_t submissions{};
    H::SubmitReceipt last_receipt;
    struct Recording {
        Impl& owner;
        int exceptions{std::uncaught_exceptions()};
        ~Recording() { if (std::uncaught_exceptions() > exceptions) owner.submission_state->poisoned = true; }
    };
    ~Impl() {
        if (submission_state->owner) submission_state->poisoned = true;
        if (submission_state->poisoned && last_receipt.data) {
            try { executor.wait_completed(last_receipt); } catch (...) {}
        }
    }
    void usable(GpuSubmissionBatch* batch = nullptr) const { submission_state->check(batch); }
    void finish(H::HardwareStream& stream, GpuSubmissionBatch* batch = nullptr, bool readback = true) {
        if (batch) {
            if (readback) (void)batch->flush();
            return;
        }
        submission_state->poisoned = true;
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
        submission_state->poisoned = false;
    }
    template <class T>
    H::HardwareBuffer stage(Workspace& staging, std::span<const T> values, GpuSubmissionBatch* batch = nullptr) {
        if (batch) return batch->upload(std::as_bytes(values));
        staging.ensure(values.size_bytes() / sizeof(float), H::BufferUsage_TransferSrc, H::CpuAccessMode::Write);
        if (!staging.data.write_bytes(std::as_bytes(values))) throw std::runtime_error("GPU floor/sleep upload failed");
        return staging.data;
    }
    template <class T>
    void read(Workspace& staging, std::span<T> values) {
        if (!staging.data.read_bytes(std::as_writable_bytes(values))) throw std::runtime_error("GPU floor/sleep readback failed");
    }
};

GpuFloorSleepPipeline::GpuFloorSleepPipeline() : impl_(std::make_unique<Impl>()) {}
GpuFloorSleepPipeline::~GpuFloorSleepPipeline() = default;
std::uint64_t GpuFloorSleepPipeline::submission_count() const noexcept { return impl_->submissions; }
GpuRigidVelocityView GpuFloorSleepPipeline::initial_velocities() const noexcept {
    return impl_->count && !impl_->submission_state->poisoned
        ? GpuRigidVelocityView{impl_->initial.data, impl_->count, impl_->submission_state} : GpuRigidVelocityView{};
}
GpuRigidVelocityTarget GpuFloorSleepPipeline::velocity_target() const noexcept {
    return impl_->count && !impl_->submission_state->poisoned
        ? GpuRigidVelocityTarget{impl_->working.data, impl_->count, impl_->submission_state} : GpuRigidVelocityTarget{};
}

void GpuFloorSleepPipeline::begin(std::span<const Velocity> velocities) {
    GpuPerformance::Scope performance{"floor.begin"};
    GpuPerformance::record_work_items(velocities.size());
    auto& p = *impl_;
    p.usable(); validate_count(velocities.size());
    for (const auto& value : velocities) { finite(value.velocity); finite(value.angular_velocity); }
    p.count = 0; p.pending = false; p.pending_count = 0;
    p.identities.clear(); p.resident_mode = false;
    if (velocities.empty()) return;
    const auto count = velocities.size();
    p.initial.ensure(count * 6, device_usage); p.working.ensure(count * 6, device_usage);
    p.resets.ensure(count, device_usage);
    p.stage(p.upload, velocities);
    std::vector<std::uint32_t> zeroes(count, 0);
    p.stage(p.resets_upload, std::span<const std::uint32_t>(zeroes));
    {
        Impl::Recording recording{p};
        auto stream = p.executor.stream();
        GpuPerformance::record_upload(velocities.size_bytes());
        stream << H::CopyBufferCommand{p.upload.data, p.initial.data, 0, 0, velocities.size_bytes()};
        GpuPerformance::record_upload(velocities.size_bytes());
        stream << H::CopyBufferCommand{p.upload.data, p.working.data, 0, 0, velocities.size_bytes()};
        GpuPerformance::record_upload(count * sizeof(std::uint32_t));
        stream << H::CopyBufferCommand{p.resets_upload.data, p.resets.data, 0, 0, count * sizeof(std::uint32_t)};
        p.finish(stream);
    }
    p.count = static_cast<std::uint32_t>(count);
}

void GpuFloorSleepPipeline::begin_resident(std::span<const std::uint64_t> identities, GpuSubmissionBatch* batch) {
    GpuPerformance::Scope performance{"floor.begin_resident"};
    GpuPerformance::record_work_items(identities.size());
    auto& p = *impl_;
    p.usable(batch); validate_count(identities.size());
    std::unordered_map<std::uint64_t, std::uint32_t> old_indices, unique;
    for (std::size_t i = 0; i < p.identities.size(); ++i)
        old_indices.emplace(p.identities[i], static_cast<std::uint32_t>(i));
    std::vector<std::uint32_t> indices;
    indices.reserve(identities.size());
    for (const auto key : identities) {
        if (!key || !unique.emplace(key, static_cast<std::uint32_t>(unique.size())).second)
            throw std::invalid_argument("GPU resident body identities must be nonzero and unique");
        const auto old = old_indices.find(key);
        indices.push_back(old == old_indices.end() ? 0xffffffffu : old->second);
    }
    std::vector<std::uint64_t> next_identities(identities.begin(), identities.end());
    if (identities.empty()) {
        p.identities.clear(); p.count = 0; p.pending = false; p.pending_count = 0; p.resident_mode = true;
        return;
    }
    Impl::Recording mutation{p};
    if (batch) (void)batch->record(p.submission_state);
    // Retain old storage before resizing. Reading and writing are disjoint even
    // when identities reorder or the pool grows/shrinks across allocation sizes.
    auto previous = p.working.data;
    auto previous_timers = p.resident_timers.data;
    auto previous_status = p.resident_status.data;
    const auto count = identities.size();
    p.initial.ensure(count * 6, device_usage); p.working.ensure(count * 6, device_usage);
    p.next_timers.ensure(count, device_usage); p.next_status.ensure(count, device_usage);
    p.resets.ensure(count, device_usage); p.remap_indices.ensure(count, device_usage);
    if (!previous) previous = p.working.data;
    // First-use sentinel rows never read these bindings, but descriptors still
    // require storage distinct from this dispatch's writable destinations.
    if (!previous_timers) {
        p.resident_timers.ensure(1, device_usage); previous_timers = p.resident_timers.data;
    }
    if (!previous_status) {
        p.resident_status.ensure(1, device_usage); previous_status = p.resident_status.data;
    }
    const auto staged_indices = p.stage(p.info_upload, std::span<const std::uint32_t>(indices), batch);
    auto& kernel = p.remap;
    kernel.previous.bind(previous); kernel.previous_timers.bind(previous_timers); kernel.previous_status.bind(previous_status);
    kernel.initial.bind(p.initial.data); kernel.next_timers.bind(p.next_timers.data); kernel.next_status.bind(p.next_status.data);
    kernel.indices.bind(p.remap_indices.data); kernel.resets.bind(p.resets.data); kernel.count = static_cast<std::uint32_t>(count);
    {
        Impl::Recording recording{p};
        std::optional<H::HardwareStream> standalone;
        if (!batch) standalone.emplace(p.executor);
        auto& stream = batch ? batch->record(p.submission_state) : *standalone;
        GpuPerformance::record_upload(count * sizeof(std::uint32_t));
        stream << H::CopyBufferCommand{staged_indices, p.remap_indices.data, 0, 0, count * sizeof(std::uint32_t)};
        GpuPerformance::record_dispatch();
        stream << kernel.pipeline.groups((count + threads - 1u) / threads, 1);
        GpuPerformance::record_device_copy(count * 6 * sizeof(float));
        stream << H::CopyBufferCommand{p.initial.data, p.working.data, 0, 0, count * 6 * sizeof(float)};
        p.finish(stream, batch, false);
    }
    std::swap(p.resident_timers, p.next_timers); std::swap(p.resident_status, p.next_status);
    p.identities = std::move(next_identities); p.count = static_cast<std::uint32_t>(count);
    p.pending = false; p.pending_count = 0; p.resident_mode = true;
}

void GpuFloorSleepPipeline::set_velocities(const GpuRigidVelocityView& source, GpuSubmissionBatch* batch) {
    GpuPerformance::Scope performance{"floor.set_velocities"};
    GpuPerformance::record_work_items(source.count);
    auto& p = *impl_;
    p.usable(batch);
    if (source.readiness) source.readiness->check(batch);
    if (p.pending) throw std::logic_error("GPU floor candidates must be committed before replacing velocities");
    if (source.count != p.count) throw std::invalid_argument("GPU resident velocity count mismatch");
    if (!p.count) return;
    const auto bytes = std::size_t(p.count) * 6 * sizeof(float);
    if (!source.buffer || source.buffer.get_byte_size() < bytes)
        throw std::invalid_argument("GPU resident velocities require a valid buffer");
    if (source.buffer.same_resource(p.working.data)) return;
    Impl::Recording recording{p};
    std::optional<H::HardwareStream> standalone;
    if (!batch) standalone.emplace(p.executor);
    if (batch && source.readiness) (void)batch->record(source.readiness);
    auto& stream = batch ? batch->record(p.submission_state) : *standalone;
    GpuPerformance::record_device_copy(bytes);
    stream << H::CopyBufferCommand{source.buffer, p.working.data, 0, 0, bytes};
    p.finish(stream, batch, false);
}

std::vector<GpuFloorSleepPipeline::FloorContact> GpuFloorSleepPipeline::evaluate_floor(
    std::span<const FloorInput> inputs, const FloorParams& params) {
    GpuPerformance::Scope performance{"floor.evaluate"};
    GpuPerformance::record_work_items(inputs.size());
    auto& p = *impl_;
    p.usable();
    if (p.pending) throw std::logic_error("GPU floor/sleep evaluation must be committed first");
    validate_count(inputs.size());
    finite(params.floor_y); finite(params.gravity_y); nonnegative(params.floor_eps);
    finite(params.restitution); nonnegative(params.dt); nonnegative(params.low_velocity);
    nonnegative(params.zero_velocity); nonnegative(params.friction); nonnegative(params.static_friction);
    finite(params.floor_y + params.floor_eps);
    std::vector<std::uint8_t> seen(p.count, 0);
    for (const auto& value : inputs) {
        if (value.body >= p.count) throw std::out_of_range("GPU floor/sleep body index is out of range");
        if (seen[value.body]++) throw std::invalid_argument("GPU floor/sleep duplicate body index");
        finite(value.center); finite(value.inertia_inv); finite(value.lowest); finite(value.rotation);
        finite(value.mass); finite(value.bottom);
    }
    if (inputs.empty()) { p.pending = true; p.pending_count = 0; return {}; }
    std::vector<float> packed;
    std::vector<std::uint32_t> info;
    packed.reserve(inputs.size() * floor_words); info.reserve(inputs.size() * 2);
    for (const auto& value : inputs) {
        append(packed, value.center); append(packed, value.inertia_inv); append(packed, value.lowest);
        append(packed, value.rotation); packed.push_back(value.mass); packed.push_back(value.bottom);
        info.push_back(value.body);
        info.push_back(std::uint32_t(value.enabled) | (std::uint32_t(value.skinned) << 1u) |
                       (std::uint32_t(value.linear_lock & 7u) << 2u) | (std::uint32_t(value.angular_lock & 7u) << 5u));
    }
    p.input.ensure(packed.size(), device_usage); p.info.ensure(info.size(), device_usage);
    p.candidates.ensure(inputs.size() * 7, device_usage); p.metadata.ensure(inputs.size() * 2, device_usage);
    p.readback.ensure(inputs.size() * 2, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    p.stage(p.upload, std::span<const float>(packed)); p.stage(p.info_upload, std::span<const std::uint32_t>(info));
    auto& kernel = p.floor;
    kernel.inputs.bind(p.input.data); kernel.info.bind(p.info.data); kernel.velocities.bind(p.working.data);
    kernel.candidates.bind(p.candidates.data); kernel.metadata.bind(p.metadata.data);
    kernel.count = static_cast<std::uint32_t>(inputs.size());
    kernel.floor_y = params.floor_y; kernel.floor_eps = params.floor_eps; kernel.restitution = params.restitution;
    kernel.gravity_y = params.gravity_y; kernel.dt = params.dt; kernel.low_velocity = params.low_velocity;
    kernel.zero_velocity = params.zero_velocity; kernel.friction = params.friction; kernel.static_friction = params.static_friction;
    {
        Impl::Recording recording{p};
        auto stream = p.executor.stream();
        GpuPerformance::record_upload(packed.size() * sizeof(float));
        stream << H::CopyBufferCommand{p.upload.data, p.input.data, 0, 0, packed.size() * sizeof(float)};
        GpuPerformance::record_upload(info.size() * sizeof(std::uint32_t));
        stream << H::CopyBufferCommand{p.info_upload.data, p.info.data, 0, 0, info.size() * sizeof(std::uint32_t)};
        GpuPerformance::record_dispatch();
        stream << kernel.pipeline.groups((inputs.size() + threads - 1u) / threads, 1);
        GpuPerformance::record_readback(inputs.size() * 2 * sizeof(float));
        stream << H::CopyBufferCommand{p.metadata.data, p.readback.data, 0, 0, inputs.size() * 2 * sizeof(float)};
        p.finish(stream);
    }
    std::vector<std::array<float, 2>> raw(inputs.size());
    p.read(p.readback, std::span(raw));
    std::vector<FloorContact> result(inputs.size());
    for (std::size_t i = 0; i < result.size(); ++i) result[i] = {raw[i][0], raw[i][1] != 0.0f};
    p.pending = true; p.pending_count = static_cast<std::uint32_t>(inputs.size());
    return result;
}

void GpuFloorSleepPipeline::commit_floor(std::span<const std::uint8_t> accepted, GpuSubmissionBatch* batch) {
    GpuPerformance::Scope performance{"floor.commit"};
    GpuPerformance::record_work_items(accepted.size());
    auto& p = *impl_;
    p.usable(batch);
    if (!p.pending) throw std::logic_error("GPU floor/sleep has no pending evaluation");
    if (accepted.size() != p.pending_count) throw std::invalid_argument("GPU floor/sleep acceptance count mismatch");
    for (const auto value : accepted)
        if (value > 1) throw std::invalid_argument("GPU floor/sleep acceptance must be zero or one");
    if (accepted.empty()) { p.pending = false; return; }
    Impl::Recording mutation{p};
    if (batch) (void)batch->record(p.submission_state);
    std::vector<std::uint32_t> values(accepted.begin(), accepted.end());
    p.accepted.ensure(values.size(), device_usage);
    const auto staged_accepted = p.stage(p.upload, std::span<const std::uint32_t>(values), batch);
    auto& kernel = p.commit;
    kernel.velocities.bind(p.working.data); kernel.candidates.bind(p.candidates.data); kernel.metadata.bind(p.metadata.data);
    kernel.info.bind(p.info.data); kernel.accepted.bind(p.accepted.data); kernel.resets.bind(p.resets.data);
    kernel.count = p.pending_count;
    {
        Impl::Recording recording{p};
        std::optional<H::HardwareStream> standalone;
        if (!batch) standalone.emplace(p.executor);
        auto& stream = batch ? batch->record(p.submission_state) : *standalone;
        GpuPerformance::record_upload(values.size() * sizeof(std::uint32_t));
        stream << H::CopyBufferCommand{staged_accepted, p.accepted.data, 0, 0, values.size() * sizeof(std::uint32_t)};
        GpuPerformance::record_dispatch();
        stream << kernel.pipeline.groups((accepted.size() + threads - 1u) / threads, 1);
        p.finish(stream, batch, false);
    }
    p.pending = false; p.pending_count = 0;
}

std::vector<GpuFloorSleepPipeline::FloorVelocity> GpuFloorSleepPipeline::readback_velocities() {
    GpuPerformance::Scope performance{"floor.velocities_readback"};
    GpuPerformance::record_work_items(impl_->count);
    auto& p = *impl_;
    p.usable();
    if (!p.count) return {};
    p.readback.ensure(p.count * 6, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    p.resets_readback.ensure(p.count, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    {
        Impl::Recording recording{p};
        auto stream = p.executor.stream();
        GpuPerformance::record_readback(std::size_t(p.count) * 6 * sizeof(float));
        stream << H::CopyBufferCommand{p.working.data, p.readback.data, 0, 0, std::size_t(p.count) * 6 * sizeof(float)};
        GpuPerformance::record_readback(std::size_t(p.count) * sizeof(std::uint32_t));
        stream << H::CopyBufferCommand{p.resets.data, p.resets_readback.data, 0, 0, std::size_t(p.count) * sizeof(std::uint32_t)};
        p.finish(stream);
    }
    std::vector<Velocity> velocities(p.count);
    std::vector<std::uint32_t> resets(p.count);
    p.read(p.readback, std::span(velocities)); p.read(p.resets_readback, std::span(resets));
    std::vector<FloorVelocity> result(p.count);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = {velocities[i].velocity, velocities[i].angular_velocity, resets[i] != 0};
    return result;
}

std::vector<GpuFloorSleepPipeline::SleepResult> GpuFloorSleepPipeline::finish_sleep(
    std::span<const SleepInput> inputs, const SleepParams& params) {
    GpuPerformance::Scope performance{"floor.sleep_host"};
    GpuPerformance::record_work_items(inputs.size());
    auto& p = *impl_;
    p.usable();
    if (p.resident_mode) throw std::logic_error("GPU resident steps require finish_sleep_resident");
    if (p.pending) throw std::logic_error("GPU floor/sleep evaluation must be committed before sleep");
    if (inputs.size() != p.count) throw std::invalid_argument("GPU floor/sleep sleep input count mismatch");
    nonnegative(params.dt); nonnegative(params.velocity_threshold); nonnegative(params.time_threshold);
    finite(params.velocity_threshold * params.velocity_threshold);
    for (const auto& input : inputs) { nonnegative(input.sleep_timer); finite(input.sleep_timer + params.dt); }
    if (inputs.empty()) return {};
    std::vector<float> timers;
    std::vector<std::uint32_t> flags;
    timers.reserve(inputs.size()); flags.reserve(inputs.size());
    for (const auto& input : inputs) {
        timers.push_back(input.sleep_timer);
        flags.push_back(std::uint32_t(input.sleeping) | (std::uint32_t(input.dynamic) << 1u));
    }
    p.input.ensure(inputs.size(), device_usage); p.info.ensure(inputs.size(), device_usage);
    p.output.ensure(inputs.size() * 9, device_usage);
    p.readback.ensure(inputs.size() * 9, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    p.stage(p.upload, std::span<const float>(timers)); p.stage(p.info_upload, std::span<const std::uint32_t>(flags));
    auto& kernel = p.sleep;
    kernel.velocities.bind(p.working.data); kernel.resets.bind(p.resets.data); kernel.timers.bind(p.input.data);
    kernel.flags.bind(p.info.data); kernel.output.bind(p.output.data);
    kernel.count = p.count; kernel.dt = params.dt;
    kernel.velocity_threshold = params.velocity_threshold; kernel.time_threshold = params.time_threshold;
    {
        Impl::Recording recording{p};
        auto stream = p.executor.stream();
        GpuPerformance::record_upload(timers.size() * sizeof(float));
        stream << H::CopyBufferCommand{p.upload.data, p.input.data, 0, 0, timers.size() * sizeof(float)};
        GpuPerformance::record_upload(flags.size() * sizeof(std::uint32_t));
        stream << H::CopyBufferCommand{p.info_upload.data, p.info.data, 0, 0, flags.size() * sizeof(std::uint32_t)};
        GpuPerformance::record_dispatch();
        stream << kernel.pipeline.groups((inputs.size() + threads - 1u) / threads, 1);
        GpuPerformance::record_readback(inputs.size() * 9 * sizeof(float));
        stream << H::CopyBufferCommand{p.output.data, p.readback.data, 0, 0, inputs.size() * 9 * sizeof(float)};
        p.finish(stream);
    }
    std::vector<std::array<float, 9>> raw(inputs.size());
    p.read(p.readback, std::span(raw));
    std::vector<SleepResult> result(inputs.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        std::copy_n(raw[i].begin(), 3, result[i].velocity.begin());
        std::copy_n(raw[i].begin() + 3, 3, result[i].angular_velocity.begin());
        result[i].sleep_timer = raw[i][6]; result[i].sleeping = raw[i][7] != 0.0f; result[i].was_awake = raw[i][8] != 0.0f;
    }
    return result;
}
std::vector<GpuFloorSleepPipeline::SleepFlags> GpuFloorSleepPipeline::finish_sleep_resident(
    std::span<const ResidentSleepInput> inputs, const SleepParams& params, GpuSubmissionBatch* batch) {
    GpuPerformance::Scope performance{"floor.sleep_resident"};
    GpuPerformance::record_work_items(inputs.size());
    auto& p = *impl_;
    p.usable(batch);
    if (!p.resident_mode || p.pending)
        throw std::logic_error("GPU resident sleep requires a resident step with no pending floor evaluation");
    if (inputs.size() != p.count) throw std::invalid_argument("GPU resident sleep input count mismatch");
    nonnegative(params.dt); nonnegative(params.velocity_threshold); nonnegative(params.time_threshold);
    finite(params.velocity_threshold * params.velocity_threshold);
    if (inputs.empty()) return {};
    // The GPU becomes authoritative before flags reach the CPU. Any exception
    // through publication makes this state unusable rather than allowing replay
    // with stale CPU sleeping flags.
    Impl::Recording mutation{p};
    if (batch) (void)batch->record(p.submission_state);
    std::vector<std::uint32_t> flags;
    flags.reserve(inputs.size());
    for (const auto& input : inputs)
        flags.push_back(std::uint32_t(input.sleeping) | (std::uint32_t(input.dynamic) << 1u) |
                        (std::uint32_t(input.reset_timer) << 2u));
    p.info.ensure(inputs.size(), device_usage);
    p.readback.ensure(inputs.size(), H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    const auto staged_flags = p.stage(p.info_upload, std::span<const std::uint32_t>(flags), batch);
    auto& kernel = p.resident_sleep;
    kernel.velocities.bind(p.working.data); kernel.resets.bind(p.resets.data); kernel.timers.bind(p.resident_timers.data);
    kernel.flags.bind(p.info.data); kernel.status.bind(p.resident_status.data);
    kernel.count = p.count; kernel.dt = params.dt;
    kernel.velocity_threshold = params.velocity_threshold; kernel.time_threshold = params.time_threshold;
    // Preserve exact FLT_MAX bits: decimal shader literals can round above the
    // representable range when the EDSL serializes nine significant digits.
    kernel.timer_limit = std::numeric_limits<float>::max();
    {
        Impl::Recording recording{p};
        std::optional<H::HardwareStream> standalone;
        if (!batch) standalone.emplace(p.executor);
        auto& stream = batch ? batch->record(p.submission_state) : *standalone;
        GpuPerformance::record_upload(flags.size() * sizeof(std::uint32_t));
        stream << H::CopyBufferCommand{staged_flags, p.info.data, 0, 0, flags.size() * sizeof(std::uint32_t)};
        GpuPerformance::record_dispatch();
        stream << kernel.pipeline.groups((inputs.size() + threads - 1u) / threads, 1);
        GpuPerformance::record_readback(flags.size() * sizeof(std::uint32_t));
        stream << H::CopyBufferCommand{p.resident_status.data, p.readback.data, 0, 0, flags.size() * sizeof(std::uint32_t)};
        p.finish(stream, batch);
    }
    p.read(p.readback, std::span(flags));
    std::vector<SleepFlags> result(inputs.size());
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = {(flags[i] & 1u) != 0, (flags[i] & 2u) != 0};
    return result;
}

std::vector<GpuFloorSleepPipeline::SleepResult> GpuFloorSleepPipeline::readback_resident_state() {
    GpuPerformance::Scope performance{"floor.state_readback"};
    GpuPerformance::record_work_items(impl_->count);
    auto& p = *impl_;
    p.usable();
    if (!p.resident_mode) throw std::logic_error("GPU resident snapshot requires a resident step");
    if (!p.count) return {};
    const auto velocity_bytes = std::size_t(p.count) * 6 * sizeof(float);
    const auto scalar_bytes = std::size_t(p.count) * sizeof(float);
    p.readback.ensure(p.count * 8, H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
    {
        Impl::Recording recording{p};
        auto stream = p.executor.stream();
        GpuPerformance::record_readback(velocity_bytes);
        stream << H::CopyBufferCommand{p.working.data, p.readback.data, 0, 0, velocity_bytes};
        GpuPerformance::record_readback(scalar_bytes);
        stream << H::CopyBufferCommand{p.resident_timers.data, p.readback.data, 0, velocity_bytes, scalar_bytes};
        GpuPerformance::record_readback(scalar_bytes);
        stream << H::CopyBufferCommand{p.resident_status.data, p.readback.data, 0, velocity_bytes + scalar_bytes, scalar_bytes};
        p.finish(stream);
    }
    std::vector<float> raw(std::size_t(p.count) * 8);
    p.read(p.readback, std::span(raw));
    std::vector<SleepResult> result(p.count);
    for (std::size_t i = 0; i < result.size(); ++i) {
        std::copy_n(raw.begin() + i * 6, 3, result[i].velocity.begin());
        std::copy_n(raw.begin() + i * 6 + 3, 3, result[i].angular_velocity.begin());
        result[i].sleep_timer = raw[p.count * 6 + i];
        const auto flags = std::bit_cast<std::uint32_t>(raw[p.count * 7 + i]);
        result[i].sleeping = (flags & 1u) != 0; result[i].was_awake = (flags & 2u) != 0;
    }
    return result;
}
}  // namespace Corona::Systems
