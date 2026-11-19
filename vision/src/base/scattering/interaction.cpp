//
// Created by Zero on 05/12/2022.
//

#include "interaction.h"
#include "base/scattering/medium.h"
#include "base/sampler.h"

namespace vision {
using namespace ocarina;

Float HenyeyGreenstein::f(const Float3 &wo, const Float3 &wi) const noexcept {
    return medium_phase_hg(dot(wo, wi), g_);
}

PhaseSample HenyeyGreenstein::sample(const Float3 &wo, TSampler &sampler) const noexcept {
    Float2 u = sampler->next_2d();
    Float cos_theta = 1.f - 2.f * u.x;
    $if(abs(g_) >= 1e-3f) {
        Float sqr_term = (1.f - sqr(g_)) / (1.f + g_ - 2.f * g_ * u.x);
        cos_theta = -(1.f + sqr(g_) - sqr(sqr_term)) / (2.f * g_);
    };

    Float sin_theta = safe_sqrt(1 - sqr(cos_theta));
    Float phi = 2 * Pi * u.y;
    Float3 v1, v2;
    coordinate_system(wo, v1, v2);
    Float3 wi = spherical_direction(sin_theta, cos_theta, phi, v1, v2, wo);
    Float f = medium_phase_hg(cos_theta, g_);
    PhaseSample phase_sample{swl_->dimension()};
    phase_sample.eval = {{swl_->dimension(), f}, f, 0};
    phase_sample.wi = wi;
    return phase_sample;
}

float Interaction::s_ray_offset_factor = 1.f;

float Interaction::ray_offset_factor() noexcept {
    float factor = s_ray_offset_factor;
    return factor;
}

void Interaction::set_ray_offset_factor(float value) noexcept {
    s_ray_offset_factor = value;
}

Interaction::Interaction(bool has_medium) {
    init_volumetric_param(has_medium);
}

Interaction::Interaction(Float3 pos, const Float3 &wo, bool has_medium)
    : pos(pos), wo(wo) {
    init_volumetric_param(has_medium);
}

void Interaction::init_phase(Float g, const SampledWavelengths &swl) {
    if (!phase_) {
        return;
    }
    (*phase_).init(g, swl);
}

void Interaction::init_volumetric_param(bool has_medium) noexcept {
    if (!has_medium) {
        return;
    }
    phase_.emplace(HenyeyGreenstein{});
    mi_.emplace(MediumInterface{});
}

Bool Interaction::has_phase() {
    if (phase_) {
        return phase().valid();
    }
    return false;
}

Float3 Interaction::local_wo() const noexcept {
    return shading.to_local(wo);
}

Float Interaction::correct_eta(const Float &ior) const noexcept {
    Float cos_theta = dot(wo, ng);
    return ocarina::select(cos_theta > 0.f, ior, rcp(ior));
}

float_array Interaction::correct_eta(const float_array &ior) const noexcept {
    Float cos_theta = dot(wo, ng);
    return ocarina::select(cos_theta > 0.f, ior, rcp(ior));
}

Uint Interaction::material_inst_id() const noexcept {
    return decode_id<D>(mat_id_).first;
}

Uint Interaction::material_type_id() const noexcept {
    return decode_id<D>(mat_id_).second;
}

Uint Interaction::light_inst_id() const noexcept {
    return decode_id<D>(light_id_).first;
}

Uint Interaction::light_type_id() const noexcept {
    return decode_id<D>(light_id_).second;
}

RayVar Interaction::spawn_ray(const Float3 &dir) const noexcept {
    return vision::spawn_ray(pos, ng, dir);
}

RayVar Interaction::spawn_ray(const Float3 &dir, const Float &t) const noexcept {
    return vision::spawn_ray(pos, ng, dir, t);
}

RayState Interaction::spawn_ray_state(const Float3 &dir) const noexcept {
    RayVar ray = vision::spawn_ray(pos, ng, dir);
    Uint medium{InvalidUI32};
    Float ior{1.f};
    if (mi_) {
        Uint incident = incident_medium_ ? *incident_medium_ : Uint{InvalidUI32};
        medium = medium_after_surface(incident, mi().inside, mi().outside, mi().declared, dot(ng, dir));
        if (incident_ior_) ior = *incident_ior_;
    }
    return {.ray = ray, .ior = ior, .medium = medium};
}

RayState Interaction::spawn_ray_state_to(const Float3 &p) const noexcept {
    RayVar ray = vision::spawn_ray_to(pos, ng, p);
    Uint medium{InvalidUI32};
    Float ior{1.f};
    if (mi_) {
        Uint incident = incident_medium_ ? *incident_medium_ : Uint{InvalidUI32};
        medium = medium_after_surface(incident, mi().inside, mi().outside, mi().declared, dot(ng, ray->direction()));
        if (incident_ior_) ior = *incident_ior_;
    }
    return {.ray = ray, .ior = ior, .medium = medium};
}

Float3 Interaction::robust_position() const noexcept {
    return custom_offset_ray_origin(pos, ng);
}

Float3 Interaction::robust_position(const Float3 &w) const noexcept {
    return custom_offset_ray_origin(pos, ng, w);
}

RayVar Interaction::spawn_ray_to(const Float3 &p) const noexcept {
    return vision::spawn_ray_to(pos, ng, p);
}

void Interaction::set_medium(const Uint &inside, const Uint &outside, const Bool &declared) {
    if (!mi_) {
        return;
    }
    (*mi_).inside = inside;
    (*mi_).outside = outside;
    (*mi_).declared = declared;
}

void Interaction::set_incident_state(const RayState &state) noexcept {
    if (!mi_) return;
    incident_medium_.emplace(state.medium);
    incident_ior_.emplace(state.ior);
}

void AttrEvalContext::init(const vision::Interaction &it) noexcept {
    auto fill_array = []<size_t N>(float_array &arr, uint &cursor,
                                   const Var<Vector<float, N>> &vec) {
        for (int i = 0; i < N; ++i) {
            arr[cursor++] = vec[i];
        }
    };
    uint cursor = 0;
    fill_array(array, cursor, it.uv);
    pos_.emplace(it.pos);
    ng_.emplace(it.ng);
    ng_local_.emplace(it.ng_local);
    ns_.emplace(it.shading.normal());
    tangent_.emplace(it.shading.dp_du());
    wo_.emplace(it.wo);
}

Float3 AttrEvalContext::uvw() const noexcept {
    uint dim = array.size();
    switch(dim) {
        case 2u:
            return make_float3(uv(), 0.f);
        default:
            return array.as_vec3();
    }
}
Float2 AttrEvalContext::uv() const noexcept { return array.as_vec2(); }
Float3 AttrEvalContext::pos() const noexcept { return *pos_; }
Float3 AttrEvalContext::ng() const noexcept { return *ng_; }
Float3 AttrEvalContext::ng_local() const noexcept { return *ng_local_; }
Float3 AttrEvalContext::ns() const noexcept { return *ns_; }
Float3 AttrEvalContext::tangent() const noexcept { return *tangent_; }
Float3 AttrEvalContext::wo() const noexcept { return *wo_; }

}// namespace vision
