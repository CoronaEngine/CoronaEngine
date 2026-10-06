//
// Created by Z on 2025/11/29.
//

#pragma once

#include "base/import/project_desc.h"
#include "global.h"
#include "base/sensor/sensor.h"
#include "base/shape.h"
#include "base/illumination/lightsampler.h"
#include "base/scattering/material.h"
#include "base/scattering/medium.h"
#include "base/warper.h"
#include "registries.h"
#include "geometry.h"
#include "UI/GUI.h"
#include "hotfix/hotfix.h"
#include "base/using.h"
#include "image_pool.h"
#include <cstdint>
#include <functional>

namespace vision {

class LightManager : public GUI, public hotfix::Observer {
protected:
    PolymorphicGUI<TLight> lights_{};
    TEnvironment env_light_{};

public:
    LightManager() = default;
    OC_MAKE_MEMBER_GETTER(lights, &)
    VS_MAKE_GUI_STATUS_FUNC(GUI, lights_)
    bool render_UI(Widgets *widgets) noexcept override;
    void init(const vector<LightDesc> &light_descs) noexcept;
    void add_light(TLight light) noexcept;
    void remove_light(TLight light) noexcept;
    [[nodiscard]] const Environment *env_light() const noexcept { return env_light_.get(); }
    [[nodiscard]] Environment *env_light() noexcept { return env_light_.get(); }
    [[nodiscard]] uint env_index() const noexcept { return env_light()->index(); }
    void update_runtime_object(const vision::IObjectConstructor *constructor) noexcept override;
    void upload_immediately() noexcept;
    [[nodiscard]] CommandBatch upload(bool async = true) noexcept;
    void tidy_up() noexcept;

    template<typename Func>
    void for_each(Func &&func) noexcept {
        if constexpr (std::invocable<Func, TLight>) {
            for (TLight light : lights()) {
                func(light);
            }
        } else {
            uint i = 0u;
            for (TLight light : lights()) {
                func(light, i++);
            }
        }
    }

    template<typename Func>
    void for_each(Func &&func) const noexcept {
        if constexpr (std::invocable<Func, SP<const Light>>) {
            for (const TLight &light : lights()) {
                func(light);
            }
        } else {
            uint i = 0u;
            for (const TLight &light : lights()) {
                func(light, i++);
            }
        }
    }
};

class SceneData {
private:
    Geometry geometry_;
    ImagePool image_pool_;
    Box3f aabb_;
    SP<Material> black_body_{};
    LightManager light_manager_{};
    MaterialRegistry material_registry_{};
    MediumRegistry medium_registry_{};
    vector<SP<ShapeGroup>> groups_;
    vector<SP<ShapeInstance>> instances_;
    float min_radius_{};
    bool initialized_{false};

    friend class Scene;
};

class Scene : public GUI, public hotfix::Observer {
private:
    SP<SceneData> data_{make_shared<SceneData>()};
    PolymorphicGUI<TSensor> sensors_{};
    uint cur_sensor_index_{0};
    TSensor *sensor_override_{nullptr};
    std::function<void(Scene &)> cached_shape_initializer_;
    friend class Pipeline;

public:
    Scene() = default;
    explicit Scene(SP<SceneData> data) { bind_shared_data(ocarina::move(data)); }
    void bind_shared_data(SP<SceneData> data) noexcept {
        data_ = data ? ocarina::move(data) : make_shared<SceneData>();
        sensor_override_ = nullptr;
    }
    [[nodiscard]] SP<SceneData> shared_data() noexcept { return data_; }
    [[nodiscard]] SP<const SceneData> shared_data() const noexcept { return data_; }
    [[nodiscard]] std::uintptr_t shared_data_identity() const noexcept {
        return reinterpret_cast<std::uintptr_t>(data_.get());
    }
    [[nodiscard]] bool is_initialized() const noexcept { return data_->initialized_; }
    void init(const SceneDesc &scene_desc);
    // One-shot restoration after local materials/media exist, before Pipeline
    // consumes world bounds. The callback must create independent instances.
    void set_cached_shape_initializer(std::function<void(Scene &)> initializer) {
        cached_shape_initializer_ = std::move(initializer);
    }
    void set_min_radius(float min_radius) noexcept { data_->min_radius_ = min_radius; }
    void prepare() noexcept;
    void update_runtime_object(const vision::IObjectConstructor *constructor) noexcept override;

    void reset_status() noexcept override {
        GUI::reset_status();
        UI::reset_status(sensors_);
        UI::reset_status(data_->light_manager_);
        UI::reset_status(data_->material_registry_);
        UI::reset_status(data_->medium_registry_);
    }
    bool has_changed() noexcept override {
        bool ret = GUI::has_changed();
        ret |= UI::has_changed(sensors_);
        ret |= UI::has_changed(data_->light_manager_);
        ret |= UI::has_changed(data_->material_registry_);
        ret |= UI::has_changed(data_->medium_registry_);
        return ret;
    }
    bool render_UI(Widgets *widgets) noexcept override {
        widgets->use_window("scene data", [&] {
            UI::render_UI(sensors_, widgets);
            UI::render_UI(data_->light_manager_, widgets);
            UI::render_UI(data_->material_registry_, widgets);
            UI::render_UI(data_->medium_registry_, widgets);
        });
        return true;
    }

