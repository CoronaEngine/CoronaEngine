//
// Created by Z on 2025/12/13.
//

#pragma once

#include "rhi/common.h"
#include "base/shape.h"
#include "base/color/spectrum.h"
#include "base/scattering/interaction.h"
#include "base/sampler.h"
#include "base/using.h"

namespace vision {

class Scene;
class Pipeline;
class ShapeInstance;

class GeometryData {
private:
    Device *device_{};
    BindlessArray *bindless_array_{};
    RegistrableManaged<InstanceData> instances_;
    RegistrableManaged<MeshHandle> mesh_handles_;

    // mesh registry (merged from MeshRegistry)
    std::map<uint64_t, SP<Mesh>> mesh_map_;
    vector<Mesh *> meshes_;

public:
    GeometryData(Device &device, BindlessArray &bindless) noexcept;
    void add_instance(InstanceData instance) noexcept;
    void add_mesh_handle(MeshHandle handle) noexcept;
    void reset_gpu_buffers() noexcept;
    void clear_host() noexcept;
    void clear_all() noexcept;
    [[nodiscard]] Device &device() noexcept { return *device_; }
    [[nodiscard]] const Device &device() const noexcept { return *device_; }
    [[nodiscard]] BindlessArray &bindless_array() noexcept { return *bindless_array_; }
    [[nodiscard]] const BindlessArray &bindless_array() const noexcept { return *bindless_array_; }
    OC_MAKE_MEMBER_GETTER(instances, &)
    OC_MAKE_MEMBER_GETTER(mesh_handles, &)

    // mesh registry methods
    [[nodiscard]] SP<Mesh> register_mesh(Mesh mesh) noexcept;
    [[nodiscard]] SP<Mesh> register_mesh(SP<Mesh> mesh) noexcept;
    [[nodiscard]] SP<const Mesh> get_mesh(uint64_t hash) const noexcept;
    [[nodiscard]] SP<Mesh> get_mesh(uint64_t hash) noexcept;
    [[nodiscard]] bool contain_mesh(uint64_t hash) noexcept;
    bool remove_mesh(uint64_t hash) noexcept;
    [[nodiscard]] CommandBatch upload_meshes() noexcept;
    void for_each_mesh(const std::function<void(Mesh *, uint)> &func) noexcept;
    void for_each_mesh(const std::function<void(const Mesh *, uint)> &func) const noexcept;
    void tidy_up_meshes() noexcept;
    void clear_meshes() noexcept;
};

class GeometryGpuResource {
private:
    Device *device_{};
    BindlessArray bindless_array_;
    GeometryData data_;
    ocarina::Accel accel_;
    vector<uint> accel_mesh_ids_;
    uint64_t build_count_{0};

public:
    explicit GeometryGpuResource(Device &device) noexcept;
    GeometryGpuResource(const GeometryGpuResource &) = delete;
    GeometryGpuResource &operator=(const GeometryGpuResource &) = delete;
    GeometryGpuResource(GeometryGpuResource &&) = delete;
    GeometryGpuResource &operator=(GeometryGpuResource &&) = delete;

    [[nodiscard]] Device &device() noexcept { return *device_; }
    [[nodiscard]] const Device &device() const noexcept { return *device_; }
    [[nodiscard]] BindlessArray &bindless_array() noexcept { return bindless_array_; }
    [[nodiscard]] const BindlessArray &bindless_array() const noexcept { return bindless_array_; }
    [[nodiscard]] GeometryData &data() noexcept { return data_; }
    [[nodiscard]] const GeometryData &data() const noexcept { return data_; }
    [[nodiscard]] ocarina::Accel &accel() noexcept { return accel_; }
    [[nodiscard]] const ocarina::Accel &accel() const noexcept { return accel_; }
    [[nodiscard]] vector<uint> &accel_mesh_ids() noexcept { return accel_mesh_ids_; }
    [[nodiscard]] const vector<uint> &accel_mesh_ids() const noexcept { return accel_mesh_ids_; }
    [[nodiscard]] uint64_t build_count() const noexcept { return build_count_; }
    void record_build() noexcept { ++build_count_; }
};

class Geometry {
private:
    SP<GeometryGpuResource> gpu_resource_;
    bool process_mediums_{false};

public:
    Geometry();
    void init(Device &device);
    void bind_gpu_resource(SP<GeometryGpuResource> resource) noexcept;
    [[nodiscard]] bool has_gpu_resource() const noexcept { return gpu_resource_ != nullptr; }
    void set_process_mediums(bool process_mediums) noexcept { process_mediums_ = process_mediums; }
    [[nodiscard]] bool process_mediums() const noexcept { return process_mediums_; }
    [[nodiscard]] SP<GeometryGpuResource> gpu_resource() noexcept { return gpu_resource_; }
    [[nodiscard]] SP<const GeometryGpuResource> gpu_resource() const noexcept { return gpu_resource_; }

