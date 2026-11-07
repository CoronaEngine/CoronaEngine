"""Execute actual GI proxy/shared-receiver controls plus exact joint sampling.

CPU scalar fixtures do not establish Vulkan compilation or image quality.
"""
import re
from _restir_cpu import ROOT, body, compile_run
from _restir_gi_cpu import GI_CPP as CPP, functions

DIRECTORY = ROOT / 'vision/src/render_core/integrator/ReSTIR'
MAIN = r'''

struct HitBSDF{Cell bsdf{1};double pdf=.5;};
GIReservoir fresh(GISampleVar sample,HitBSDF hit_bsdf){__FRESH__ return rsv;}
namespace ocarina {using string=std::string;struct ByteBufferDesc{uint h;size_t offset,size;uint head()const{return h;}};}
struct GIReservoirStorage{static constexpr const char*Name=__NAME__;uint words[11];};
__MATCHER__
void require(bool ok,const char*m){if(!ok)throw std::runtime_error(m);}
void close(double a,double b,const char*m){require(std::abs(a-b)<1e-10,m);}
GIReservoir sample_rsv(double C,double W,double Lo=1){
 GIReservoir r;r.C=C;r.W=W;r.sample.sp={10,true};r.sample.Lo.value=Lo;r.weight_sum=C*Lo*.5*W;return r;
}
int main(){
 uint cases=0;State s;state=&s;Interaction it;it.pos=1;it.id=0;
 GISample sample;sample.sp={10,true};sample.Lo.value=3;
 for(double f:{0.,.25,1.,8.})for(double pdf:{0.,-1.,.125,.5,2.,double(INFINITY),double(NAN)})for(double Lo:{0.,1.,3.}){
  s=State{};s.f[0]=f;s.pdf[0]=pdf;sample.Lo.value=Lo;
  double actual=candidate::compute_p_hat(it,sample);
  double expected=pdf>0&&std::isfinite(pdf)?Lo*pdf:0;
  close(actual,expected,"Target is not finite-positive PDF times Lo, independent of F");
  require(s.all_calls==0,"Target evaluated full F instead of PDF only");
  ++cases;
 }
 for(bool valid:{false,true})for(double pdf:{0.,-1.,.125,.5,2.,double(INFINITY),double(NAN)})for(double Lo:{0.,1.,3.}){
  sample.sp.active=valid;sample.Lo.value=Lo;auto r=fresh(sample,HitBSDF{{0},pdf});
  close(r.C,1,"Fresh zero target dropped stream C");
  bool positive=valid&&pdf>0&&std::isfinite(pdf)&&Lo>0;
  close(r.W,positive?1/pdf:0,"Fresh target does not preserve1/pdf on its positive domain");
  close(r.weight_sum,positive?Lo:0,"Fresh stream weight is inconsistent with PDF proxy");++cases;
 }
 sample.sp.active=true;sample.Lo.value=1;
 for(double pdf:{0.,-1.,.125,.5,2.,double(INFINITY),double(NAN)})for(bool occluded:{false,true})for(bool jacobian:{false,true}){
  s=State{};s.f[1]=0;s.pdf[1]=pdf;s.occluded=occluded;s.jacobian=jacobian?1:0;
  Interaction source;source.id=1;source.pos=2;
  double support=candidate::selected_sample_support(it,source,sample);
  close(support,pdf>0&&std::isfinite(pdf)&&!occluded&&jacobian?1:0,"Source support retained old F gate or lost PDF/visibility/Jacobian gate");
  require(s.all_calls==0,"Source support evaluated full F instead of PDF only");++cases;
 }
 for(bool normal:{false,true})for(bool zero_donor:{false,true})for(bool occluded:{false,true})for(bool jacobian:{false,true}){
  s=State{};s.normal=normal;s.f[1]=0;s.pdf[1]=.25;s.occluded=occluded;s.jacobian=jacobian?1:0;
  reservoirs[0]=sample_rsv(1,2);reservoirs[1]=sample_rsv(2,zero_donor?0:4,zero_donor?0:1);
  SurfaceData current{0,false},other{1,false};auto history=reservoirs[1];
  auto temporal=candidate::combine_temporal(reservoirs[0],current,history,&other,0,0);
  double denominator=(!occluded&&jacobian)?3:1;
  double numerator=2+(!zero_donor&&!occluded&&jacobian?8:0);
  close(temporal.C,3,"Temporal C dropped zero/occluded stream");
  close(temporal.W,numerator/denominator,"Temporal target/support normalization inconsistent");
  require(s.draws==2&&s.builds[0]==1&&s.max_frame[0]<=1,"Temporal RNG or shared receiver lifetime changed");
  require(s.all_calls==0,"Temporal performed full F evaluation");
  auto rng=s.rng;
  s=State{};s.normal=normal;s.f[1]=0;s.pdf[1]=.25;s.occluded=occluded;s.jacobian=jacobian?1:0;
  // An intentionally stale canonical sum must be rebuilt from current g*C*W.
  reservoirs[0].weight_sum=123;
  auto spatial=candidate::constant_combine(reservoirs[0],Container<uint>{{1}});
  close(spatial.C,3,"Spatial C dropped zero/occluded stream");
  close(spatial.W,numerator/denominator,"Spatial canonical weight_sum was not rebased to proxy");
  require(s.draws==1&&s.builds[0]==1&&s.max_frame[0]<=1,"Spatial RNG or shared receiver lifetime changed");
  require(s.all_calls==0,"Spatial performed full F evaluation");++cases;
 }
 s=State{};reservoirs[0]=sample_rsv(3,2);reservoirs[0].weight_sum=999;
 auto alone=candidate::constant_combine(reservoirs[0],Container<uint>{{}});
 close(alone.W,2,"No-donor canonical rebase failed");close(alone.C,3,"No-donor C changed");
 s=State{};s.material=false;reservoirs[0]=sample_rsv(1,2);reservoirs[1]=sample_rsv(2,4);
 auto null_receiver=candidate::constant_combine(reservoirs[0],Container<uint>{{1}});
 close(null_receiver.C,3,"Null current receiver dropped represented stream confidence");
 close(null_receiver.W,0,"Null receiver acquired a positive proxy weight");
 require(s.builds[0]==0&&s.draws==1,"Null receiver fallback changed material or RNG behavior");++cases;
 // Final shading must still use exact F, not proxy/PDF. No extra RNG.
 for(double f:{0.,.25,1.,8.}){
  s=State{};s.f[0]=f;double spec=0,distance=0;
  auto r=sample_rsv(1,2,3);double value=candidate::shading(r,SurfaceData{0,false},&spec,&distance);
  close(value,6*f,"Final shading no longer uses exact F*Lo*W");
  require(s.all_calls==1&&s.pdf_calls==0&&s.draws==0,"Exact shading mode or RNG changed");++cases;
 }
 ocarina::ByteBufferDesc a{9,0,44*3},b{9,44*3,44*3};
 require(!gi_reservoir_storage_matches(__OLD_NAME__,a,b,3),"Old exact-target history ABI accepted");
 require(gi_reservoir_storage_matches(GIReservoirStorage::Name,a,b,3),"New PDF target history ABI rejected");
 std::cout<<"PASS "<<cases<<" actual target/fresh/support/combine/shading cases; old history rejected, C/RNG retained\n";
}
'''


