"""Execute production GI stable-emission ownership predicates on CPU.

Finite Fresnel/RR outcomes verify deterministic-prefix replacement preserves
the mean while stochastic/pruned suffixes remain sampled. No GPU is launched.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

from _restir_cpu import body
from test_gi_ideal_glass import ROOT, HEADER, PROGRAM

TEMP = ROOT / '.workspace/tmp/gi-guide-reuse-20261119/emission-tests'


def test_shared_pt_skips_null_emitter_before_continuing_glass_and_material_emission_callbacks():
    source = (ROOT / 'vision/src/base/integral/integrator.cpp').read_text(encoding='utf-8-sig')
    start = source.index('$if(!it.has_material() && !it.has_phase())')
    terminal = 'prev_surface_ng = it.ng;'
    end = source.index(terminal, start) + len(terminal)
    # Keep the entire real ordered region, including its null continuation,
    # emitter evaluation, observer invocation and radiance accumulation.
    emission_region = source[start:end]
    prefix = PROGRAM[:PROGRAM.index('__HELPERS__')]
    policy = HEADER.read_text(encoding='utf-8-sig')
    for signature in ('Bool gi_camera_emission_owned_by_di(', 'Float gi_camera_emission_bsdf_weight('):
        declaration, implementation = body(policy, signature)
        prefix += declaration + implementation
    shim = r'''
#include <functional>
#include <optional>
#define $super_continue continue
constexpr int D=0;
namespace ocarina {using std::isinf;}
Float3 operator*(Float3 a,Float3 b){return{a.x*b.x,a.y*b.y,a.z*b.z};}
Float3&operator+=(Float3&a,Float3 b){a.x+=b.x;a.y+=b.y;a.z+=b.z;return a;}
SampledSpectrum operator*(SampledSpectrum a,SampledSpectrum b){return{a.vec3()*b.vec3()};}
void comment(const char*){}
Float3 make_RadType3(Float3 v){return v;}
struct RayState {unsigned null_steps=0;Float3 direction()const{return make_float3(0,0,-1);}Float3 origin()const{return make_float3(0);}};
struct Interaction {
    bool material=true,emissive=false;Float3 pos=make_float3(0),ng=make_float3(0,0,1);
    bool has_material()const{return material;}bool has_phase()const{return false;}bool has_emission()const{return emissive;}
    RayState spawn_ray_state(Float3)const{RayState r;r.null_steps=1;return r;}
};
using TriangleHitVar=unsigned;
struct Capture {
    bool record_radiance=true,active=false;uint receiver_bounce=0;Float3 radiance=make_float3(0),beta=make_float3(1);
    bool first_scatter_transmission=false,first_reflection_covered_by_di=true;
    std::function<void(const Interaction&)>on_endpoint;
    std::function<void(const TriangleHitVar&,const Float3&,const Uint&,const Float3&,const Float3&)>on_emission;
};
struct Counters{struct{uint y=0,z=0;}path;};
struct HitContext{Counters*work_counters=nullptr;Interaction*it=nullptr;Capture*path_capture=nullptr;
    bool restir_direct_split=true;Float3*Ld=nullptr,*bypass=nullptr;};
struct LightSampleContext{Float3 pos,ng;};
struct LightEval{SampledSpectrum L=SampledSpectrum{7.f};float pdf=.25f;};
struct LightSampler{LightSampler*operator->(){return this;}LightEval evaluate_hit_wi(LightSampleContext,const Interaction&,int){return{};}};
struct Geometry{SampledSpectrum Tr(int,int,int,RayState){return{1.f};}};
int scene(){return 0;}struct Renderer{int sampler(){return 0;}};Renderer renderer(){return{};}
struct Spectrum{SampledSpectrum one(){return{1.f};}Float3 linear_srgb(SampledSpectrum s,int){return s.vec3();}};Spectrum*spectrum(){static Spectrum s;return&s;}
template<int>float MIS_weight(float bsdf_pdf,float light_pdf){
    if(bsdf_pdf!=.96f||light_pdf!=.25f)throw std::runtime_error("emitter MIS received the wrong PDFs");
    return .5f;
}
float correct_bsdf_weight(float w,uint){return w;}
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
void run(bool delta){
    // A null emissive boundary precedes a continuing non-emissive glass
    // interface, then a material emitter. No participating medium/phase is
    // installed, matching the GI compile-time requirement.
    std::array<Interaction,3> surfaces;surfaces[0].material=false;surfaces[0].emissive=true;surfaces[2].emissive=true;
    RayState rs;Capture capture;HitContext hc;hc.path_capture=&capture;
    unsigned calls=0,observed_hit=99,observed_bounce=99;Float3 observed=make_float3(0);
    capture.on_emission=[&](unsigned hit,Float3,uint b,Float3 L,Float3){++calls;observed_hit=hit;observed_bounce=b;observed+=L;};
    LightSampler light_sampler;Geometry geometry;int swl=0;unsigned bounces=2;
    std::optional<bool>gi_previous_delta=delta;float scatter_pdf=.96f;bool include_emission=true;
    const bool reflection_only_direct=false; // Actual ordinary PT host opt-out.
    SampledSpectrum throughput{1.f};Float3 spec_route=make_float3(1),ret=make_float3(0),prev_surface_ng=make_float3(0,0,1);
    auto route_spec=[](Float3,unsigned,Float3,float,bool){};
    for(unsigned hit=0;hit<surfaces.size();++hit,++bounces){
        Interaction it=surfaces[hit];
        __EMISSION_REGION__
    }
    require(rs.null_steps==1,"null boundary did not continue the sampled ray");
    require(calls==1&&observed_hit==2&&observed_bounce==3,"null emitter invoked callback or consumed a physical bounce");
    const float expected=delta?7.f:3.5f;
    require(observed.x==expected&&ret.x==expected,"material-emitter MIS weight changed");
}
int main(){try{
    run(false);run(true);
    std::cout<<"PASS actual shared PT null-emitter skip, glass continuation, one material callback and unchanged energy\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''
    TEMP.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix='null-emitter-', dir=TEMP))
    cpp, binary = folder / 'test.cpp', folder / 'test.exe'
    cpp.write_text(prefix + shim.replace('__EMISSION_REGION__', emission_region), encoding='utf-8')
    result = subprocess.run([shutil.which('g++'), '-std=c++20', '-O2', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_consumed_primary_keeps_denoiser_guide_without_becoming_gi_receiver():
    source = HEADER.read_text(encoding='utf-8-sig')
    helpers = []
    for signature in ('Bool gi_prefix_guide_eligible(', 'Bool gi_prefix_receiver_eligible('):
        assert signature in source, f'GI prefix guide/receiver ownership missing: {signature}'
        declaration, implementation = body(source, signature)
        helpers.append(declaration + implementation)
    prefix = PROGRAM[:PROGRAM.index('__HELPERS__')]
    checks = r'''
int main(){
    struct Case{bool hit,consumed,remainder;uint plane;bool guide,receiver;};
    Case cases[]={{true,true,false,0,true,false},{true,true,true,0,true,false},
        {true,true,false,1,false,false},{true,true,true,2,false,false},
        {true,false,true,0,true,true},{true,false,true,1,true,true},
        {true,false,false,2,true,false},{false,false,true,0,false,false},
        {false,true,true,0,false,false}};
    for(auto c:cases){
        if(gi_prefix_guide_eligible(c.hit,c.consumed,c.plane)!=c.guide)return 1;
        if(gi_prefix_receiver_eligible(c.hit,c.consumed,c.remainder)!=c.receiver)return 2;
    }
    std::cout<<"PASS consumed-primary guide kept, consumed receivers excluded, secondary and miss policies\n";
}
'''
    TEMP.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix='primary-guide-', dir=TEMP))
    cpp, binary = folder / 'test.cpp', folder / 'test.exe'
    cpp.write_text(prefix + '\n'.join(helpers) + checks, encoding='utf-8')
    result = subprocess.run([shutil.which('g++'), '-std=c++20', '-O2', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_production_stable_emission_ownership_preserves_fresnel_rr_mean_and_direct_split():
    source = HEADER.read_text(encoding='utf-8-sig')
    helpers = []
    for signature in ('BSDFSample gi_ideal_glass_branch(',
                      'Bool gi_prefix_stable_emission_allowed(',
                      'Bool gi_prefix_emission_represented('):
        assert signature in source, f'GI stable-emission ownership missing: {signature}'
        declaration, implementation = body(source, signature)
        helpers.append(declaration + implementation)
    prefix = PROGRAM[:PROGRAM.index('__HELPERS__')]
    checks = r'''
void require(bool v,const char*message){if(!v)throw std::runtime_error(message);}
void near(double value,double expected,const char*message){
    if(!std::isfinite(value)||std::abs(value-expected)>2e-6)throw std::runtime_error(message);
}
int main(){try{
    Float3 dir=make_float3(0,0,-1);
    // The compatibility default has no first-branch identity and conservatively
    // leaves the first light with DI, as before. Explicit ownership follows below.
    for(unsigned branch:{1u,5u,6u})for(unsigned depth:{1u,2u})
        require(!gi_prefix_stable_emission_allowed(true,depth),"first physical interface emission became GI stable");
    require(gi_prefix_stable_emission_allowed(true,2u,5u,true),"first ideal transmission must be Build-owned");
    require(!gi_prefix_stable_emission_allowed(true,2u,6u,true),"covered first reflection must remain DI-owned");
    require(gi_prefix_stable_emission_allowed(true,2u,6u,false),"uncovered delta reflection must be Build-owned");
    require(gi_prefix_emission_represented(true,5u,2u,dir,5u,2u,dir,true,true),"first transmission terminal must be replaced once");
    require(!gi_prefix_emission_represented(true,6u,2u,dir,6u,2u,dir,true,true),"covered reflection must not be replaced by Build");
    require(gi_prefix_emission_represented(true,6u,2u,dir,6u,2u,dir,true,false),"uncovered delta reflection terminal must be replaced once");
    require(gi_prefix_emission_represented(true,21u,3u,dir,21u,3u,dir,true),"represented transmitted emitter missing");
    require(!gi_prefix_emission_represented(true,22u,3u,dir,21u,3u,dir,true),"pruned exit reflection was removed");
    require(!gi_prefix_emission_represented(false,21u,3u,dir,21u,3u,dir,true),"rough or post-receiver suffix emission leaked into stable");
    require(!gi_prefix_emission_represented(true,21u,3u,-dir,21u,3u,dir,true),"terminal incident direction ignored");
    require(!gi_prefix_emission_represented(true,21u,3u,dir,21u,3u,dir,false),"terminal hit identity ignored");
    require(gi_prefix_emission_represented(true,21u,3u,dir,85u,4u,-dir,false),"known deterministic ancestor must be represented");
    require(!gi_prefix_emission_represented(true,21u,5u,dir,21u,3u,dir,true),"deeper stochastic revisit was removed");
    require(!gi_prefix_emission_represented(true,0u,3u,dir,21u,3u,dir,true),"unknown branch was removed");
    // The actual exact branch helper supplies discrete probability and physical
    // transport. White-blue tint=.25 gives per-interface red weight=.5.
    SampledWavelengths swl;SampledSpectrum tint{make_float3(.25f,.64f,1.f)};
    auto entryT=gi_ideal_glass_branch(swl,make_float3(0,0,1),1.5f,tint,true);
    auto exitT=gi_ideal_glass_branch(swl,make_float3(0,0,-1),1.f/1.5f,tint,true);
    auto exitR=gi_ideal_glass_branch(swl,make_float3(0,0,-1),1.f/1.5f,tint,false);
    const double pT=entryT.eval.pdfs*exitT.eval.pdfs,pR=entryT.eval.pdfs*exitR.eval.pdfs;
    const double wT=entryT.eval.throughput().x*exitT.eval.throughput().x;
    const double wR=entryT.eval.throughput().x*exitR.eval.throughput().x;
    const double lightT=7.,lightR=3.,suffix=5.,survival=.25;
    const double physicalStable=pT*wT*lightT;
    near(physicalStable,1.6128,"two-interface physical stable energy");
    double oldMean=0,newMean=0,stableMean=0,stableVariance=0;
    for(unsigned outcome:{0u,1u,2u,3u}){
        const bool transmit=outcome==1,reflect=outcome==2;
        const double probability=outcome==0?pT*(1-survival):transmit?pT*survival:reflect?pR:1-pT-pR;
        const double sampledStable=transmit?wT*lightT/survival:0;
        const double noisyPruned=reflect?wR*lightR:0;
        const double stochasticSuffix=transmit?wT*suffix/survival:0;
        const bool remove=gi_prefix_emission_represented(true,transmit?21u:22u,3u,dir,21u,3u,dir,true);
        const double captured=remove?sampledStable:0;
        const double before=sampledStable+noisyPruned+stochasticSuffix;
        const double after=before-captured+physicalStable;
        oldMean+=probability*before;newMean+=probability*after;
        stableMean+=probability*physicalStable;
        stableVariance+=probability*std::pow(physicalStable-1.6128,2);
        if(transmit)near(after-physicalStable,stochasticSuffix,"opaque receiver suffix Lo was subtracted");
        if(reflect)near(after-physicalStable,noisyPruned,"minority pruned branch was discarded");
    }
    near(oldMean,2.8224,"independent Fresnel hand-derived old mean");
    near(newMean,oldMean,"stable replacement changed finite Fresnel/RR ensemble mean");
    near(stableMean,1.6128,"stable energy counted more than once");
    near(stableVariance,0,"deterministic prefix should have no Fresnel/RR variance");
    std::cout<<"PASS direct ownership, ancestor/endpoint matching, four Fresnel/RR leaves, exact-once stable energy and suffix independence\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''
    TEMP.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix='stable-emission-', dir=TEMP))
    cpp, binary = folder / 'test.cpp', folder / 'test.exe'
    cpp.write_text(prefix + '\n'.join(helpers) + checks, encoding='utf-8')
    result = subprocess.run([shutil.which('g++'), '-std=c++20', '-O2', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_actual_gi_emission_callback_counts_represented_energy_once_and_preserves_suffix():
    source = (ROOT / 'vision/src/render_core/integrator/restir/gi/restir_gi.cpp').read_text(encoding='utf-8-sig')
    declaration, implementation = body(source, 'capture.on_emission =')
    callback = declaration + implementation + ';'
    helpers = []
    source = HEADER.read_text(encoding='utf-8-sig')
    for signature in ('Bool gi_prefix_stable_emission_allowed(', 'Bool gi_prefix_emission_represented('):
        declaration, implementation = body(source, signature)
        helpers.append(declaration + implementation)
    terminal_source = (ROOT / 'vision/src/render_core/integrator/restir/gi/restir_gi_prefix_storage.h').read_text(encoding='utf-8-sig')
    terminal_helpers = [line for line in terminal_source.splitlines()
                        if line.startswith('constexpr uint PtPrefix')]
    for signature in ('Uint pt_prefix_depth(', 'Bool pt_prefix_ideal_path(', 'void pt_prefix_store_controls('):
        declaration, implementation = body(terminal_source, signature)
        terminal_helpers.append(declaration + implementation)
    prefix = PROGRAM[:PROGRAM.index('__HELPERS__')]
    shim = r'''
#include <bit>
#include <functional>
#define $for(index,count) for(Uint index=0;index<(count);++index)
#define $continue continue
#define $break break
template<class T,class V>T cast(V v){return static_cast<T>(v);}
struct Float2 {float x{},y{};};using float2=Float2;
Float2 operator-(Float2 a,Float2 b){return{a.x-b.x,a.y-b.y};}
float length_squared(Float2 a){return a.x*a.x+a.y*a.y;}
Float3&operator+=(Float3&a,Float3 b){a.x+=b.x;a.y+=b.y;a.z+=b.z;return a;}
struct Uint2{uint x,y;};
template<class T>T as(Uint2 v){return{std::bit_cast<float>(v.x),std::bit_cast<float>(v.y)};}
struct HitWords{uint x=7,y=8,z=std::bit_cast<uint>(.25f),w=std::bit_cast<uint>(.5f);Uint2 zw()const{return{z,w};}};
struct Float4{Float3 direction=make_float3(0,0,-1);float w=3;Float3 xyz()const{return direction;}};
struct Node{HitWords hit_words;Float4 direction_vertex;struct{uint w{};}interiors_branch;};
using ReSTIRGIPrefixPlaneVar=Node;
struct Storage{std::array<Node,3>nodes;Node read(uint i)const{return nodes[i];}};
struct TriangleHitVar{
    uint inst_id=7,prim_id=8;Float2 bary{.25f,.5f};
    const TriangleHitVar*operator->()const{return this;}bool is_miss()const{return prim_id==InvalidUI32;}bool is_hit()const{return!is_miss();}
};
namespace restir_gi{constexpr uint PrefixInvalid=~0u,PrefixQueued=~1u;}
namespace restir_gi{__TERMINAL_HELPERS__}
struct Capture{float suffix_Lo=11;bool first_reflection_covered_by_di=true;
    std::function<void(const TriangleHitVar&,const Float3&,const Uint&,const Float3&,const Float3&)>on_emission;};
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
'''
    checks = r'''
int main(){try{
    Capture capture;Storage storage;std::array<uint,3>header{1,21,6};
    for(uint p=0u;p<3u;++p)
        restir_gi::pt_prefix_store_controls(storage.nodes[p],p==0u?1u:p==2u?2u:3u,false,true);
    bool ideal_prefix=true,sampled_stable_event=false;uint sampled_branch=21;
    Float3 sampled_stable=make_float3(0);
    __CALLBACK__
    TriangleHitVar hit;Float3 dir=make_float3(0,0,-1);
    auto emit=[&](uint bounce=2){capture.on_emission(hit,dir,bounce,make_float3(7),make_float3(3));};
    auto reset=[&]{sampled_stable=make_float3(0);sampled_stable_event=false;};
    emit();require(sampled_stable.x==7&&sampled_stable_event,"actual terminal emission not captured");
    reset();sampled_branch=6;emit(1);require(sampled_stable.x==0,"covered first-BSDF reflection emission double-counted DI");
    reset();sampled_branch=5;emit(1);require(sampled_stable.x==7&&sampled_stable_event,"first transmission was not matched to Build exactly once");
    reset();sampled_branch=6;capture.first_reflection_covered_by_di=false;emit(1);
    require(sampled_stable.x==7&&sampled_stable_event,"uncovered delta reflection was not matched to Build");
    capture.first_reflection_covered_by_di=true;
    reset();sampled_branch=22;emit();require(sampled_stable.x==0,"pruned branch removed");
    reset();sampled_branch=21;hit.prim_id=9;emit();require(sampled_stable.x==0,"wrong terminal identity accepted");hit.prim_id=8;
    reset();ideal_prefix=false;emit();require(sampled_stable.x==0&&capture.suffix_Lo==11,"opaque/stochastic suffix capture changed");ideal_prefix=true;
    reset();restir_gi::pt_prefix_store_controls(storage.nodes[1],3u,false,false);emit();require(sampled_stable.x==0,"non-ideal producer path accepted");restir_gi::pt_prefix_store_controls(storage.nodes[1],3u,false,true);
    reset();header={1,85,86};for(uint p:{1u,2u})restir_gi::pt_prefix_store_controls(storage.nodes[p],4u,false,true);
    emit();require(sampled_stable.x==7&&sampled_stable_event,"shared ancestor emission counted twice");
    reset();header={1,21,6};restir_gi::pt_prefix_store_controls(storage.nodes[1],3u,false,true);
    hit.inst_id=hit.prim_id=storage.nodes[1].hit_words.x=storage.nodes[1].hit_words.y=InvalidUI32;
    emit();require(sampled_stable.x==7,"represented environment miss missing");
    std::cout<<"PASS actual callback hit/miss, direct exclusion, pruning, identity, suffix isolation and shared-ancestor exact-once capture\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''
    TEMP.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix='emission-callback-', dir=TEMP))
    cpp, binary = folder / 'test.cpp', folder / 'test.exe'
    shim = shim.replace('__TERMINAL_HELPERS__', '\n'.join(terminal_helpers))
    cpp.write_text(prefix + '\n'.join(helpers) + shim + checks.replace('__CALLBACK__', callback), encoding='utf-8')
    result = subprocess.run([shutil.which('g++'), '-std=c++20', '-O2', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
