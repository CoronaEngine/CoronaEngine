#include <corona/shared_data_hub.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>

#include "base/mgr/registries.h"
#include "base/scattering/material.h"
#include "rhi/context.h"
#include "vision/vision_material_adapter.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::uint64_t material_hash(const Corona::OpticsDevice& optics) {
    Corona::MeshDevice mesh;
    mesh.materialColor = {0.25f, 0.5f, 0.75f, 1.0f};

    auto material = Corona::Systems::Vision::create_vision_material(optics, mesh);
    require(static_cast<bool>(material), "create_vision_material returned null");
    return material->hash();
}

}  // namespace

int main() {
    // CTest runs from the plugin directory, not the standalone hotfix layout.
    ocarina::DynamicModule::clear_search_path();
    ocarina::RHIContext::instance().init(std::filesystem::current_path());

    Corona::OpticsDevice base;
    base.metallic = 0.2f;
    base.roughness = 0.4f;

    Corona::OpticsDevice extended = base;
    extended.subsurface = 0.35f;
    extended.anisotropic = 0.45f;
    extended.sheen = 0.55f;
    extended.sheenTint = 0.65f;
    extended.clearcoat = 0.75f;
    extended.clearcoatGloss = 0.25f;

    require(material_hash(base) != material_hash(extended),
            "extended Corona optics fields did not affect Vision material hash");

    Corona::OpticsDevice clamped = extended;
    clamped.subsurface = 5.0f;
    clamped.anisotropic = 5.0f;
    clamped.sheen = 5.0f;
    clamped.sheenTint = 5.0f;
    clamped.clearcoat = 5.0f;
    clamped.clearcoatGloss = -5.0f;

    require(material_hash(clamped) != 0,
            "clamped extended Corona optics fields produced an invalid material hash");

    auto import_material = [](const nlohmann::json& fields) {
        auto principled = std::make_shared<Corona::Materials::Principled>();
        principled->expressions = std::make_shared<const nlohmann::json>(fields);
        Corona::OpticsDevice optics;
        optics.principled = principled;
        return Corona::Systems::Vision::create_vision_material(optics, Corona::MeshDevice{});
    };
    auto material = import_material({{"alpha_mode", "MASK"}, {"alpha_cutoff", 0.25f},
                                     {"has_base_texture", false}});
    const auto& params = material->parameters();
    require(params.alpha_masked && params.alpha_cutoff == 0.25f, "alpha mask was lost during import");
    require(params.base_texture_specified && params.has_base_texture == false, "explicit base texture override was lost");

    vision::TRegistry<vision::Material> registry;
    auto register_fields = [&](const nlohmann::json& fields) {
        return registry.register_(import_material(fields));
    };
    auto opaque = register_fields(nlohmann::json::object());
    const auto& defaults = opaque->parameters();
    require(!defaults.alpha_masked && defaults.alpha_cutoff == 0.5f && !defaults.has_base_texture &&
            defaults.workflow.empty() && !defaults.glossiness_factor, "unexpected default material parameters");
    require(register_fields({{"alpha_mode", "OPAQUE"}, {"workflow", ""}}) == opaque,
            "explicit default mode and workflow should reuse the default material");
    for (const char* mode : {"OPAQUE", "BLEND"}) {
        auto unmasked = import_material({{"alpha_mode", mode}, {"alpha_cutoff", 0.75f}, {"has_base_texture", true}});
        const auto& alpha = unmasked->parameters();
        require(!alpha.alpha_masked && alpha.alpha_cutoff == defaults.alpha_cutoff &&
                    !alpha.base_texture_specified && !alpha.has_base_texture,
                "unmasked materials should ignore alpha mask overrides during import");
        require(registry.register_(unmasked) == opaque, "unused mask overrides must not change deduplication");
    }
    auto mask = register_fields({{"alpha_mode", "MASK"}});
    require(mask != opaque, "registry merged a masked material with an opaque one");
    require(register_fields({{"alpha_mode", "MASK"}, {"alpha_cutoff", 0.5f}}) == mask,
            "explicit default cutoff should reuse the same masked material");
    require(register_fields({{"alpha_mode", "MASK"}, {"alpha_cutoff", 0.75f}}) != mask,
            "registry merged materials with different alpha cutoffs");
    auto no_texture = register_fields({{"alpha_mode", "MASK"}, {"has_base_texture", false}});
    auto with_texture = register_fields({{"alpha_mode", "MASK"}, {"has_base_texture", true}});
    require(mask != no_texture && mask != with_texture && no_texture != with_texture,
            "registry lost the distinction between absent, false and true base texture overrides");

    auto metallic = register_fields({{"workflow", "metallic_roughness"}});
    auto specular = register_fields({{"workflow", "specular_glossiness"}});
    require(metallic != opaque && specular != opaque && metallic != specular,
            "registry merged distinct material workflows");
    auto zero_gloss = register_fields({{"workflow", "specular_glossiness"}, {"glossiness_factor", 0.0f}});
    auto full_gloss = register_fields({{"workflow", "specular_glossiness"}, {"glossiness_factor", 1.0f}});
    require(specular != zero_gloss && specular != full_gloss && zero_gloss != full_gloss,
            "registry lost the distinction between absent, zero and unit glossiness overrides");

    const auto create_vision_mask = [](const vision::DataWrap& texture, const vision::DataWrap& gloss) {
        vision::MaterialDesc desc;
        desc.init(vision::ParameterSet{vision::DataWrap{
            {"type", "principled_bsdf"}, {"param", {{"alpha_mode", "MASK"}, {"has_base_texture", texture},
                                                     {"workflow", "specular_glossiness"}, {"glossiness_factor", gloss}}}}});
        return vision::Material::create_root(desc);
    };
    const auto null_overrides = create_vision_mask(nullptr, nullptr);
    const auto& null_params = null_overrides->parameters();
    require(null_params.base_texture_specified && null_params.glossiness_factor_specified &&
            !null_params.has_base_texture && !null_params.glossiness_factor,
            "null Vision overrides should use the texture and glossiness inferred from the material graph");
    require(null_overrides->hash() == create_vision_mask(false, 0.f)->hash(),
            "null Vision overrides should retain the hash of explicit false and zero");

    auto previous = import_material({{"alpha_mode", "MASK"}, {"alpha_cutoff", 0.25f}, {"has_base_texture", false},
                                     {"workflow", "specular_glossiness"}, {"glossiness_factor", 0.7f}});
    const auto previous_hash = previous->hash();
    auto restored = import_material(nlohmann::json::object());
    restored->restore(previous.get());
    restored->reset_hash();
    require(restored->hash() == previous_hash, "restoring a Principled material changed its hash");
    const auto& restored_params = restored->parameters();
    require(restored_params.alpha_masked && restored_params.alpha_cutoff == 0.25f &&
            restored_params.has_base_texture == false && restored_params.workflow == "specular_glossiness" &&
            restored_params.glossiness_factor == 0.7f, "restoring a Principled material lost its parameters");

    return 0;
}
