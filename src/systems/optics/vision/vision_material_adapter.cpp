#include "vision_material_adapter.h"

#include <corona/shared_data_hub.h>

#include <algorithm>

#include "base/import/node_desc.h"
#include "base/scattering/material.h"

namespace Corona::Systems::Vision {

namespace {

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

::vision::DataWrap grayscale(float value) {
    const float v = clamp01(value);
    return ::vision::DataWrap::array({v, v, v});
}

}  // namespace

std::shared_ptr<::vision::Material> create_vision_material(
    const OpticsDevice& optics,
    const MeshDevice& mesh_dev) {

    // Build a MaterialDesc for principled_bsdf.
    // Pass an explicit type plus an empty param object (not a default-constructed null ParameterSet):
    // MaterialDesc::init -> GraphDesc::init -> NodeDesc::set_parameter asserts
    // is_object() and uses nlohmann value()/at(), which abort()/SIGABRT on a
    // null payload under JSON_NOEXCEPTION.
    auto material_json = ::vision::DataWrap::object();
    material_json["type"] = "principled_bsdf";
    material_json["param"] = ::vision::DataWrap::object();
    ::vision::MaterialDesc desc("principled_bsdf");
    desc.init(::vision::ParameterSet{material_json});

    // baseColor from MeshDevice::materialColor (RGBA, use RGB)
    float r = mesh_dev.materialColor[0];
    float g = mesh_dev.materialColor[1];
    float b = mesh_dev.materialColor[2];
    desc.set_value("color", ::vision::DataWrap::array({r, g, b}));

    // Corona OpticsDevice exposes a Disney-style principled parameter set.
    // Map only fields that have a clear Vision principled_bsdf slot; unsupported
    // fields intentionally fall back to Vision defaults instead of pretending to
    // be visually equivalent.
    desc.set_value("roughness", optics.roughness);
    desc.set_value("metallic", optics.metallic);
    desc.set_value("subsurface_weight", clamp01(optics.subsurface));
    desc.set_value("anisotropic", std::clamp(optics.anisotropic, -1.0f, 1.0f));
    desc.set_value("sheen_weight", clamp01(optics.sheen));
    desc.set_value("sheen_tint", grayscale(optics.sheenTint));
    desc.set_value("coat_weight", clamp01(optics.clearcoat));
    desc.set_value("coat_roughness", std::clamp(1.0f - optics.clearcoatGloss, 0.0001f, 1.0f));

    return ::vision::Material::create_root(desc);
}

}  // namespace Corona::Systems::Vision
