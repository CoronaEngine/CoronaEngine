//
// Created by ling.zhu on 2025/5/22.
//

#include "base/shader_graph/shader_graph.h"
#include "base/import/param_schema.h"
#include "GUI/widgets.h"

namespace vision {
using namespace ocarina;

class FresnelNode : public SlotsShaderNode {
private:
#define FRESNEL_SLOTS(X) \
    X(ior, 1.5f, Number, .set_range(0, 20), 1u, false) \
    X(normal, make_float3(0, 0, 0), Number, .set_range(0, 1), 3u, false)

#define FRESNEL_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    FRESNEL_SLOTS(FRESNEL_DECLARE_SLOT_)
#undef FRESNEL_DECLARE_SLOT_

public:
    FresnelNode() = default;
    explicit FresnelNode(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc) {}
    VS_MAKE_PLUGIN_NAME_FUNC_(fresnel)

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define FRESNEL_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            FRESNEL_SLOTS(FRESNEL_REGISTER_PARAM_)
#undef FRESNEL_REGISTER_PARAM_
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define FRESNEL_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        FRESNEL_SLOTS(FRESNEL_INIT_SLOT_)
#undef FRESNEL_INIT_SLOT_
    }

    [[nodiscard]] AttrEvalContext evaluate(const std::string &key, const vision::AttrEvalContext &ctx,
                                           const vision::SampledWavelengths &swl) const noexcept override {
        Float3 normal = normal_.evaluate(ctx, swl)->as_vec3();
        Float3 wo = ctx.wo();
        Float ior = ior_.evaluate(ctx, swl)->as_scalar();
        Float3 fr = make_float3(fresnel_dielectric(abs_dot(normal, wo), ior));
        return AttrEvalContext{float_array ::from_vec(fr)};
    }
};

class NormalMap : public SlotsShaderNode {
public:
    using SlotsShaderNode::SlotsShaderNode;
    VS_MAKE_PLUGIN_NAME_FUNC_(normal_map)

private:
#define NORMAL_MAP_SLOTS(X) \
    X(color, make_float3(0.5, 0.5, 1), Albedo, , 3u, false) \
    X(strength, 1.f, Number, .set_range(-10, 10), 1u, false)

#define NORMAL_MAP_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    NORMAL_MAP_SLOTS(NORMAL_MAP_DECLARE_SLOT_)
#undef NORMAL_MAP_DECLARE_SLOT_

public:
    NormalMap() = default;
    explicit NormalMap(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc) {}

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define NORMAL_MAP_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            NORMAL_MAP_SLOTS(NORMAL_MAP_REGISTER_PARAM_)
#undef NORMAL_MAP_REGISTER_PARAM_
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override     {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define NORMAL_MAP_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        NORMAL_MAP_SLOTS(NORMAL_MAP_INIT_SLOT_)
#undef NORMAL_MAP_INIT_SLOT_
    }

    [[nodiscard]] AttrEvalContext evaluate(const AttrEvalContext &ctx,
                                           const SampledWavelengths &swl) const noexcept override {
        Float3 normal = color_.evaluate(ctx, swl)->as_vec3() * 2.f - make_float3(1.f);
        Float s = strength_.evaluate(ctx, swl)->as_scalar();
        normal.x *= s;
        normal.y *= s;
        normal.z = ocarina::lerp(s, 1.f, normal.z);
        normal = ocarina::normalize(normal);
        return AttrEvalContext(float_array::from_vec(normal));
    }
};

