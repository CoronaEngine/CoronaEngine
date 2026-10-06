//
// Created by Zero on 09/09/2022.
//

#pragma once

#include "base/scattering/interaction.h"
#include "base/node.h"
#include "base/shader_graph/shader_graph.h"
#include "base/scattering/sample.h"
#include "base/encoded_object.h"
#include "base/color/spectrum.h"
#include "math/warp.h"
#include "base/using.h"

namespace vision {

class ParamSchema;
struct LightBound {
    ocarina::array<float, 3> axis_{};
    float theta_o{};
    float theta_e{};
    [[nodiscard]] auto axis() const noexcept {
        return make_float3(axis_[0], axis_[1], axis_[2]);
    }
};
}// namespace vision

// clang-format off
OC_STRUCT(vision,LightBound, axis_, theta_o, theta_e){
    [[nodiscard]] auto axis() const noexcept {
        return axis_.as_vec3();
    }
};
// clang-format on

namespace vision {
enum class LightEvalMode {
    L = 1 << 0,
    PDF = 1 << 1,
    All = L | PDF
};
}// namespace vision

OC_MAKE_ENUM_BIT_OPS(vision::LightEvalMode, |, &, <<, >>)

namespace vision {
// LightType Definition
enum class LightType {
    Unset = 0,
    Area = 1 << 0,
    DeltaPosition = 1 << 1,
    DeltaDirection = 1 << 2,
    Infinite = 1 << 3
};
}// namespace vision

OC_MAKE_ENUM_BIT_OPS(vision::LightType, |, &, <<, >>)

#undef VS_MAKE_LIGHT_TYPE_OP

namespace vision {

struct LightSurfacePoint {
    Uint light_index{InvalidUI32};
    Uint prim_id;
    Float2 bary;
};

class Light : public Node, public GUIRenderable, public Encodable, public ShaderGraph {
public:
    using Desc = LightDesc;

protected:
    const LightType type_{LightType::Unset};
    LightDesc source_desc_;
    EncodedData<float> scale_{1.f};
    EncodedData<uint> switch_{true};

#define LIGHT_BASE_SLOTS(X) \
    X(color, make_float3(0.5f), Albedo, , 3u, false) \
    X(strength, make_float3(0.5f), Number, , 3u, false)

#define LIGHT_BASE_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    LIGHT_BASE_SLOTS(LIGHT_BASE_DECLARE_SLOT_)
#undef LIGHT_BASE_DECLARE_SLOT_

    uint index_{InvalidUI32};

protected:
    [[nodiscard]] float3 average() const noexcept;

    [[nodiscard]] static bool match_L(LightEvalMode mode) noexcept;

    [[nodiscard]] static bool match_PDF(LightEvalMode mode) noexcept;

    [[nodiscard]] virtual LightEval _evaluate_point(const LightSampleContext &p_ref,
                                                    const LightEvalContext &p_light,
                                                    const SampledWavelengths &swl,
                                                    LightEvalMode mode) const noexcept;

    void validate_params(const Desc &desc, const ParamSchema &schema) const noexcept;

public:
    Light(LightType type) noexcept : type_(type) {}
    explicit Light(const LightDesc &desc, LightType light_type);
    [[nodiscard]] const LightDesc& source_desc() const noexcept { return source_desc_; }
    [[nodiscard]] static TObject<Light, LightDesc> create_root(const LightDesc &desc) noexcept;
    VS_HOTFIX_MAKE_RESTORE(Node, scale_, switch_, color_, strength_, index_, source_desc_)
    OC_ENCODABLE_FUNC(Encodable, scale_, color_, strength_, switch_)
    void initialize_root(const Desc &desc) noexcept;
    void initialize_attached(const SP<ShaderGraph> &graph, const Desc &desc) noexcept;
    virtual void initialize_slots(const Desc &desc) noexcept;
    void initialize_(const vision::NodeDesc &node_desc) noexcept override;
    [[nodiscard]] uint64_t compute_topology_hash() const noexcept override;
    OC_MAKE_MEMBER_GETTER_SETTER(index, )
    VS_MAKE_GUI_STATUS_FUNC(Node, color_, strength_)
    bool render_UI(Widgets *widgets) noexcept override;
    [[nodiscard]] virtual LightBound bound() const noexcept;
    [[nodiscard]] virtual float3 power() const noexcept = 0;
    [[nodiscard]] Float scale() const noexcept;
    [[nodiscard]] virtual SampledSpectrum Le(const LightSampleContext &p_ref,
                                             const LightEvalContext &p_light,
                                             const SampledWavelengths &swl) const noexcept = 0;
    [[nodiscard]] static Float G(const LightSampleContext &p_ref,
                                 const LightEvalContext &p_light) noexcept;
    [[nodiscard]] virtual SampledSpectrum Li(const LightSampleContext &p_ref,
                                             const LightEvalContext &p_light,
                                             const SampledWavelengths &swl) const noexcept;
    [[nodiscard]] virtual Float PMF(const Uint &prim_id) const noexcept;
    [[nodiscard]] virtual Float PDF_wi(const LightSampleContext &p_ref,
                                       const LightEvalContext &p_light) const noexcept = 0;
    [[nodiscard]] virtual Float PDF_point(const LightSampleContext &p_ref,
                                          const LightEvalContext &p_light) const noexcept;
    [[nodiscard]] virtual Float PDF_point(const LightSampleContext &p_ref,
                                          const LightEvalContext &p_light,
                                          const Float &pdf_wi) const noexcept;
    [[nodiscard]] Float PDF_point(const LightSampleContext &p_ref,
                                  LightSurfacePoint lsp,
                                  const Float &pdf_wi) const noexcept;
    [[nodiscard]] virtual LightSample sample_wi(const LightSampleContext &p_ref, Float2 u,
                                                const SampledWavelengths &swl) const noexcept = 0;
    [[nodiscard]] LightType type() const noexcept;
    [[nodiscard]] bool match(LightType t) const noexcept;
    [[nodiscard]] bool is(LightType t) const noexcept;
    [[nodiscard]] virtual LightEval evaluate_wi(const LightSampleContext &p_ref,
                                                const LightEvalContext &p_light,
                                                const SampledWavelengths &swl,
                                                LightEvalMode mode) const noexcept;

