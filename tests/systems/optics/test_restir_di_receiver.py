"""Actual DI RIS/Li controls with mutating normal-map and deterministic RNG boundaries."""
from _restir_cpu import ROOT, balanced, compile_run

def control(source):
    start = source.index('DIReservoirVar ReSTIRDI::RIS(')
    end = source.index('Float ReSTIRDI::neighbor_pairwise_MIS', start)
    function = source[start:end]
    first = function.index('$if(hit) {')
    last = balanced(function, first)
    return function[first:last], function[:first] + '<control>' + function[last:]


def li_block(source, signature):
    start = source.index(signature)
    first = source.index('if (!bsdf) {', start)
    end = balanced(source, first)
    assert source[end:end+7] == ' else {'
    return source[first:balanced(source, end)]


CPP = r'''

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>
using uint = unsigned;
using Uint = unsigned;
using std::addressof;
#define $if if
#define $for(i, n) for (uint i = 0; i < uint(n); ++i)
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct Hit { double wi=0, f=0, pdf=-1; bool operator==(const Hit&) const = default; };
struct State {
    bool normal=false, dispersive=false, support=true, separate=false, srgb=false;
    uint max_evaluator_frame=0, builds=0, checks=0, flag=0, light_count=0, bsdf_count=0, outlines=0;
    std::uint64_t random=12345, draws=0;
    std::vector<double> values;
    Hit hit;
};
State *state;
struct Spectrum { std::string_view impl_type() const { return state->srgb ? "srgb" : "spectral"; } };
struct Interaction { mutable uint shading=0; double wo=1; uint material_id() const { return 0; } };
struct Sampler {
    double next_1d() { state->random=state->random*6364136223846793005ULL+1442695040888963407ULL;
        state->draws++; return double((state->random >> 32) & 65535) / 65536.; }
};
struct Eval { double f=0, density=0; double pdf() const { return density; } };
struct BSDFSample { Eval eval; double wi=0; };
struct MaterialEvaluator;
struct Wavelengths {
    mutable double secondary_pdf=1;
    void check_dispersive(const Spectrum *, const MaterialEvaluator&) const {
        state->checks++;
        if (state->dispersive) secondary_pdf=0;
    }
};
struct MaterialEvaluator {
    uint frame;
    MaterialEvaluator(const Interaction& it,const Wavelengths&) : frame(it.shading) {}
    void update_frame(uint shading) { frame=shading; }
    uint flag() const { return 17; }
    Eval evaluate(double wo,double wi) const { return {double(frame+1)*(wo+wi),state->support ? .25 : 0}; }
    BSDFSample sample(double wo,Sampler& sampler) const {
        double wi=sampler.next_1d()+sampler.next_1d()+sampler.next_1d();
        return {evaluate(wo,wi),wi};
    }
};
struct Material {
    MaterialEvaluator create_evaluator(const Interaction& it,const Wavelengths& swl) const {
        MaterialEvaluator evaluator(it,swl);
        build_evaluator(evaluator,it,swl);
        evaluator.update_frame(it.shading);
        return evaluator;
    }
    void build_evaluator(MaterialEvaluator&,const Interaction& it,const Wavelengths&) const {
        state->builds++;
        if(state->normal) it.shading++;
        state->max_evaluator_frame=std::max(state->max_evaluator_frame,it.shading);
    }
};
struct Materials { template<class F> void dispatch(uint,F f) { Material m; f(&m); } };
struct Scene { Materials materials() { return {}; } };
Scene scene() { return {}; }
struct Integrator { bool separate() { return state->separate; } };
Integrator *integrator() { static Integrator i; return &i; }
template<class F> void outline(std::string_view name,F f) {
    if(name=="ReSTIRDI::RIS build evaluator") state->outlines++;
    f();
}
double actual_light_dispatch(MaterialEvaluator *bsdf,const Interaction& it,const Wavelengths& swl,
                             double wi) {
    const Spectrum *spectrum=nullptr;
    double f=0;
    struct { struct { double L=2; } eval; } ls;
    Eval eval;
    __LIGHT_DISPATCH__
    return f;
}
BSDFSample actual_bsdf_dispatch(MaterialEvaluator *bsdf,const Interaction& it,const Wavelengths& swl,
                                Sampler& sampler,Uint *flag) {
    const Spectrum *spectrum=nullptr;
    BSDFSample result;
    BSDFSample *bs=&result;
    __BSDF_DISPATCH__
    return result;
}
struct Result { State s; uint frame; double secondary_pdf; };
template<class F> Result run(F control,uint M_light,uint M_bsdf,bool hit,State input) {
    State storage=input; state=&storage;
    Interaction it; Wavelengths swl; Sampler sampler; Spectrum spectrum;
    auto sample_light = [&](MaterialEvaluator *bsdf) {
        double wi=sampler.next_1d()+sampler.next_1d()+sampler.next_1d();
        storage.values.push_back(actual_light_dispatch(bsdf,it,swl,wi));
        (void)sampler.next_1d(); // unchanged reservoir replacement draw
        storage.light_count++;
    };
    auto sample_bsdf = [&](MaterialEvaluator *bsdf) {
        auto bs=actual_bsdf_dispatch(bsdf,it,swl,sampler,&storage.flag);
        storage.hit={bs.wi,bs.eval.f,bs.eval.pdf()};
        storage.values.push_back(bs.eval.f);
        (void)sampler.next_1d();
        storage.bsdf_count++;
    };
    control(hit,M_light,M_bsdf,M_light,M_bsdf,it,swl,&spectrum,sample_light,sample_bsdf);
    return {storage,it.shading,swl.secondary_pdf};
}

int main(){
 auto actual=[](bool hit,uint M_light,uint M_bsdf,uint M_light_,uint M_bsdf_,
               const Interaction&it,const Wavelengths&swl,const Spectrum *spectrum,auto sample_light,auto sample_bsdf){
  __CONTROL__
 };
 uint cases=0;
 for(uint ml:{0u,1u,2u,10u})for(uint mb:{0u,1u,3u})for(bool hit:{false,true})
 for(bool normal:{false,true})for(bool disp:{false,true})for(bool support:{false,true})
 for(bool separate:{false,true})for(bool srgb:{false,true}){
  State input;input.normal=normal;input.dispersive=disp;input.support=support;input.separate=separate;input.srgb=srgb;
  auto now=run(actual,ml,mb,hit,input);
  uint count=hit?ml+mb:0,builds=hit?(separate||srgb?1:ml+mb):0;
  require(now.s.builds==builds&&now.s.checks==builds,"Evaluator lifetime or dispersion-check count changed");
  require(now.s.outlines==uint(hit&&separate),"Separated RIS must export the evaluator from its material case");
  require(now.frame==(normal&&hit&&separate?1u:0u),"Material normal mutation escaped its private Li receiver");
  require(now.s.max_evaluator_frame==(normal&&builds?1u:0u),"Repeated Li calls accumulate a normal map");
  require(now.secondary_pdf==(disp&&builds?0:1),"Wavelength dispersion state changed");
  require(now.s.light_count==(hit?ml:0)&&now.s.bsdf_count==(hit?mb:0),"RIS proposal counts changed");
  require(now.s.draws==4*count&&now.s.values.size()==count,"RIS RNG budget changed");
  std::uint64_t rng=12345;
  auto draw=[&]{rng=rng*6364136223846793005ULL+1442695040888963407ULL;return double((rng>>32)&65535)/65536.;};
  Hit last;
  for(uint proposal=0;proposal<count;++proposal){
   double wi=draw()+draw()+draw();
   double f=(normal?2.:1.)*(1+wi);
   bool light=proposal<ml;
   require(std::abs(now.s.values[proposal]-f*(light?2.:1.))<1e-12,"Proposal order or fresh mapped-frame BSDF changed");
   if(!light)last={wi,f,support?.25:0};
   (void)draw();
  }
  require(now.s.random==rng,"RIS RNG stream changed");
  require(now.s.hit==last&&now.s.flag==(hit&&mb?17u:0u),"Last HitBSDF proposal/support/flag changed");
  ++cases;
 }
 std::cout<<"PASS "<<cases<<" actual RIS/Li cases; receiver frame isolated, RNG/dispersion/proposal support preserved\n";
}
'''


def test_di_proposals_use_private_receiver_frames():
    source = (ROOT/'vision/src/render_core/integrator/ReSTIR/direct.cpp').read_text(encoding='utf-8')
    control_block, _ = control(source)
    program = CPP.replace('__CONTROL__', control_block)
    for token, signature in {
        '__LIGHT_DISPATCH__': 'SampledSpectrum ReSTIRDI::Li(const Interaction &it, MaterialEvaluator *bsdf, const DISampleVar &sample,',
        '__BSDF_DISPATCH__': 'SampledSpectrum ReSTIRDI::Li(const Interaction &it, MaterialEvaluator *bsdf, DISampleVar *sample,'
    }.items():
        program = program.replace(token, li_block(source, signature))
    compile_run(program, 'di-receiver')