def make_program(source,storage):
    base=CPP
    base=base.replace('#include <vector>','#include <vector>\n#include <string>')
    base=base.replace('unsigned rays=0;', 'unsigned rays=0,all_calls=0,pdf_calls=0; double jacobian=1;\n    std::array<double,8> f{1,1,1,1,1,1,1,1},pdf{.5,.5,.5,.5,.5,.5,.5,.5};')
    base=base.replace('enum class MaterialEvalMode {All};','enum class MaterialEvalMode {All,PDF};')
    old='''ScatterEval evaluate(double,double,MaterialEvalMode)const {
        ScatterEval e(1);e.f.value=(1.+frame*.2)*(1.+id*.1);e.density=.5;return e;
    }'''
    new='''ScatterEval evaluate(double,double,MaterialEvalMode mode)const {
        ScatterEval e(1);e.density=state->pdf[id];
        if(mode==MaterialEvalMode::All){state->all_calls++;e.f.value=state->f[id];}
        else state->pdf_calls++;return e;
    }'''
    assert old in base;base=base.replace(old,new)
    base=base.replace('p.pos!=b?1:0;', 'p.pos!=b?state->jacobian:0;')
    reservoir=(DIRECTORY/'reservoir.h').read_text(encoding='utf-8')
    rsv=reservoir[reservoir.index('struct GIReservoir {'):]
    replacements={
        '__CAL_WEIGHT__':body(rsv,'cal_weight(')[1],
        '__SAFE_WEIGHT__':body(rsv,'static auto safe_weight')[1].replace('oc_float<p_>','double'),
        '__UPDATE__':body(rsv,'Bool update(')[1].replace('oc_float<p>','double'),
        '__CAL_W__':body(rsv,'cal_W(')[1].replace('oc_float<p>','double'),
        '__UPDATE_W__':body(rsv,'void update_W(')[1],
        '__UPDATE_SUPPORT__':body(rsv,'void update_W_with_support(')[1].replace('oc_float<p>','double').replace('vision::','')}
    for key,value in replacements.items():base=base.replace(key,value)
    initial=body(source,'void ReSTIRGI::compile_temporal_reuse()')[1]
    begin=initial.index('        GIReservoirVar rsv;')
    fresh=initial[begin:initial.index('        Float2 motion_vec',begin)]
    name=re.search(r'static constexpr const char \*Name = ("[^"]+");',storage).group(1)
    old_name='"ReSTIRGI::reservoirs_ gi44-oct16-v1 x 2"'
    sig,matcher=body(storage,'[[nodiscard]] inline bool gi_reservoir_storage_matches(')
    return (base+functions(source,'candidate')+MAIN.replace('__FRESH__',fresh)
            .replace('__NAME__',name).replace('__OLD_NAME__',old_name)
            .replace('__MATCHER__',sig+matcher))


