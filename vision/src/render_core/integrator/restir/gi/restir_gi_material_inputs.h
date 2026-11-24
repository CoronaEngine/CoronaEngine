#pragma once

#include "base/mgr/scene.h"
#include "rhi/device.h"
#include <algorithm>
#include <bit>

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

// Legacy exporter grammar: metallic is literal zero, spec_tint differs from
// literal one (or is image*factor25). Explicit workflow metadata takes priority.
// This recognizes the archived exporter representation, not a BRDF/lobe shape.
[[nodiscard]] inline bool standard_specular_glossiness_workflow(string_view explicit_workflow,
                                                               bool metallic_zero,
                                                               bool specular_nonunit) noexcept {
    if (!explicit_workflow.empty()) return explicit_workflow == "specular_glossiness";
    return metallic_zero && specular_nonunit;
}

[[nodiscard]] inline const ShaderNodeSlot *standard_gloss_weight(const Material &material) noexcept {
    if (!material.parameters().roughness_weight_inline_multiply) return nullptr;
    const auto *roughness = material.input_slot("roughness");
    if (!roughness || !roughness->valid() || roughness->node()->impl_type() != "mix") return nullptr;
    if (!standard_values_equal(standard_number_values(standard_child(roughness, "A")), 1.f) ||
        !standard_values_equal(standard_number_values(standard_child(roughness, "B")), 0.f)) return nullptr;
    const auto *weight = standard_child(roughness, "t");
    if (!weight || !weight->valid() || weight->node()->impl_type() != "math") return nullptr;
    return standard_child(weight, "lhs") ? weight : nullptr;
}
[[nodiscard]] inline const ShaderNodeSlot *standard_base_texture(const Material &material) noexcept {
    const auto *color = material.input_slot("color");
    if (!color || !color->valid()) return nullptr;
    if (color->node()->impl_type() == "image") return color;
    if (material.parameters().color_inline_multiply && color->node()->impl_type() == "math") {
        const auto *image = standard_child(color, "lhs");
        if (image && image->valid() && image->node()->impl_type() == "image") return image;
    }
    return nullptr;
}

}// namespace detail

class StandardMaterialInputs : private Toolkit {
    static constexpr uint SpecularGlossiness = 1u, SpecularTexture = 2u, GlossTexture = 4u, ConstantGloss = 8u;
    static constexpr uint AlphaTested = 16u;
    // Keep the stride used by shaders and buffers across GI hot reloads.
    static constexpr uint MaterialStride = 3u;
    Buffer<uint4> metadata_;
    bool has_alpha_tested_{false};

public:
    void prepare() noexcept {
        auto &materials = scene().materials();
        uint largest_id = 0u, sg_count = 0u, legacy_count = 0u;
        has_alpha_tested_ = false;
        materials.for_each_instance([&](const SP<Material> &material) {
            largest_id = std::max(largest_id, materials.encode_id(material->index(), material.get()));
        });
        vector<uint4> values((size_t(largest_id) + 1u) * MaterialStride, make_uint4(0u));
        materials.for_each_instance([&](const SP<Material> &material) {
            const auto &params = material->parameters();
            const auto *specular = material->input_slot("spec_tint");
            const auto specular_number = detail::standard_number_values(specular);
            const auto *specular_lhs = detail::standard_child(specular, "lhs");
            const auto specular_rhs = detail::standard_number_values(detail::standard_child(specular, "rhs"));
            const bool specular_multiply = params.spec_tint_inline_multiply && specular_lhs && specular_rhs &&
                specular->node()->impl_type() == "math";
            const bool nonunit = (specular_number && !detail::standard_values_equal(specular_number, 1.f)) ||
                                 (specular_multiply && !detail::standard_values_equal(specular_rhs, 1.f));
            const auto &workflow = params.workflow;
            OC_ERROR_IF(!workflow.empty() && workflow != "specular_glossiness" && workflow != "metallic_roughness",
                        "ReSTIR GI: unknown explicit material workflow {}", workflow);
            const bool sg = detail::standard_specular_glossiness_workflow(workflow,
                detail::standard_values_equal(detail::standard_number_values(material->input_slot("metallic")), 0.f), nonunit);
            uint flags = sg ? SpecularGlossiness : 0u;
            const auto *ior = material->input_slot("ior");
            const auto ior_values = detail::standard_number_values(ior);
            OC_ERROR_IF(ior && (!ior_values || ior_values->empty()), "ReSTIR GI material IoR must be a numeric input");
            const float material_ior = ior_values && !ior_values->empty() ? ior_values->front() : 1.5f;
            if (params.alpha_masked) {
                const auto *image = detail::standard_base_texture(*material);
                const bool base_texture = params.has_base_texture.value_or(image != nullptr);
                OC_ERROR_IF(base_texture && !image,
                            "ReSTIR GI alpha source needs an accessible original base texture");
                if (base_texture) { flags |= AlphaTested; has_alpha_tested_ = true; }
            }
            const uint quantized_cutoff = uint(std::clamp(params.alpha_cutoff, 0.f, 1.f) * 255.f + .5f);
            const float cutoff = float(quantized_cutoff) / 255.f;
            float gloss = 1.f;
            if (sg) {
                ++sg_count;
                legacy_count += uint(workflow.empty());
                const auto &encoded_factor = specular_multiply ? specular_rhs : specular_number;
                OC_ERROR_IF(!encoded_factor || encoded_factor->empty(), "ReSTIR GI unsupported specular/glossiness source expression");
                if (specular_multiply) flags |= SpecularTexture;
                if (const auto *weight = detail::standard_gloss_weight(*material)) {
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
                gloss = params.glossiness_factor.value_or(gloss);
            }
            const uint id = materials.encode_id(material->index(), material.get());
            values[size_t(id) * MaterialStride] = make_uint4(flags, std::bit_cast<uint>(material_ior), std::bit_cast<uint>(gloss), std::bit_cast<uint>(cutoff));
        });
        metadata_ = device().create_buffer<uint4>(values.size(), "ReSTIR GI source material metadata");
        metadata_.upload_immediately(values.data());
        OC_INFO_FORMAT("ReSTIR GI material inputs: materials={}, specular_glossiness={}, legacy_exporter={}, metadata_bytes={}",
                       materials.all_instance_num(), sg_count, legacy_count, values.size() * sizeof(uint4));
        OC_INFO_FORMAT("ReSTIR GI alpha metadata: tested={}", has_alpha_tested_);
    }

    [[nodiscard]] Float ior(const Uint &material_id, const SampledWavelengths &swl) const noexcept {
        (void)swl;
        return as<float>(metadata_.read(material_id * MaterialStride, false).y);
    }
    [[nodiscard]] bool has_alpha_tested() const noexcept { return has_alpha_tested_; }
    [[nodiscard]] Bool alpha_tested(const Uint &material_id) const noexcept {
        return (metadata_.read(material_id * MaterialStride, false).x & AlphaTested) != 0u;
    }
    [[nodiscard]] Float alpha_cutoff(const Uint &material_id) const noexcept {
        return as<float>(metadata_.read(material_id * MaterialStride, false).w);
    }
    [[nodiscard]] Float opacity(const Interaction &it, const SampledWavelengths &swl) const noexcept {
        Float alpha = 1.f;
        scene().materials().dispatch(it.material_id(), [&](const Material *source) {
            if (const auto *image = detail::standard_base_texture(*source))
                alpha = image->node()->evaluate(it, swl)->as_vec4().w;
        });
        return alpha;
    }
};

}// namespace vision::restir_gi
