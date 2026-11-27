#include "vision_skinning_bridge.h"
#include "vision_shared_buffer.h"
#include <corona/shared_data_hub.h>
#include "base/mgr/pipeline.h"
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>

namespace Corona::Systems::Vision {

struct SkinningReadView {
    ocarina::Device* device{};
    ocarina::Buffer<float> packed;
};

namespace {
using namespace ocarina;

auto convert_vertices() {
    return ocarina::Kernel{[](BufferVar<float> source, BufferVar<Vertex> output) {
        const Uint i = dispatch_idx().x;
        $if(i >= dispatch_dim().x) { $return(); };
        const Uint base = i * 8u;
        Var<Vertex> vertex;
        vertex->set_position(make_float3(source.read(base), source.read(base + 1u), -source.read(base + 2u)));
        vertex->set_normal(make_float3(source.read(base + 3u), source.read(base + 4u), -source.read(base + 5u)));
        vertex.uv[0] = source.read(base + 6u);
        vertex.uv[1] = source.read(base + 7u);
        vertex->set_lightmap_uv(make_float2(0.f));
        output.write(i, vertex);
    }};
}

struct Binding {
    VisionSkinnedMesh target;
    std::shared_ptr<SkinnedMeshWriteTarget> shared;
    std::optional<std::uint64_t> last_revision;
};

struct PoseUpdate {
    Binding* binding;
    SkinnedMeshOutput pose;
};

void return_to_corona(SkinnedMeshWriteTarget& target) {
    auto& view = *target.reader;
    view.device->set_buffer_external_ownership(view.packed.handle(), true);
    target.vertices.set_external_ownership(false);
    target.corona_owned = true;
}

std::shared_ptr<SkinnedMeshWriteTarget> register_target(Device& device, const VisionSkinnedMesh& mesh,
                                                       GeometryDevice& geometry) {
    const auto count = mesh.mesh->vertices().size();
    constexpr auto stride = sizeof(Resource::Vertex) / sizeof(float);
    if (count > std::numeric_limits<std::uint32_t>::max() / stride)
        throw std::overflow_error("Vision skinned mesh exceeds shader indexing range");
    const auto words = count * stride;
    if (geometry.skinned_write_targets.size() <= mesh.mesh_index)
        geometry.skinned_write_targets.resize(mesh.mesh_index + 1);
    auto& slot = geometry.skinned_write_targets[mesh.mesh_index];
    if (auto target = slot.lock(); target && target->model_id == mesh.model && target->reader->packed.size() == words) {
        if (target->reader->device != &device) throw std::runtime_error("Skinning target belongs to a different Vision device");
        return target;
    }
    if (!device.supports_buffer_export()) throw std::runtime_error("Vision skinned geometry requires Vulkan buffer export");
    auto target = std::make_shared<SkinnedMeshWriteTarget>();
    target->model_id = mesh.model;
    target->reader = std::make_shared<SkinningReadView>();
    auto& view = *target->reader;
    view.device = &device;
    view.packed = device.create_exported_buffer<float>(words, "Corona-Vision skinning output");
    auto desc = Horizon::HardwareBufferDesc::typed<float>(words, Horizon::BufferUsage_Storage |
        Horizon::BufferUsage_TransferSrc | Horizon::BufferUsage_TransferDst, "Corona-Vision skinning output");
    target->vertices = import_vision_buffer(device, view.packed.handle(), desc);
    return_to_corona(*target);

    // A paused character can have a valid native pose before the first Vision
    // camera opens. Seed this new allocation once; all future skin dispatches
    // (and all other runtimes on this device) use the advertised storage directly.
    if (mesh.mesh_index < geometry.skinned_mesh_outputs.size()) {
        auto& pose = geometry.skinned_mesh_outputs[mesh.mesh_index];
        if (pose.vertices && pose.model_id == mesh.model && pose.vertices.get_byte_size() == count * sizeof(Resource::Vertex)) {
            Horizon::HardwareExecutor executor;
            auto copy = executor.stream();
            copy << Horizon::CopyBufferCommand{pose.vertices, target->vertices, 0, 0, pose.vertices.get_byte_size()};
            executor.wait_completed(copy << Horizon::commit());
            // Revision zero marks the copied pose in this allocation. Direct skin
            // dispatches start at one, even when their bone palette is unchanged.
            pose.vertices = target->vertices;
            pose.revision = 0;
        }
    }
    slot = target;
    return target;
}
}  // namespace

struct VisionSkinningBridge::State {
    std::vector<Binding> bindings;
    ocarina::Shader<void(ocarina::Buffer<float>, ocarina::Buffer<ocarina::Vertex>)> convert;
};

VisionSkinningBridge::VisionSkinningBridge(ocarina::Device& device, std::vector<VisionSkinnedMesh> meshes)
    : state_(std::make_unique<State>()) {
    std::lock_guard lock(SharedDataHub::instance().skinned_output_mutex);
    for (auto& mesh : meshes) {
        if (auto* geometry = Corona::find_geometry(mesh.geometry)) {
            auto shared = register_target(device, mesh, *geometry);
            state_->bindings.push_back({std::move(mesh), std::move(shared)});
        }
    }
}
VisionSkinningBridge::~VisionSkinningBridge() {
    // Drain any producer inspecting weak targets before the exporting device is destroyed.
    std::lock_guard lock(SharedDataHub::instance().skinned_output_mutex);
    state_->bindings.clear();
}

bool VisionSkinningBridge::update(::vision::Pipeline& pipeline, bool transforms_changed) {
    using namespace ocarina;
    auto& state = *state_;
    std::vector<PoseUpdate> updates;
    std::vector<Binding*> restore_bind_pose;
    std::unique_lock lock(SharedDataHub::instance().skinned_output_mutex, std::defer_lock);
    if (!state.bindings.empty()) lock.lock();
    for (auto& binding : state.bindings) {
        auto geometry = Corona::find_geometry(binding.target.geometry);
        if (!geometry) continue;
        auto& shared = *binding.shared;
        if (!shared.corona_owned) return_to_corona(shared);
        const auto& outputs = geometry->skinned_mesh_outputs;
        if (binding.target.mesh_index >= outputs.size() || !outputs[binding.target.mesh_index].vertices) {
            const auto* model = Corona::find_model_resource(geometry->model_resource_handle);
            if (binding.last_revision && model && model->model_id == binding.target.model)
                restore_bind_pose.push_back(&binding);
            continue;
        }
        const auto& pose = outputs[binding.target.mesh_index];
        if (pose.model_id != binding.target.model) continue;
        if (!pose.vertices.same_resource(shared.vertices)) {
            const auto active = geometry->skinning_write_buffer(binding.target.mesh_index, pose.model_id,
                                                                 shared.vertices.get_byte_size());
            if (active.same_resource(shared.vertices))
                throw std::runtime_error("Waiting for the first pose in the current Vision skinning target");
            continue; // A topology rebuild is replacing this binding.
        }
        if (binding.last_revision != pose.revision) updates.push_back({&binding, pose});
    }
    if (updates.empty() && lock.owns_lock()) lock.unlock();
    if (updates.empty() && restore_bind_pose.empty() && !transforms_changed) return false;
    pipeline.activate_global_context();
    auto& stream = pipeline.stream();
    // Finish trace users before overwriting instance/vertex buffers or replacing a BLAS.
    stream << synchronize() << commit();
    auto& scene = pipeline.scene();
    vector<bool> dirty_meshes;
    if (!updates.empty() || !restore_bind_pose.empty())
        dirty_meshes.resize(scene.geometry().data()->mesh_handles().host_buffer().size());
    if (!updates.empty()) {
        auto& device = pipeline.device();
        if (!state.convert.has_function()) state.convert = device.compile(convert_vertices(), "Corona-Vision-Skinned-Vertices");
        std::vector<SkinnedMeshWriteTarget*> borrowed;
        borrowed.reserve(updates.size());
        std::exception_ptr failure;
        try {
            for (const auto& update : updates) {
                auto& shared = *update.binding->shared;
                auto& buffer = shared.reader->packed;
                if (shared.corona_owned) {
                    borrowed.push_back(&shared);
                    shared.corona_owned = false;
                    shared.vertices.set_external_ownership(true);
                    device.set_buffer_external_ownership(buffer.handle(), false);
                }
                auto& mesh = *update.binding->target.mesh;
                stream << state.convert(buffer, mesh.vertex_buffer().view()).dispatch(static_cast<uint>(mesh.vertices().size()));
                dirty_meshes.at(mesh.index()) = true;
            }
        } catch (...) {
            failure = std::current_exception();
        }
        // Complete the last shader read before allowing Mechanics to overwrite
        // the shared output. BLAS builds and tracing use Vision's converted mesh.
        stream << synchronize() << commit();
        for (auto* target : borrowed) {
            try {
                return_to_corona(*target);
            } catch (...) {
                if (!failure) failure = std::current_exception();
            }
        }
        lock.unlock();
        if (failure) std::rethrow_exception(failure);
    }
    for (const auto* binding : restore_bind_pose) {
        auto& mesh = *binding->target.mesh;
        stream << mesh.upload_vertices(mesh.vertices());
        dirty_meshes.at(mesh.index()) = true;
    }
    if (transforms_changed) scene.update_geometry_instances();
    scene.geometry().update_dynamic_geometry(stream, dirty_meshes, transforms_changed);
    for (const auto& update : updates) {
        const auto& lo = update.pose.min_xyz;
        const auto& hi = update.pose.max_xyz;
        Box3f bounds;
        bounds.extend(make_float3(lo[0], lo[1], -hi[2]));
        bounds.extend(make_float3(hi[0], hi[1], -lo[2]));
        update.binding->target.mesh->set_deformed_bounds(bounds);
    }
    for (const auto* binding : restore_bind_pose) binding->target.mesh->set_deformed_bounds({});
    for (const auto& group : scene.groups()) {
        Box3f bounds;
        group->for_each([&](SP<::vision::ShapeInstance> instance, uint) {
            if (!dirty_meshes.empty() && dirty_meshes.at(instance->mesh()->index())) instance->init_aabb();
            bounds.extend(instance->aabb);
        });
        // Infinite-light kernels capture the scene bounds. A loose group bound
        // keeps those kernels valid across small movements. BLAS and TLAS still
        // use the current vertices and transforms for every changed pose.
        if (!bounds.empty() && !group->aabb.contains(bounds)) {
            const float margin = bounds.radius() * 0.125f;
            group->aabb = bounds;
            group->aabb.lower -= make_float3(margin);
            group->aabb.upper += make_float3(margin);
        }
    }
    pipeline.refresh_world_bounds_dependents();
    pipeline.activate_view_context(0u);
    pipeline.invalidate();
    pipeline.invalidate_all_view_contexts();
    for (const auto& update : updates) update.binding->last_revision = update.pose.revision;
    for (auto* binding : restore_bind_pose) binding->last_revision.reset();
    return true;
}
}  // namespace Corona::Systems::Vision