def test_gi_pdf_target_and_shared_receiver_controls():
    source = (DIRECTORY/'indirect.cpp').read_text(encoding='utf-8')
    storage = (DIRECTORY/'gi_reservoir_storage.h').read_text(encoding='utf-8')
    compile_run(make_program(source, storage), 'gi-target')


from collections import defaultdict
from dataclasses import dataclass, replace
from fractions import Fraction as F
from itertools import product

@dataclass(frozen=True)
class Reservoir:
    sample: int | None
    W: F
    C: F=F(1)

def joint(distributions):
    for draws in product(*(d.items() for d in distributions)):
        values,probabilities=zip(*draws)
        probability=F(1)
        for p in probabilities:probability*=p
        yield values,probability

def fresh(proposal,target):
    result=defaultdict(F)
    for i,p in enumerate(proposal):
        if p:
            r=Reservoir(i,1/p) if target[i]>0 else Reservoir(None,F(0))
            result[r]+=p
    return result

def merge(candidates,target,supports):
    count=sum(r.C for r in candidates)
    weights=[r.C*target[r.sample]*r.W if r.sample is not None else F(0) for r in candidates]
    total=sum(weights)
    if not total:return{Reservoir(None,F(0),count):F(1)}
    result=defaultdict(F)
    for r,w in zip(candidates,weights):
        if w:
            normalization=sum(source.C*support[r.sample] for source,support in zip(candidates,supports))
            result[Reservoir(r.sample,total/(normalization*target[r.sample]),count)]+=w/total
    return result

