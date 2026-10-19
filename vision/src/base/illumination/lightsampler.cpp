//
// Created by Zero on 22/10/2022.
//

#include "lightsampler.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/scene.h"
#include "base/shape.h"

namespace vision {

LightSampler::LightSampler(const LightSamplerDesc &desc)
    : Node(desc),
      env_separate_(desc["env_separate"].as_bool(false)),
      env_prob_(ocarina::clamp(desc["env_prob"].as_float(0.5f), 0.01f, 0.99f)) {
}

const PolymorphicGUI<TLight> &LightSampler::lights() const noexcept {
    return light_manager_->lights();
}

PolymorphicGUI<TLight> &LightSampler::lights() noexcept {
    return light_manager_->lights();
}

const Environment *LightSampler::env_light() const noexcept {
    return light_manager_->env_light();
}

Environment *LightSampler::env_light() noexcept {
    return light_manager_->env_light();
}

uint LightSampler::env_index() const noexcept {
    return light_manager_->env_index();
}

void LightSampler::add_light(TLight light) noexcept {
    light_manager_->add_light(ocarina::move(light));
}

Uint LightSampler::correct_index(Uint index) const noexcept {
    if (env_light() && env_separate_) {
        return ocarina::select(index < env_index(), index, index + 1u);
    }
    return index;
}

void LightSampler::update_runtime_object(const vision::IObjectConstructor *constructor) noexcept {
    light_manager_->update_runtime_object(constructor);
}

void LightSampler::prepare(BindlessArray &bindless_array, Device &device) noexcept {
    for_each([&](TLight light, uint index) noexcept {
        light->prepare();
    });
    lights().prepare(bindless_array, device);
}

void LightSampler::prepare() noexcept {
    if (auto *bindless = Global::instance().scene_bindless_array()) {
        if (auto *device = Global::instance().scene_device()) {
            prepare(*bindless, *device);
            return;
        }
    }
    auto rp = pipeline();
    OC_ASSERT(rp != nullptr);
    if (rp->scene().geometry().has_gpu_resource()) {
        prepare(rp->scene().geometry().bindless_array(),
                rp->scene().geometry().gpu_resource()->device());
        return;
    }
    prepare(rp->bindless_array(), rp->device());
}

void LightSampler::update_device_data() const noexcept {
    light_manager_->upload_immediately();
}

bool LightSampler::render_UI(Widgets *widgets) noexcept {
    bool open = widgets->use_folding_header(
        ocarina::format("{} light sampler", impl_type().data()),
        [&] {
            widgets->check_box("env_separate", &env_separate_);
            widgets->drag_float("env_prob", &env_prob_, 0.01, 0.01, 0.99);
            render_sub_UI(widgets);
            lights().render_UI(widgets);
        });
    return open;
}

Uint LightSampler::extract_light_index(const vision::Interaction &it) const noexcept {
    return combine_to_light_index(it.light_type_id(), it.light_inst_id());
}

Uint LightSampler::combine_to_light_index(const Uint &type_id, const Uint &inst_id) const noexcept {
    vector<uint> nums;
    Uint ret = 0u;
    switch (lights().mode()) {
        case ocarina::EInstance: {
            ret = inst_id;
            break;
        }
        case ocarina::ETopology: {
            nums.reserve(lights().topology_num());
            for (int i = 0; i < lights().topology_num(); ++i) {
                nums.push_back(static_cast<uint>(lights().instance_num(i)));
            }
            DynamicArray<uint> arr{nums};
            $for(i, type_id) {
                ret += arr[i];
            };
            ret += inst_id;
            break;
        }
        default:
            break;
    }
    return ret;
}

pair<Uint, Uint> LightSampler::extract_light_id(const Uint &index) const noexcept {
    Uint type_id = 0u;
    Uint inst_id = 0u;
    vector<uint> nums;
    nums.reserve(lights().topology_num());
    for (int i = 0; i < lights().topology_num(); ++i) {
        nums.push_back(static_cast<uint>(lights().instance_num(i)));
    }

    Uint accum = 0u;
    for (uint i = 0; i < nums.size(); ++i) {
        type_id = select(index >= accum, i, type_id);
        inst_id = select(index >= accum, index - accum, inst_id);
        accum += nums[i];
    }
    switch (lights().mode()) {
        case ocarina::EInstance:
            return {type_id, index};
        case ocarina::ETopology:
            return {type_id, inst_id};
        default:
            break;
    }
    OC_ASSERT(false);
    return {type_id, inst_id};
}

Float LightSampler::PMF(const LightSampleContext &lsc, const Uint &index) const noexcept {
    if (env_separate_) {
        if (env_prob() == 1) {
            return 1.f;
        } else if (env_prob() == 0) {
            return PMF_(lsc, index);
        }
        Float ret = 0;
        $if(index == env_index()) {
            ret = env_prob();
        }
        $else {
            ret = (1 - env_prob()) * PMF_(lsc, index);
        };
        return ret;
    }
    return PMF_(lsc, index);
}

SampledLight LightSampler::select_light(const LightSampleContext &lsc, Float u) const noexcept {
    if (env_separate_) {
        if (env_prob() == 1) {
            return SampledLight{0, 1.f};
        } else if (env_prob() == 0) {
            return select_light_(lsc, u);
        }
        SampledLight sampled_light;
        $if(u < env_prob()) {
            sampled_light = SampledLight{env_index(), env_prob()};
        }
        $else {
            u = remapping(u, env_prob(), 1);
            sampled_light = select_light_(lsc, u);
            sampled_light.PMF *= 1 - env_prob();
        };
        return sampled_light;
    }
    return select_light_(lsc, u);
}

LightSample LightSampler::sample_wi(const SampledLight &sampled_light, const LightSampleContext &lsc,
                                    const Float2 &u, const SampledWavelengths &swl) const noexcept {
    LightSample ls{swl.dimension()};
    auto [type_id, inst_id] = extract_light_id(sampled_light.light_index);
    dispatch_light(type_id, inst_id, [&](const Light *light) {
        ls = light->sample_wi(lsc, u, swl);
    });
    ls.eval.pdf *= sampled_light.PMF;
    return ls;
}

LightSample LightSampler::sample_wi(const LightSampleContext &lsc, TSampler &sampler,
                                    const SampledWavelengths &swl) const noexcept {
    Float u_light = sampler->next_1d();
    Float2 u_surface = sampler->next_2d();
    SampledLight sampled_light = select_light(lsc, u_light);
    return sample_wi(sampled_light, lsc, u_surface, swl);
}

LightSurfacePoint LightSampler::sample_only(const LightSampleContext &lsc, TSampler &sampler) const noexcept {
    LightSurfacePoint lsp;
    SampledLight sampled_light = select_light(lsc, sampler->next_1d());
    auto [type_id, inst_id] = extract_light_id(sampled_light.light_index);
    Float2 u = sampler->next_2d();
    dispatch_light(type_id, inst_id, [&](const Light *light) {
        lsp = light->sample_only(u);
    });
    lsp.light_index = sampled_light.light_index;
    return lsp;
}

LightSample LightSampler::evaluate_point(const LightSampleContext &lsc, const LightSurfacePoint &lsp,
                                         const SampledWavelengths &swl, LightEvalMode mode) const noexcept {
    auto [type_id, inst_id] = extract_light_id(lsp.light_index);
    Float pmf = PMF(lsc, lsp.light_index);
    LightSample ls{swl.dimension()};
    dispatch_light(type_id, inst_id, [&](const Light *light) {
        ls = light->evaluate_point(lsc, lsp, swl, mode);
    });
    ls.eval.pdf *= pmf;
    return ls;
}

Float LightSampler::PDF_point(const LightSampleContext &lsc, const LightSurfacePoint &lsp,
                              const Float &pdf_wi) const noexcept {
    auto [type_id, inst_id] = extract_light_id(lsp.light_index);
    Float ret = 0.f;
    dispatch_light(type_id, inst_id, [&](const Light *light) {
        ret = light->PDF_point(lsc, lsp, pdf_wi);
    });
    return ret;
}

LightEval LightSampler::evaluate_hit_wi(const LightSampleContext &p_ref, const Interaction &it,
                                        const SampledWavelengths &swl, LightEvalMode mode) const noexcept {
    LightEval ret = LightEval{swl.dimension()};
    Uint light_idx = extract_light_index(it);
    dispatch_light(it.light_id(), [&](const Light *light) {
        if (!light->match(LightType::Area)) {
            return;
        }
        LightEvalContext p_light{it};
        p_light.PDF_pos *= light->PMF(it.prim_id);
        ret = light->evaluate_wi(p_ref, p_light, swl, mode);
    });
    Float pmf = PMF(p_ref, light_idx);
    ret.pdf *= pmf;
    return ret;
}

LightEval LightSampler::evaluate_hit_point(const LightSampleContext &p_ref, const Interaction &it,
                                           const Float &pdf_wi,
                                           const SampledWavelengths &swl,
                                           Float *light_pdf_point, LightEvalMode mode) const noexcept {
    LightEval ret = LightEval{swl.dimension()};
    Uint light_idx = extract_light_index(it);
    dispatch_light(it.light_id(), [&](const Light *light) {
        if (!light->match(LightType::Area)) {
            return;
        }
        LightEvalContext p_light{it};
        ret = light->evaluate_point(p_ref, p_light, pdf_wi, swl, mode);
        if (light_pdf_point) {
            Float prim_pmf = light->PMF(it.prim_id);
            Float light_pmf = PMF(p_ref, light_idx);
            *light_pdf_point = p_light.PDF_pos * prim_pmf * light_pmf;
        }
    });
    return ret;
}

LightEval LightSampler::evaluate_miss_wi(const LightSampleContext &p_ref, const Float3 &wi,
                                         const SampledWavelengths &swl, LightEvalMode mode) const noexcept {
    LightEvalContext p_light{p_ref.pos + wi};
    LightEval ret{swl.dimension()};
    dispatch_environment([&](const Environment *env) {
        ret = env->evaluate_wi(p_ref, p_light, swl, mode);
    });
    Float pmf = PMF(p_ref, env_index());
    ret.pdf *= pmf;
    return ret;
}

LightEval LightSampler::evaluate_miss_point(const LightSampleContext &p_ref, const Float3 &wi,
                                            const Float &pdf_wi, const SampledWavelengths &swl,
                                            Float *light_pdf_point, LightEvalMode mode) const noexcept {
    LightEvalContext p_light{p_ref.pos + wi};
    LightEval ret{swl.dimension()};
    dispatch_environment([&](const Environment *env) {
        ret = env->evaluate_wi(p_ref, p_light, swl, mode);
    });
    Float light_pmf = PMF(p_ref, env_index());
    if (light_pdf_point) {
        *light_pdf_point = ret.pdf * light_pmf;
    }
    ret.pdf = pdf_wi;
    return ret;
}

void LightSampler::dispatch_environment(const std::function<void(const Environment *)> &func) const noexcept {
    auto lambda = [&](const Light *light) {
        auto env = dynamic_cast<const Environment *>(light);
        if (env) {
            func(env);
        }
    };
    if (lights().mode() == PolymorphicMode::EInstance) {
        lights().dispatch_instance(env_index(), lambda);
    } else {
        uint type_index = lights().topology_index(env_light());
        dispatch_light(type_index, 0, lambda);
    }
}

void LightSampler::dispatch_light(const Uint &id, const std::function<void(const Light *)> &func) const noexcept {
    lights().dispatch(id, func);
}

void LightSampler::dispatch_light(const Uint &type_id, const Uint &inst_id,
                                  const std::function<void(const Light *)> &func) const noexcept {
    lights().dispatch(type_id, inst_id, func);
}

}// namespace vision
