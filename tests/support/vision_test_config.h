#pragma once
#include "base/import/project_desc.h"

namespace Corona::Tests {
inline void configure_vision_test_project(vision::ProjectDesc& desc, const vision::DataWrap& configuration) {
    using namespace vision;
    const auto scene = configuration.value("scene", DataWrap::object());
    const auto render = configuration.value("render", DataWrap::object());
    desc.scene_desc.init_material_descs(scene.value("materials", DataWrap::array()));
    desc.scene_desc.init_medium_descs(scene.value("mediums", DataWrap::object()));
    desc.scene_desc.init_shape_descs(scene.value("shapes", DataWrap::array()));
    desc.scene_desc.init_light_descs(scene.value("lights", DataWrap::array()));
    desc.scene_desc.sensor_desc.init(scene.value("camera", DataWrap::object()));
    desc.scene_desc.sensor_desc.medium.name = desc.scene_desc.mediums_desc.global;
    desc.renderer_desc.integrator_desc.init(render.value("integrator", DataWrap::object()));
    desc.renderer_desc.light_sampler_desc.init(render.value("light_sampler", DataWrap::object()));
    desc.renderer_desc.spectrum_desc.init(render.value("spectrum", DataWrap::object()));
    desc.renderer_desc.sampler_desc.init(render.value("sampler", DataWrap::object()));
    desc.renderer_desc.warper_desc.init(render.value("warper", DataWrap::object()));
    desc.renderer_desc.render_setting.init(render.value("render_setting", DataWrap::object()));
    desc.renderer_desc.denoiser_desc.init(render.value("denoiser", DataWrap::object()));
    desc.output_desc.init(configuration.value("output", DataWrap::object()));
    desc.pipeline_desc.init(configuration.value("pipeline", DataWrap::object()));
}

}
