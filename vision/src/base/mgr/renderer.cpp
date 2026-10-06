//
// Created by Z on 2025/12/13.
//

#include "renderer.h"
#include "switch_profile.h"
#include "scene.h"
#include "base/scattering/interaction.h"

namespace vision {

void Renderer::pre_init(const RendererDesc &renderer_desc) {
    warper_desc_ = renderer_desc.warper_desc;
    render_setting_ = renderer_desc.render_setting;
    spectrum_.init(renderer_desc.spectrum_desc);
}

void Renderer::init(const RendererDesc &renderer_desc, Scene &scene) {
    scene.materials().set_mode(render_setting_.polymorphic_mode);
    scene.mediums().set_mode(render_setting_.polymorphic_mode);
    OC_INFO_FORMAT("polymorphic mode is {}", static_cast<int>(scene.materials().mode()));
    light_sampler_.init(renderer_desc.light_sampler_desc);
    light_sampler_->set_light_manager(&scene.light_manager());
    light_sampler_->set_mode(render_setting_.polymorphic_mode);
    integrator_.init(renderer_desc.integrator_desc);
    sampler_.init(renderer_desc.sampler_desc);
    Interaction::set_ray_offset_factor(renderer_desc.render_setting.ray_offset_factor);
}

void Renderer::prepare(Scene &scene) noexcept {
    switch_profile::Scope profile{"renderer.prepare", "resources"};
    {
        switch_profile::Scope sampler_profile{"sampler.prepare", "resources"};
        sampler_->prepare();
    }
    integrator_->prepare();
    prepare_lights(scene);
    spectrum()->set_scene_has_dispersive_materials(scene.material_registry().has_dispersive());
    spectrum()->prepare();
}

void Renderer::tidy_up() noexcept {

}

void Renderer::prepare_lights(Scene &scene) noexcept {
    switch_profile::Scope profile{"lights.prepare", "lights"};
    OC_ASSERT(scene.geometry().has_gpu_resource());
    light_sampler_->prepare(scene.geometry().bindless_array(),
                            scene.geometry().gpu_resource()->device());
    auto &light = light_sampler_->lights();
    OC_INFO_FORMAT("This scene contains {} light types with {} light instances",
                   light.topology_num(),
                   light.all_instance_num());
}

void Renderer::update_runtime_object(const vision::IObjectConstructor *constructor) noexcept {
    std::tuple tp = {addressof(light_sampler_.impl()),
                     addressof(integrator_.impl()),
                     addressof(sampler_.impl()),
                     addressof(spectrum_.impl())};
    HotfixSystem::replace_objects(constructor, tp);
}

void Renderer::upload_data(Scene &scene) noexcept {
    light_sampler_->update_device_data();
    scene.medium_registry().upload_device_data();
    scene.material_registry().upload_device_data();
}

}// namespace vision
