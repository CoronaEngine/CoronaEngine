"""CPU execution of the production GI ideal-glass helpers with scalar DSL shims.

No renderer build or GPU launch. The transport boundary and endpoint tests run
the actual extracted C++ helpers; integration still needs compiled GPU captures.
"""
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess
import tempfile

from _restir_cpu import body

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'vision/src').is_dir())
HEADER = ROOT / 'vision/src/base/scattering/gi_ideal_glass.h'
TEMP = ROOT / '.workspace/tmp/rtxpt-glass-opt-20261115/transport'

PROGRAM = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <string>
using Float=float; using Bool=bool; using Uint=unsigned; using uint=unsigned; constexpr uint InvalidUI32=~0u;
#define $if if
#define $else else
using std::max; using std::abs;
struct Float3 {float x{},y{},z{};};
Float3 make_float3(float x,float y,float z){return{x,y,z};}
Float3 make_float3(float x){return{x,x,x};}
Float3 operator-(Float3 a){return{-a.x,-a.y,-a.z};}
Float3 operator+(Float3 a,Float3 b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 operator-(Float3 a,Float3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 operator*(Float3 a,float b){return{a.x*b,a.y*b,a.z*b};}
Float3 operator/(Float3 a,float b){return a*(1/b);}
float dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float length_squared(Float3 a){return dot(a,a);}
float abs_cos_theta(Float3 a){return abs(a.z);}
Float3 face_forward(Float3 n,Float3 a){return dot(n,a)<0?-n:n;}
struct SampledWavelengths{};
struct SampledSpectrum : Float3 {
    SampledSpectrum()=default;
    SampledSpectrum(float v){x=y=z=v;}
    SampledSpectrum(Float3 a){x=a.x;y=a.y;z=a.z;}
    SampledSpectrum(const SampledWavelengths&,float v):SampledSpectrum(v){}
    static SampledSpectrum one(const SampledWavelengths&){return{1.f};}
    Float3 vec3()const{return{x,y,z};}
    template<class F>SampledSpectrum map(F f)const{return{make_float3(f(x),f(y),f(z))};}
    SampledSpectrum&operator*=(float v){x*=v;y*=v;z*=v;return *this;}
    float operator[](uint i)const{return i==0?x:i==1?y:z;}
};
SampledSpectrum operator*(SampledSpectrum a,float b){return{a.vec3()*b};}
SampledSpectrum operator/(SampledSpectrum a,float b){return{a.vec3()/b};}
SampledSpectrum operator+(float a,SampledSpectrum b){return{make_float3(a+b.x,a+b.y,a+b.z)};}
SampledSpectrum sqrt(SampledSpectrum a){return{make_float3(std::sqrt(a.x),std::sqrt(a.y),std::sqrt(a.z))};}
struct BxDFFlag {static constexpr uint Reflection=2,Transmission=4,Specular=32;
    static constexpr uint All=127,SpecRefl=Reflection|Specular,SpecTrans=Transmission|Specular;
    static bool is_specular(uint f){return f&Specular;}static bool is_transmission(uint f){return f&Transmission;}};
struct StableLobe {Float3 wi;float probability{};SampledSpectrum weight;float eta=1;bool valid=false;bool transmission=false;};
struct StableLobes {static constexpr uint transmission_index=0,reflection_index=1;
    std::array<StableLobe,2>lobes;bool has_stochastic_remainder=true;explicit StableLobes(const SampledWavelengths&){};};
struct ScatterEval {SampledSpectrum f;float pdfs{};uint flags{};
    ScatterEval()=default;explicit ScatterEval(const SampledWavelengths&){}float pdf()const{return pdfs;}
    SampledSpectrum throughput()const{return f/pdfs;}};
struct BSDFSample {ScatterEval eval;Float3 wi;float eta=1;uint stable_lobe_index=~0u;
    explicit BSDFSample(const SampledWavelengths&){} bool valid()const{return eval.pdfs>0;}};
float fresnel_dielectric(float c,float eta){
    float st=(1-c*c)/(eta*eta); if(st>=1)return 1;
    float ct=std::sqrt(std::max(0.f,1-st));
    float a=(eta*c-ct)/(eta*c+ct),b=(c-eta*ct)/(c+eta*ct);return(a*a+b*b)*.5f;
}
bool refract(Float3 wo,Float3 n,float eta,Float3*wi){
    float ci=dot(n,wo),st=std::max(0.f,1-ci*ci)/(eta*eta);
    *wi=-wo/eta+n*(ci/eta-std::sqrt(std::max(0.f,1-st)));return st<1;
}
template<class T>T select(bool p,T a,T b){return p?a:b;}
namespace ocarina {using std::sqrt;using ::select;}
__HELPERS__
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
void near(float v,float want,const char*m){require(std::isfinite(v)&&abs(v-want)<3e-6f,m);}
void branches(){
    SampledWavelengths swl;SampledSpectrum tint{make_float3(.25f,.64f,1.f)};
    auto t=gi_ideal_glass_branch(swl,make_float3(.6f,0,.8f),1.5f,tint,true);
    auto r=gi_ideal_glass_branch(swl,make_float3(.6f,0,.8f),1.5f,tint,false);
    require(t.valid()&&r.valid(),"both Fresnel branches must be valid");
    require(t.eval.flags==BxDFFlag::SpecTrans&&r.eval.flags==BxDFFlag::SpecRefl,"ideal branches must carry true delta flags");
    require(t.stable_lobe_index==0&&r.stable_lobe_index==1,"native transmission/reflection order");
    near(t.wi.x,-.4f,"exact refracted direction");near(t.wi.z,-.916515139f,"Snell transmitted cosine");
    near(r.wi.x,-.6f,"exact reflected direction");near(r.wi.z,.8f,"reflected cosine");
    near(r.eval.pdfs,.043894736f,"exact oblique Fresnel branch mass");
    near(t.eval.pdfs+r.eval.pdfs,1,"Fresnel masses sum to one");
    near(t.eval.throughput().x*abs(t.wi.z),.5f,"thick tint is square root per interface");
    near(t.eval.throughput().y*abs(t.wi.z),.8f,"green interface tint");
    near(r.eval.throughput().x*abs(r.wi.z),1,"reflection Fresnel cancels sampling mass");
    for(float channel:{tint.x,tint.y,tint.z}){
        float integrated=r.eval.pdfs+t.eval.pdfs*std::sqrt(channel);
        require(integrated<=1.f&&integrated>=0,"enumerated Fresnel energy finite and bounded");
    }
}
void exit_and_tir(){
    SampledWavelengths swl;SampledSpectrum tint{make_float3(.25f,.64f,1.f)};
    auto front=gi_ideal_glass_branch(swl,make_float3(0,0,1),1.5f,tint,true);
    auto exit=gi_ideal_glass_branch(swl,make_float3(0,0,-1),1.f/1.5f,tint,true);
    near(front.eval.pdfs,.96f,"normal-incidence entry mass");near(exit.eval.pdfs,.96f,"normal-incidence exit mass");
    near(front.wi.z,-1,"entry transmission direction");near(exit.wi.z,1,"exit transmission direction");
    near(front.eval.throughput().x*exit.eval.throughput().x,.25f,"two interfaces recover full authored tint");
    near(front.eta*exit.eta,1,"entry/exit eta product");
    auto tir=gi_ideal_glass_branch(swl,make_float3(.866025404f,0,-.5f),1.f/1.5f,tint,false);
    auto impossible=gi_ideal_glass_branch(swl,make_float3(.866025404f,0,-.5f),1.f/1.5f,tint,true);
    require(tir.valid()&&!impossible.valid(),"TIR retains reflection only");near(tir.eval.pdfs,1,"TIR mass");
    near(tir.eval.throughput().x*abs(tir.wi.z),1,"TIR unit weight");
    auto equal=gi_ideal_glass_branch(swl,make_float3(.6f,0,.8f),1,tint,true);
    near(equal.wi.x,-.6f,"equal-IOR straight-through direction");near(equal.eval.pdfs,1,"equal-IOR no reflection");
}
void policy(){
    for(bool scoped:{false,true})for(bool pure:{false,true})for(float r:{0.f,.0799f,.08f,.1f,.5f}){
        bool expected=scoped&&pure&&(r==0||r==.0799f);
        require(gi_ideal_glass_enabled(scoped,pure,r)==expected,"native cutoff or default/mixed isolation changed");
    }
}
void endpoints(){
    Float3 incoming=make_float3(.6f,0,-.8f);
    require(gi_prefix_endpoint_matches(21u,3u,incoming,21u,3u,incoming),"exact transmitted exit endpoint must match");
    require(!gi_prefix_endpoint_matches(22u,3u,incoming,21u,3u,incoming),"exit reflection must not claim transmitted endpoint");
    require(!gi_prefix_endpoint_matches(21u,2u,incoming,21u,3u,incoming),"same primitive at another bounce must not match");
    require(!gi_prefix_endpoint_matches(21u,3u,-incoming,21u,3u,incoming),"opposite incident direction must not match");
    require(!gi_prefix_endpoint_matches(0u,3u,incoming,0u,3u,incoming),"unknown stochastic branch cannot match");
    require(gi_prefix_next_branch(gi_prefix_next_branch(1u,0u,true),0u,true)==21u,"entry-exit transmission branch chain");
    require(gi_prefix_next_branch(5u,1u,true)==22u,"exit reflection branch chain");
    require(gi_prefix_next_branch(5u,~0u,true)==0u,"non-stable scattering invalidates prefix branch");
    require(gi_prefix_next_branch(0u,0u,true)==0u,"unknown branch cannot regain identity");
    require(gi_prefix_next_branch(5u,0u,false)==0u,"invalid sample cannot advance prefix");
    require(gi_prefix_reuse_lobe(true,true)==0u,"occupied exit plane must continue transmission");
    require(gi_prefix_reuse_lobe(false,true)==1u,"TIR must reuse reflection");
    require(gi_prefix_reuse_lobe(false,false)==~0u,"empty distribution has no continuation");
}
int main(int argc,char**argv){try{
    std::string test=argv[1];if(test=="branches")branches();else if(test=="exit")exit_and_tir();else if(test=="endpoints")endpoints();else policy();
    std::cout<<"PASS "<<test<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''


@lru_cache(maxsize=1)
def executable():
    source = HEADER.read_text(encoding='utf-8-sig') if HEADER.exists() else ''
    helpers = []
    for signature in ('Bool gi_ideal_glass_enabled(', 'BSDFSample gi_ideal_glass_branch(',
                      'Bool gi_prefix_endpoint_matches(', 'Uint gi_prefix_next_branch(',
                      'Uint gi_prefix_reuse_lobe('):
        assert signature in source, f'GI ideal glass behavior is missing: {signature}'
        declaration, implementation = body(source, signature)
        helpers.append(declaration + implementation)
    compiler = shutil.which('g++')
    assert compiler, 'g++ required for production helper fixture'
    TEMP.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix='ideal-glass-', dir=TEMP))
    cpp, binary = folder / 'test.cpp', folder / 'test.exe'
    cpp.write_text(PROGRAM.replace('__HELPERS__', '\n'.join(helpers)), encoding='utf-8')
    result = subprocess.run([compiler, '-std=c++20', '-O2', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return binary


def run(name):
    result = subprocess.run([str(executable()), name], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_real_ideal_branches_have_snell_directions_discrete_fresnel_masses_and_tint():
    run('branches')


def test_real_two_interfaces_tir_and_equal_ior():
    run('exit')


def test_native_cutoff_and_default_mixed_policy_isolation():
    run('policy')


def test_branch_depth_and_incident_direction_distinguish_revisited_endpoints():
    run('endpoints')


def test_actual_material_sample_dispatch_preserves_default_and_individual_normal_paths():
    source = (ROOT / 'vision/src/base/scattering/material.cpp').read_text(encoding='utf-8-sig')
    helpers = HEADER.read_text(encoding='utf-8-sig')
    extracted = []
    for text, signature in ((helpers, 'BSDFSample gi_ideal_glass_branch('),
                            (source, 'BSDFSample MaterialEvaluator::sample_delta('),
                            (source, 'StableLobes MaterialEvaluator::eval_stable_lobes('),
                            (source, 'BSDFSample MaterialEvaluator::sample(')):
        declaration, implementation = body(text, signature)
        extracted.append(declaration + implementation)
    prefix = PROGRAM[:PROGRAM.index('__HELPERS__')]
    shim = r'''
enum TransportMode {Radiance}; using TSampler=int;
bool tilted=false;
struct Frame {
    Float3 to_local(Float3 v)const{if(!tilted)return v;return{.5f*v.x-.866025404f*v.z,v.y,.866025404f*v.x+.5f*v.z};}
    Float3 to_world(Float3 v)const{if(!tilted)return v;return{.5f*v.x+.866025404f*v.z,v.y,-.866025404f*v.x+.5f*v.z};}
};
bool same_hemisphere(Float3 a,Float3 b,Float3){return a.z*b.z>0;}
bool scoped=false,individual=false;unsigned ordinary_calls=0,delta_calls=0;
namespace gi_principled {bool reading(){return scoped;}bool writing(){return false;}}
struct MaterialRegistry {static MaterialRegistry&instance(){static MaterialRegistry r;return r;}
    bool individual_ns()const{return individual;}};
struct Lobe {BSDFSample sample(Float3,uint,TSampler&,TransportMode)const{
    ordinary_calls++;SampledWavelengths w;BSDFSample s(w);s.wi=make_float3(0,0,1);s.eval.f=42.f;s.eval.pdfs=7;return s;}
    StableLobes eval_stable_lobes(Float3 wo)const{
        SampledWavelengths w;StableLobes s(w);s.has_stochastic_remainder=false;
        for(unsigned i=0;i<2;i++){auto sample=gi_ideal_glass_branch(w,wo,1.5f,SampledSpectrum{1.f},i==0);
            auto&branch=s.lobes[i];branch.wi=sample.wi;branch.weight=sample.eval.f*abs(sample.wi.z);
            branch.probability=sample.eval.pdfs;branch.valid=sample.valid();branch.transmission=i==0;}
        return s;
    }
};
struct MaterialEvaluator {
    SampledWavelengths wavelengths;const SampledWavelengths*swl_=&wavelengths;Frame shading_frame_;Float3 ng_=make_float3(0,0,1);
    uint flags=0;bool reflection=false;
    uint flag()const{return flags;}
    template<class F>void dispatch(F f)const{Lobe l;f(&l);}
    BSDFSample sample_local(Float3 w,uint f,TSampler&s,TransportMode t)const{return Lobe{}.sample(w,f,s,t);}
    BSDFSample sample_delta_local(Float3 wo,TSampler&)const{delta_calls++;return gi_ideal_glass_branch(*swl_,wo,1.5f,SampledSpectrum{make_float3(.25f,.64f,1.f)},!reflection);}
    BSDFSample sample_delta(const Float3&,TSampler&)const noexcept;
    StableLobes eval_stable_lobes(const Float3&)const noexcept;
    BSDFSample sample(const Float3&,TSampler&,const Uint&,TransportMode)const noexcept;
};
'''
    helper, *methods = extracted
    checks = r'''
int main(){
    TSampler sampler=0;unsigned cases=0;
    for(bool gi:{false,true})for(bool ns:{false,true})for(bool spec:{false,true}){
        scoped=gi;individual=ns;ordinary_calls=delta_calls=0;
        MaterialEvaluator e;e.flags=spec?BxDFFlag::SpecTrans:0;
        auto sample=e.sample(make_float3(0,0,1),sampler,BxDFFlag::All,Radiance);
        bool ideal=gi&&spec;
        if(ideal){
            if(delta_calls!=1||ordinary_calls!=0||sample.stable_lobe_index!=0||abs(sample.eval.throughput().x-.5f)>1e-6f)return 1;
        }else if(delta_calls!=0||ordinary_calls!=1||sample.eval.f.x!=42||sample.eval.pdfs!=7)return 2;
        ++cases;
    }
    tilted=true;scoped=true;individual=false;MaterialEvaluator rotated;rotated.flags=BxDFFlag::SpecRefl;rotated.reflection=true;
    auto rejected=rotated.sample(make_float3(0,0,1),sampler,BxDFFlag::All,Radiance);
    if(rejected.valid()||abs(rejected.wi.z+.5f)>1e-6f)return 3;
    auto branches=rotated.eval_stable_lobes(make_float3(0,0,1));
    if(branches.lobes[1].valid||branches.lobes[1].probability!=0||branches.lobes[1].weight.x!=0)return 4;
    if(!branches.lobes[0].valid)return 5;
    std::cout<<"PASS "<<cases<<" actual MaterialEvaluator sample routes\n";
}
'''
    TEMP.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix='dispatch-', dir=TEMP))
    cpp, binary = folder / 'test.cpp', folder / 'test.exe'
    cpp.write_text(prefix + helper + shim + '\n'.join(methods) + checks, encoding='utf-8')
    result = subprocess.run([shutil.which('g++'), '-std=c++20', '-O2', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, f'wrapper fixture exit={result.returncode}: ' + result.stdout + result.stderr


def test_actual_gi_dielectric_sampling_enumeration_continuous_evaluation_and_guides_agree():
    source = (ROOT / 'vision/src/render_core/material/principled_bsdf.cpp').read_text(encoding='utf-8-sig')
    declaration, implementation = body(source, 'class GIIdealDielectricLobe final')
    class_source = declaration + implementation + ';'
    helpers = HEADER.read_text(encoding='utf-8-sig')
    declaration, implementation = body(helpers, 'BSDFSample gi_ideal_glass_branch(')
    helper = declaration + implementation
    prefix = PROGRAM[:PROGRAM.index('__HELPERS__')]
    shim = r'''
enum MaterialEvalMode {All};enum TransportMode {Radiance};
template<class T>using SP=std::shared_ptr<T>;
struct Sampler{float selector=.5f;float next_1d(){return selector;}};using TSampler=SP<Sampler>;
struct Float2 {float x,y;};Float2 make_float2(float a){return{a,a};}
struct SurfaceData {static constexpr uint Glossy=16;};using ShadingFrame=int;
constexpr int D=0;template<int>struct Microfacet{};
struct Fresnel{SampledSpectrum eta()const{return{1.5f};}};
#define VS_MAKE_LOBE_ASSIGNMENT(x)
class DielectricLobe {
protected:
    SP<Fresnel>fresnel_;SampledSpectrum kt_;
    virtual ScatterEval evaluate_local_impl(const Float3&,const Float3&,MaterialEvalMode,const Uint&,TransportMode)const noexcept{
        SampledWavelengths w;ScatterEval e(w);e.f=7.f;e.pdfs=2;return e;}
    virtual ScatterEval evaluate_local_impl(const Float3&w,const Float3&i,MaterialEvalMode m,const Uint&f,TransportMode t,Float*)const noexcept{
        return DielectricLobe::evaluate_local_impl(w,i,m,f,t);}
public:
    DielectricLobe(SP<Fresnel>f,SP<Microfacet<D>>,SampledSpectrum c,bool,uint,int):fresnel_(f),kt_(c){}
    const SampledWavelengths*swl()const{static SampledWavelengths w;return&w;}
    virtual bool supports_prepared_local_evaluation()const noexcept{return true;}
    virtual uint flag()const noexcept{return 0;}
    virtual BSDFSample sample_delta_local(const Float3&,TSampler&)const noexcept{return BSDFSample(*swl());}
    virtual StableLobes eval_stable_lobes(const Float3&)const noexcept{return StableLobes(*swl());}
    virtual void reuse_material(SampledSpectrum&d,SampledSpectrum&s,Float2&r)const noexcept{d=.2f;s=.04f;r={.9f,.9f};}
    virtual SampledSpectrum albedo(const Float&)const noexcept{return{.75f};}
    ScatterEval evaluate_local(Float3 w,Float3 i)const{return evaluate_local_impl(w,i,All,127,Radiance);}
};
'''
    checks = r'''
int main(){
    auto sampler=std::make_shared<Sampler>();auto fresnel=std::make_shared<Fresnel>();auto microfacet=std::make_shared<Microfacet<D>>();
    GIIdealDielectricLobe ideal(fresnel,microfacet,SampledSpectrum{make_float3(.25f,.64f,1)},64,true,0);
    GIIdealDielectricLobe rough(fresnel,microfacet,SampledSpectrum{1.f},64,false,0);
    Float3 w=make_float3(0,0,1);auto lobes=ideal.eval_stable_lobes(w);
    if(lobes.has_stochastic_remainder||!lobes.lobes[0].valid||!lobes.lobes[1].valid)return 1;
    if(abs(lobes.lobes[0].weight.x-.48f)>1e-6f||abs(lobes.lobes[1].weight.x-.04f)>1e-6f)return 2;
    for(float u:{0.f,.02f,.041f,.5f,.99f}){
        sampler->selector=u;auto sample=ideal.sample_delta_local(w,sampler);auto&branch=lobes.lobes[u<.04f?1:0];
        if(sample.stable_lobe_index!=(u<.04f?1u:0u)||abs(sample.eval.f.x*abs(sample.wi.z)-branch.weight.x)>1e-6f)return 3;
    }
    if(ideal.evaluate_local(w,w).pdfs!=0||rough.evaluate_local(w,w).f.x!=7||rough.evaluate_local(w,w).pdfs!=2)return 4;
    if(!rough.eval_stable_lobes(w).has_stochastic_remainder||rough.flag()!=SurfaceData::Glossy)return 5;
    SampledSpectrum d,s;Float2 r;ideal.reuse_material(d,s,r);if(r.x!=0||r.y!=0)return 6;
    rough.reuse_material(d,s,r);if(r.x!=.9f||r.y!=.9f)return 7;
    std::cout<<"PASS actual GI dielectric enumeration, sampling, continuous measure, rough fallback and guides\n";
}
'''
    TEMP.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix='dielectric-', dir=TEMP))
    cpp, binary = folder / 'test.cpp', folder / 'test.exe'
    cpp.write_text(prefix + helper + shim + class_source + checks, encoding='utf-8')
    result = subprocess.run([shutil.which('g++'), '-std=c++20', '-O2', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
