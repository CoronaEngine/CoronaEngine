"""CPU fixtures for native GI stable-plane signal capture; no renderer/GPU."""
from pathlib import Path
import shutil
import subprocess
import tempfile

from _restir_cpu import body
from test_gi_ideal_glass import PROGRAM, ROOT

TEMP = ROOT / '.workspace/tmp/restir-gi-signal-mis-20261115/signals'
SOURCE = ROOT / 'vision/src/base/integral/integrator.cpp'


def run(program, name):
    compiler = shutil.which('g++') or shutil.which('clang++')
    assert compiler, 'Standalone GI stable-plane regression needs g++ or clang++ on PATH'
    TEMP.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix=name + '-', dir=TEMP))
    cpp, binary = folder / 'test.cpp', folder / 'test.exe'
    cpp.write_text(program, encoding='utf-8')
    result = subprocess.run([compiler, '-std=c++20', '-O0', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.startswith('PASS '), result.stdout
    print(result.stdout.strip())


def test_actual_receiver_relative_distance_and_first_diffuse_signal_helpers():
    source = SOURCE.read_text(encoding='utf-8-sig')
    helpers = []
    for signature in ('Float pt_signal_accumulate_hit_distance(', 'Float3 pt_signal_specular_fraction('):
        assert signature in source, f'GI receiver-local production helper missing: {signature}'
        declaration, implementation = body(source, signature)
        helpers.append(declaration + implementation)
    checks = r'''
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
void near(float v,float want,const char*m){require(std::isfinite(v)&&abs(v-want)<1e-6f,m);}
int main(){try{
    // Receiver is camera bounce two; the camera-prefix distances do not enter.
    unsigned receiver=2;float t=0;
    t=pt_signal_accumulate_hit_distance(t,7,3-receiver,true);near(t,7,"first receiver segment");
    t=pt_signal_accumulate_hit_distance(t,2,4-receiver,true);near(t,9,"delta entry segment");
    t=pt_signal_accumulate_hit_distance(t,3,5-receiver,true);near(t,12,"delta exit segment");
    t=pt_signal_accumulate_hit_distance(t,90,6-receiver,true);near(t,12,"fourth segment must stop");
    near(pt_signal_accumulate_hit_distance(7,20,2,false),7,"non-delta suffix must stop");
    // A second plane transition starts a new first segment even at camera depth six.
    receiver=6;t=0;t=pt_signal_accumulate_hit_distance(t,4,7-receiver,false);near(t,4,"plane transition reset");
    Float3 rgb=make_float3(.1f,.4f,.8f);
    for(bool diff:{false,true}){
        auto base=pt_signal_specular_fraction(rgb,diff,2u,2u,false);
        near(base.x,.1f,"base direct red");near(base.y,.4f,"base direct green");near(base.z,.8f,"base direct blue");
        for(unsigned b:{3u,4u,8u})for(bool emission:{false,true}){
            auto suffix=pt_signal_specular_fraction(rgb,diff,b,2u,emission);
            near(suffix.x,diff?0:1,"receiver first Diffuse suffix ownership");
            near(suffix.y,suffix.x,"suffix binary green");near(suffix.z,suffix.x,"suffix binary blue");
        }
    }
    std::cout<<"PASS actual receiver-local D/S and hit-distance helpers, depth>1 and delta cap\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''
    run(PROGRAM[:PROGRAM.index('__HELPERS__')] + '\n'.join(helpers) + checks, 'receiver-helpers')


def test_actual_per_plane_signal_events_keep_prefix_and_remove_fresh_once():
    source = (ROOT / 'vision/src/render_core/integrator/restir/gi/restir_gi.cpp').read_text(encoding='utf-8-sig')
    helpers = []
    for signature in ('void gi_signal_switch_plane(', 'void gi_signal_commit(', 'void gi_signal_commit_visit('):
        assert signature in source, f'GI per-plane production helper missing: {signature}'
        declaration, implementation = body(source, signature)
        helpers.append(declaration + implementation)
    assert 'capture.on_signal =' in source, 'GI signal events are not connected to plane accumulation'
    declaration, implementation = body(source, 'capture.on_signal =')
    callback = declaration + implementation + ';'
    policy = (ROOT / 'vision/src/base/scattering/gi_ideal_glass.h').read_text(encoding='utf-8-sig')
    declaration, implementation = body(policy, 'Bool gi_camera_emission_owned_by_di(')
    helpers.append(declaration + implementation)
    native = (ROOT / 'vision/src/render_core/integrator/restir/gi/restir_gi_denoiser.h').read_text(encoding='utf-8-sig')
    native_helpers = []
    for signature in ('Float native_reblur_combine_hit(', 'Float4 native_reblur_commit('):
        declaration, implementation = body(native, signature)
        native_helpers.append(declaration + implementation)
    shim = r'''
#include <functional>
struct Float4{float x{},y{},z{},w{};Float3 xyz()const{return{x,y,z};}};
Float4 make_float4(Float3 a,float w){return{a.x,a.y,a.z,w};}
Float4 make_float4(float a){return{a,a,a,a};}
Float3 operator*(Float3 a,Float3 b){return{a.x*b.x,a.y*b.y,a.z*b.z};}
Float3&operator+=(Float3&a,Float3 b){a.x+=b.x;a.y+=b.y;a.z+=b.z;return a;}
Float3&operator-=(Float3&a,Float3 b){a.x-=b.x;a.y-=b.y;a.z-=b.z;return a;}
struct PTPathCapture{
    uint signal_bounce=0;float signal_hit_distance=0;bool signal_delta_only=true,signal_base_diffuse=false,active=false;
    bool first_scatter_transmission=false,first_reflection_covered_by_di=true;
    std::function<void(const Float3&,const Float3&,const Float&,const Float&,const Uint&,bool)> on_signal;
};
'''
    checks = r'''
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
void near(float v,float want,const char*m){require(std::isfinite(v)&&abs(v-want)<2e-5f,m);}
int main(){try{
    PTPathCapture capture;uint signal_plane=0;
    std::array<Float4,3>signal_diffuse{},signal_specular{};
    Float3 signal_secondary{};
    Float4 signal_base_diffuse{},signal_base_specular{};
    bool signal_scatter_diffuse=false;
    bool sampled_stable_event=false;bool skip_distance=false,skip_dominant_signal_suffix=false;
    __CALLBACK__
    auto emit=[&](float L,float S,float light_t,float path_t,unsigned bounce,bool emission){
        capture.signal_hit_distance=path_t;
        capture.on_signal(make_float3(L),make_float3(S),light_t,path_t,bounce,emission);
    };
    auto switch_plane=[&](unsigned plane,unsigned bounce){
        gi_signal_commit_visit(signal_diffuse[signal_plane],signal_specular[signal_plane],signal_base_diffuse,
            signal_base_specular,signal_secondary,capture.signal_hit_distance,signal_scatter_diffuse);
        gi_signal_switch_plane(capture,signal_plane,plane,bounce);
    };
    // DI ownership is absent from GI signals, including the first emission.
    emit(30,20,100,0,0,false);emit(11,11,100,100,1,true);
    near(signal_secondary.x,0,"camera direct was counted twice");
    // The receiver's own first light stays in this plane residual, never Lo.
    capture.active=true;capture.first_scatter_transmission=true;
    emit(11,11,100,100,1,true);near(signal_secondary.x,11,"first transmission signal was discarded");
    sampled_stable_event=true;emit(11,11,100,100,1,true);
    near(signal_secondary.x,11,"represented first transmission duplicated its Build bypass");
    sampled_stable_event=false;capture.first_scatter_transmission=false;capture.first_reflection_covered_by_di=false;
    emit(7,7,100,100,1,true);near(signal_secondary.x,18,"uncovered delta reflection signal was discarded");
    capture.first_reflection_covered_by_di=true;capture.active=false;signal_secondary={};
    // A rough prefix leaves nonzero NEE on plane zero before a deeper receiver.
    capture.signal_base_diffuse=false;capture.signal_delta_only=false;
    emit(3,3,7,7,1,false);
    switch_plane(1,2);
    require(!capture.signal_delta_only,"plane switch restored global delta-only state");
    near(capture.signal_hit_distance,0,"plane distance was not reset");
    capture.signal_base_diffuse=true;
    emit(10,2,6,0,2,false);emit(5,0,4,4,3,false);
    near(signal_specular[0].x,3,"receiver stole pre-receiver rough-prefix NEE");
    near(signal_specular[0].w,7,"prefix final hit distance changed");
    near(signal_base_diffuse.x,8,"receiver base direct diffuse missing");
    near(signal_secondary.x,5,"receiver suffix missing");
    near(signal_base_specular.x,2,"receiver direct did not keep evaluated D/S");
    // A second switch retains the first receiver's samples, even without a reservoir.
    capture.active=false;switch_plane(2,5);
    capture.signal_base_diffuse=false;
    emit(8,6,9,0,5,false);
    near(signal_diffuse[1].x+signal_specular[1].x,15,"later plane transition discarded earlier samples");
    near(signal_base_diffuse.x+signal_base_specular.x,8,"inactive receiver lost direct signal");
    // A delta-only inactive candidate can emit before its distance finishes.
    emit(1,1,2,2,6,true);emit(1,1,5,5,7,true);
    Float4 grouped_d=signal_base_diffuse,grouped_s=signal_base_specular;
    gi_signal_commit(grouped_d,grouped_s,signal_secondary,capture.signal_hit_distance,signal_scatter_diffuse);
    near(grouped_s.x,8,"inactive candidate suffix was discarded");
    near(grouped_s.w,8,"base NEE distance did not weight against suffix finalT=5");
    Float4 suffix_d{},suffix_s{};
    gi_signal_commit(suffix_d,suffix_s,signal_secondary,capture.signal_hit_distance,false);
    near(suffix_s.w,5,"suffix events must share final sampleHitT instead of intermediate distances");
    // Represented stable emission is stored only in the stable bypass.
    sampled_stable_event=true;
    emit(13,13,5,5,8,true);
    near(signal_secondary.x,2,"represented stable emission duplicated");
    sampled_stable_event=false;
    Float4 tiny_d{},tiny_s{};
    gi_signal_commit(tiny_d,tiny_s,make_float3(1e-6f),100,false);
    near(tiny_s.x,0,"native luminance threshold lost");near(tiny_s.w,0,"tiny suffix changed native distance");
    // PSR may rebase a rough pure-reflection prefix to the same plane zero.
    signal_diffuse={};signal_specular={};signal_secondary={};signal_base_diffuse={};signal_base_specular={};
    signal_plane=0;capture.signal_bounce=0;capture.signal_base_diffuse=false;capture.signal_hit_distance=0;
    emit(3,3,7,7,1,false);switch_plane(0,2);
    capture.signal_base_diffuse=true;emit(10,2,6,0,2,false);
    gi_signal_commit_visit(signal_diffuse[0],signal_specular[0],signal_base_diffuse,signal_base_specular,
        signal_secondary,capture.signal_hit_distance,signal_scatter_diffuse);
    near(signal_specular[0].x,5,"same-plane rebase reclassified earlier specular prefix");
    near(signal_diffuse[0].x,8,"same-plane rebase put rough-prefix NEE in new diffuse receiver");
    near(signal_specular[0].w,6.6f,"same-plane rebase replaced earlier specular prefix distance");
    // An active terminal candidate belongs to the current (final) visit.
    signal_diffuse={};signal_specular={};signal_secondary={};signal_base_diffuse={};signal_base_specular={};
    signal_plane=1;capture.signal_bounce=2;capture.signal_base_diffuse=true;capture.active=true;
    emit(10,2,6,0,2,false);emit(5,0,4,4,3,false);
    signal_secondary-=make_float3(5);
    gi_signal_commit_visit(signal_diffuse[1],signal_specular[1],signal_base_diffuse,signal_base_specular,
        signal_secondary,capture.signal_hit_distance,signal_scatter_diffuse);
    near(signal_diffuse[1].x+signal_specular[1].x,10,"fresh suffix was not removed exactly once");
    near(signal_specular[1].x,2,"fresh diffuse subtraction altered receiver direct specular");
    near(signal_diffuse[1].w,6,"fresh suffix distance was retained");near(signal_specular[1].w,6,"base specular light distance missing");
    std::cout<<"PASS actual signal callback and native visit commits, rough prefix, distinct/same-plane rebases, final delta distance and fresh ownership\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''
    run(PROGRAM[:PROGRAM.index('__HELPERS__')] + shim + 'namespace restir_gi{' + '\n'.join(native_helpers) + '}' + '\n'.join(helpers) + checks.replace('__CALLBACK__', callback), 'plane-events')


def test_actual_shared_nee_fragment_uses_base_only_exact_light_direction():
    source = SOURCE.read_text(encoding='utf-8-sig')
    direct_call = source.index('auto direct = direct_light_mis(')
    token = 'if (hc.path_capture)'
    start = source.rindex(token, 0, direct_call)
    declaration, implementation = body(source[start:], token)
    fragment = declaration + implementation
    shim = r'''
#include <functional>
Float3 normalize(Float3 a){return a/std::sqrt(dot(a,a));}
struct Interaction{Float3 wo=make_float3(0,0,1),pos=make_float3(0);};
struct TriangleHitVar{};
struct MaterialEvaluator{
    mutable unsigned fraction_calls=0;mutable Float3 evaluated_wi{};
    Float3 specular_fraction_rgb(Float3,Float3 wi)const{++fraction_calls;evaluated_wi=wi;return{wi.x,wi.y,wi.z};}
};
struct LightSample{Float3 p_light=make_float3(0,3,4);};
struct Capture{
    uint signal_bounce=0;bool on_signal=true,evaluate_signal_direct_fraction=true;
    std::function<void(const Interaction&,const TriangleHitVar&,const MaterialEvaluator&,
        const BSDFSample&,const Uint&,const SampledSpectrum&)>on_sample;
};
struct Context{Capture*path_capture=nullptr;};
unsigned nee_samples=0;
SampledSpectrum direct_light_mis(Interaction,const MaterialEvaluator&,LightSample,bool,int& sampler,int,BSDFSample&ss){
    ++nee_samples;++sampler;ss.wi=make_float3(1,0,0);return{7.f};
}
'''
    checks = r'''
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
void near(float v,float want,const char*m){require(std::isfinite(v)&&abs(v-want)<1e-6f,m);}
int main(){try{
    Capture capture;Context hc{&capture};Interaction it;TriangleHitVar hit;MaterialEvaluator evaluator;
    SampledWavelengths wavelengths;BSDFSample bsdf_sample{wavelengths};LightSample light_sample;
    unsigned bounces=0,samples_observed=0;int sampler=0,swl=0;bool occluded=false;
    SampledSpectrum throughput{1.f};Float3 signal_direct_fraction=make_float3(0);
    capture.on_sample=[&](const auto&,const auto&,const auto&,const auto&sample,const auto&bounce,const auto&){
        require(nee_samples==samples_observed+1,"BSDF observer did not follow exactly one NEE sample");
        near(sample.wi.x,1,"sampled BSDF direction changed");++samples_observed;
        // This is the newly selected receiver, discovered after BSDF sampling.
        if(bounce==2)capture.signal_bounce=bounce;
    };
    auto run=[&](){__FRAGMENT__return SampledSpectrum{0.f};};
    auto primary=run();near(primary.x,7,"primary NEE radiance changed");
    require(nee_samples==1&&sampler==1&&samples_observed==1,"primary sampling changed");
    require(evaluator.fraction_calls==0,"primary NEE evaluated the unused RGB fraction");
    bounces=2;
    auto L=run();near(L.x,7,"secondary NEE radiance changed");
    require(nee_samples==2&&sampler==2&&samples_observed==2,"secondary sampling changed");
    near(evaluator.evaluated_wi.x,0,"fraction used sampled BSDF direction");
    near(evaluator.evaluated_wi.y,.6f,"fraction did not use exact shadow direction");
    near(evaluator.evaluated_wi.z,.8f,"fraction did not use exact shadow direction");
    require(evaluator.fraction_calls==1,"base was not evaluated once");
    bounces=3;run();require(evaluator.fraction_calls==1,"later NEE evaluated unnecessary D/S");
    capture.on_signal=false;bounces=2;run();require(evaluator.fraction_calls==1,"non-signal path evaluated D/S");
    require(nee_samples==4&&sampler==4&&samples_observed==4,"observer changed sampling");
    std::cout<<"PASS actual shared NEE fragment, exact shadow direction, base-only D/S, unchanged sampling and ordinary opt-out\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''
    run(PROGRAM[:PROGRAM.index('__HELPERS__')] + shim + checks.replace('__FRAGMENT__', fragment), 'base-nee')


def test_actual_gi_miss_segment_matches_native_without_changing_ordinary_distance():
    source = SOURCE.read_text(encoding='utf-8-sig')
    token = 'if (hc.path_capture && hc.path_capture->on_signal && hc.path_capture->record_hit_distance)'
    declaration, implementation = body(source, token)
    native_fragment = declaration + implementation
    declaration, implementation = body(source, 'Float pt_signal_accumulate_hit_distance(')
    helper = declaration + implementation
    legacy_start = source.index('Float segment_distance = scene().world_diameter();', source.index('auto mis_bsdf'))
    legacy_end = source.index('first_distance = ', legacy_start)
    legacy_fragment = source[legacy_start:legacy_end]
    shim = r'''
float length(Float3 a){return std::sqrt(dot(a,a));}
struct PTPathCapture{bool on_signal=true,record_hit_distance=true,signal_delta_only=true;
    bool reuse_signal_distance=false,native_signal_miss=true;
    unsigned signal_bounce=2;float signal_hit_distance=0;};
struct Counters{struct{unsigned z=0;}trace;};
struct Context{PTPathCapture*path_capture=nullptr;Counters*work_counters=nullptr;};
struct Hit{const Hit*operator->()const{return this;}bool is_hit()const{return false;}bool is_miss()const{return true;}};
struct RayState{int ray=0;Float3 origin()const{return make_float3(0);}};
struct Geometry{struct Endpoint{Float3 pos=make_float3(0);};Endpoint compute_surface_interaction(Hit,int){return{};}};
struct Scene{float world_diameter()const{return 1234;}};Scene scene(){return{};}
'''
    checks = r'''
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
int main(){try{
    PTPathCapture capture;Context hc{&capture};Geometry geometry;Hit hit;RayState rs;unsigned bounces=3;
    __NATIVE__
    require(capture.signal_hit_distance==50000,"GI miss used scene diameter instead of native kMaxSceneDistance");
    __LEGACY__
    require(segment_distance==1234,"ordinary PT miss no longer uses scene diameter");
    std::cout<<"PASS actual GI native miss distance 50000 and unchanged ordinary world-diameter segment\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''
    run(PROGRAM[:PROGRAM.index('__HELPERS__')] + shim + helper + checks.replace('__NATIVE__', native_fragment).replace('__LEGACY__', legacy_fragment), 'miss-distance')
