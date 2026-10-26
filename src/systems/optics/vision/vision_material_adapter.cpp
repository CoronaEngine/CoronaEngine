#include "vision_material_adapter.h"

#include <corona/shared_data_hub.h>

#include <algorithm>

#include "base/import/node_desc.h"
#include "base/scattering/material.h"
#include "base/illumination/light.h"
#include "base/shape.h"
#include "vision_texture_adapter.h"

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
    // Imported surfaces do not request Vision's default subsurface component.
    desc.set_value("subsurface_weight",0.f);

    if (optics.principled && optics.principled->expressions) {
        const auto& p=*optics.principled->expressions;
        // Reflection sidedness is independent of the area light's sidedness.
        desc.set_value("two_sided", p.value("two_sided", false));
        const std::pair<const char*,const char*> slots[]={{"base_color","color"},{"metallic","metallic"},
            {"roughness","roughness"},{"ior","ior"},{"specular","spec_tint"},
            {"transmission","transmission_weight"},{"anisotropic","anisotropic"},
            {"coat","coat_weight"},{"coat_roughness","coat_roughness"}};
        for (auto [source,target]:slots) if (p.contains(source))
            desc.set_value(target,vision_json(appearance_slot(p[source],
                std::string_view(source)=="base_color" || std::string_view(source)=="specular" ? 3:1)));
        if (p.contains("normal")) {
            // Material's optional normal input references a named graph node;
            // unlike regular Principled slots it cannot take an inline node.
            ::vision::ShaderNodeDesc normal;
            normal.init(::vision::ParameterSet{vision_json({{"type","converter"},
                {"construct_name","normal_map"},
                {"param",{{"color",appearance_slot(p["normal"])},{"strength",1.f}}}})});
            desc.node_map.emplace("corona_normal",std::move(normal));
            desc.set_value("normal",::vision::DataWrap{{"channels","xyz"},{"node","corona_normal"}});
        }
        return ::vision::Material::create_root(desc);
    }

    if (mesh_dev.material) {
        auto value=static_cast<Resource::MaterialParameters>(mesh_dev.material->source);
        if (optics.material_overrides) for (bool specific:{false,true})
            for (const auto& row:*optics.material_overrides)
                if ((row.index.has_value() || row.material!="*")==specific &&
                    row.matches(mesh_dev.materialIndex,mesh_dev.material->source.name)) row.apply(value);
        desc.set_value("color",::vision::DataWrap::array({value.base_color[0],value.base_color[1],value.base_color[2]}));
        desc.set_value("roughness",value.roughness);
        desc.set_value("metallic",value.metallic);
        desc.set_value("ior",std::max(1.0001f,value.ior));
        desc.set_value("spec_tint",::vision::DataWrap::array({value.specular_factor,value.specular_factor,value.specular_factor}));
        desc.set_value("coat_weight",value.clearcoat);
        desc.set_value("coat_roughness",value.clearcoat_roughness);
        return ::vision::Material::create_root(desc);
    }

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

void attach_vision_emission(::vision::Scene& scene, ::vision::ShapeInstance& instance,
    const OpticsDevice& optics, const MeshDevice& mesh, unsigned instance_index) {
    AppearanceJson color=AppearanceJson::array({0.,0.,0.});
    bool two_sided=false;
    if (optics.principled) {
        color=optics.principled->expressions->value("emission",color);
        two_sided=optics.principled->emission_two_sided;
    } else if (mesh.material) {
        auto value=static_cast<Resource::MaterialParameters>(mesh.material->source);
        if (optics.material_overrides) for (const auto& row:*optics.material_overrides)
            if (row.matches(mesh.materialIndex,mesh.material->source.name)) row.apply(value);
        color=value.emissive;
    }
    if (color.is_number() && color.get<float>()<=0.f) return;
    if (color.is_array() && std::none_of(color.begin(),color.end(),[](const auto& v){return v.template get<float>()>0.f;})) return;
    ::vision::LightDesc desc;
    desc.init(::vision::ParameterSet{::vision::DataWrap{{"type","area"},{"param",::vision::DataWrap::object()}}});
    desc.set_value("inst_id",instance_index);
    desc.set_value("color",vision_json(appearance_slot(color)));
    desc.set_value("scale",1.f);
    desc.set_value("two_sided",two_sided);
    auto light=::vision::dynamic_object_cast<::vision::IAreaLight>(scene.load_light(desc));
    instance.set_emission(light);
}

}  // namespace Corona::Systems::Vision
