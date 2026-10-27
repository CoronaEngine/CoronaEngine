//
// Created by Zero on 06/09/2022.
//

#pragma once

#include "math/basic_types.h"
#include "core/stl.h"
#include "node_desc.h"
#include "base/using.h"

namespace vision {

struct MediumsDesc {
    vector<MediumDesc> mediums;
    bool process{false};
    string global;
};

struct SceneDesc {
public:
    SensorDesc sensor_desc;
    vector<MaterialDesc> material_descs;
    vector<ShapeDesc> shape_descs;
    MediumsDesc mediums_desc;
    vector<LightDesc> light_descs;

public:
    void init_material_descs(const DataWrap &materials) noexcept;
    void init_shape_descs(const DataWrap &shapes) noexcept;
    void init_medium_descs(const DataWrap &mediums) noexcept;
    void init_light_descs(const DataWrap &lights) noexcept;
};

struct RendererDesc {
public:
    SamplerDesc sampler_desc;
    SpectrumDesc spectrum_desc;
    LightSamplerDesc light_sampler_desc;
    IntegratorDesc integrator_desc;
    WarperDesc warper_desc;
    DenoiserDesc denoiser_desc;
    RenderSettingDesc render_setting;
};

struct ProjectDesc {
public:
    SceneDesc scene_desc;
    RendererDesc renderer_desc;
    OutputDesc output_desc;
    PipelineDesc pipeline_desc;
    fs::path scene_path;

public:
    ProjectDesc() = default;
};

}// namespace vision
