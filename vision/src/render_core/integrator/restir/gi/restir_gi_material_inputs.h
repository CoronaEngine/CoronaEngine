#pragma once

#include "base/mgr/scene.h"
#include "rhi/device.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace vision::restir_gi {
namespace detail {

// Read only actual Number nodes. Uniform image averages are not scalar inputs.
[[nodiscard]] inline optional<vector<float>> standard_number_values(const ShaderNodeSlot *slot) noexcept {
    if (!slot || !slot->valid() || slot->node()->impl_type() != "number") return {};
    return slot->average();
}
[[nodiscard]] inline bool standard_values_equal(const optional<vector<float>> &values, float value) noexcept {
    return values && !values->empty() && std::all_of(values->begin(), values->end(),
                                                    [&](float component) { return component == value; });
}
[[nodiscard]] inline const ShaderNodeSlot *standard_child(const ShaderNodeSlot *parent,
                                                         string_view name) noexcept {
    if (!parent || !parent->valid()) return nullptr;
    const auto *slots = dynamic_cast<const ShaderNodeSlotSet *>(parent->node());
    return slots ? slots->input_slot(name) : nullptr;
}

[[nodiscard]] inline bool standard_multiply_description(const DataWrap &slot) noexcept {
    if (!slot.is_object() || !slot.contains("node") || !slot["node"].is_object()) return false;
    const auto &node = slot["node"];
    if (node.value("type", "") != "math") return false;
    const auto &param = node["param"];
    return param.is_object() && param.value("op", "Multiply") == "Multiply";
}

// Legacy exporter grammar: metallic is literal zero, spec_tint differs from
// literal one (or is image*factor25). Explicit workflow metadata takes priority.
// This recognizes the archived exporter representation, not a BRDF/lobe shape.
[[nodiscard]] inline bool standard_specular_glossiness_workflow(string_view explicit_workflow,
                                                               bool metallic_zero,
                                                               bool specular_nonunit) noexcept {
    if (!explicit_workflow.empty()) return explicit_workflow == "specular_glossiness";
    return metallic_zero && specular_nonunit;
}

[[nodiscard]] inline const ShaderNodeSlot *standard_gloss_texture(const Material &material) noexcept {
    const auto *roughness = material.input_slot("roughness");
    if (!roughness || !roughness->valid() || roughness->node()->impl_type() != "mix") return nullptr;
    if (!standard_values_equal(standard_number_values(standard_child(roughness, "A")), 1.f) ||
        !standard_values_equal(standard_number_values(standard_child(roughness, "B")), 0.f)) return nullptr;
    const auto *weight = standard_child(roughness, "t");
    if (!weight || !weight->valid() || weight->node()->impl_type() != "math") return nullptr;
    const DataWrap desc = material.source_desc()["roughness"].data();
    if (!desc.is_object() || !desc.contains("node") || !desc["node"].is_object() ||
        !desc["node"].contains("param") || !desc["node"]["param"].contains("t") ||
        !standard_multiply_description(desc["node"]["param"]["t"])) return nullptr;
    return standard_child(weight, "lhs");
}
[[nodiscard]] inline const ShaderNodeSlot *standard_base_texture(const Material &material) noexcept {
    const auto *color = material.input_slot("color");
    if (!color || !color->valid()) return nullptr;
    if (color->node()->impl_type() == "image") return color;
    if (color->node()->impl_type() == "math" &&
        standard_multiply_description(material.source_desc()["color"].data())) {
        const auto *image = standard_child(color, "lhs");
        if (image && image->valid() && image->node()->impl_type() == "image") return image;
    }
    return nullptr;
}


struct StandardSourceMetadata {
    uint header_x, header_y;
    float3 sigma_a;
};

// MaterialData.hlsli defines the header layout. PathTracerBridgeDonut.hlsli
// maps nested priorities 0..14 to header values 1..15.
// The host evaluates the loadHomogeneousVolumeData absorption expression once.
[[nodiscard]] inline StandardSourceMetadata standard_source_metadata(
    const DataWrap &source, bool legacy_thin = false) noexcept {
    const bool has_source = source.is_object();
    const bool thin = has_source ? source.value("thin_surface", legacy_thin) : legacy_thin;
    const int priority = has_source ? source.value("nested_priority", 14) : 14;
    const int dominant = has_source ? source.value("psd_dominant_delta_lobe", -1) : -1;
    const bool psd_exclude = has_source ? source.value("psd_exclude", false) : false;
    const uint native_priority = 1u + uint(std::clamp(priority, 0, 14));
    const uint dominant_p1 = uint(std::clamp(dominant + 1, 0, 7));
    StandardSourceMetadata result{
        (native_priority << 16u) | (0xffu << 20u) | (thin ? 1u << 29u : 0u),
        (psd_exclude ? 1u << 17u : 0u) | (dominant_p1 << 18u), make_float3(0.f)};
    const float distance = has_source ? source.value("volume_attenuation_distance", std::numeric_limits<float>::max())
                                     : std::numeric_limits<float>::max();
    for (uint c = 0u; c < 3u; ++c) {
        const float color = has_source && source.contains("volume_attenuation_color")
            ? source["volume_attenuation_color"].at(c).get<float>() : 1.f;
        result.sigma_a[c] = -std::log(std::clamp(color, 1e-7f, 1.f)) / std::max(1e-30f, distance);
    }
    return result;
}

}// namespace detail