def one_stage(proposals,targets,exact,include_realized_null_counts=True):
    mean=F(0);mass=F(0)
    distributions=[fresh(p,g) for p,g in zip(proposals,targets)]
    support=[tuple(int(q>0 and g>0) for q,g in zip(p,target)) for p,target in zip(proposals,targets)]
    for draws,p in joint(distributions):
        if not include_realized_null_counts:
            draws=tuple(replace(r,C=F(0)) if r.sample is None else r for r in draws)
        for r,conditional in merge(draws,targets[0],support).items():
            mass+=p*conditional
            mean+=p*conditional*(exact[r.sample]*r.W if r.sample is not None else F(0))
    assert mass==1
    return mean

def repeated(proxy):
    proposals=((F(1,4),F(3,4)),(F(2,3),F(1,3)))
    exact=((F(1),F(4)),(F(3),F(2)))
    lo=(F(2),F(3))
    targets=tuple(tuple(l*q for l,q in zip(lo,p)) for p in proposals) if proxy else exact
    support=((1,1),(1,1))
    initial=[fresh(p,g) for p,g in zip(proposals,targets)]
    histories={None:F(1)};frames=[]
    for frame in range(2):
        next_histories=defaultdict(F);means=[F(0),F(0)];mass=F(0)
        for previous,ph in histories.items():
            for current,pc in joint(initial):
                temporal=[({current[i]:F(1)} if previous is None else
                           merge((current[i],replace(previous[i],C=F(1))),targets[i],support)) for i in range(2)]
                for temporals,pt in joint(temporal):
                    spatial=[merge((temporals[i],temporals[1-i],temporals[1-i]),targets[i],((1,1),)*3) for i in range(2)]
                    for finals,ps in joint(spatial):
                        probability=ph*pc*pt*ps;mass+=probability
                        next_histories[temporals]+=probability
                        for i,r in enumerate(finals):means[i]+=probability*exact[i][r.sample]*r.W
        assert mass==1 and means==[F(5),F(5)],(proxy,frame,means)
        frames.append({'frame':frame,'mean':list(map(str,means)),'history_states':len(next_histories)})
        histories=next_histories
    return frames

def test_pdf_proxy_exact_joint_expectation():
    # Half the draws are zero Lo. They still contribute C and source support
    # when the final winner is one of the two nonzero-Lo endpoints.
    proposals=((F(1,4),F(1,4),F(1,2)),(F(1,8),F(3,8),F(1,2)))
    lo=(F(2),F(3),F(0))
    proxy=tuple(tuple(l*q for l,q in zip(lo,p)) for p in proposals)
    exact=(F(1),F(4),F(0))
    good=one_stage(proposals,proxy,exact)
    dropped=one_stage(proposals,proxy,exact,False)
    assert good==5 and dropped!=5
    partial=((F(1,2),F(1,2),F(0)),(F(0),F(1,2),F(1,2)))
    partial_targets=tuple(tuple(l*q for l,q in zip((F(2),F(3),F(4)),p)) for p in partial)
    asymmetric=one_stage(partial,partial_targets,exact)
    assert asymmetric==5
    # A has zero integrand at this selected spectrum but positive proposal PDF;
    # B has unit integrand. Positive proxy weights both streams, so using the
    # OLD source F>0 support test halves the denominator and doubles the answer.
    candidates=(Reservoir(0,F(1)),Reservoir(0,F(1)))
    correct=next(iter(merge(candidates,(F(1),),((1,),(1,)))))
    stale=next(iter(merge(candidates,(F(1),),((1,),(0,)))))
    assert correct.W==1 and stale.W==2
    # Enumerate full joint histories, including duplicate spatial donors, and
    # assert the exact mean at each frame inside repeated(). No artifacts or
    # old source snapshots are needed by this mathematical regression.
    repeated(False)
    repeated(True)
