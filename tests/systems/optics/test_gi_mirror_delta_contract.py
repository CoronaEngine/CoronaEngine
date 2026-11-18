"""Execute the GI mirror's production flag/sampler/measure contract on CPU.

The scalar shims replace DSL value types, not the extracted renderer decisions.
They do not build the renderer or launch a GPU.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

from _restir_cpu import body
from test_gi_ideal_glass import PROGRAM, ROOT

SOURCE = ROOT / 'vision/src/render_core/material/principled_bsdf.cpp'
TEMP = ROOT / '.workspace/tmp/bedroom-mirror-fix-20261117/mirror-contract'


def run(program, name):
    compiler = shutil.which('g++') or shutil.which('clang++')
    assert compiler, 'Standalone GI mirror regression needs a C++ compiler'
    TEMP.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix=name + '-', dir=TEMP))
    cpp, binary = folder / 'test.cpp', folder / 'test.exe'
    cpp.write_text(program, encoding='utf-8')
    result = subprocess.run([compiler, '-std=c++20', '-O0', str(cpp), '-o', str(binary)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.startswith('PASS '), result.stdout
    print(result.stdout.strip())


def flag_method(source):
    source = source[source.index('class PrincipledLobeSet final'):]
    declaration, implementation = body(source, 'Uint flag() const noexcept override')
    return declaration + implementation


def test_actual_gi_stable_metal_flag_is_delta_while_ordinary_and_rough_stay_original():
    prefix = PROGRAM[:PROGRAM.index('__HELPERS__')]
    checks = r'''
#include <optional>
struct SurfaceData {static constexpr unsigned NearSpec=64;};
struct LobeSet {virtual unsigned flag()const noexcept{return 16;} virtual ~LobeSet()=default;};
struct PrincipledLobeSet : LobeSet {
    unsigned stable_lobe_=~0u;std::optional<bool>ideal_glass_;
    __FLAG__
};
int main(){
    PrincipledLobeSet p;
    p.stable_lobe_=0;p.ideal_glass_=false;
    if(p.flag()!=BxDFFlag::SpecRefl){std::cerr<<"GI complete stable metal flags="<<p.flag()<<" expected discrete reflection34\n";return 1;}
    p.ideal_glass_.reset();
    if(p.flag()!=SurfaceData::NearSpec){std::cerr<<"ordinary pure metal changed\n";return 2;}
    p.stable_lobe_=~0u;p.ideal_glass_=false;
    if(p.flag()!=16){std::cerr<<"rough/mixed GI metal changed\n";return 3;}
    p.ideal_glass_=true;
    if(p.flag()!=(BxDFFlag::SpecRefl|BxDFFlag::SpecTrans)){std::cerr<<"ideal glass changed\n";return 4;}
    std::cout<<"PASS actual GI pure-metal delta classification; ordinary, rough and glass isolation\n";
}
'''
    run(prefix + checks.replace('__FLAG__', flag_method(SOURCE.read_text(encoding='utf-8-sig'))), 'flags')


def test_actual_ideal_metal_branch_has_discrete_mass_exact_direction_and_fresnel_transport():
    source = SOURCE.read_text(encoding='utf-8-sig')
    declaration, implementation = body(source, 'BSDFSample gi_ideal_metallic_branch(')
    checks = r'''
int main(){
    SampledWavelengths swl;SampledSpectrum F{make_float3(.25f,.64f,1.f)};
    for(Float3 wo:{make_float3(.6f,0,.8f),make_float3(.6f,0,-.8f)}){
        auto s=gi_ideal_metallic_branch(swl,wo,F);
        if(!s.valid()||s.eval.pdfs!=1||s.eval.flags!=BxDFFlag::SpecRefl||s.stable_lobe_index!=1){std::cerr<<"not a unit-mass delta reflection\n";return 1;}
        if(length_squared(s.wi-make_float3(-wo.x,-wo.y,wo.z))>1e-12f){std::cerr<<"random/incorrect mirror direction\n";return 2;}
        auto transport=s.eval.throughput()*abs_cos_theta(s.wi);
        if(length_squared(transport-F.vec3())>1e-12f){std::cerr<<"GGX density or cosine leaked into physical mirror weight\n";return 3;}
    }
    auto grazing=gi_ideal_metallic_branch(swl,make_float3(1,0,0),F);
    if(grazing.valid()){std::cerr<<"undefined tangent delta sample accepted\n";return 4;}
    std::cout<<"PASS actual exact mirror branch direction, discrete unit PDF and RGB Fresnel transport\n";
}
'''
    run(PROGRAM[:PROGRAM.index('__HELPERS__')] + declaration + implementation + checks, 'measure')


def test_actual_material_dispatch_uses_gi_mirror_delta_and_keeps_ordinary_proposal():
    source = SOURCE.read_text(encoding='utf-8-sig')
    material = (ROOT / 'vision/src/base/scattering/material.cpp').read_text(encoding='utf-8-sig')
    extracted = []
    for text, signature in ((source, 'BSDFSample gi_ideal_metallic_branch('),
                            (material, 'BSDFSample MaterialEvaluator::sample_delta('),
                            (material, 'BSDFSample MaterialEvaluator::sample(')):
        declaration, implementation = body(text, signature)
        extracted.append(declaration + implementation)
    shim = r'''
#include <optional>
enum TransportMode{Radiance};using TSampler=int;
struct SurfaceData{static constexpr unsigned NearSpec=64;};
struct LobeSet{virtual unsigned flag()const noexcept{return 16;}virtual~LobeSet()=default;};
struct PrincipledLobeSet:LobeSet{unsigned stable_lobe_=~0u;std::optional<bool>ideal_glass_;__FLAG__};
bool scoped=false,individual=false;unsigned ordinary_calls=0,delta_calls=0;
namespace gi_principled{bool reading(){return scoped;}bool writing(){return false;}}
struct Frame{Float3 to_local(Float3 v)const{return v;}Float3 to_world(Float3 v)const{return v;}};
bool same_hemisphere(Float3 a,Float3 b,Float3){return a.z*b.z>0;}
struct MaterialRegistry{static MaterialRegistry&instance(){static MaterialRegistry r;return r;}bool individual_ns()const{return individual;}};
struct Lobe{BSDFSample sample(Float3 wo,unsigned,TSampler&,TransportMode)const{
    ordinary_calls++;SampledWavelengths w;BSDFSample s(w);s.wi=make_float3(0,0,wo.z);s.eval.f=42.f;s.eval.pdfs=7;s.eval.flags=16;return s;}};
BSDFSample gi_ideal_metallic_branch(const SampledWavelengths&,const Float3&,const SampledSpectrum&)noexcept;
struct MaterialEvaluator{
    SampledWavelengths wavelengths;const SampledWavelengths*swl_=&wavelengths;Frame shading_frame_;Float3 ng_=make_float3(0,0,1);
    PrincipledLobeSet closure;
    unsigned flag()const{return closure.flag();}
    template<class F>void dispatch(F f)const{Lobe l;f(&l);}
    BSDFSample sample_local(Float3 wo,unsigned f,TSampler&s,TransportMode t)const{return Lobe{}.sample(wo,f,s,t);}
    BSDFSample sample_delta_local(Float3 wo,TSampler&)const{
        delta_calls++;return gi_ideal_metallic_branch(*swl_,wo,SampledSpectrum{make_float3(.25f,.64f,1.f)});}
    BSDFSample sample_delta(const Float3&,TSampler&)const noexcept;
    BSDFSample sample(const Float3&,TSampler&,const Uint&,TransportMode)const noexcept;
};
'''
    checks = r'''
int main(){
    MaterialEvaluator e;e.closure.stable_lobe_=0;TSampler sampler=0;
    for(bool ind:{false,true}){
        individual=ind;scoped=false;e.closure.ideal_glass_.reset();ordinary_calls=delta_calls=0;
        auto old=e.sample(make_float3(.6f,0,.8f),sampler,BxDFFlag::All,Radiance);
        if(ordinary_calls!=1||delta_calls||old.eval.pdfs!=7||old.stable_lobe_index!=~0u){std::cerr<<"ordinary proposal changed\n";return 1;}
        scoped=true;e.closure.ideal_glass_=false;ordinary_calls=delta_calls=0;
        auto mirror=e.sample(make_float3(.6f,0,.8f),sampler,BxDFFlag::All,Radiance);
        if(ordinary_calls||delta_calls!=1||mirror.eval.pdfs!=1||mirror.stable_lobe_index!=1||mirror.eval.flags!=BxDFFlag::SpecRefl){std::cerr<<"GI mirror used random GGX/continuous PDF\n";return 2;}
        if(length_squared(mirror.wi-make_float3(-.6f,0,.8f))>1e-12f||length_squared(mirror.eval.throughput()-make_float3(.25f,.64f,1.f))>1e-12f){std::cerr<<"GI world mirror direction/weight differs from enumerated prefix\n";return 3;}
        e.closure.stable_lobe_=~0u;ordinary_calls=delta_calls=0;
        e.sample(make_float3(.6f,0,.8f),sampler,BxDFFlag::All,Radiance);
        if(ordinary_calls!=1||delta_calls){std::cerr<<"rough/mixed GI proposal changed\n";return 4;}
        e.closure.stable_lobe_=0;
    }
    std::cout<<"PASS actual GI/ordinary material dispatch and exact mirror sample, with both normal modes\n";
}
'''
    run(PROGRAM[:PROGRAM.index('__HELPERS__')] + shim.replace('__FLAG__', flag_method(source)) +
        '\n'.join(extracted) + checks, 'dispatch')


def test_actual_gi_metal_lobe_enumeration_and_pdf_interfaces_use_same_discrete_measure():
    source = SOURCE.read_text(encoding='utf-8-sig')
    declaration, implementation = body(source, 'BSDFSample gi_ideal_metallic_branch(')
    helper = declaration + implementation
    scoped = source[source.index('class GIIdealMetallicLobe final'):source.index('class GIIdealDielectricLobe final')]
    methods = []
    for signature in ('ScatterEval evaluate_local_impl(', 'bool supports_prepared_local_evaluation()',
                      'const MicrofacetReflection *prepared_reflection_pdf()', 'Uint flag()',
                      'Bool supports_stable_reflection(', 'BSDFSample sample_delta_local(',
                      'StableLobes eval_stable_lobes(', 'void reuse_material('):
        declaration, implementation = body(scoped, signature)
        methods.append(declaration + implementation)
    shim = r'''
enum MaterialEvalMode{All};enum TransportMode{Radiance};using TSampler=int;
struct Float2{float x{},y{};};Float2 make_float2(float v){return{v,v};}
struct MicrofacetReflection{};
struct MetallicLobe{
    SampledWavelengths wavelengths;
    const SampledWavelengths*swl()const{return&wavelengths;}
    SampledSpectrum albedo(float)const{return{make_float3(.25f,.64f,1.f)};}
    virtual ScatterEval evaluate_local_impl(const Float3&,const Float3&,MaterialEvalMode,const Uint&,TransportMode)const noexcept{
        ScatterEval s;s.f=SampledSpectrum{.5f};s.pdfs=3;s.flags=64;return s;}
    virtual bool supports_prepared_local_evaluation()const noexcept{return true;}
    virtual const MicrofacetReflection*prepared_reflection_pdf()const noexcept{static MicrofacetReflection r;return&r;}
    virtual Uint flag()const noexcept{return 64;}
    virtual Bool supports_stable_reflection(const Float3&)const noexcept{return true;}
    virtual BSDFSample sample_delta_local(const Float3&,TSampler&)const noexcept{return BSDFSample{wavelengths};}
    virtual StableLobes eval_stable_lobes(const Float3&)const noexcept{return StableLobes{wavelengths};}
    virtual void reuse_material(SampledSpectrum&d,SampledSpectrum&s,Float2&r)const noexcept{d=SampledSpectrum{0.f};s=albedo(1);r=make_float2(.031622777f);}
    virtual~MetallicLobe()=default;
};
struct GIIdealMetallicLobe:MetallicLobe{
    bool ideal_;explicit GIIdealMetallicLobe(bool v):ideal_(v){}
    __METHODS__
};
'''
    checks = r'''
int main(){
    GIIdealMetallicLobe mirror{true},rough{false};TSampler sampler=0;
    Float3 wo=make_float3(.6f,0,.8f);
    auto sampled=mirror.sample_delta_local(wo,sampler);
    auto enumerated=mirror.eval_stable_lobes(wo);
    const auto&r=enumerated.lobes[StableLobes::reflection_index];
    if(enumerated.has_stochastic_remainder||!r.valid||enumerated.lobes[0].valid||r.probability!=sampled.eval.pdfs||
       length_squared(r.wi-sampled.wi)>1e-12f||length_squared(r.weight-sampled.eval.throughput()*abs_cos_theta(sampled.wi))>1e-12f){
        std::cerr<<"Build/Fill mirror directions or measures disagree\n";return 1;}
    if(mirror.supports_prepared_local_evaluation()||mirror.prepared_reflection_pdf()!=nullptr){
        std::cerr<<"continuous GGX prepared PDF can bypass ideal mirror evaluator\n";return 2;}
    auto eval=mirror.evaluate_local_impl(wo,sampled.wi,All,127,Radiance);
    if(eval.pdfs!=0||length_squared(eval.f)>0){std::cerr<<"delta mirror leaked a continuous F/PDF\n";return 3;}
    if(mirror.flag()!=BxDFFlag::SpecRefl||!mirror.supports_stable_reflection(wo)){std::cerr<<"discrete mirror classification inconsistent\n";return 4;}
    SampledSpectrum d,s;Float2 roughness;mirror.reuse_material(d,s,roughness);
    if(roughness.x!=0||roughness.y!=0){std::cerr<<"mirror receiver guide roughness not discrete\n";return 5;}
    auto fallback=rough.evaluate_local_impl(wo,sampled.wi,All,127,Radiance);
    if(rough.flag()!=64||rough.supports_stable_reflection(wo)||fallback.pdfs!=3||fallback.f.x!=.5f||!rough.eval_stable_lobes(wo).has_stochastic_remainder){
        std::cerr<<"nonideal GI lobe no longer delegates original continuous closure\n";return 6;}
    rough.reuse_material(d,s,roughness);
    if(roughness.x<=0||roughness.y<=0){std::cerr<<"rough GI guide changed to delta\n";return 7;}
    std::cout<<"PASS actual GI lobe Build/Fill measure, continuous F/PDF exclusion, prepared-hook exclusion and rough fallback\n";
}
'''
    run(PROGRAM[:PROGRAM.index('__HELPERS__')] + helper + shim.replace('__METHODS__', '\n'.join(methods)) + checks, 'lobe')


def test_actual_gi_metal_cache_preserves_authored_measure_across_half_quantization():
    import struct

    source = SOURCE.read_text(encoding='utf-8-sig')
    gi = source[source.index('UP<Lobe> create_gi_lobe_set('):]
    start = gi.index('Bool ideal_metal =')
    classification = gi[start:gi.index('WeightedLobe metal_lobe', start)]
    half_metal = struct.unpack('e', struct.pack('e', .9999))[0]
    half_coat = struct.unpack('e', struct.pack('e', 1e-8))[0]
    assert half_metal == 1. and half_coat == 0., 'fixture no longer straddles FP16 measure boundary'
    shim = r'''
struct SurfaceData{static constexpr unsigned NearSpec=64;};
namespace gi_principled{
    bool decoding=false;unsigned bits=0;constexpr unsigned IdealMetalBit=1u<<12u;
    bool reading(){return decoding;}bool writing(){return !decoding;}
    unsigned flags(){return bits;}void set_flags(unsigned v){bits=v;}
}
bool classify(bool read,float metallic,bool stable_layers,unsigned&bits){
    gi_principled::decoding=read;gi_principled::bits=bits;unsigned flag=SurfaceData::NearSpec;
    __CLASSIFICATION__
    bits=gi_principled::bits;return ideal_metal;
}
int main(){
    unsigned bits=0;
    if(classify(false,.9999f,true,bits)){std::cerr<<"authored mixed metal was ideal\n";return 1;}
    if(classify(true,__HALF_METAL__,true,bits)){std::cerr<<"FP16 metallic .9999->1 changed continuous material into ideal mirror\n";return 2;}
    bits=0;
    if(classify(false,1.f,false,bits)){std::cerr<<"authored tiny-coat mixture was ideal\n";return 3;}
    if(classify(true,1.f,__HALF_COAT__==0.f,bits)){std::cerr<<"FP16 tiny-coat 1e-8->0 changed layered material into ideal mirror\n";return 4;}
    bits=0;
    if(!classify(false,1.f,true,bits)||!classify(true,1.f,true,bits)){std::cerr<<"pure authored mirror lost exact measure on cache read\n";return 5;}
    std::cout<<"PASS authored ideal-metal classification survives FP16 metallic/coat rounding and pure mirror cache\n";
}
'''
    shim = shim.replace('__CLASSIFICATION__', classification).replace('__HALF_METAL__', repr(half_metal)+'f').replace('__HALF_COAT__', repr(half_coat)+'f')
    run(PROGRAM[:PROGRAM.index('__HELPERS__')] + shim, 'cache')


def test_actual_rough_mis_surrogate_clears_ideal_metal_without_changing_true_material():
    source = (ROOT / 'vision/src/render_core/integrator/ReSTIR/rtxpt_gi_pt.h').read_text(encoding='utf-8-sig')
    declaration, implementation = body(source, 'Float3 f_roughness_clamp(')
    shim = r'''
#include <bit>
template<class T,class V>T as(V v){return std::bit_cast<T>(v);}
namespace gi_principled{constexpr unsigned BaseNearBit=1u<<8u,IdealGlassBit=1u<<11u,IdealMetalBit=1u<<12u;}
struct Row{float w=0;};
struct PTSurfaceView{
    std::array<Row,6>parameters;
    Float3 f(const Float3&,const SampledWavelengths&)const noexcept{
        return make_float3(parameters[0].w,float(as<unsigned>(parameters[5].w)),0);}
    __ROUGH__
};
int main(){
    PTSurfaceView surface;SampledWavelengths swl;
    unsigned bits=gi_principled::BaseNearBit|gi_principled::IdealGlassBit|gi_principled::IdealMetalBit|0x400u|0x51u;
    surface.parameters[5].w=as<float>(bits);surface.parameters[0].w=0;
    auto rough=surface.f_roughness_clamp(.2f,make_float3(0,0,1),swl);
    if(rough.x!=.2f||unsigned(rough.y)!=(0x400u|0x51u)){
        std::cerr<<"rough MIS surrogate retained ideal metal measure or changed unrelated flags\n";return 1;}
    if(surface.parameters[0].w!=0||as<unsigned>(surface.parameters[5].w)!=bits){
        std::cerr<<"rough surrogate changed true receiver material\n";return 2;}
    std::cout<<"PASS actual rough MIS copy clears all delta bits and preserves original material and unrelated flags\n";
}
'''
    run(PROGRAM[:PROGRAM.index('__HELPERS__')] + shim.replace('__ROUGH__', declaration + implementation), 'rough-mis')
