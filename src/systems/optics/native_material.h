#pragma once

#include "hardware.h"
#include <corona/shared_data_hub.h>
#include <corona/systems/geometry/geometry_system.h>
#include <algorithm>
#include <cmath>

namespace Corona::Systems::OpticsDetail {

inline Hardware::MaterialInfo pack_native_material(const GeometrySystem::MeshSlot& mesh,
    const OpticsDevice& optics, std::uint32_t base_descriptor,
    bool disable_textures, bool lighting_enabled) {
    Hardware::MaterialInfo result{};
    result.textureDescriptor = base_descriptor;
    result.lightingEnabled = lighting_enabled ? 1.0f : 0.0f;
    result.materialColor = {mesh.material_color[0], mesh.material_color[1], mesh.material_color[2], mesh.material_color[3]};
    if (!mesh.material && !optics.principled) {
        // Procedural/editor geometry has no imported material. Keep its existing
        // rendering contract; loaded models always take the data-driven branch.
        result.metallic = optics.metallic;
        result.roughness = optics.roughness;
        result.specular = optics.specular;
        result.subsurface = optics.subsurface;
        result.specularTint = optics.specularTint;
        result.anisotropic = optics.anisotropic;
        result.sheen = optics.sheen;
        result.sheenTint = optics.sheenTint;
        result.clearcoat = optics.clearcoat;
        result.clearcoatGloss = optics.clearcoatGloss;
        return result;
    }
    const MeshMaterialDevice empty_material{};
    const auto& material = mesh.material ? *mesh.material : empty_material;
    Resource::MaterialParameters value = optics.principled ? optics.principled->surface : material.source;
    if (optics.material_overrides && !optics.principled) {
        // Global scene values first; material-specific values always win,
        // independent of JSON row ordering.
        for (bool specific : {false, true})
            for (const auto& row : *optics.material_overrides)
                if ((row.index.has_value() || row.material != "*") == specific &&
                    row.matches(mesh.material_index, material.source.name)) row.apply(value);
    }
    const auto finite = [](float v, float fallback, float low, float high) {
        return std::isfinite(v) ? std::clamp(v, low, high) : fallback;
    };
    result.metallic = finite(value.metallic, 0.0f, 0.0f, 1.0f);
    result.roughness = finite(value.roughness, 0.5f, 0.0f, 1.0f);
    const float ior = finite(value.ior, 1.5f, 0.0f, 10000.0f);
    const float reflectance = (ior - 1.0f) / (ior + 1.0f);
    result.specular = reflectance * reflectance / 0.08f;
    // Imported MR uses the legacy sheen-tint word for specular weight. This
    // keeps the material table at 128 bytes; its sheen lobe remains disabled.
    result.sheenTint = finite(value.specular_factor, 1.0f, 0.0f, 1.0f);
    result.clearcoat = finite(value.clearcoat, 0.0f, 0.0f, 1.0f);
    result.clearcoatGloss = 1.0f - finite(value.clearcoat_roughness, 0.0f, 0.0f, 1.0f);
    result.normalScale = finite(value.normal_scale, 1.0f, -65504.0f, 65504.0f);
    result.occlusionStrength = finite(value.occlusion_strength, 1.0f, 0.0f, 1.0f);
    result.lightingEnabled = lighting_enabled && !value.unlit ? 1.0f : 0.0f;
    for (std::size_t c = 0; c < 4; ++c) result.materialColor[c] = finite(value.base_color[c], 1.0f, 0.0f, 1.0f);
    for (std::size_t c = 0; c < 3; ++c) {
        result.emissive[c] = finite(value.emissive[c], 0.0f, 0.0f, 65504.0f);
        result.specularColor[c] = finite(value.specular_color[c], 1.0f, 0.0f, 65504.0f);
    }
    result.flags = (material.source.metallic_channel & 3u) |
        ((material.source.roughness_channel & 3u) << 2u) | (value.double_sided ? 16u : 0u) | 32u;
    if (optics.principled) {
        // Source scene appearance replaces unrelated OBJ/PLY material slots.
        // Constant factors also provide the fallback when textures are disabled.
        // The native collector attaches the shared appearance program separately
        // from the model's own texture descriptors.
        result.textureDescriptor=0u;
        result.anisotropic=optics.principled->anisotropic;
        return result;
    }
    if (!disable_textures) {
        const auto descriptor = [&](MeshMaterialDevice::Slot slot) {
            const auto& image = material.textures[slot];
            return image ? image.store_descriptor() : 0u;
        };
        result.normalDescriptor = descriptor(MeshMaterialDevice::Normal);
        result.metallicDescriptor = descriptor(MeshMaterialDevice::Metallic);
        result.roughnessDescriptor = descriptor(MeshMaterialDevice::Roughness);
        result.occlusionDescriptor = descriptor(MeshMaterialDevice::Occlusion);
        result.emissiveDescriptor = descriptor(MeshMaterialDevice::Emissive);
        result.specularDescriptor = descriptor(MeshMaterialDevice::Specular);
        result.specularColorDescriptor = descriptor(MeshMaterialDevice::SpecularColor);
    }
    return result;
}

} // namespace Corona::Systems::OpticsDetail