    [[nodiscard]] virtual LightSample evaluate_point(const LightSampleContext &p_ref, LightSurfacePoint lsp,
                                                     const SampledWavelengths &swl, LightEvalMode mode) const noexcept = 0;

    [[nodiscard]] virtual LightEvalContext compute_light_eval_context(const LightSampleContext &p_ref,
                                                                      LightSurfacePoint lsp) const noexcept = 0;

    [[nodiscard]] virtual LightSurfacePoint sample_only(Float2 u) const noexcept;

    [[nodiscard]] LightEval evaluate_point(const LightSampleContext &p_ref,
                                           const LightEvalContext &p_light,
                                           const Float &pdf_wi,
                                           const SampledWavelengths &swl,
                                           LightEvalMode mode) const noexcept;
};

class Mesh;
class ShapeInstance;

class IAreaLight : public Light {
protected:
    EncodedData<uint> inst_idx_{InvalidUI32};

public:
    IAreaLight() : Light(LightType::Area) {}
    explicit IAreaLight(const LightDesc &desc)
        : Light(desc, LightType::Area),
          inst_idx_(desc["inst_id"].as_uint(InvalidUI32)) {}
    OC_ENCODABLE_FUNC(Light, inst_idx_)
    void restore(vision::RuntimeObject *old_obj) noexcept override;
    template<typename T>
    void add_emission_reference(T shape_instance) noexcept {}
    [[nodiscard]] ShapeInstance *instance() const noexcept;
    void bind_instance(uint index) noexcept { inst_idx_ = index; }
};

class IPointLight : public Light {
public:
    IPointLight() : Light(LightType::DeltaPosition) {}
    explicit IPointLight(const LightDesc &desc) : Light(desc, LightType::DeltaPosition) {}

protected:
    // Shared transform helpers for point-like lights that use an o2w matrix as
    // their single source of truth for position and forward direction. Spot and
    // projector both use local +Z as the light's forward axis, and older spot
    // scenes can still be upgraded from explicit position/direction fields.
    [[nodiscard]] static float4x4 resolve_o2w(const LightDesc &desc) noexcept;

    template<typename Mat>
    [[nodiscard]] static auto o2w_position(const Mat &o2w) noexcept {
        return o2w[3].xyz();
    }

    [[nodiscard]] static float3 &host_o2w_position(float4x4 &o2w) noexcept;

    template<typename Mat>
    [[nodiscard]] static auto o2w_direction(const Mat &o2w) noexcept {
        return transform_vector<D>(o2w, make_float3(0, 0, 1));
    }

public:
    void render_sub_UI(Widgets *widgets) noexcept override;
    [[nodiscard]] Float PDF_wi(const LightSampleContext &p_ref,
                               const LightEvalContext &p_light) const noexcept override;
    [[nodiscard]] virtual Float3 direction(const LightSampleContext &p_ref) const noexcept;
    [[nodiscard]] virtual Float3 position() const noexcept = 0;
    [[nodiscard]] virtual float3 &host_position() noexcept = 0;
    [[nodiscard]] LightSample sample_wi(const LightSampleContext &p_ref, Float2 u,
                                        const SampledWavelengths &swl) const noexcept override;
    [[nodiscard]] LightSample evaluate_point(const LightSampleContext &p_ref, LightSurfacePoint lsp,
                                             const SampledWavelengths &swl, LightEvalMode mode) const noexcept override;
    [[nodiscard]] LightEvalContext compute_light_eval_context(const LightSampleContext &p_ref,
                                                              LightSurfacePoint lsp) const noexcept override;
};

class Environment : public Light {
public:
    using Light::Light;
    [[nodiscard]] virtual Float2 convert_to_bary(const Float3 &world_dir) const noexcept;
};

using TLight = TObject<Light>;
using TEnvironment = TObject<Environment>;

}// namespace vision
