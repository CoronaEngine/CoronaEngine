//
// Created by Zero on 18/03/2023.
//

#include "light.h"
#include "base/import/param_schema.h"
#include "base/mgr/scene.h"

namespace vision {
using namespace ocarina;

Light::Light(const LightDesc &desc, LightType light_type)
    : Node(desc), type_(light_type),
      scale_(desc["scale"].as_float(1.f)) {}

float3 Light::average() const noexcept {
    auto avg = color_.average();
    return make_float3(avg[0], avg[1], avg[2]) * scale_.hv();
}

bool Light::match_L(LightEvalMode mode) noexcept {
    return static_cast<bool>(mode & LightEvalMode::L);
}

bool Light::match_PDF(LightEvalMode mode) noexcept {
    return static_cast<bool>(mode & LightEvalMode::PDF);
}

LightEval Light::_evaluate_point(const LightSampleContext &p_ref,
                                 const LightEvalContext &p_light,
                                 const SampledWavelengths &swl,
                                 LightEvalMode mode) const noexcept {
    LightEval ret{swl.dimension()};
    if (match_L(mode)) {
        ret.L = Li(p_ref, p_light, swl);
    }
    if (match_PDF(mode)) {
        ret.pdf = PDF_point(p_ref, p_light);
    }
    return ret;
}

TObject<Light, LightDesc> Light::create_root(const LightDesc &desc) noexcept {
    auto light = TObject<Light, LightDesc>(desc);
    light->initialize_root(desc);
    return light;
}

uint64_t Light::compute_topology_hash() const noexcept {
    return hash64(color_.topology_hash(), strength_.topology_hash());
}

void Light::initialize_root(const Desc &desc) noexcept {
    source_desc_ = desc;
    initialize_root_graph();
    initialize_slots(desc);
}

void Light::initialize_attached(const SP<ShaderGraph> &graph, const Desc &desc) noexcept {
    source_desc_ = desc;
    initialize_attached_graph(graph);
    initialize_slots(desc);
}

void Light::initialize_(const vision::NodeDesc &node_desc) noexcept {
    VS_CAST_DESC
    set_graph_blueprint(desc.node_map);
}

void Light::initialize_slots(const vision::Light::Desc &desc) noexcept {
    ensure_graph_ready();
    static const ParamSchema schema = [] {
        ParamSchema ret;
#define LIGHT_BASE_REGISTER_PARAM_(name, val, tag, extra, dim, required) ret.add_slot(#name, tag, dim, required);
        LIGHT_BASE_SLOTS(LIGHT_BASE_REGISTER_PARAM_)
#undef LIGHT_BASE_REGISTER_PARAM_
        return ret;
    }();
    validate_params(desc, schema);
#define LIGHT_BASE_INIT_SLOT_(name, val, tag, extra, dim, required) VS_INIT_SLOT(name, val, tag) extra;
    LIGHT_BASE_SLOTS(LIGHT_BASE_INIT_SLOT_)
#undef LIGHT_BASE_INIT_SLOT_
    float factor = color_->normalize();
    scale_ = scale_.hv() * factor;
}

void Light::validate_params(const Desc &desc, const ParamSchema &schema) const noexcept {
    string context = "light";
    if (!desc.name.empty()) {
        context += " '" + desc.name + "'";
    }
    string type = string(impl_type());
    if (!type.empty()) {
        context += " [" + type + "]";
    }
    schema.validate(desc, context);
}

Float Light::scale() const noexcept {
    return *scale_ * *switch_;
}

LightBound Light::bound() const noexcept {
    return {};
}

Float Light::G(const LightSampleContext &p_ref, const LightEvalContext &p_light) noexcept {
    Float3 w = p_ref.pos - p_light.pos;
    return dot(normalize(w), p_light.ng) / length_squared(w);
}

SampledSpectrum Light::Li(const LightSampleContext &p_ref,
                          const LightEvalContext &p_light,
                          const SampledWavelengths &swl) const noexcept {
    return Le(p_ref, p_light, swl);
}

Float Light::PMF(const Uint &prim_id) const noexcept {
    return 0.f;
}

Float Light::PDF_point(const LightSampleContext &p_ref,
                       const LightEvalContext &p_light) const noexcept {
    return PDF_wi(p_ref, p_light);
}

Float Light::PDF_point(const LightSampleContext &p_ref,
                       const LightEvalContext &p_light,
                       const Float &pdf_wi) const noexcept {
    Float ret = vision::PDF_point(pdf_wi, p_light.ng, p_ref.pos - p_light.pos);
    return select(ocarina::isinf(ret), 0.f, ret);
}

Float Light::PDF_point(const LightSampleContext &p_ref,
                       LightSurfacePoint lsp,
                       const Float &pdf_wi) const noexcept {
    return PDF_point(p_ref, compute_light_eval_context(p_ref, lsp), pdf_wi);
}

LightType Light::type() const noexcept {
    return type_;
}

bool Light::match(LightType t) const noexcept {
    return static_cast<bool>(t & type_);
}

bool Light::is(LightType t) const noexcept {
    return t == type_;
}

LightEval Light::evaluate_wi(const LightSampleContext &p_ref,
                             const LightEvalContext &p_light,
                             const SampledWavelengths &swl,
                             LightEvalMode mode) const noexcept {
    LightEval ret{swl.dimension()};
    if (match_L(mode)) {
        ret.L = Le(p_ref, p_light, swl);
    }
    if (match_PDF(mode)) {
        ret.pdf = PDF_wi(p_ref, p_light);
    }
    return ret;
}

LightSurfacePoint Light::sample_only(Float2 u) const noexcept {
    LightSurfacePoint ret{};
    ret.bary = u;
    ret.prim_id = 0;
    return ret;
}

LightEval Light::evaluate_point(const LightSampleContext &p_ref,
                                const LightEvalContext &p_light,
                                const Float &pdf_wi,
                                const SampledWavelengths &swl,
                                LightEvalMode mode) const noexcept {
    LightEval ret{swl.dimension()};
    if (match_L(mode)) {
        ret.L = Li(p_ref, p_light, swl);
    }
    if (match_PDF(mode)) {
        ret.pdf = PDF_point(p_ref, p_light, pdf_wi);
    }
    return ret;
}

bool Light::render_UI(Widgets *widgets) noexcept {
    LightManager& light_manager = scene().light_manager();

    string label = format("{} {} light: {}, topology index:{}", index_,
        impl_type().data(),name_.c_str(), light_manager.lights().topology_index(this));
    bool open = widgets->use_tree(label, [&] {
        changed_ |= widgets->check_box("turn on", reinterpret_cast<bool *>(addressof(switch_.hv())));
        changed_ |= widgets->drag_float("scale", &scale_.hv(), 0.05, 0, 1000);
        color_.render_UI(widgets);
        strength_.render_UI(widgets);
        render_sub_UI(widgets);
    });
    return open;
}

ShapeInstance *IAreaLight::instance() const noexcept {
    return scene().get_instance(inst_idx_.hv());
}

void IAreaLight::restore(vision::RuntimeObject *old_obj) noexcept {
    Light::restore(old_obj);
    VS_HOTFIX_MOVE_ATTRS(inst_idx_)
    instance()->set_emission_name(name());
    instance()->set_emission(std::static_pointer_cast<IAreaLight>(shared_from_this()));
}

float4x4 IPointLight::resolve_o2w(const LightDesc &desc) noexcept {
    if (desc.contains("o2w")) {
        return desc.o2w.mat;
    }
    float3 position = desc["position"].as_float3(make_float3(0.f));
    float3 direction = normalize(desc["direction"].as_float3(make_float3(0.f, 0.f, 1.f)));
    float3 up = make_float3(0.f, 1.f, 0.f);
    if (ocarina::abs(dot(direction, up)) > .999f) {
        up = make_float3(1.f, 0.f, 0.f);
    }
    return look_at<H>(position, position + direction, up);
}

float3 &IPointLight::host_o2w_position(float4x4 &o2w) noexcept {
    return reinterpret_cast<float3 &>(o2w[3]);
}

Float IPointLight::PDF_wi(const LightSampleContext &p_ref,
                          const LightEvalContext &p_light) const noexcept {
    return -1.f;
}

Float3 IPointLight::direction(const LightSampleContext &p_ref) const noexcept {
    return normalize(p_ref.pos - position());
}

LightSample IPointLight::sample_wi(const LightSampleContext &p_ref, Float2 u,
                                   const SampledWavelengths &swl) const noexcept {
    LightSample ret{swl.dimension()};
    LightEvalContext p_light;
    p_light.ng = direction(p_ref);
    p_light.pos = position();
    ret.eval = evaluate_wi(p_ref, p_light, swl, LightEvalMode::All);
    ret.p_light = p_light.pos;
    return ret;
}

void IPointLight::render_sub_UI(Widgets *widgets) noexcept {
    changed_ |= widgets->drag_float3("position", &host_position(),
                                     0.02, 0, 0);
}

LightSample IPointLight::evaluate_point(const LightSampleContext &p_ref,
                                        LightSurfacePoint lsp,
                                        const SampledWavelengths &swl,
                                        LightEvalMode mode) const noexcept {
    LightSample ls{swl.dimension()};
    LightEvalContext lec{position()};
    ls.eval = _evaluate_point(p_ref, lec, swl, mode);
    ls.p_light = position();
    return ls;
}

LightEvalContext IPointLight::compute_light_eval_context(const LightSampleContext &p_ref,
                                                         LightSurfacePoint lsp) const noexcept {
    return LightEvalContext{position(), normalize(p_ref.pos - position())};
}

Float2 Environment::convert_to_bary(const Float3 &world_dir) const noexcept {
    return make_float2(0.f);
}

}// namespace vision
