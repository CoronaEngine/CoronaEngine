#include "gpu_rigid_integration.h"
#include "gpu_performance.h"
#include "gpu_compute_pipeline.h"

#include <horizon.h>
#include <Codegen/BuiltinVariate.h>
#include <Codegen/ControlFlows.h>
#include <Codegen/CustomLibrary.h>
#include <Codegen/TypeAlias.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace Corona::Systems {
namespace {
namespace H = Corona::Horizon;
using namespace EmbeddedShader;
constexpr std::uint32_t threads = 64;
constexpr std::uint32_t velocity_words = 7, pose_words = 16, resident_pose_words = 10;
constexpr auto device_usage = H::BufferUsage_Storage | H::BufferUsage_TransferSrc | H::BufferUsage_TransferDst;
static_assert(GpuRigidIntegrationPipeline::max_bodies <= threads * 65535u);
static_assert(std::uint64_t(GpuRigidIntegrationPipeline::max_bodies) * pose_words * sizeof(float) <= 128u * 1024u * 1024u);
static_assert(std::uint64_t(GpuRigidIntegrationPipeline::max_bodies) * pose_words <= std::numeric_limits<std::uint32_t>::max());
static_assert(sizeof(GpuRigidIntegrationPipeline::VelocityResult) == 6 * sizeof(float));
static_assert(sizeof(GpuRigidIntegrationPipeline::PoseResult) == 7 * sizeof(float));
static_assert(sizeof(GpuRigidIntegrationPipeline::PoseVelocityResult) == 13 * sizeof(float));

template <class T>
struct Binding {
    H::HardwareBuffer handle;
    EmbeddedShader::Array<T> values;
    void bind(const H::HardwareBuffer& value) {
        handle = value;
        values = handle;
    }
};
using Floats = Binding<float>;
using UInts = Binding<std::uint32_t>;

void clamp_velocity(std::array<Float, 3>& velocity, const Float& limit) {
    Float squared;
    squared = velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2];
    $IF(squared > limit * limit) {
        Float scale;
        scale = limit / sqrt(squared);
        for (auto& component : velocity) component = component * scale;
    }
}

struct VelocityKernel {
    Floats input, output, resident_velocities;
    UInts flags;
    Uint count;
    Float dt, gravity_x, gravity_y, gravity_z, rot_factor, max_linear, max_angular;
    H::ComputePipeline<> pipeline;
    explicit VelocityKernel(bool resident = false) {
        count.as_push_constant();
        dt.as_push_constant();
        gravity_x.as_push_constant();
        gravity_y.as_push_constant();
        gravity_z.as_push_constant();
        rot_factor.as_push_constant();
        max_linear.as_push_constant();
        max_angular.as_push_constant();
        pipeline = make_compute_pipeline([&, resident] {
            Uint i;
            i = dispatchThreadID()->x;
            $IF(i < count) {
                Uint base, bits;
                base = i * Uint(resident ? 1u : velocity_words);
                bits = flags.values[i];
                std::array<Float, 3> v, w;
                for (std::uint32_t a = 0; a < 3; ++a) {
                    if (resident) {
                        v[a] = resident_velocities.values[i * Uint(6u) + Uint(a)];
                        w[a] = resident_velocities.values[i * Uint(6u) + Uint(a + 3u)];
                    } else {
                        v[a] = input.values[base + Uint(a)];
                        w[a] = input.values[base + Uint(a + 3u)];
                    }
                }
                $IF((bits & Uint(256u)) != Uint(0u)) {
                    for (std::uint32_t a = 0; a < 3; ++a) { v[a] = 0.0f; w[a] = 0.0f; }
                }
                $IF((bits & Uint(2u)) == Uint(0u)) {
                    $IF((bits & Uint(1u)) != Uint(0u)) {
                        Float exponent, damping, linear_scale, angular_base, angular_scale;
                        exponent = dt * Float(60.0f);
                        damping = input.values[base + Uint(resident ? 0u : 6u)];
                        linear_scale = 1.0f;
                        angular_scale = 1.0f;
                        // Vulkan pow(0,0) is undefined. CPU std::pow returns 1;
                        // retain that behavior explicitly for a zero timestep.
                        $IF(dt > Float(0.0f)) {
                            linear_scale = 0.0f;
                            $IF(damping > Float(0.0f)) { linear_scale = pow(damping, exponent); }
                            angular_base = max(damping * rot_factor, Float(0.9f));
                            angular_scale = pow(angular_base, exponent);
                        }
                        for (auto& component : v) component = component * linear_scale;
                        v[0] = v[0] + gravity_x * dt;
                        v[1] = v[1] + gravity_y * dt;
                        v[2] = v[2] + gravity_z * dt;
                        for (auto& component : w) component = component * angular_scale;
                        // Axis locks follow the original speed clamp, so a
                        // locked component still contributes to its magnitude.
                        clamp_velocity(v, max_linear);
                        clamp_velocity(w, max_angular);
                    }
                    for (std::uint32_t a = 0; a < 3; ++a) {
                        $IF((bits & Uint(1u << (a + 2u))) != Uint(0u)) { v[a] = 0.0f; }
                        $IF((bits & Uint(1u << (a + 5u))) != Uint(0u)) { w[a] = 0.0f; }
                    }
                }
                for (std::uint32_t a = 0; a < 3; ++a) {
                    output.values[i * Uint(6u) + Uint(a)] = v[a];
                    output.values[i * Uint(6u) + Uint(a + 3u)] = w[a];
                }
            }
        }, ktm::uvec3(threads, 1, 1));
    }
};

