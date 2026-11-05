"""Actual GI/SP roulette producers and Li guard, including exact expectations."""
from _restir_cpu import ROOT, body, balanced, compile_run

GI_CPP = r'''

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using Float=double;using Uint=unsigned;using uint=unsigned;
#define $if if
#define $break throw Stopped{}
struct Stopped{};
template<class A,class B> double min(A a,B b){return std::min(double(a),double(b));}
template<class A,class B> double max(A a,B b){return std::max(double(a),double(b));}
template<class A,class B,class C> double clamp(A a,B b,C c){return std::clamp(double(a),double(b),double(c));}
template<class T>T*addressof(T&x){return &x;}
struct RGB{double x,y,z;RGB as_vec3()const{return *this;}};
using Float3=RGB;
RGB operator/(RGB a,double b){return{a.x/b,a.y/b,a.z/b};}
namespace ocarina{RGB zero_if_nan_inf(RGB a){return{std::isfinite(a.x)?a.x:0,std::isfinite(a.y)?a.y:0,std::isfinite(a.z)?a.z:0};}}
struct Hit{RGB bsdf;double pdf; __THROUGHPUT__ __SAFE_THROUGHPUT__};
struct Context{const uint*rr_depth_offset=nullptr;const double*rr_survival_scale=nullptr;};
struct Sampler{double u;uint calls=0;double next_1d(){++calls;return u;}};
struct Result{bool survived;uint calls;double throughput,scale;};
Result run(bool with_scale,RGB primary,double pdf,double lum,double eta,uint depth,double u,uint prefix_depth=0,bool with_offset=false){
    Hit storage{primary,pdf};Hit*hit_bsdf=&storage;Context hit_context;
    __ASSIGN__
    if(!with_scale)hit_context.rr_survival_scale=nullptr;
    if(!with_offset)hit_context.rr_depth_offset=nullptr;
    Context hc=hit_context;uint bounces=depth,minimum=5;uint*min_depth_=&minimum;
    double threshold=1,*rr_threshold_=&threshold,eta_scale=eta;
    Sampler rng{u};Sampler*sampler=&rng;double throughput=lum*.5;
    double scale=hc.rr_survival_scale?*hc.rr_survival_scale:1;
    try{__BLOCK__}catch(Stopped&){return{false,rng.calls,0,scale};}
    return{true,rng.calls,throughput,scale};
}
void require(bool ok,const char*msg){if(!ok)throw std::runtime_error(msg);}
int main(){
    uint cases=0,expectations=0;
    for(bool scaled:{false,true})for(double primary:{0.,.125,.25,.5,1.,2.})
    for(double pdf:{.5,1.,2.})for(double lum:{.125,.5,1.,2.,8.})
    for(double eta:{1.,4.})for(uint depth:{0u,4u,5u,12u}){
        RGB f{primary,primary*.25,primary*.5};
        double expected_scale=scaled?clamp(primary/pdf,.25,1.):1.;
        auto actual=run(scaled,f,pdf,lum,eta,depth,.123);
        require(actual.scale==expected_scale,"GI roulette scale missing or incorrectly clamped");
        bool eligible=eta*lum*expected_scale<1 && depth>=5;
        require(actual.calls==(eligible?1u:0u),"Scaled roulette threshold or depth changed incorrectly");
        if(!scaled)require(actual.calls==(eta*lum<1&&depth>=5?1u:0u),"PT empty-context roulette changed");
        if(!eligible)require(actual.throughput==lum*.5,"Non-roulette throughput changed");
        ++cases;
    }
    for(uint prefix:{0u,1u,3u,8u})for(uint bounce=0;bounce<16;++bounce){
        auto r=run(false,{1,0,0},1,.5,1,bounce,.125,prefix,true);
        require(r.calls==uint(bounce+prefix+1>=5),"GI secondary producer lost its whole-path depth offset");
        ++cases;
    }
    // Midpoints integrate these binary rational probabilities exactly.
    for(double primary:{0.,.125,.25,.5,1.,2.})for(double lum:{.125,.5,1.,2.}){
        double sum=0;uint alive=0;
        for(uint i=0;i<4096;++i){auto r=run(true,{primary,0,0},1,lum,1,8,(i+.5)/4096.);sum+=r.throughput/4096.;alive+=r.survived;}
        require(std::abs(sum-lum*.5)<1e-12,"One-bounce roulette expectation changed");
        require(alive>0,"Zero primary throughput lost reusable secondary radiance support");
        ++expectations;
    }
    // Enumerate both outcomes at each of six future vertices. The compensation
    // becomes the next vertex's lum; scale is never multiplied into Lo itself.
    for(double primary:{0.,.25,.5,1.,2.}){
        double probability=1,weight=.5,mean=0,expected=0;
        for(uint bounce=0;bounce<6;++bounce){
            mean+=probability*weight;expected+=.5*std::pow(.5,int(bounce));
            auto r=run(true,{primary,0,0},1,weight,1,8,0.);
            double q=r.calls?min(.95f,weight*r.scale):1.;
            probability*=q;weight=r.throughput;
        }
        require(std::abs(mean-expected)<1e-12,"Repeated roulette changed the path expectation");
        ++expectations;
    }
    // Retain the production 0.95f cap, including its float representation.
    auto capped=run(true,{1,0,0},1,.99,1,8,.96);
    require(!capped.survived&&capped.calls==1,"Existing 0.95 roulette cap changed");
    require(run(true,{NAN,.5,0},1,1,1,8,0).scale==.5,"Finite components lost during scale sanitization");
    require(run(true,{INFINITY,-INFINITY,NAN},1,1,1,8,0).scale==.25,"Invalid components did not retain positive scale floor");
    require(run(true,{0,.75,.125},1,1,1,8,0).scale==.75,"Component maximum changed");
    std::cout<<"PASS "<<cases<<" actual guard/scale cases + "<<expectations<<" discrete path expectations; PT unchanged\n";
}
'''