class VectorMapping : public SlotsShaderNode {
public:
    using SlotsShaderNode::SlotsShaderNode;
    VS_MAKE_PLUGIN_NAME_FUNC_(vector_mapping)

private:
    std::string type_;

#define VECTOR_MAPPING_SLOTS(X) \
    X(vector, make_float3(0, 0, 0), Number, , 3u, false) \
    X(location, make_float3(0, 0, 0), Number, .set_range(-10, 10), 3u, false) \
    X(rotation, make_float3(0, 0, 0), Number, .set_range(0, 360), 3u, false) \
    X(scale, make_float3(1), Number, .set_range(-10, 10), 3u, false)

#define VECTOR_MAPPING_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    VECTOR_MAPPING_SLOTS(VECTOR_MAPPING_DECLARE_SLOT_)
#undef VECTOR_MAPPING_DECLARE_SLOT_

public:
    VectorMapping() = default;
    explicit VectorMapping(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc) {}

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define VECTOR_MAPPING_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            VECTOR_MAPPING_SLOTS(VECTOR_MAPPING_REGISTER_PARAM_)
#undef VECTOR_MAPPING_REGISTER_PARAM_
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define VECTOR_MAPPING_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        VECTOR_MAPPING_SLOTS(VECTOR_MAPPING_INIT_SLOT_)
#undef VECTOR_MAPPING_INIT_SLOT_
    }

    [[nodiscard]] AttrEvalContext evaluate(const std::string &key, const vision::AttrEvalContext &ctx,
                                           const vision::SampledWavelengths &swl) const noexcept override {
        return outline("VectorMapping", [&] {
            Float3 uvw = vector_.evaluate(ctx, swl).uvw();
            Float3 s = scale_.evaluate(ctx, swl)->as_vec3();
            Float3 angle = rotation_.evaluate(ctx, swl)->as_vec3();
            Float3 pos = location_.evaluate(ctx, swl)->as_vec3();
            Float4x4 rotation = rotation_y(angle.y) * rotation_z(angle.z) * rotation_x(angle.x);
            Float4x4 trs = translation(pos) * rotation * scale(s);
            uvw = transform_point(trs, uvw);
            AttrEvalContext ctx_processed{float_array::from_vec(uvw)};
            return ctx_processed;
        });
    }
};

class Gamma : public SlotsShaderNode {
public:
    VS_MAKE_PLUGIN_NAME_FUNC_(gamma)

private:
#define GAMMA_SLOTS(X) \
    X(color, make_float3(0.5f), Albedo, , 3u, false) \
    X(gamma, 2.2f, Number, , 1u, false)

#define GAMMA_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    GAMMA_SLOTS(GAMMA_DECLARE_SLOT_)
#undef GAMMA_DECLARE_SLOT_

public:
    Gamma() = default;
    explicit Gamma(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc) {}

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define GAMMA_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            GAMMA_SLOTS(GAMMA_REGISTER_PARAM_)
#undef GAMMA_REGISTER_PARAM_
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define GAMMA_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        GAMMA_SLOTS(GAMMA_INIT_SLOT_)
#undef GAMMA_INIT_SLOT_
    }

    AttrEvalContext evaluate(const AttrEvalContext &ctx,
                             const SampledWavelengths &swl) const noexcept override {
        Float gamma = gamma_.evaluate(ctx, swl)->as_scalar();
        Float3 color = color_.evaluate(ctx, swl)->as_vec3();
        color = ocarina::pow(color, make_float3(gamma));
        return {float_array::from_vec(color)};
    }
};

class CombineXYZ : public SlotsShaderNode {
public:
    VS_MAKE_PLUGIN_NAME_FUNC_(combine_xyz)

private:
#define COMBINE_XYZ_SLOTS(X) \
    X(x, 0.5f, Number, , 1u, false) \
    X(y, 0.5f, Number, , 1u, false) \
    X(z, 0.5f, Number, , 1u, false)

#define COMBINE_XYZ_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    COMBINE_XYZ_SLOTS(COMBINE_XYZ_DECLARE_SLOT_)
#undef COMBINE_XYZ_DECLARE_SLOT_

public:
    CombineXYZ() = default;
    explicit CombineXYZ(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc) {}

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define COMBINE_XYZ_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            COMBINE_XYZ_SLOTS(COMBINE_XYZ_REGISTER_PARAM_)
#undef COMBINE_XYZ_REGISTER_PARAM_
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define COMBINE_XYZ_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        COMBINE_XYZ_SLOTS(COMBINE_XYZ_INIT_SLOT_)
#undef COMBINE_XYZ_INIT_SLOT_
    }

    AttrEvalContext evaluate(const AttrEvalContext &ctx,
                             const SampledWavelengths &swl) const noexcept override {
        Float x = x_.evaluate(ctx, swl)->as_scalar();
        Float y = y_.evaluate(ctx, swl)->as_scalar();
        Float z = z_.evaluate(ctx, swl)->as_scalar();
        return {float_array::from_vec(make_float3(x, y, z))};
    }
};