class StandardMaterialInputs : private Toolkit {
    static constexpr uint SpecularGlossiness = 1u, SpecularTexture = 2u, GlossTexture = 4u, ConstantGloss = 8u;
    static constexpr uint AlphaTested = 16u, VisibilityExcluded = 32u;
    // Three uint4 records per encoded material ID: flags/IoR/gloss/cutoff,
    // original specular factor RGB/native headerX, absorption RGB/headerY.
    // No closure cache; all original model features stay in the native header.
    Buffer<uint4> metadata_;
    bool has_alpha_tested_{false}, has_visibility_exclusions_{false};

public:
    void prepare() noexcept {
        auto &materials = scene().materials();
        uint largest_id = 0u, sg_count = 0u, legacy_count = 0u;
        has_alpha_tested_ = has_visibility_exclusions_ = false;
        materials.for_each_instance([&](const SP<Material> &material) {
            largest_id = std::max(largest_id, materials.encode_id(material->index(), material.get()));
        });
        vector<uint4> values((size_t(largest_id) + 1u) * 3u, make_uint4(0u));
        materials.for_each_instance([&](const SP<Material> &material) {
            const auto &desc = material->source_desc();
            const DataWrap native_source = desc["rtxpt_source"].data();
            const auto *specular = material->input_slot("spec_tint");
            const auto specular_number = detail::standard_number_values(specular);
            const auto *specular_lhs = detail::standard_child(specular, "lhs");
            const auto specular_rhs = detail::standard_number_values(detail::standard_child(specular, "rhs"));
            const bool specular_multiply = specular && specular->valid() &&
                specular->node()->impl_type() == "math" && specular_lhs && specular_rhs &&
                detail::standard_multiply_description(desc["spec_tint"].data());
            const bool nonunit = (specular_number && !detail::standard_values_equal(specular_number, 1.f)) ||
                                 (specular_multiply && !detail::standard_values_equal(specular_rhs, 1.f));
            const string workflow = native_source.is_object()
                ? native_source.value("workflow", desc["rtxpt_workflow"].as_string(""))
                : desc["rtxpt_workflow"].as_string("");
            OC_ERROR_IF(!workflow.empty() && workflow != "specular_glossiness" && workflow != "metallic_roughness",
                        "ReSTIR GI: unknown explicit material workflow {}", workflow);
            const bool sg = detail::standard_specular_glossiness_workflow(workflow,
                detail::standard_values_equal(detail::standard_number_values(material->input_slot("metallic")), 0.f), nonunit);
            uint flags = sg ? SpecularGlossiness : 0u;
            const auto ior_values = detail::standard_number_values(material->input_slot("ior"));
            const bool source_ior = native_source.is_object() && native_source.contains("ior");
            OC_ERROR_IF(!source_ior && material->input_slot("ior") && (!ior_values || ior_values->empty()), "ReSTIR GI material IoR must be a numeric input");
            const float material_ior = source_ior ? native_source["ior"].get<float>()
                : ior_values && !ior_values->empty() ? ior_values->front() : 1.5f;
            float cutoff = .5f;
            if (native_source.is_object()) {
                const bool mask = native_source.value("alpha_mode", string{"OPAQUE"}) == "MASK";
                const bool base_texture = native_source.value("has_base_texture", detail::standard_base_texture(*material) != nullptr);
                OC_ERROR_IF(mask && base_texture && !detail::standard_base_texture(*material),
                            "ReSTIR GI alpha source needs an accessible original base texture");
                if (mask && base_texture) { flags |= AlphaTested; has_alpha_tested_ = true; }
                if (native_source.value("exclude_from_nee", false)) { flags |= VisibilityExcluded; has_visibility_exclusions_ = true; }
                cutoff = native_source.value("alpha_cutoff", cutoff);
            }
            const uint quantized_cutoff = uint(std::clamp(cutoff, 0.f, 1.f) * 255.f + .5f);
            cutoff = float(quantized_cutoff) / 255.f;
            float gloss = 1.f;
            float3 factor = make_float3(0.f);
            if (sg) {
                ++sg_count;
                legacy_count += uint(workflow.empty());
                const auto &encoded_factor = specular_multiply ? specular_rhs : specular_number;
                OC_ERROR_IF(!encoded_factor || encoded_factor->empty(), "ReSTIR GI unsupported specular/glossiness source expression");
                for (uint c = 0u; c < 3u; ++c)
                    factor[c] = (*encoded_factor)[std::min<size_t>(c, encoded_factor->size() - 1u)] * .04f;
                if (native_source.is_object() && native_source.contains("specular_factor"))
                    for (uint c = 0u; c < 3u; ++c) factor[c] = native_source["specular_factor"].at(c).get<float>();
                if (specular_multiply) flags |= SpecularTexture;
                if (detail::standard_gloss_texture(*material)) {
                    const auto *weight = detail::standard_child(material->input_slot("roughness"), "t");
                    const auto scalar = detail::standard_number_values(detail::standard_child(weight, "rhs"));
                    OC_ERROR_IF(!scalar || scalar->empty(), "ReSTIR GI gloss texture needs a numeric factor");
                    flags |= GlossTexture;
                    gloss = scalar->front();
                } else {
                    const auto roughness = detail::standard_number_values(material->input_slot("roughness"));
                    OC_ERROR_IF(!roughness || roughness->empty(), "ReSTIR GI unsupported gloss source expression");
                    flags |= ConstantGloss;
                    gloss = 1.f - roughness->front();
                }
                if (desc.contains("rtxpt_glossiness")) gloss = desc["rtxpt_glossiness"].as_float(gloss);
                if (native_source.is_object() && native_source.contains("glossiness_factor"))
                    gloss = native_source["glossiness_factor"].get<float>();
            }
            const auto resolved = detail::standard_source_metadata(native_source, desc["thin_surface"].as_bool(false));
            const uint id = materials.encode_id(material->index(), material.get());
            values[size_t(id) * 3u] = make_uint4(flags, std::bit_cast<uint>(material_ior), std::bit_cast<uint>(gloss), std::bit_cast<uint>(cutoff));
            values[size_t(id) * 3u + 1u] = make_uint4(std::bit_cast<uint>(factor.x), std::bit_cast<uint>(factor.y), std::bit_cast<uint>(factor.z), resolved.header_x);
            values[size_t(id) * 3u + 2u] = make_uint4(std::bit_cast<uint>(resolved.sigma_a.x), std::bit_cast<uint>(resolved.sigma_a.y), std::bit_cast<uint>(resolved.sigma_a.z), resolved.header_y);
        });
        metadata_ = device().create_buffer<uint4>(values.size(), "ReSTIR GI source material metadata");
        metadata_.upload_immediately(values.data());
        OC_INFO_FORMAT("ReSTIR GI material inputs: materials={}, specular_glossiness={}, legacy_exporter={}, metadata_bytes={}",
                       materials.all_instance_num(), sg_count, legacy_count, values.size() * sizeof(uint4));
        OC_INFO_FORMAT("ReSTIR GI alpha metadata: tested={}, visibility_exclusions={}", has_alpha_tested_, has_visibility_exclusions_);
    }