    [[nodiscard]] GeometryData *data() noexcept { return gpu_resource_ ? &gpu_resource_->data() : nullptr; }
    [[nodiscard]] const GeometryData *data() const noexcept { return gpu_resource_ ? &gpu_resource_->data() : nullptr; }
    [[nodiscard]] ocarina::Accel &accel() noexcept { return gpu_resource_->accel(); }
    [[nodiscard]] const ocarina::Accel &accel() const noexcept { return gpu_resource_->accel(); }
    [[nodiscard]] BindlessArray &bindless_array() noexcept { return gpu_resource_->bindless_array(); }
    [[nodiscard]] const BindlessArray &bindless_array() const noexcept { return gpu_resource_->bindless_array(); }

    void update_instances(const vector<SP<ShapeInstance>> &instances);
    void reset_device_buffer();
    void build_accel(Stream &stream);
    void update_accel(Stream &stream);
    void upload(Stream &stream);
    void upload_bindless_array(Stream &stream);
    void clear() noexcept;

    // DSL methods
    [[nodiscard]] TriangleHitVar trace_closest(const RayVar &ray) const noexcept;
    [[nodiscard]] Bool trace_occlusion(const RayVar &ray) const noexcept;
    [[nodiscard]] Bool occluded(const Interaction &it, const Float3 &pos, RayState *rs = nullptr) const noexcept;
    template<typename ...Args>
    [[nodiscard]] auto visibility(Args &&...args) const noexcept {
        Bool occ = occluded(OC_FORWARD(args)...);
        return cast<int>(!occ);
    }
    [[nodiscard]] SampledSpectrum Tr(Scene &scene, TSampler &sampler, const SampledWavelengths &swl, const RayState &ray_state) const noexcept;
    [[nodiscard]] LightEvalContext compute_light_eval_context(const Uint &inst_id,
                                                              const Uint &prim_id,
                                                              const Float2 &bary) const noexcept;
    [[nodiscard]] TriangleVar get_triangle(const Uint &buffer_index, const Uint &index) const noexcept;
    [[nodiscard]] array<Var<Vertex>, 3> get_vertices(const Uint &buffer_index,
                                                     const Var<Triangle> &tri) const noexcept;
    [[nodiscard]] Interaction compute_surface_interaction(const TriangleHitVar &hit, bool is_complete) const noexcept;
    [[nodiscard]] Interaction compute_surface_interaction(const TriangleHitVar &hit, const Float3 &view_pos) const noexcept {
        auto ret = compute_surface_interaction(hit, true);
        ret.update_wo(view_pos);
        return ret;
    }
    [[nodiscard]] Interaction compute_surface_interaction(const TriangleHitVar &hit, RayVar &ray, bool is_complete = true) const noexcept {
        auto ret = compute_surface_interaction(hit, is_complete);
        ret.wo = normalize(-ray->direction());
        ray.dir_max.w = length(ret.pos - ray->origin()) / length(ray->direction());
        return ret;
    }
    [[nodiscard]] Bool is_emissive(const Uint &inst_id) const noexcept {
        return data()->instances().read(inst_id).light_id != InvalidUI32;
    }
};

}// namespace vision