class CombineColor : public SlotsShaderNode {
public:
    VS_MAKE_PLUGIN_NAME_FUNC_(combine_color)

private:
#define COMBINE_COLOR_SLOTS(X) \
    X(channel0, 0.5f, Number, , 1u, false) \
    X(channel1, 0.5f, Number, , 1u, false) \
    X(channel2, 0.5f, Number, , 1u, false)

#define COMBINE_COLOR_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    COMBINE_COLOR_SLOTS(COMBINE_COLOR_DECLARE_SLOT_)
#undef COMBINE_COLOR_DECLARE_SLOT_

public:
    CombineColor() = default;
    explicit CombineColor(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc) {}

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define COMBINE_COLOR_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            COMBINE_COLOR_SLOTS(COMBINE_COLOR_REGISTER_PARAM_)
#undef COMBINE_COLOR_REGISTER_PARAM_
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define COMBINE_COLOR_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        COMBINE_COLOR_SLOTS(COMBINE_COLOR_INIT_SLOT_)
#undef COMBINE_COLOR_INIT_SLOT_
    }

    AttrEvalContext evaluate(const AttrEvalContext &ctx,
                             const SampledWavelengths &swl) const noexcept override {
        Float c0 = channel0_.evaluate(ctx, swl)->as_scalar();
        Float c1 = channel1_.evaluate(ctx, swl)->as_scalar();
        Float c2 = channel2_.evaluate(ctx, swl)->as_scalar();
        return {float_array::from_vec(make_float3(c0, c1, c2))};
    }
};

class SeparateColor : public SlotsShaderNode {
public:
    VS_MAKE_PLUGIN_NAME_FUNC_(separate_color)
private:
#define SEPARATE_COLOR_SLOTS(X) \
    X(value, 0.5f, Number, , 1u, false)

#define SEPARATE_COLOR_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    SEPARATE_COLOR_SLOTS(SEPARATE_COLOR_DECLARE_SLOT_)
#undef SEPARATE_COLOR_DECLARE_SLOT_

public:
    SeparateColor() = default;
    explicit SeparateColor(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc) {}

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define SEPARATE_COLOR_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            SEPARATE_COLOR_SLOTS(SEPARATE_COLOR_REGISTER_PARAM_)
#undef SEPARATE_COLOR_REGISTER_PARAM_
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define SEPARATE_COLOR_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        SEPARATE_COLOR_SLOTS(SEPARATE_COLOR_INIT_SLOT_)
#undef SEPARATE_COLOR_INIT_SLOT_
    }

    AttrEvalContext evaluate(const std::string &key, const AttrEvalContext &ctx,
                             const SampledWavelengths &swl) const noexcept override {
        Float3 value = value_.evaluate(ctx, swl)->as_vec3();
        if (key == "Red") {
            return float_array(1, value.x);
        } else if (key == "Green") {
            return float_array(1, value.y);
        } else if (key == "Blue") {
            return float_array(1, value.z);
        }
        OC_ASSERT(false);
        return float_array::from_vec(value);
    }
};