SP_CPP = r'''

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using Float=double;using Float2=double;using Float3=double;using RadType3Var=double;
using Uint=unsigned;using uint=unsigned;using Bool=bool;
struct TriangleHitVar{};struct Interaction{};
#define $if if
#define $break throw Stopped{}
struct Stopped{};
template<class A,class B>double min(A a,B b){return std::min(double(a),double(b));}
double make_float3(double v){return v;}double make_float2(double v){return v;}
void require(bool condition,const char*message){if(!condition)throw std::runtime_error(message);}
__HIT_CONTEXT__;
struct Sampler{double u;uint calls=0;double next_1d(){++calls;return u;}};
struct Result{bool survived;uint calls;double throughput;};
Result roulette(const HitContext&hc,uint bounces,double lum,double eta_scale,double u){
    uint minimum=5;uint*min_depth_=&minimum;double threshold=1;double*rr_threshold_=&threshold;
    Sampler rng{u};Sampler*sampler=&rng;double throughput=lum*.5;
    try{__GUARD__}catch(Stopped&){return{false,rng.calls,0};}
    return{true,rng.calls,throughput};
}
struct RayState{struct Ray{struct DirMax{double w=.01;}dir_max;}ray;};
constexpr double ray_t_max=100000;
struct Integrator {
    uint prefix{},budget{},bounce{},calls{};double expected_throughput{},eta{},u{};bool lobes{},covered{};
    Result result{};
    template<class Env>
    Float3 Li(RayState ray,Float scatter_pdf,uint remaining,double throughput,bool only_direct,HitContext&hc,Env&){
        ++calls;
        require(hc.rr_depth_offset&&*hc.rr_depth_offset==prefix,"SP producer omitted or miscounted the global RR prefix");
        require(hc.rr_survival_scale==nullptr,"SP full prefix throughput must not receive GI primary scale again");
        require(remaining==budget-prefix&&bounce<remaining,"Residual path budget changed");
        require(throughput==expected_throughput,"SP prefix throughput was changed");
        require(ray.ray.dir_max.w==ray_t_max,"Residual traversal max was not restored");
        require(!only_direct&&hc.complete_terminal_direct&&hc.restir_direct_split,"Residual terminal/direct contract changed");
        require(hc.suppress_initial_emission_if&&*hc.suppress_initial_emission_if==covered,"Covered emission suppression changed");
        require(hc.Ld!=nullptr&&hc.split()==lobes,"Residual lobe channel ownership changed");
        require(bool(hc.direct_specular)==lobes&&bool(hc.bypass)==lobes&&bool(hc.lobe_hitdist)==lobes,"Residual lobe pointers changed");
        result=roulette(hc,bounce,throughput,eta,u);
        return result.throughput;
    }
};
struct Caller {
    bool lobes{};Integrator recorder{};
    Integrator*integrator(){return &recorder;}
    Result run(uint depth,uint total_depth,double throughput,Bool covered,uint bounce,double eta,double u){
        recorder={depth,total_depth,bounce,0,throughput,eta,u,lobes,covered};RayState ray;
        __PRODUCER__
        require(recorder.calls==1,"Residual Li call count changed");
        return recorder.result;
    }
};
int main(){
    uint cases=0,expectations=0;
    for(uint depth:{0u,1u,4u,5u,15u})for(uint budget:{16u,32u})
    for(uint bounce=0;bounce<budget-depth;++bounce)for(double lum:{.125,.5,1.,2.})
    for(double eta:{1.,4.})for(bool lobes:{false,true})for(bool covered:{false,true}){
        Caller caller;caller.lobes=lobes;
        auto result=caller.run(depth,budget,lum,covered,bounce,eta,.0625);
        bool eligible=eta*lum<1&&depth+bounce>=5;
        require(result.calls==(eligible?1u:0u),"Residual roulette starts at the wrong whole-path depth");
        if(!eligible)require(result.throughput==lum*.5,"Ineligible roulette changed throughput");
        ++cases;
    }
    for(uint depth:{0u,1u,4u,5u,15u})for(double lum:{.125,.25,.5,.75}){
        Caller caller;double expected=lum*.5,sum=0;uint alive=0;
        uint bounce=depth<5?5-depth:0;
        for(uint i=0;i<256;++i){
            auto result=caller.run(depth,32,lum,false,bounce,1,(i+.5)/256.);
            sum+=result.throughput/256.;alive+=result.survived;
        }
        require(std::abs(sum-expected)<1e-12,"Residual roulette changed expected path weight");
        require(alive==uint(256*lum),"Residual roulette applied a second primary survival scale");
        ++expectations;
    }
    HitContext pt;
    for(uint bounce=0;bounce<16;++bounce)for(double lum:{.125,.5,1.,2.}){
        const auto result=roulette(pt,bounce,lum,1,.0625);
        require(result.calls==uint(lum<1&&bounce>=5),"PT default roulette changed");
        ++cases;
    }
    std::cout<<"PASS "<<cases<<" actual producer/guard cases and "<<expectations<<" exact expectation cases; full prefix weight, remaining budget, lobe/terminal flags and PT default retained\n";
}
'''


