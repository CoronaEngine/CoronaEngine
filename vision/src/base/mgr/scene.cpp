//
// Created by Z on 2025/11/29.
//

#include "scene.h"
#include "pipeline.h"
#include "switch_profile.h"

#include <optional>
#include <unordered_set>

namespace vision {

// ========== LightManager ==========

void LightManager::add_light(vision::TLight light) noexcept {
    lights_.push_back(std::move(light));
}

void LightManager::remove_light(vision::TLight light) noexcept {
    std::erase_if(lights_, [&](TLight elm) {
        return elm.get() == light.get();
    });
    if (env_light() == light.get()) {
        env_light_.impl() = nullptr;
    }
}

bool LightManager::render_UI(Widgets *widgets) noexcept {
    bool open = widgets->use_folding_header("lights", [&] {
        render_sub_UI(widgets);
        lights().render_UI(widgets);
    });
    return open;
}

void LightManager::init(const vector<vision::LightDesc> &light_descs) noexcept {
    for (const LightDesc &light_desc : light_descs) {
        TLight light = Light::create_root(light_desc);
        if (light->match(LightType::Area)) {
            TObject<IAreaLight> emission = dynamic_object_cast<IAreaLight>(light);
            emission->instance()->set_emission(emission);
        }
        if (light->match(LightType::Infinite)) {
            env_light_ = dynamic_object_cast<Environment>(light);
        }
        add_light(light);
    }
}

void LightManager::tidy_up() noexcept {
    std::sort(lights().begin(), lights().end(), [&](TLight a, TLight b) {
        return lights().topology_index(a.get()) < lights().topology_index(b.get());
    });
    for_each([&](TLight light, uint index) noexcept {
        light->set_index(index);
    });
}

void LightManager::update_runtime_object(const vision::IObjectConstructor *constructor) noexcept {
    for (int i = 0; i < lights_.size(); ++i) {
        TLight light = lights_[i];
        if (!constructor->match(light.get())) {
            continue;
        }
        auto new_light = TLight{constructor->construct_shared<Light>()};
        switch (new_light->type()) {
            case LightType::Area: {
                TObject<IAreaLight> new_al = dynamic_object_cast<IAreaLight>(new_light);
                TObject<IAreaLight> old_al = dynamic_object_cast<IAreaLight>(light);
                ShapeInstance *shape_instance = old_al->instance();
                shape_instance->set_emission(new_al);
                break;
            }
            case LightType::Infinite: {
                TObject<Environment> new_env = dynamic_object_cast<Environment>(new_light);
                TObject<Environment> old_env = dynamic_object_cast<Environment>(light);
                env_light_ = new_env;
                break;
            }
            default:
                break;
        }
        new_light->restore(light.get());
        lights_.replace(i, std::move(new_light));
    }
}

void LightManager::upload_immediately() noexcept {
    if (lights_.has_changed()) {
        lights_.upload_immediately();
    }
}

CommandBatch LightManager::upload(bool async) noexcept {
    CommandBatch ret;
    if (lights_.has_changed()) {
        ret << lights_.upload(async);
    }
    return ret;
}

// ========== Scene ==========

void Scene::init(const SceneDesc &scene_desc) {
    switch_profile::Scope profile{"Scene::init", "scene"};
    TIMER(init_scene);
    std::optional<Global::SceneGpuContextScope> scene_gpu_context;
    if (geometry().has_gpu_resource()) {
        scene_gpu_context.emplace(geometry().bindless_array(),
                                  geometry().gpu_resource()->device());
    }

    if (sensors_.empty()) {
        TSensor s;
        s.init(scene_desc.sensor_desc);
        sensors_.push_back(ocarina::move(s));
    }

    if (data_->initialized_) {
        return;
    }

    data_->light_manager_.init(scene_desc.light_descs);
    load_materials(scene_desc.material_descs);
    load_mediums(scene_desc.mediums_desc);
    if (cached_shape_initializer_) {
        switch_profile::Scope restore_profile{"Scene::restore_cached_shapes", "scene_reuse"};
        auto initializer = std::move(cached_shape_initializer_);
        initializer(*this);
    } else {
        load_shapes(scene_desc.shape_descs);
    }
    data_->initialized_ = true;
}

void Scene::prepare() noexcept {
    switch_profile::Scope profile{"scene.prepare", "scene"};
    std::optional<Global::SceneGpuContextScope> scene_gpu_context;
    if (geometry().has_gpu_resource()) {
        scene_gpu_context.emplace(geometry().bindless_array(),
                                  geometry().gpu_resource()->device());
    }
    material_registry().remove_unused_elements();
    geometry().set_process_mediums(process_mediums());
    register_instance_meshes();
    tidy_up();
    fill_instances();
    sensor()->prepare();
    sensor()->update_device_data();
    prepare_materials();
    OC_ASSERT(geometry().has_gpu_resource());
    medium_registry().prepare(geometry().bindless_array(),
                              geometry().gpu_resource()->device());
}

void Scene::update_runtime_object(const vision::IObjectConstructor *constructor) noexcept {
    std::tuple tp = {addressof(sensor().impl())};
    HotfixSystem::replace_objects(constructor, tp);
}

void Scene::tidy_up() noexcept {
    material_registry().tidy_up();
    data_->medium_registry_.tidy_up();
    if (auto *data = data_->geometry_.data()) {
        data->tidy_up_meshes();
    }
    light_manager().tidy_up();
    OC_INFO_FORMAT("This scene contains {} material types with {} material instances",
                   materials().topology_num(),
                   materials().all_instance_num());
}

SP<Material> Scene::obtain_black_body() noexcept {
    if (!data_->black_body_) {
        MaterialDesc md;
        md.sub_type = "black_body";
        data_->black_body_ = Material::create_root(md);
        materials().push_back(data_->black_body_);
    }
    return data_->black_body_;
}

void Scene::add_material(SP<vision::Material> material) noexcept {
    materials().push_back(ocarina::move(material));
}

void Scene::add_light(TLight light) noexcept {
    data_->light_manager_.add_light(ocarina::move(light));
}

TLight Scene::load_light(const LightDesc &desc) noexcept {
    auto light = Light::create_root(desc);
    data_->light_manager_.add_light(light);
    return light;
}

void Scene::load_materials(const vector<MaterialDesc> &material_descs) {
    switch_profile::Scope profile{"Scene::load_materials", "scene_materials"};
    for (const MaterialDesc &desc : material_descs) {
        auto material = Material::create_root(desc);
        add_material(ocarina::move(material));
    }
}

void Scene::add_shape(const SP<vision::ShapeGroup> &group, ShapeDesc desc) {
    data_->groups_.push_back(group);
    data_->aabb_.extend(group->aabb);
    group->for_each([&](SP<ShapeInstance> instance, uint i) {
        auto iter = materials().find_if([&](SP<Material> &material) {
            return material->name() == instance->material_name();
        });

        if (iter != materials().end() && !instance->has_material()) {
            instance->set_material(*iter);
        }

        if (desc.emission.valid()) {
            desc.emission.set_value("inst_id", data_->instances_.size());
            TObject<IAreaLight> light = dynamic_object_cast<IAreaLight>(load_light(desc.emission));
            instance->set_emission(light);
        }
        if (process_mediums()) {
            auto inside = mediums().find_if([&](SP<Medium> &medium) {
                return medium->name() == instance->inside_name();
            });
            if (inside != mediums().end()) {
                instance->set_inside(*inside);
            }
            auto outside = mediums().find_if([&](SP<Medium> &medium) {
                return medium->name() == instance->outside_name();
            });
            if (outside != mediums().end()) {
                instance->set_outside(*outside);
            }
        }
        data_->instances_.push_back(instance);
    });
}

void Scene::bind_geometry_gpu_resource(SP<GeometryGpuResource> resource) noexcept {
    if (!resource) {
        return;
    }
    data_->geometry_.bind_gpu_resource(ocarina::move(resource));
    data_->geometry_.set_process_mediums(process_mediums());
    register_instance_meshes();
    fill_instances();
}

void Scene::register_instance_meshes() noexcept {
    auto *data = data_->geometry_.data();
    if (!data) {
        return;
    }
    for (auto &instance : data_->instances_) {
        if (!instance || !instance->mesh()) {
            continue;
        }
        auto mesh = data->register_mesh(instance->mesh());
        instance->set_mesh(ocarina::move(mesh));
        instance->fill_mesh_id();
    }
    data->tidy_up_meshes();
}

void Scene::clear_shapes() noexcept {
    data_->instances_.clear();
    data_->groups_.clear();
    data_->aabb_ = {};
}

bool Scene::recompute_world_bounds() noexcept {
    Box3f bounds;
    for (const auto &group : groups()) {
        if (group && !group->aabb.empty()) {
            bounds.extend(group->aabb);
        }
    }
    const bool changed = any(bounds.lower != data_->aabb_.lower) ||
                         any(bounds.upper != data_->aabb_.upper);
    data_->aabb_ = bounds;
    return changed;
}

void Scene::remove_shape(uint group_index, bool defer_world_bounds) noexcept {
    if (group_index >= data_->groups_.size()) {
        return;
    }

    const auto &group = data_->groups_[group_index];
    if (group) {
        std::unordered_set<const ShapeInstance *> to_remove;
        group->for_each([&](SP<ShapeInstance> instance, uint) {
            if (instance) {
                to_remove.insert(instance.get());
            }
        });
        std::erase_if(data_->instances_, [&](const SP<ShapeInstance> &instance) {
            return instance && to_remove.contains(instance.get());
        });
    }

    data_->groups_.erase(data_->groups_.begin() + group_index);
    if (!defer_world_bounds) {
        recompute_world_bounds();
    }
}

void Scene::load_shapes(const vector<ShapeDesc> &descs) {
    switch_profile::Scope profile{"Scene::load_shapes", "scene_shapes"};
    for (const auto &desc : descs) {
        SP<ShapeGroup> group = Node::create_shared<ShapeGroup>(desc);
        add_shape(group, desc);
    }
}

void Scene::fill_instances() {
    // Membership/order can change independently of the source light registry.
    // Area-light CPU and generated GPU code both consume this instance index.
    std::unordered_set<IAreaLight*> live_emissions;
    for (uint i = 0; i < data_->instances_.size(); ++i) {
        auto& instance = data_->instances_[i];
        if (!instance->has_emission()) continue;
        auto light = instance->emission();
        if (!live_emissions.insert(light.get()).second) {
            auto desc = light->source_desc();
            desc.set_value("inst_id", i);
            light = dynamic_object_cast<IAreaLight>(load_light(desc)).impl();
            instance->set_emission(light);
            live_emissions.insert(light.get());
        }
        light->bind_instance(i);
    }
    auto& lights = light_manager().lights();
    for (auto it = lights.begin(); it != lights.end();) {
        if ((*it)->match(LightType::Area) &&
            !live_emissions.contains(static_cast<IAreaLight*>(it->get()))) it = lights.erase(it);
        else ++it;
    }
    light_manager().tidy_up();
    for (auto &instance : data_->instances_) {
        if (instance->has_material()) {
            const Material *material = instance->material().get();
            instance->update_material_id(materials().encode_id(material->index(), material));
        }
        if (instance->has_emission()) {
            const Light *emission = instance->emission().get();
            instance->update_light_id(data_->light_manager_.lights().encode_id(emission->index(), emission));
        }
        instance->fill_mesh_id();
        if (process_mediums()) {
            if (instance->has_inside()) {
                const Medium *inside = instance->inside().get();
                instance->update_inside_medium_id(mediums().encode_id(inside->index(), inside));
            }
            if (instance->has_outside()) {
                const Medium *outside = instance->outside().get();
                instance->update_outside_medium_id(mediums().encode_id(outside->index(), outside));
            }
        }
    }
}

void Scene::load_mediums(const MediumsDesc &md) {
    medium_registry().load_mediums(md);
    data_->geometry_.set_process_mediums(process_mediums());
}

void Scene::prepare_materials() {
    OC_ASSERT(geometry().has_gpu_resource());
    material_registry().prepare(geometry().bindless_array(),
                                geometry().gpu_resource()->device());
}

}// namespace vision