    void update_resolution(uint2 res) noexcept { sensor()->update_resolution(res); }

    // Sensor access
    [[nodiscard]] TSensor &sensor() noexcept {
        return sensor_override_ != nullptr ? *sensor_override_ : sensors_[cur_sensor_index_];
    }
    [[nodiscard]] const TSensor &sensor() const noexcept {
        return sensor_override_ != nullptr ? *sensor_override_ : sensors_[cur_sensor_index_];
    }
    void set_sensor_override(TSensor *sensor) noexcept { sensor_override_ = sensor; }
    void add_sensor(TSensor s) noexcept { sensors_.push_back(ocarina::move(s)); }
    void remove_sensor(const TSensor &s) noexcept {
        std::erase_if(sensors_, [&](const TSensor &elm) { return elm.get() == s.get(); });
    }
    [[nodiscard]] uint cur_sensor_index() const noexcept { return cur_sensor_index_; }
    void set_cur_sensor_index(uint index) noexcept { cur_sensor_index_ = index; }

    // Geometry
    [[nodiscard]] Geometry &geometry() noexcept { return data_->geometry_; }
    [[nodiscard]] const Geometry &geometry() const noexcept { return data_->geometry_; }
    [[nodiscard]] ImagePool &image_pool() noexcept { return data_->image_pool_; }
    [[nodiscard]] const ImagePool &image_pool() const noexcept { return data_->image_pool_; }
    void bind_geometry_gpu_resource(SP<GeometryGpuResource> resource) noexcept;
    void update_geometry_instances() noexcept { data_->geometry_.update_instances(data_->instances_); }

    // Shapes
    [[nodiscard]] vector<SP<ShapeGroup>> &groups() noexcept { return data_->groups_; }
    [[nodiscard]] const vector<SP<ShapeGroup>> &groups() const noexcept { return data_->groups_; }
    [[nodiscard]] vector<SP<ShapeInstance>> &instances() noexcept { return data_->instances_; }
    [[nodiscard]] const vector<SP<ShapeInstance>> &instances() const noexcept { return data_->instances_; }
    void load_shapes(const vector<ShapeDesc> &descs);
    void add_shape(const SP<ShapeGroup> &group, ShapeDesc desc = {});
    // Batch callers defer the union until Pipeline commits the geometry update.
    void remove_shape(uint group_index, bool defer_world_bounds = false) noexcept;
    void clear_shapes() noexcept;

    // Materials
    [[nodiscard]] const auto &material_registry() const noexcept { return data_->material_registry_; }
    [[nodiscard]] auto &material_registry() noexcept { return data_->material_registry_; }
    [[nodiscard]] const auto &materials() const noexcept { return material_registry().elements(); }
    [[nodiscard]] auto &materials() noexcept { return material_registry().elements(); }
    void add_material(SP<Material> material) noexcept;
    void load_materials(const vector<MaterialDesc> &material_descs);
    void prepare_materials();
    [[nodiscard]] SP<Material> obtain_black_body() noexcept;

    // Mediums
    [[nodiscard]] const auto &medium_registry() const noexcept { return data_->medium_registry_; }
    [[nodiscard]] auto &medium_registry() noexcept { return data_->medium_registry_; }
    [[nodiscard]] const auto &mediums() const noexcept { return data_->medium_registry_.elements(); }
    [[nodiscard]] auto &mediums() noexcept { return data_->medium_registry_.elements(); }
    [[nodiscard]] bool process_mediums() const noexcept { return data_->medium_registry_.process_mediums(); }
    void load_mediums(const MediumsDesc &desc);

    // Lights
    [[nodiscard]] LightManager &light_manager() noexcept { return data_->light_manager_; }
    [[nodiscard]] const LightManager &light_manager() const noexcept { return data_->light_manager_; }
    void add_light(TLight light) noexcept;
    TLight load_light(const LightDesc &desc) noexcept;

    // Instances
    void register_instance_meshes() noexcept;
    void fill_instances();
    [[nodiscard]] ShapeInstance *get_instance(uint id) noexcept { return data_->instances_[id].get(); }
    [[nodiscard]] const ShapeInstance *get_instance(uint id) const noexcept { return data_->instances_[id].get(); }

    // World bounds
    bool recompute_world_bounds() noexcept;
    [[nodiscard]] float3 world_center() const noexcept {
        return data_->aabb_.empty() ? make_float3(0.f) : data_->aabb_.center();
    }
    [[nodiscard]] float world_radius() const noexcept {
        return ocarina::max(data_->aabb_.empty() ? 0.f : data_->aabb_.radius(), data_->min_radius_);
    }
    [[nodiscard]] float world_diameter() const noexcept { return world_radius() * 2; }

    void tidy_up() noexcept;
};

}// namespace vision