    [[nodiscard]] Float ior(const Uint &material_id, const SampledWavelengths &swl) const noexcept {
        (void)swl;
        return as<float>(metadata_.read(material_id * 3u, false).y);
    }
    [[nodiscard]] bool has_alpha_tested() const noexcept { return has_alpha_tested_; }
    [[nodiscard]] bool has_visibility_exclusions() const noexcept { return has_visibility_exclusions_; }
    [[nodiscard]] Bool alpha_tested(const Uint &material_id) const noexcept {
        return (metadata_.read(material_id * 3u, false).x & AlphaTested) != 0u;
    }
    [[nodiscard]] Bool exclude_from_nee(const Uint &material_id) const noexcept {
        return (metadata_.read(material_id * 3u, false).x & VisibilityExcluded) != 0u;
    }
    [[nodiscard]] Float alpha_cutoff(const Uint &material_id) const noexcept {
        return as<float>(metadata_.read(material_id * 3u, false).w);
    }
    [[nodiscard]] Float opacity(const Interaction &it, const SampledWavelengths &swl) const noexcept {
        Float alpha = 1.f;
        scene().materials().dispatch(it.material_id(), [&](const Material *source) {
            if (const auto *image = detail::standard_base_texture(*source))
                alpha = image->node()->evaluate(it, swl)->as_vec4().w;
        });
        return alpha;
    }

    [[nodiscard]] Bool thin_surface(const Uint &material_id) const noexcept {
        return (metadata_.read(material_id * 3u + 1u, false).w & (1u << 29u)) != 0u;
    }
    // Encoded native priority 1..15; zero is reserved for empty interior slots.
    [[nodiscard]] Uint nested_priority(const Uint &material_id) const noexcept {
        return (metadata_.read(material_id * 3u + 1u, false).w >> 16u) & 15u;
    }
    [[nodiscard]] Bool psd_exclude(const Uint &material_id) const noexcept {
        return (metadata_.read(material_id * 3u + 2u, false).w & (1u << 17u)) != 0u;
    }
    [[nodiscard]] Uint psd_dominant_delta_lobe_p1(const Uint &material_id) const noexcept {
        return (metadata_.read(material_id * 3u + 2u, false).w >> 18u) & 15u;
    }
    [[nodiscard]] Float3 sigma_a(const Uint &material_id, const SampledWavelengths &swl) const noexcept {
        (void)swl;
        const Uint4 volume = metadata_.read(material_id * 3u + 2u, false);
        return as<float3>(Uint3{volume.xyz()});
    }

};

}// namespace vision::restir_gi