def _roulette_block():
    source = (ROOT/'vision/src/base/integral/integrator.cpp').read_text(encoding='utf-8')
    start = source.index('        Uint rr_depth = bounces;')
    guard = source.index('        $if(eta_scale * ', start)
    return source[start:balanced(source, guard)]


def test_gi_roulette_depth_scale_and_expectation():
    gi = (ROOT/'vision/src/render_core/integrator/ReSTIR/indirect.cpp').read_text(encoding='utf-8')
    start = gi.index('        Uint rr_depth_offset = prefix_depth + 1u;')
    end = gi.index('        Uint suffix_depth = ', start)
    assignment = gi[start:end]
    interaction = (ROOT/'vision/src/base/scattering/interaction.h').read_text(encoding='utf-8')
    hit = interaction[interaction.index('OC_STRUCT(vision,HitBSDF,'):]
    methods = {name: ''.join(body(hit, '[[nodiscard]] Float3 '+name+'()'))
               for name in ('throughput', 'safe_throughput')}
    program = (GI_CPP.replace('__ASSIGN__', assignment).replace('__BLOCK__', _roulette_block())
               .replace('__THROUGHPUT__', methods['throughput'])
               .replace('__SAFE_THROUGHPUT__', methods['safe_throughput']))
    compile_run(program, 'gi-roulette')


def test_sp_residual_roulette_budget_and_lobe_contract():
    stable = (ROOT/'vision/src/render_core/integrator/ReSTIR/stable_planes.cpp').read_text(encoding='utf-8')
    start = stable.index('                Float3 direct = make_float3(0.f);', stable.index('Kernel fill_kernel'))
    call = stable.index('Float3 total = integrator()->Li(', start)
    producer = stable[start:stable.index(';', call)+1]
    interaction = (ROOT/'vision/src/base/scattering/interaction.h').read_text(encoding='utf-8')
    context = ''.join(body(interaction, 'struct HitContext'))
    program = (SP_CPP.replace('__HIT_CONTEXT__', context).replace('__PRODUCER__', producer)
               .replace('__GUARD__', _roulette_block()))
    compile_run(program, 'sp-residual')