struct PoseKernel {
    Floats input, output;
    Floats resident_velocities, projected_velocities;
    UInts flags, velocity_indices, destination_indices;
    Uint count;
    Float dt;
    H::ComputePipeline<> pipeline;
    explicit PoseKernel(bool resident = false, bool scatter = false, bool pose_only = false) {
        count.as_push_constant();
        dt.as_push_constant();
        pipeline = make_compute_pipeline([&, resident, scatter, pose_only] {
            Uint i;
            i = dispatchThreadID()->x;
            $IF(i < count) {
                Uint base, bits;
                base = i * Uint(resident ? resident_pose_words : pose_words);
                bits = flags.values[i];
                std::array<Float, 3> position;
                std::array<Float, 4> q;
                for (std::uint32_t a = 0; a < 3; ++a) position[a] = input.values[base + Uint(a)];
                for (std::uint32_t a = 0; a < 4; ++a) q[a] = input.values[base + Uint(a + 3u)];
                std::array<Float, 3> linear, angular;
                if (resident) {
                    Uint velocity_base;
                    velocity_base = velocity_indices.values[i] * Uint(6u);
                    for (std::uint32_t a = 0; a < 3; ++a) {
                        linear[a] = resident_velocities.values[velocity_base + Uint(a)];
                        angular[a] = resident_velocities.values[velocity_base + Uint(a + 3u)];
                        $IF((bits & Uint(128u)) != Uint(0u)) {
                            $IF((bits & Uint(1u << (a + 1u))) != Uint(0u)) { linear[a] = 0.0f; }
                            $IF((bits & Uint(1u << (a + 4u))) != Uint(0u)) { angular[a] = 0.0f; }
                        }
                    }
                } else {
                    for (std::uint32_t a = 0; a < 3; ++a) {
                        linear[a] = input.values[base + Uint(a + 7u)];
                        angular[a] = input.values[base + Uint(a + 10u)];
                    }
                }
                $IF((bits & Uint(1u)) != Uint(0u)) {
                    std::array<Float, 3> omega;
                    for (std::uint32_t a = 0; a < 3; ++a) {
                        Float velocity, correction;
                        velocity = linear[a];
                        omega[a] = angular[a];
                        correction = input.values[base + Uint(a + (resident ? 7u : 13u))];
                        $IF((bits & Uint(1u << (a + 1u))) != Uint(0u)) {
                            velocity = 0.0f;
                            correction = 0.0f;
                        }
                        $IF((bits & Uint(1u << (a + 4u))) != Uint(0u)) { omega[a] = 0.0f; }
                        position[a] = position[a] + velocity * dt;
                        position[a] = position[a] + correction;
                    }
                    // KTM quat_mul: (0,omega_world) * q in xyzw storage order.
                    // Compute every derivative before changing any component.
                    std::array<Float, 4> dq;
                    dq[0] = q[3] * omega[0] + omega[1] * q[2] - omega[2] * q[1];
                    dq[1] = q[3] * omega[1] + omega[2] * q[0] - omega[0] * q[2];
                    dq[2] = q[3] * omega[2] + omega[0] * q[1] - omega[1] * q[0];
                    dq[3] = Float(0.0f) - omega[0] * q[0] - omega[1] * q[1] - omega[2] * q[2];
                    for (std::uint32_t a = 0; a < 4; ++a) q[a] = q[a] + Float(0.5f) * dt * dq[a];
                    Float inverse;
                    inverse = Float(1.0f) / sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                    for (auto& component : q) component = component * inverse;
                }
                const std::uint32_t output_words = resident && !scatter && !pose_only ? 13u : 7u;
                for (std::uint32_t a = 0; a < 3; ++a) output.values[i * Uint(output_words) + Uint(a)] = position[a];
                for (std::uint32_t a = 0; a < 4; ++a) output.values[i * Uint(output_words) + Uint(a + 3u)] = q[a];
                if (scatter) {
                    Uint destination;
                    destination = destination_indices.values[i] * Uint(6u);
                    for (std::uint32_t a = 0; a < 3; ++a) {
                        projected_velocities.values[destination + Uint(a)] = linear[a];
                        projected_velocities.values[destination + Uint(a + 3u)] = angular[a];
                    }
                } else if (resident && !pose_only) {
                    for (std::uint32_t a = 0; a < 3; ++a) {
                        output.values[i * Uint(output_words) + Uint(a + 7u)] = linear[a];
                        output.values[i * Uint(output_words) + Uint(a + 10u)] = angular[a];
                    }
                }
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
        desc.element_count = words;
        desc.element_size = sizeof(float);
        desc.usage = usage;
        desc.cpu_access = cpu;
        desc.debug_name = "mechanics.rigid_integration";
        data = H::HardwareBuffer(desc);
        capacity = words;
    }
};

void nonnegative(float value, const char* error) {
    if (!std::isfinite(value) || value < 0.0f) throw std::invalid_argument(error);
}
template <std::size_t N>
void finite(const std::array<float, N>& values) {
    for (const auto value : values)
        if (!std::isfinite(value)) throw std::invalid_argument("GPU rigid integration requires finite state");
}
void validate_count(std::size_t count) {
    if (count > GpuRigidIntegrationPipeline::max_bodies)
        throw std::length_error("GPU rigid integration batch exceeds storage, dispatch or indexing limit");
}
template <std::size_t N>
void append(std::vector<float>& packed, const std::array<float, N>& values) {
    packed.insert(packed.end(), values.begin(), values.end());
}
void validate_velocity_params(const GpuRigidIntegrationPipeline::VelocityParams& params) {
    nonnegative(params.dt, "Invalid GPU rigid integration timestep");
    nonnegative(params.rot_damping_factor, "Invalid GPU rigid integration rotational damping");
    nonnegative(params.max_linear_speed, "Invalid GPU rigid integration linear speed limit");
    nonnegative(params.max_angular_speed, "Invalid GPU rigid integration angular speed limit");
    finite(params.gravity);
    if (!std::isfinite(params.dt * 60.0f)) throw std::invalid_argument("GPU rigid integration timestep is too large");
}
void validate_velocity_metadata(const GpuRigidIntegrationPipeline::VelocityInput& value,
                                const GpuRigidIntegrationPipeline::VelocityParams& params) {
    if (!std::isfinite(value.damping)) throw std::invalid_argument("Invalid GPU rigid integration damping");
    if (value.dynamic && !value.sleeping) {
        nonnegative(value.damping, "Invalid GPU rigid integration damping");
        if (!std::isfinite(value.damping * params.rot_damping_factor))
            throw std::invalid_argument("GPU rigid integration damping product is too large");
    }
}
std::uint32_t velocity_flags(const GpuRigidIntegrationPipeline::VelocityInput& value) {
    return std::uint32_t(value.dynamic) | (std::uint32_t(value.sleeping) << 1u) |
           (std::uint32_t(value.linear_lock & 7u) << 2u) | (std::uint32_t(value.angular_lock & 7u) << 5u) |
           (std::uint32_t(value.reset_velocity) << 8u);
}
void set_velocity_params(VelocityKernel& kernel, const GpuRigidIntegrationPipeline::VelocityParams& params) {
    kernel.dt = params.dt;
    kernel.gravity_x = params.gravity[0];
    kernel.gravity_y = params.gravity[1];
    kernel.gravity_z = params.gravity[2];
    kernel.rot_factor = params.rot_damping_factor;
    kernel.max_linear = params.max_linear_speed;
    kernel.max_angular = params.max_angular_speed;
}
}  // namespace

struct GpuRigidIntegrationPipeline::Impl {
    H::HardwareExecutor executor;
    VelocityKernel velocity;
    VelocityKernel resident_velocity{true};
    PoseKernel pose;
    PoseKernel resident_pose{true};
    PoseKernel scatter_pose{true, true};
    PoseKernel prediction_pose{true, false, true};
    Workspace input, flags, output, input_upload, flags_upload, readback;
    Workspace indices, indices_upload;
    Workspace destinations, destinations_upload;
    // Never share velocity output with the generic pose/readback workspace.
    // Two buffers also permit a previous result to become the next input.
    std::array<Workspace, 2> resident_velocity_outputs;
    std::shared_ptr<GpuSubmissionState> submission_state = std::make_shared<GpuSubmissionState>();
    std::uint64_t submissions{};
    H::SubmitReceipt last_receipt;

    struct Recording {
        Impl& owner;
        int exceptions{std::uncaught_exceptions()};
        ~Recording() {
            if (std::uncaught_exceptions() > exceptions) owner.submission_state->poisoned = true;
        }
    };
    ~Impl() {
        if (submission_state->owner) submission_state->poisoned = true;
        if (submission_state->poisoned && last_receipt.data) {
            try {
                executor.wait_completed(last_receipt);
            } catch (...) {
            }
        }
    }
    void usable(GpuSubmissionBatch* batch = nullptr) const {
        submission_state->check(batch);
    }
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
    H::HardwareBuffer stage(Workspace& staging, std::span<const T> values, GpuSubmissionBatch* batch) {
        if (batch) return batch->upload(std::as_bytes(values));
        staging.ensure(values.size_bytes() / sizeof(float), H::BufferUsage_TransferSrc, H::CpuAccessMode::Write);
        if (!staging.data.write_bytes(std::as_bytes(values)))
            throw std::runtime_error("GPU rigid integration upload failed");
        return staging.data;
    }
    template <class Kernel, class Result>
    std::vector<Result> run(Kernel& kernel, const std::vector<float>& packed, const std::vector<std::uint32_t>& bits,
                            std::span<const std::uint32_t> source_indices = {},
                            std::span<const std::uint32_t> destination_indices = {}, GpuSubmissionBatch* batch = nullptr) {
        Recording mutation{*this};
        if (batch) (void)batch->record(submission_state);
        std::vector<Result> result(bits.size());
        const auto result_bytes = result.size() * sizeof(Result);
        input.ensure(packed.size(), device_usage);
        flags.ensure(bits.size(), device_usage);
        output.ensure(result_bytes / sizeof(float), device_usage);
        // The batch owns distinct upload slots until completion. Its pool is
        // reused on later flushes without overwriting pending velocity inputs.
        const auto staged_input = stage(input_upload, std::span(packed), batch);
        const auto staged_flags = stage(flags_upload, std::span(bits), batch);
        H::HardwareBuffer staged_indices, staged_destinations;
        readback.ensure(result_bytes / sizeof(float), H::BufferUsage_TransferDst, H::CpuAccessMode::Read);
        if (!source_indices.empty()) {
            indices.ensure(source_indices.size(), device_usage);
            staged_indices = stage(indices_upload, source_indices, batch);
            if constexpr (std::is_same_v<Kernel, PoseKernel>) kernel.velocity_indices.bind(indices.data);
        }
        if (!destination_indices.empty()) {
            destinations.ensure(destination_indices.size(), device_usage);
            staged_destinations = stage(destinations_upload, destination_indices, batch);
            scatter_pose.destination_indices.bind(destinations.data);
        }
        {
            Recording recording{*this};
            std::optional<H::HardwareStream> standalone;
            if (!batch) standalone.emplace(executor);
            auto& stream = batch ? batch->record(submission_state) : *standalone;
            GpuPerformance::record_upload(packed.size() * sizeof(float));
            stream << H::CopyBufferCommand{staged_input, input.data, 0, 0, packed.size() * sizeof(float)};
            GpuPerformance::record_upload(bits.size() * sizeof(std::uint32_t));
            stream << H::CopyBufferCommand{staged_flags, flags.data, 0, 0, bits.size() * sizeof(std::uint32_t)};
            if (!source_indices.empty()) {
                GpuPerformance::record_upload(source_indices.size_bytes());
                stream << H::CopyBufferCommand{staged_indices, indices.data, 0, 0, source_indices.size_bytes()};
            }
            if (!destination_indices.empty()) {
                GpuPerformance::record_upload(destination_indices.size_bytes());
                stream << H::CopyBufferCommand{staged_destinations, destinations.data, 0, 0, destination_indices.size_bytes()};
            }
            kernel.input.bind(input.data);
            kernel.flags.bind(flags.data);
            kernel.output.bind(output.data);
            kernel.count = static_cast<std::uint32_t>(bits.size());
            GpuPerformance::record_dispatch();
            stream << kernel.pipeline.groups((bits.size() + threads - 1u) / threads, 1);
            GpuPerformance::record_readback(result_bytes);
            stream << H::CopyBufferCommand{output.data, readback.data, 0, 0, result_bytes};
            finish(stream, batch);
        }
        if (!readback.data.read_bytes(std::as_writable_bytes(std::span(result))))
            throw std::runtime_error("GPU rigid integration readback failed");
        return result;
    }
    GpuRigidVelocityView run_resident_velocity(const std::vector<float>& damping,
                                              const std::vector<std::uint32_t>& bits,
                                              const GpuRigidVelocityView& source, GpuSubmissionBatch* batch) {
        Recording mutation{*this};
        if (batch) (void)batch->record(submission_state);
        if (batch && source.readiness) (void)batch->record(source.readiness);
        auto& target = resident_velocity_outputs[source.buffer.same_resource(resident_velocity_outputs[0].data) ? 1 : 0];
        input.ensure(damping.size(), device_usage);
        flags.ensure(bits.size(), device_usage);
        target.ensure(bits.size() * 6u, device_usage);
        const auto staged_input = stage(input_upload, std::span(damping), batch);
        const auto staged_flags = stage(flags_upload, std::span(bits), batch);
        {
            Recording recording{*this};
            std::optional<H::HardwareStream> standalone;
            if (!batch) standalone.emplace(executor);
            auto& stream = batch ? batch->record(submission_state) : *standalone;
            GpuPerformance::record_upload(damping.size() * sizeof(float));
            stream << H::CopyBufferCommand{staged_input, input.data, 0, 0, damping.size() * sizeof(float)};
            GpuPerformance::record_upload(bits.size() * sizeof(std::uint32_t));
            stream << H::CopyBufferCommand{staged_flags, flags.data, 0, 0, bits.size() * sizeof(std::uint32_t)};
            resident_velocity.input.bind(input.data);
            resident_velocity.flags.bind(flags.data);
            resident_velocity.output.bind(target.data);
            resident_velocity.resident_velocities.bind(source.buffer);
            resident_velocity.count = static_cast<std::uint32_t>(bits.size());
            GpuPerformance::record_dispatch();
            stream << resident_velocity.pipeline.groups((bits.size() + threads - 1u) / threads, 1);
            finish(stream, batch, false);
        }
        return {target.data, static_cast<std::uint32_t>(bits.size()), submission_state};
    }
};

GpuRigidIntegrationPipeline::GpuRigidIntegrationPipeline() : impl_(std::make_unique<Impl>()) {}
GpuRigidIntegrationPipeline::~GpuRigidIntegrationPipeline() = default;
std::uint64_t GpuRigidIntegrationPipeline::submission_count() const noexcept { return impl_->submissions; }

std::vector<GpuRigidIntegrationPipeline::VelocityResult> GpuRigidIntegrationPipeline::update_velocities(
    std::span<const VelocityInput> inputs, const VelocityParams& params) {
    GpuPerformance::Scope performance{"integration.velocity_host"};
    GpuPerformance::record_work_items(inputs.size());
    impl_->usable();
    validate_count(inputs.size());
    validate_velocity_params(params);
    for (const auto& value : inputs) {
        finite(value.velocity);
        finite(value.angular_velocity);
        validate_velocity_metadata(value, params);
    }
    if (inputs.empty()) return {};
    std::vector<float> packed;
    std::vector<std::uint32_t> bits;
    packed.reserve(inputs.size() * velocity_words);
    bits.reserve(inputs.size());
    for (const auto& value : inputs) {
        append(packed, value.velocity);
        append(packed, value.angular_velocity);
        packed.push_back(value.damping);
        bits.push_back(velocity_flags(value));
    }
    auto& kernel = impl_->velocity;
    set_velocity_params(kernel, params);
    return impl_->run<VelocityKernel, VelocityResult>(kernel, packed, bits);
}

GpuRigidVelocityView GpuRigidIntegrationPipeline::update_velocities_device(
    std::span<const VelocityInput> metadata, const GpuRigidVelocityView& source, const VelocityParams& params,
    GpuSubmissionBatch* batch) {
    GpuPerformance::Scope performance{"integration.velocity_device"};
    GpuPerformance::record_work_items(metadata.size());
    impl_->usable(batch);
    if (source.readiness) source.readiness->check(batch);
    validate_count(metadata.size());
    validate_count(source.count);
    validate_velocity_params(params);
    if (metadata.size() != source.count)
        throw std::invalid_argument("GPU rigid integration requires one metadata record per resident velocity");
    for (const auto& value : metadata) validate_velocity_metadata(value, params);
    if (metadata.empty()) return {};
    if (!source.buffer || source.buffer.get_byte_size() < std::uint64_t(source.count) * 6u * sizeof(float))
        throw std::invalid_argument("GPU rigid integration requires a valid velocity buffer");
    std::vector<float> damping;
    std::vector<std::uint32_t> bits;
    damping.reserve(metadata.size());
    bits.reserve(metadata.size());
    for (const auto& value : metadata) {
        damping.push_back(value.damping);
        bits.push_back(velocity_flags(value));
    }
    set_velocity_params(impl_->resident_velocity, params);
    return impl_->run_resident_velocity(damping, bits, source, batch);
}

std::vector<GpuRigidIntegrationPipeline::PoseResult> GpuRigidIntegrationPipeline::integrate_poses(
    std::span<const PoseInput> inputs, float dt) {
    GpuPerformance::Scope performance{"integration.pose_host"};
    GpuPerformance::record_work_items(inputs.size());
    impl_->usable();
    validate_count(inputs.size());
    nonnegative(dt, "Invalid GPU rigid integration timestep");
    for (const auto& value : inputs) {
        finite(value.position);
        finite(value.orientation);
        finite(value.velocity);
        finite(value.angular_velocity);
        finite(value.correction);
        if (value.advance) {
            float norm_squared = 0.0f;
            for (const auto component : value.orientation) norm_squared += component * component;
            if (!std::isfinite(norm_squared) || norm_squared <= 0.0f)
                throw std::invalid_argument("GPU rigid integration requires a nondegenerate orientation");
        }
    }
    if (inputs.empty()) return {};
    std::vector<float> packed;
    std::vector<std::uint32_t> bits;
    packed.reserve(inputs.size() * pose_words);
    bits.reserve(inputs.size());
    for (const auto& value : inputs) {
        append(packed, value.position);
        append(packed, value.orientation);
        append(packed, value.velocity);
        append(packed, value.angular_velocity);
        append(packed, value.correction);
        bits.push_back(std::uint32_t(value.advance) | (std::uint32_t(value.linear_lock & 7u) << 1u) |
                       (std::uint32_t(value.angular_lock & 7u) << 4u));
    }
    impl_->pose.dt = dt;
    return impl_->run<PoseKernel, PoseResult>(impl_->pose, packed, bits);
}

namespace {
struct PackedResidentPoses {
    std::vector<float> packed;
    std::vector<std::uint32_t> bits, indices;
};
PackedResidentPoses pack_resident_poses(std::span<const GpuRigidIntegrationPipeline::ResidentPoseInput> inputs,
                                       const GpuRigidVelocityView& velocities, float dt, GpuSubmissionBatch* batch = nullptr) {
    if (velocities.readiness) velocities.readiness->check(batch);
    validate_count(inputs.size());
    nonnegative(dt, "Invalid GPU rigid integration timestep");
    if (inputs.empty()) return {};
    validate_count(velocities.count);
    if (!velocities.buffer || velocities.buffer.get_byte_size() < std::uint64_t(velocities.count) * 6u * sizeof(float))
        throw std::invalid_argument("GPU rigid integration requires a valid velocity buffer");
    for (const auto& input : inputs) {
        if (input.velocity_index >= velocities.count)
            throw std::out_of_range("GPU rigid integration velocity index is out of range");
        finite(input.pose.position);
        finite(input.pose.orientation);
        finite(input.pose.correction);
        if (input.pose.advance) {
            float norm_squared = 0.0f;
            for (const auto component : input.pose.orientation) norm_squared += component * component;
            if (!std::isfinite(norm_squared) || norm_squared <= 0.0f)
                throw std::invalid_argument("GPU rigid integration requires a nondegenerate orientation");
        }
    }
    PackedResidentPoses result;
    auto& packed = result.packed;
    auto& bits = result.bits;
    auto& source_indices = result.indices;
    packed.reserve(inputs.size() * resident_pose_words);
    bits.reserve(inputs.size());
    source_indices.reserve(inputs.size());
    for (const auto& input : inputs) {
        const auto& value = input.pose;
        append(packed, value.position);
        append(packed, value.orientation);
        // Upload only pose/correction: solver velocity is consumed on device.
        append(packed, value.correction);
        bits.push_back(std::uint32_t(value.advance) | (std::uint32_t(value.linear_lock & 7u) << 1u) |
                       (std::uint32_t(value.angular_lock & 7u) << 4u) |
                       (std::uint32_t(input.apply_velocity_locks) << 7u));
        source_indices.push_back(input.velocity_index);
    }
    return result;
}
}  // namespace

std::vector<GpuRigidIntegrationPipeline::PoseVelocityResult> GpuRigidIntegrationPipeline::integrate_poses_from_velocities(
    std::span<const ResidentPoseInput> inputs, const GpuRigidVelocityView& velocities, float dt) {
    GpuPerformance::Scope performance{"integration.pose_velocity_readback"};
    GpuPerformance::record_work_items(inputs.size());
    impl_->usable();
    const auto data = pack_resident_poses(inputs, velocities, dt);
    if (inputs.empty()) return {};
    impl_->resident_pose.dt = dt;
    impl_->resident_pose.resident_velocities.bind(velocities.buffer);
    return impl_->run<PoseKernel, PoseVelocityResult>(impl_->resident_pose, data.packed, data.bits, data.indices);
}

std::vector<GpuRigidIntegrationPipeline::PoseResult> GpuRigidIntegrationPipeline::predict_poses_from_velocities(
    std::span<const ResidentPoseInput> inputs, const GpuRigidVelocityView& velocities, float dt, GpuSubmissionBatch* batch) {
    GpuPerformance::Scope performance{"integration.predict"};
    GpuPerformance::record_work_items(inputs.size());
    impl_->usable(batch);
    const auto data = pack_resident_poses(inputs, velocities, dt, batch);
    if (inputs.empty()) return {};
    impl_->prediction_pose.dt = dt;
    impl_->prediction_pose.resident_velocities.bind(velocities.buffer);
    if (batch && velocities.readiness) (void)batch->record(velocities.readiness);
    return impl_->run<PoseKernel, PoseResult>(impl_->prediction_pose, data.packed, data.bits, data.indices, {}, batch);
}

std::vector<GpuRigidIntegrationPipeline::PoseResult> GpuRigidIntegrationPipeline::integrate_poses_to_velocities(
    std::span<const ResidentPoseInput> inputs, const GpuRigidVelocityView& source,
    const GpuRigidVelocityTarget& destination, std::span<const std::uint32_t> destination_indices, float dt,
    GpuSubmissionBatch* batch) {
    GpuPerformance::Scope performance{"integration.pose_scatter"};
    GpuPerformance::record_work_items(inputs.size());
    impl_->usable(batch);
    if (destination.readiness) destination.readiness->check(batch);
    const auto data = pack_resident_poses(inputs, source, dt, batch);
    if (destination_indices.size() != inputs.size())
        throw std::invalid_argument("GPU rigid integration requires one destination index per pose");
    if (inputs.empty()) return {};
    validate_count(destination.count);
    if (!destination.buffer || destination.buffer.get_byte_size() < std::uint64_t(destination.count) * 6u * sizeof(float))
        throw std::invalid_argument("GPU rigid integration requires a valid velocity destination");
    if (destination.buffer.same_resource(source.buffer))
        throw std::invalid_argument("GPU rigid integration source and destination must not alias");
    std::vector<std::uint32_t> sorted(destination_indices.begin(), destination_indices.end());
    std::sort(sorted.begin(), sorted.end());
    if (sorted.back() >= destination.count)
        throw std::out_of_range("GPU rigid integration destination index is out of range");
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        throw std::invalid_argument("GPU rigid integration destination indices must be unique");
    // A failed scatter can modify the destination even if its producer did not
    // record any work in this batch (e.g. a second shared-transform pose batch).
    struct DestinationMutation {
        std::shared_ptr<GpuSubmissionState> state;
        int exceptions{std::uncaught_exceptions()};
        ~DestinationMutation() {
            if (state && std::uncaught_exceptions() > exceptions) state->poisoned = true;
        }
    } mutation{destination.readiness};
    if (batch && source.readiness) (void)batch->record(source.readiness);
    if (batch && destination.readiness) (void)batch->record(destination.readiness);
    impl_->scatter_pose.dt = dt;
    impl_->scatter_pose.resident_velocities.bind(source.buffer);
    impl_->scatter_pose.projected_velocities.bind(destination.buffer);
    return impl_->run<PoseKernel, PoseResult>(impl_->scatter_pose, data.packed, data.bits, data.indices, destination_indices, batch);
}
}  // namespace Corona::Systems
