"""Numerical fixtures execute the production media helpers with scalar DSL types.

These catch wrong physical distance, non-finite vacuum sampling, slab crossing,
phase normalization and lost incident medium/IOR. They do not render or mock a
GPU backend; the complete shader is validated by the engine build separately.
"""
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'vision/src').is_dir())
TEMP = ROOT / '.workspace/tmp/volumetric-caustic-media-20261118/numerical'


def _function(source, signature):
    assert signature in source, f'Production helper missing: {signature}'
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


SHIM = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
using Float = float;
using Uint = uint32_t;
using uint = uint32_t;
using Bool = bool;
constexpr Uint InvalidUI32 = UINT32_MAX;
constexpr float RayTMax = 1e16f, Inv4Pi = .07957747154594767f;
using std::abs; using std::min; using std::max;
using std::exp; using std::log; using std::sqrt;
using std::optional;
template<class T> T select(bool test, T yes, T no) { return test ? yes : no; }
float sqr(float x) { return x*x; }
struct Float2 { float x, y; };
Float2 make_float2(float x, float y) { return {x,y}; }
struct Float3 {
    float x, y, z;
    float operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
Float3 operator+(Float3 a, Float3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Float3 operator*(Float3 a, float b) { return {a.x*b,a.y*b,a.z*b}; }
float dot(Float3 a, Float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
namespace ocarina { using ::select; }
#define $if if
#define $else else
#define $for(i, n) for (Uint i = 0; i < (n); ++i)
#define $break break
void near(float actual, double expected, double tolerance, const char *message) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>tolerance) {
        std::cerr << message << ": actual=" << actual << " expected=" << expected << '\n';
        throw std::runtime_error(message);
    }
}
void require(bool actual, const char *message) { if (!actual) throw std::runtime_error(message); }
'''


def _compile(name, program, include_dirs=()):
    compiler = shutil.which('g++')
    assert compiler, 'g++ is required for the production media numerical fixtures'
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix=f'{name}-', dir=TEMP))
    cpp, binary = directory / f'{name}.cpp', directory / f'{name}.exe'
    cpp.write_text(SHIM + program, encoding='utf-8')
    result = subprocess.run([compiler, '-std=c++20', '-O2', *('-I'+str(p) for p in include_dirs), str(cpp), '-o', str(binary)],
                            text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    return binary


@lru_cache(maxsize=1)
def _helpers():
    path = ROOT / 'vision/src/base/scattering/medium_transport.h'
    assert path.exists(), 'Production homogeneous transport helpers are missing'
    source = path.read_text(encoding='utf-8-sig')
    names = ['Float medium_world_distance(', 'Float homogeneous_transmittance(',
             'Float homogeneous_sample_distance(', 'Float medium_phase_hg(',
             'Float2 medium_cube_interval(', 'Uint medium_after_surface(']
    functions = '\n'.join(_function(source, name) for name in names)
    return _compile('media-helpers', functions + r'''
int main(int argc, char **argv) {
    try {
        require(argc == 2, "one fixture required");
        const std::string name = argv[1];
        if (name == "beer") {
            near(homogeneous_transmittance(.5f, 2.f), .36787944117144233, 1e-6, "Beer-Lambert .5 * 2");
            near(homogeneous_transmittance(0.f, RayTMax), 1., 0., "vacuum over unbounded segment");
            near(homogeneous_transmittance(.5f, medium_world_distance(1.f, 2.f)), .36787944117144233, 1e-6, "nonunit ray physical distance");
            near(homogeneous_transmittance(.5f, medium_world_distance(2.f, 1.f)), .36787944117144233, 1e-6, "unit ray physical distance");
        } else if (name == "flight") {
            near(homogeneous_sample_distance(.5f, .6321205588285577f), 2., 5e-7, "physical free flight");
            near(homogeneous_sample_distance(0.f, 0.f), RayTMax, 0., "zero extinction zero sample");
            near(homogeneous_sample_distance(0.f, .5f), RayTMax, 0., "zero extinction nonzero sample");
            require(std::isfinite(homogeneous_sample_distance(.5f, 1.f)), "closed-end sample must be finite");
        } else if (name == "phase") {
            near(medium_phase_hg(-1.f, 0.f), .07957747154594767, 1e-8, "isotropic phase");
            near(medium_phase_hg(.27f, 0.f), .07957747154594767, 1e-8, "isotropic direction invariance");
            for (float g : {0.f, -.5f, .5f, .9f}) {
                double integral = 0.;
                constexpr int n = 200000;
                for (int i = 0; i < n; ++i) integral += medium_phase_hg(-1.f+(i+.5f)*(2.f/n), g);
                near(float(integral * (2./n) * 6.283185307179586), 1., 2e-5, "HG solid-angle normalization");
            }
        } else if (name == "boundary") {
            auto a = medium_cube_interval({-3,0,0}, {1,0,0});
            near(a.x, 2., 0., "entry parameter"); near(a.y, 4., 0., "exit parameter");
            auto b = medium_cube_interval({0,0,0}, {2,0,0});
            near(b.x, -.5, 0., "inside origin entry behind camera"); near(b.y, .5, 0., "nonunit exit parameter");
            auto c = medium_cube_interval({-3,2,0}, {1,0,0});
            require(c.x > c.y, "parallel ray outside slab must miss");
            auto d = medium_cube_interval({-3,1,0}, {1,0,0});
            near(d.x, 2., 0., "face-parallel ray stays finite");
            // World cube scaled 2 in X: world ray (-6,0,0)+(2,0,0)t.
            // Inverse-transforming both origin and direction keeps its t.
            near(homogeneous_transmittance(.5f, medium_world_distance(a.y-a.x, 2.f)), .1353352832366127, 1e-6, "transformed cube world length");
        } else if (name == "inherit") {
            require(medium_after_surface(7u, InvalidUI32, InvalidUI32, false, 1.f) == 7u, "undeclared glass must inherit medium");
            require(medium_after_surface(7u, 3u, InvalidUI32, true, -1.f) == 3u, "declared entry selects inside");
            require(medium_after_surface(3u, 3u, InvalidUI32, true, 1.f) == InvalidUI32, "declared exit selects vacuum");
            require(medium_after_surface(7u, InvalidUI32, InvalidUI32, true, -1.f) == InvalidUI32, "explicit vacuum is not inheritance");
        } else throw std::runtime_error("unknown fixture");
        std::cout << "PASS " << name << '\n';
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
''')


def _run(binary, *args):
    result = subprocess.run([str(binary), *args], text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr


def test_beer_lambert_and_nonunit_physical_distance():
    _run(_helpers(), 'beer')


def test_free_flight_is_finite_at_zero_extinction_and_sample_endpoints():
    _run(_helpers(), 'flight')


def test_isotropic_and_hg_phase_are_normalized():
    _run(_helpers(), 'phase')


def test_cube_entry_exit_parallel_faces_and_transformed_world_length():
    _run(_helpers(), 'boundary')


def test_undeclared_glass_inherits_and_explicit_vacuum_transitions():
    _run(_helpers(), 'inherit')


def test_null_continuation_preserves_incident_ior_and_medium():
    source = (ROOT / 'vision/src/base/scattering/interaction.cpp').read_text(encoding='utf-8-sig')
    spawn = _function(source, 'RayState Interaction::spawn_ray_state(')
    program = r'''
struct RayVar {};
struct RayState { RayVar ray; Float ior; Uint medium; };
struct MediumInterface { Uint inside{InvalidUI32}, outside{InvalidUI32}; Bool declared{false}; };
struct Interaction {
    Float3 pos{}, ng{0,0,1};
    optional<MediumInterface> mi_{MediumInterface{}};
    optional<Uint> incident_medium_{7u};
    optional<Float> incident_ior_{1.5f};
    MediumInterface mi() const { return *mi_; }
    RayState spawn_ray_state(const Float3 &) const noexcept;
};
namespace vision { RayVar spawn_ray(Float3, Float3, Float3) { return {}; } }
Uint medium_after_surface(const Uint &incident, const Uint &inside, const Uint &outside,
                          const Bool &declared, const Float &cosine) {
    return declared ? (cosine > 0 ? outside : inside) : incident;
}
__SPAWN__
int main() {
    Interaction it;
    const auto ray = it.spawn_ray_state({0,0,1});
    near(ray.ior, 1.5, 0., "null crossing reset IOR");
    require(ray.medium == 7u, "undeclared surface reset active medium");
    it.mi_->declared = true; it.mi_->inside = 3u;
    const auto entered = it.spawn_ray_state({0,0,-1});
    near(entered.ior, 1.5, 0., "declared null entry reset IOR");
    require(entered.medium == 3u, "declared null entry chose wrong medium");
    it.mi_.reset(); it.incident_ior_.reset(); it.incident_medium_.reset();
    const auto surface_only = it.spawn_ray_state({0,0,1});
    near(surface_only.ior, 1., 0., "media-off surface IOR changed");
    require(surface_only.medium == InvalidUI32, "media-off surface acquired medium");
}
'''
    _run(_compile('incident-state', program.replace('__SPAWN__', spawn)))


def test_production_segmented_shadow_crosses_null_boundaries_in_world_distance():
    path = ROOT / 'vision/src/base/mgr/geometry.cpp'
    source = path.read_text(encoding='utf-8-sig')
    functions = '\n'.join(_function(source, signature) for signature in [
        'MediumBoundaryEvent Geometry::trace_medium_boundary(',
        'Uint Geometry::initial_medium(',
        'TriangleHitVar Geometry::trace_transport(',
        'SampledSpectrum Geometry::Tr(',
    ])
    helpers = (ROOT / 'vision/src/base/scattering/medium_transport.h').read_text(encoding='utf-8-sig')
    functions = _function(helpers, 'Float2 medium_cube_interval(') + '\n' + functions
    program = r'''
Float3 make_float3(float x, float y, float z) { return {x,y,z}; }
struct RayVar {
    Float3 org, dir; float limit;
    const RayVar *operator->() const { return this; }
    Float3 origin() const { return org; } Float3 direction() const { return dir; }
    float t_max() const { return limit; }
    Float3 at(float t) const { return org+dir*t; }
};
RayVar make_ray(Float3 o, Float3 d, float t=RayTMax) { return {o,d,t}; }
struct RayState {
    RayVar ray; Float ior{1.f}; Uint medium{InvalidUI32};
    Float3 origin() const { return ray.org; } Float3 direction() const { return ray.dir; }
    float t_max() const { return ray.limit; } bool in_medium() const { return medium != InvalidUI32; }
};
struct MediumBoundaryEvent { Float t; Uint medium, before; Bool hit; };
struct Matrix { Float3 scale, translate; };
Float3 transform_point(Matrix m, Float3 p) { return {p.x*m.scale.x+m.translate.x,p.y*m.scale.y+m.translate.y,p.z*m.scale.z+m.translate.z}; }
Float3 transform_vector(Matrix m, Float3 p) { return {p.x*m.scale.x,p.y*m.scale.y,p.z*m.scale.z}; }
struct Boundary { Matrix world_to_object; Uint inside, outside; float volume{8.f}; };
struct Boundaries : std::vector<Boundary> {
    Boundary read(Uint i) const { return at(i); }
    const Boundaries &host_buffer() const { return *this; }
};
struct SampledWavelengths { Uint dimension() const { return 1; } };
struct SampledSpectrum {
    float value; SampledSpectrum(Uint, float v):value(v) {}
    SampledSpectrum &operator*=(SampledSpectrum s) { value *= s.value; return *this; }
};
struct Uint2 { uint x{}, y{}; };
struct TriangleHitVar {
    bool found{false};
    const TriangleHitVar *operator->() const { return this; }
    bool is_hit() const { return found; } bool is_miss() const { return !found; }
};
struct Interaction {
    Float3 pos{}; bool phase{}, material{};
    Interaction(bool = true) {}
    bool has_phase() const { return phase; }
    void set_incident_state(const RayState &) {}
};
struct TSampler {};
struct Medium {
    float sigma;
    SampledSpectrum Tr(const RayVar &r, const SampledWavelengths &, TSampler &) const {
        return {1, exp(-sigma * sqrt(dot(r.dir,r.dir)) * r.limit)};
    }
    SampledSpectrum sample(const RayState &rs, Interaction &it, TSampler &, const SampledWavelengths &) const {
        // A fixed .5-world-unit proposal exercises clipping/ordering in the
        // production transport loop. The production sampler itself is tested
        // independently below, including its physical weights.
        const auto length=sqrt(dot(rs.ray.dir,rs.ray.dir));
        if (.5f < length * rs.t_max()) { it.phase=true; it.material=false; it.pos=rs.ray.at(.5f/length); }
        return {1,1.f};
    }
};
struct Registry {
    template<class F> void dispatch(Uint index, F fn) { Medium m{index==0u ? .5f : .25f}; fn(&m); }
};
struct Scene {
    bool enabled{true}; Registry registry;
    bool process_mediums() const { return enabled; } Registry &mediums() { return registry; }
};
struct Geometry {
    Boundaries medium_boundaries_;
    float surface_t{RayTMax};
    TriangleHitVar trace_closest(const RayVar &) const { return {surface_t<RayTMax}; }
    Interaction compute_surface_interaction(const TriangleHitVar &, RayVar &ray) const {
        ray.limit=surface_t; Interaction it; it.material=true; it.pos=ray.at(surface_t); return it;
    }
    MediumBoundaryEvent trace_medium_boundary(const RayVar &, const Float &) const noexcept;
    Uint initial_medium(const Float3 &, const Uint &) const noexcept;
    TriangleHitVar trace_transport(Scene &, TSampler &, const SampledWavelengths &, RayState &, Interaction &, SampledSpectrum &, Uint2 *) const noexcept;
    SampledSpectrum Tr(Scene &, TSampler &, const SampledWavelengths &, const RayState &) const noexcept;
};
__FUNCTIONS__
int main() {
    Geometry geometry;
    geometry.medium_boundaries_.push_back({{{1,1,1},{0,0,0}},0u,InvalidUI32});
    Scene scene; TSampler sampler; SampledWavelengths swl;
    RayState connection{make_ray({-3,0,0},{1,0,0},6.f),1.5f,InvalidUI32};
    near(geometry.Tr(scene,sampler,swl,connection).value, .36787944117144233, 1e-6, "vacuum-medium-vacuum shadow");
    connection.ray = make_ray({-3,0,0},{2,0,0},3.f);
    near(geometry.Tr(scene,sampler,swl,connection).value, .36787944117144233, 1e-6, "nonunit segmented shadow");
    require(geometry.initial_medium({0,0,0},InvalidUI32)==0u, "inside camera must start in medium");
    require(geometry.initial_medium({-3,0,0},InvalidUI32)==InvalidUI32, "outside camera must start in vacuum");
    geometry.medium_boundaries_[0].outside=1u;
    require(geometry.initial_medium({3,100,0},InvalidUI32)==1u, "outside camera lost declared exterior when fixed line misses cube");
    // The inner region's exterior is the outer region. A camera outside both
    // must still select the outermost exterior, independent of source order.
    geometry.medium_boundaries_.push_back({{{2,2,2},{0,0,0}},2u,0u,1.f});
    require(geometry.initial_medium({0,0,0},InvalidUI32)==2u,"nested camera did not select innermost medium");
    require(geometry.initial_medium({3,100,0},InvalidUI32)==1u,"outside nested camera selected an inner exterior");
    std::reverse(geometry.medium_boundaries_.begin(),geometry.medium_boundaries_.end());
    require(geometry.initial_medium({0,0,0},InvalidUI32)==2u,"medium classification depends on source boundary order");
    std::reverse(geometry.medium_boundaries_.begin(),geometry.medium_boundaries_.end());
    geometry.medium_boundaries_.resize(1); geometry.medium_boundaries_[0].outside=InvalidUI32;
    connection = {make_ray({0,0,0},{1,0,0},3.f),1.5f,0u};
    near(geometry.Tr(scene,sampler,swl,connection).value, .6065306597126334, 1e-6, "inside light connection exits medium");
    require(connection.ior==1.5f && connection.medium==0u, "shadow integration mutated incident state");
    scene.enabled=false;
    near(geometry.Tr(scene,sampler,swl,connection).value, 1., 0., "media-off shadow changed");
    scene.enabled=true;
    // A separate second cube uses a different medium. Product of two analytic
    // optical depths is exp(-.5*2 - .25*2) = exp(-1.5).
    geometry.medium_boundaries_.push_back({{{1,1,1},{-4,0,0}},1u,InvalidUI32});
    connection = {make_ray({-3,0,0},{1,0,0},9.f),1.f,InvalidUI32};
    near(geometry.Tr(scene,sampler,swl,connection).value, .22313016014842982, 1e-6, "multiple media separated by vacuum");
    geometry.medium_boundaries_.resize(1);
    geometry.surface_t=1.5f;
    RayState path{make_ray({-3,0,0},{1,0,0}),1.5f,InvalidUI32};
    Interaction endpoint; SampledSpectrum beta{1,1.f}; Uint2 counters;
    auto hit=geometry.trace_transport(scene,sampler,swl,path,endpoint,beta,&counters);
    require(hit.is_hit() && endpoint.material && !endpoint.phase,"surface before volume lost ordering");
    require(counters.x==0u && counters.y==0u,"unreached volume generated events");
    geometry.surface_t=3.f; path={make_ray({-3,0,0},{1,0,0}),1.5f,InvalidUI32};
    hit=geometry.trace_transport(scene,sampler,swl,path,endpoint,beta,&counters);
    require(endpoint.phase && !endpoint.material,"medium event did not precede surface");
    near(endpoint.pos.x,-.5,1e-6,"free flight starts at physical entry");
    require(counters.x==1u && counters.y==1u,"actual event counters wrong");
    near(path.ior,1.5,0.,"null entry changed IOR");
    // Reusing an interaction after a volume bounce must clear its old phase;
    // otherwise a subsequent short surface segment becomes a phantom scatter.
    geometry.surface_t=.1f; path={make_ray({0,0,0},{1,0,0}),1.5f,0u};
    hit=geometry.trace_transport(scene,sampler,swl,path,endpoint,beta,&counters);
    require(endpoint.material && !endpoint.phase,"previous phase leaked into next surface interaction");
    geometry.surface_t=RayTMax; path={make_ray({3,0,0},{1,0,0}),1.5f,InvalidUI32};
    hit=geometry.trace_transport(scene,sampler,swl,path,endpoint,beta,&counters);
    require(hit.is_miss() && !endpoint.phase && !endpoint.material,"miss inherited previous endpoint");
}
'''
    _run(_compile('segmented-shadow', program.replace('__FUNCTIONS__', functions)))


def test_production_free_flight_handles_vacuum_absorption_and_nonunit_direction():
    source = (ROOT / 'vision/src/render_core/medium/homogeneous.cpp').read_text(encoding='utf-8-sig')
    functions = '\n'.join(_function(source, signature) for signature in [
        'SampledSpectrum Tr(const Float &', 'SampledSpectrum sample(const RayState &'])
    functions = functions.replace(' override {', ' {')
    helpers = (ROOT / 'vision/src/base/scattering/medium_transport.h').read_text(encoding='utf-8-sig')
    helper_functions = '\n'.join(_function(helpers, signature) for signature in [
        'Float medium_world_distance(', 'Float homogeneous_transmittance(', 'Float homogeneous_sample_distance('])
    program = r'''
float length(Float3 v) { return sqrt(dot(v,v)); }
Float3 operator-(Float3 v) { return {-v.x,-v.y,-v.z}; }
Float3 normalize(Float3 v) { return v*(1.f/length(v)); }
Float3 make_float3(float v) { return {v,v,v}; }
template<class T, class U> T cast(U u) { return T(u); }
struct RayVar {
    Float3 org, dir; float limit;
    const RayVar *operator->() const { return this; }
    Float3 direction() const { return dir; } Float3 at(float t) const { return org+dir*t; }
    float t_max() const { return limit; }
};
struct RayState { RayVar ray; float ior{1.5f}; Uint medium{7u}; };
struct SampledWavelengths { uint dimension() const { return 1u; } };
struct SampledSpectrum {
    float value;
    SampledSpectrum(float v):value(v) {}
    SampledSpectrum(Uint, float v):value(v) {}
    float operator[](uint) const { return value; }
    float average() const { return value; }
    bool is_zero() const { return value==0.f; }
    template<class F> SampledSpectrum map(F f) const { return {f(value)}; }
};
SampledSpectrum operator-(SampledSpectrum a) { return {-a.value}; }
SampledSpectrum operator*(SampledSpectrum a, SampledSpectrum b) { return {a.value*b.value}; }
SampledSpectrum operator*(SampledSpectrum a, float b) { return {a.value*b}; }
SampledSpectrum operator/(SampledSpectrum a, float b) { return {a.value/b}; }
SampledSpectrum exp(SampledSpectrum a) { return {exp(a.value)}; }
struct Spectrum {
    struct Decode { SampledSpectrum sample; };
    Decode decode_to_unbound_spectrum(Float value, const SampledWavelengths &) const { return {{value}}; }
};
struct TSampler {
    float u; unsigned dimension{};
    TSampler *operator->() { return this; }
    float next_1d() { return (++dimension % 2u) ? .25f : u; }
};
struct Interaction {
    Float3 pos{}, wo{}, ng{}; bool phase{}; Uint medium{InvalidUI32};
    Interaction() = default;
    Interaction(Float3 p, Float3 w, bool):pos(p),wo(w) {}
    void init_phase(float, const SampledWavelengths &) { phase=true; }
    void set_medium(Uint id, Uint) { medium=id; }
    void set_incident_state(const RayState &) {}
};
__HELPERS__
struct HomogeneousMedium {
    float extinction, scattering, anisotropy{}; const float *g_{&anisotropy}; Spectrum spec;
    const Spectrum *spectrum() const { return &spec; }
    float sigma_t() const { return extinction; } float sigma_s() const { return scattering; }
    __FUNCTIONS__
};
int main() {
    SampledWavelengths swl;
    HomogeneousMedium scatter{.5f,.25f};
    RayState unit{{{0,0,0},{1,0,0},2.f}}, nonunit{{{0,0,0},{2,0,0},1.f}};
    TSampler a{.25f}, b{.25f}; Interaction first, second;
    auto one=scatter.sample(unit,first,a,swl), two=scatter.sample(nonunit,second,b,swl);
    near(first.pos.x,.5753641449,1e-6,"sampled world position");
    near(second.pos.x,first.pos.x,1e-6,"nonunit ray moved free flight");
    near(two.value,one.value,1e-6,"nonunit direction changed sampling weight");
    near(one.value,.5,1e-6,"scattering albedo weight");
    near(length(second.wo),1.,1e-6,"phase outgoing direction must be unit");
    HomogeneousMedium vacuum{0.f,0.f};
    for(float u : {0.f,.25f,1.f}) {
        TSampler s{u}; Interaction it;
        near(vacuum.sample(unit,it,s,swl).value,1.,0.,"vacuum sample must remain finite with unit weight");
        require(!it.phase,"vacuum generated a scatter event");
    }
    HomogeneousMedium absorb{.5f,0.f};
    double sum=0.;
    for(unsigned i=0;i<10000;++i) {
        TSampler s{(i+.5f)/10000.f}; Interaction it;
        const auto weight=absorb.sample(unit,it,s,swl).value;
        require(std::isfinite(weight),"pure absorption produced NaN/Inf"); sum+=weight;
    }
    near(float(sum/10000.),.36787944117144233,1e-4,"pure absorption expectation");
}
'''
    _run(_compile('free-flight', program.replace('__HELPERS__', helper_functions).replace('__FUNCTIONS__', functions)))


def test_pt_media_settings_are_explicit_atomic_and_reset_history():
    source = (ROOT / 'vision/src/render_core/integrator/pt.cpp').read_text(encoding='utf-8-sig')
    functions = '\n'.join(_function(source, signature) for signature in [
        'DataWrap settings() const override', 'bool set_settings(const DataWrap &overrides)'])
    functions = functions.replace(' override {', ' {')
    program = r'''
#include <nlohmann/json.hpp>
using DataWrap=nlohmann::json;
template<class T> struct Param { T value; T hv() const { return value; } };
struct Resource { int device() { return 0; } };
struct Geometry { int bindless_array() { return 0; } Resource *gpu_resource() { static Resource r; return &r; } };
struct Scene { Geometry geometry_; bool process_mediums() const { return true; } Geometry &geometry() { return geometry_; } };
namespace Global { struct SceneGpuContextScope { SceneGpuContextScope(int,int) {} }; }
struct Inspector { DataWrap settings() const { return DataWrap::object(); } };
struct Totals { uint64_t boundary_crossings{},scattering_events{},sampled_frames{}; };
    struct IlluminationIntegrator {
        Totals totals;
        uint reset_count{};
        void invalidation() { ++reset_count; totals={}; }
    };
    struct Harness : IlluminationIntegrator {
    Param<uint> max_depth_{16u}, min_depth_{5u}; Param<float> rr_threshold_{1.f};
    bool participating_media_{false}, medium_diagnostics_{false}, medium_diagnostics_compiled_{false};
    Inspector inspector; Inspector *inspector_{&inspector};
        Totals *medium_diagnostics_totals_{&totals};
        Scene scene_; uint compile_count{},apply_count{};
    DataWrap scene_pt_settings_{{"max_depth",16u},{"min_depth",5u},{"rr_threshold",1.f},
        {"participating_media",false},{"medium_diagnostics",false}};
    Scene &scene() { return scene_; } const Scene &scene() const { return scene_; }
    DataWrap illumination_settings() const { return {{"max_depth",max_depth_.hv()},{"min_depth",min_depth_.hv()},{"rr_threshold",rr_threshold_.hv()}}; }
    static bool merge_illumination_setting(DataWrap &d,const std::string &k,const DataWrap &v) {
        if((k!="max_depth"&&k!="min_depth"&&k!="rr_threshold")||!v.is_number())return false;
        d[k]=v;return true;
    }
    void apply_illumination_settings(const DataWrap &d) { ++apply_count; max_depth_.value=d["max_depth"]; }
    void compile_path_tracing(bool) { ++compile_count; }
    bool needs_lobe_signals() const { return false; }
    __FUNCTIONS__
};
int main() {
    Harness h;
    require(h.settings()["participating_media"]==false,"PT media must default off");
    require(h.settings()["medium_diagnostics"]==false,"PT diagnostics must default off");
    require(h.set_settings({{"participating_media",true},{"medium_diagnostics",true}}),"PT media settings rejected");
    require(h.settings()["participating_media"]==true&&h.settings()["medium_diagnostics"]==true,"PT media readback lost");
    require(h.reset_count==1u&&h.compile_count==1u,"media change must recompile and reset history once");
    auto before=h.settings(); auto writes=h.apply_count;
    require(!h.set_settings({{"participating_media",false},{"medium_diagnostics",1}}),"nonboolean diagnostics accepted");
    require(h.settings()==before&&h.apply_count==writes,"invalid request partially mutated live settings");
    require(!h.set_settings({{"participating_media",true},{"medium_diagnostics",true}}),"identical flags must be a no-op");
    h.medium_diagnostics_compiled_=true;h.totals={123u,45u,7u};
    const auto state=h.settings()["medium_diagnostics_state"];
    require(state["instrumentation"]==true&&state["boundary_crossings"]==123u&&state["scattering_events"]==45u&&state["sampled_frames"]==7u,"diagnostic readback must report actual counters");
    require(h.set_settings({{"participating_media",false},{"medium_diagnostics",false}}),"PT media disable rejected");
    require(h.reset_count==2u&&h.totals.sampled_frames==0u,"disabling media reused old history/counters");
}
'''
    include = ROOT / 'cmake-build-relwithdebinfo/_deps/nlohmann_json-src/include'
    _run(_compile('pt-settings', program.replace('__FUNCTIONS__', functions), [include]))


def test_native_homogeneous_parameters_reject_nonfinite_or_nonphysical_input():
    source = (ROOT / 'vision/src/base/scattering/medium_transport.h').read_text(encoding='utf-8-sig')
    function = _function(source, 'void validate_homogeneous_parameters(')
    program = r'''
#include <limits>
using float3=Float3;
namespace vision {
struct SpectrumAbsArgument {};
void abs(SpectrumAbsArgument) {} // Same namespace lookup collision as engine spectrum/complex abs.
__VALIDATOR__
}
using vision::validate_homogeneous_parameters;
int main() {
    validate_homogeneous_parameters({0,0,0},{1,1,1},.5f,0.f);
    validate_homogeneous_parameters({0,0,0},{0,0,0},0.f,.99f);
    const auto nan=std::numeric_limits<float>::quiet_NaN();
    const auto inf=std::numeric_limits<float>::infinity();
    struct Case { float3 a,s; float scale,g; };
    for(const auto &c:std::vector<Case>{
        {{-1,0,0},{1,1,1},1,0}, {{0,0,0},{1,-1,1},1,0},
        {{nan,0,0},{1,1,1},1,0}, {{0,0,0},{inf,1,1},1,0},
        {{0,0,0},{1,1,1},-1,0}, {{0,0,0},{1,1,1},inf,0},
        {{0,0,0},{1,1,1},1,1}, {{0,0,0},{1,1,1},1,-1},
        {{0,0,0},{1,1,1},1,nan}}) {
        bool rejected=false;
        try { validate_homogeneous_parameters(c.a,c.s,c.scale,c.g); }
        catch(const std::invalid_argument &) { rejected=true; }
        require(rejected,"nonphysical homogeneous parameters were accepted");
    }
}
'''
    _run(_compile('parameter-validation', program.replace('__VALIDATOR__', function)))


def test_empty_boundary_view_does_not_capture_an_unallocated_gpu_buffer():
    source = (ROOT / 'vision/src/base/mgr/geometry.cpp').read_text(encoding='utf-8-sig')
    functions = '\n'.join(_function(source, signature) for signature in [
        'MediumBoundaryEvent Geometry::trace_medium_boundary(', 'Uint Geometry::initial_medium('])
    program = r'''
// DSL loops generate their body once even for a zero runtime trip count. This
// narrow shim exposes accidental capture/read of an unallocated empty buffer.
#undef $for
#define $for(i,n) for(Uint i=0;i<std::max(1u,uint(n));++i)
struct RayVar {
    const RayVar *operator->() const { return this; }
    Float t_max() const { return RayTMax; }
    Float3 origin() const { return {}; } Float3 direction() const { return {1,0,0}; }
};
struct MediumBoundaryEvent { Float t; Uint medium{InvalidUI32},before{InvalidUI32}; Bool hit{false}; };
struct Boundary { int world_to_object{}; Uint inside{},outside{}; float volume{}; };
struct Boundaries : std::vector<Boundary> {
    const Boundaries &host_buffer() const { return *this; }
    Boundary read(Uint) const { throw std::runtime_error("captured unallocated boundary GPU buffer"); }
};
Float3 transform_point(int,Float3 p) { return p; } Float3 transform_vector(int,Float3 p) { return p; }
Float2 medium_cube_interval(Float3,Float3) { return {}; }
struct Geometry {
    Boundaries medium_boundaries_;
    MediumBoundaryEvent trace_medium_boundary(const RayVar &,const Float &) const noexcept;
    Uint initial_medium(const Float3 &,const Uint &) const noexcept;
};
__FUNCTIONS__
int main() {
    Geometry geometry;
    const auto event=geometry.trace_medium_boundary(RayVar{},Float{0.f});
    require(!event.hit&&event.t==RayTMax,"empty transport must return no boundary");
    require(geometry.initial_medium(Float3{},Uint{7u})==7u,"empty boundary view lost global medium fallback");
}
'''
    _run(_compile('empty-boundaries', program.replace('__FUNCTIONS__', functions)))