class SeparateXYZ : public SlotsShaderNode {
public:
    VS_MAKE_PLUGIN_NAME_FUNC_(separate_xyz)
private:
#define SEPARATE_XYZ_SLOTS(X) \
    X(value, 0.5f, Number, , 1u, false)

#define SEPARATE_XYZ_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    SEPARATE_XYZ_SLOTS(SEPARATE_XYZ_DECLARE_SLOT_)
#undef SEPARATE_XYZ_DECLARE_SLOT_

public:
    SeparateXYZ() = default;
    explicit SeparateXYZ(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc) {}

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define SEPARATE_XYZ_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            SEPARATE_XYZ_SLOTS(SEPARATE_XYZ_REGISTER_PARAM_)
#undef SEPARATE_XYZ_REGISTER_PARAM_
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define SEPARATE_XYZ_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        SEPARATE_XYZ_SLOTS(SEPARATE_XYZ_INIT_SLOT_)
#undef SEPARATE_XYZ_INIT_SLOT_
    }

    AttrEvalContext evaluate(const std::string &key, const AttrEvalContext &ctx,
                             const SampledWavelengths &swl) const noexcept override {
        Float3 value = value_.evaluate(ctx, swl)->as_vec3();
        if (key == "X") {
            return float_array(1, value.x);
        } else if (key == "Y") {
            return float_array(1, value.y);
        } else if (key == "Z") {
            return float_array(1, value.z);
        }
        OC_ASSERT(false);
        return float_array::from_vec(value);
    }
};

class Clamp : public SlotsShaderNode {
public:
    VS_MAKE_PLUGIN_NAME_FUNC_(clamp)

private:
#define CLAMP_SLOTS(X) \
    X(min, 1.f, Number, .set_range(-100, 100), 1u, false) \
    X(max, 1.f, Number, .set_range(-100, 100), 1u, false) \
    X(value, 1.f, Number, .set_range(-100, 100), 1u, false)

#define CLAMP_DECLARE_SLOT_(name, val, tag, extra, dim, required) VS_MAKE_SLOT(name)
    CLAMP_SLOTS(CLAMP_DECLARE_SLOT_)
#undef CLAMP_DECLARE_SLOT_

public:
    Clamp() = default;
    explicit Clamp(const ShaderNodeDesc &desc)
        : SlotsShaderNode(desc) {}

    [[nodiscard]] static const ParamSchema &param_schema() noexcept {
        static const ParamSchema schema = [] {
            ParamSchema ret;
#define CLAMP_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
            CLAMP_SLOTS(CLAMP_REGISTER_PARAM_)
#undef CLAMP_REGISTER_PARAM_
            return ret;
        }();
        return schema;
    }

    void initialize_slots(const vision::ShaderNodeDesc &desc) noexcept override {
        const ParamSchema &schema = param_schema();
        validate_params(desc, schema);
#define CLAMP_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
        CLAMP_SLOTS(CLAMP_INIT_SLOT_)
#undef CLAMP_INIT_SLOT_
    }

    [[nodiscard]] AttrEvalContext evaluate(const AttrEvalContext &ctx,
                                           const SampledWavelengths &swl) const noexcept override {
        Float min = min_.evaluate(ctx, swl)->as_scalar();
        Float max = max_.evaluate(ctx, swl)->as_scalar();
        float_array value = value_.evaluate(ctx, swl).array;
        return {value.clamp(min, max)};
    }
};

}// namespace vision

VS_MAKE_CLASS_CREATOR_HOTFIX_FUNC(vision, FresnelNode, fresnel)
VS_MAKE_CLASS_CREATOR_HOTFIX_FUNC(vision, NormalMap, normal_map)
VS_MAKE_CLASS_CREATOR_HOTFIX_FUNC(vision, VectorMapping, vector_mapping)
VS_MAKE_CLASS_CREATOR_HOTFIX_FUNC(vision, CombineXYZ, combine_xyz)
VS_MAKE_CLASS_CREATOR_HOTFIX_FUNC(vision, CombineColor, combine_color)
VS_MAKE_CLASS_CREATOR_HOTFIX_FUNC(vision, SeparateColor, separate_color)
VS_MAKE_CLASS_CREATOR_HOTFIX_FUNC(vision, SeparateXYZ, separate_xyz)
VS_MAKE_CLASS_CREATOR_HOTFIX_FUNC(vision, Gamma, gamma)
VS_MAKE_CLASS_CREATOR_HOTFIX_FUNC(vision, Clamp, clamp)
