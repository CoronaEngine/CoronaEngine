"""Execute the shared Li single-light sampling and visibility routes in a scalar shim."""
from _restir_cpu import ROOT, balanced, compile_run


def route(source, marker):
    point = source.index(marker)
    start = source.rindex('if (hc.restir_shadow_connections)', 0, point)
    end = balanced(source, start)
    assert source[end:].lstrip().startswith('else')
    end = balanced(source, end)
    return source[start:end]


def test_shared_nee_samples_and_traces_one_light():
    source = (ROOT / 'vision/src/base/integral/integrator.cpp').read_text(encoding='utf-8-sig')
    sample = route(source, 'light_sample = restir_shadow::sample_light(')
    shadow = route(source, 'shadow_ray.ray = restir_shadow::visibility_ray(')
    program = r"""
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
using Bool = bool;
struct Float3 { float x{}, y{}, z{}; };
Float3 operator-(Float3 a, Float3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Float3 normalize(Float3 v) {
    float d = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    return {v.x/d, v.y/d, v.z/d};
}
struct Ray { int kind{}; bool infinite{}; };
struct RayState { Ray ray{}; int medium{-1}; };
struct Interaction {
    Float3 pos{};
    RayState spawn_ray_state(Float3 d) const { return {{}, d.z > 0 ? 9 : 7}; }
};
struct LightSample { Float3 p_light{}; };
struct HitContext { bool restir_shadow_connections; };
struct Wavelengths {};
struct Sampler { float light_z; bool infinite; unsigned typed{}, ordinary{}; };
struct LightSampler {
    LightSample sample_wi(Interaction, Sampler &s, Wavelengths) const {
        ++s.ordinary;
        return {{0, 0, s.light_z}};
    }
};
struct Geometry {
    unsigned ordinary{}, typed{};
    bool occluded(Interaction it, Float3 target, RayState *state) {
        ++ordinary;
        *state = it.spawn_ray_state(target-it.pos);
        state->ray = {10, false};
        return false;
    }
    bool trace_occlusion(Ray ray) {
        ++typed;
        if (ray.kind != 30) throw std::runtime_error("GI traced the wrong shadow ray");
        return true;
    }
};
namespace restir_shadow {
LightSample sample_light(const LightSampler &, const Interaction &, Sampler &s, Wavelengths, Bool &inf) {
    ++s.typed;
    inf = s.infinite;
    return {{0, 0, s.light_z}};
}
Ray visibility_ray(Interaction, Float3, Bool infinite) { return {30, infinite}; }
}
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void run(bool gi, bool infinite, float light_z) {
    HitContext hc{gi};
    LightSampler lights;
    auto light_sampler = &lights;
    Interaction it;
    Sampler sampler{light_z, infinite};
    Wavelengths swl;
    LightSample light_sample;
    std::optional<Bool> light_sample_infinite;
    if (gi) light_sample_infinite.emplace(false);
    __SAMPLE__
    Geometry geometry;
    RayState shadow_ray;
    Bool occluded = false;
    __SHADOW__
    check(shadow_ray.medium == (light_z > 0 ? 9 : 7), "transmittance must receive the selected medium");
    if (gi) {
        check(sampler.typed == 1 && sampler.ordinary == 0 && geometry.typed == 1 && geometry.ordinary == 0,
              "GI must sample one typed light and trace it once");
        check(occluded && shadow_ray.ray.kind == 30 && shadow_ray.ray.infinite == infinite,
              "GI shadow must retain the sampled light's finite or infinite endpoint");
    } else {
        check(sampler.ordinary == 1 && sampler.typed == 0 && geometry.ordinary == 1 && geometry.typed == 0,
              "ordinary PT must keep its single light sample and visibility call");
        check(!occluded && shadow_ray.ray.kind == 10, "ordinary PT shadow ray must be retained");
    }
}
int main() {
    for (bool gi : {false, true})
        for (bool infinite : {false, true})
            for (float light_z : {-1000.f, 1000.f}) run(gi, infinite, light_z);
    std::cout << "PASS shared 1x1 ordinary and GI shadow routes\n";
}
"""
    compile_run(program.replace('__SAMPLE__', sample).replace('__SHADOW__', shadow), 'gi-shadow-routes')
