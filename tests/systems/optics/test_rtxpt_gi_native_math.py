"""Analytical CPU checks of the production RTXPT ReSTIR GI DSL math.

The renderer/GPU is never built or launched. The real helper definitions are
compiled with scalar float/vector shims, then checked against transport, ABI,
quantization and reconnection properties rather than reimplemented algorithms.
"""
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'vision/src').is_dir())
HEADER = ROOT / 'vision/src/render_core/integrator/ReSTIR/rtxpt_gi.h'
SOURCE = HEADER.with_suffix('.cpp')
TEMP = ROOT / '.workspace/tmp/rtxpt-corona-dsl-20261108/native-math-tests'


def _struct(source, name):
    start = source.index('struct ' + name + ' {')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + ';'


SHIM = r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using uint=std::uint32_t;using Uint=uint;using Int=int;using Float=float;using Bool=bool;
#define $if if
struct Float3 {float x{},y{},z{};};
Float3 make_float3(float x,float y,float z){return{x,y,z};}
Float3 make_float3(float x){return{x,x,x};}
Float3 operator+(Float3 a,Float3 b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 operator-(Float3 a,Float3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 operator*(Float3 a,float b){return{a.x*b,a.y*b,a.z*b};}
Float3 operator*(float b,Float3 a){return a*b;}
Float3 operator/(Float3 a,float b){return{a.x/b,a.y/b,a.z/b};}
float dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float length(Float3 a){return std::sqrt(dot(a,a));}
Float3 normalize(Float3 a){return a/length(a);}
float luminance(Float3 a){return dot(a,{.2126f,.7152f,.0722f});}
Float3 max(Float3 a,float b){return{std::max(a.x,b),std::max(a.y,b),std::max(a.z,b)};}
Float3 max(Float3 a,Float3 b){return{std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)};}
Float3 clamp(Float3 a,float lo,float hi){return{std::clamp(a.x,lo,hi),std::clamp(a.y,lo,hi),std::clamp(a.z,lo,hi)};}
using std::min;using std::max;using std::clamp;using std::abs;using std::round;using std::log2;using std::exp2;
float saturate(float a){return std::clamp(a,0.f,1.f);}
template<class A,class B> A select(bool p,A a,B b){return p?a:A(b);}
template<class T,class V>T cast(V v){return static_cast<T>(v);}
float zero_if_nan_inf(float v){return std::isfinite(v)?v:0.f;}
namespace ocarina {
using ::make_float3;using ::dot;using ::length;using ::normalize;using ::luminance;
using ::min;using ::max;using ::clamp;using ::abs;using ::round;using ::log2;using ::exp2;
using ::saturate;using ::select;using ::cast;using ::zero_if_nan_inf;
template<class T,class V>T as(V v){return std::bit_cast<T>(v);}
}
namespace vision {
template<class T,std::size_t N>using array=std::array<T,N>;
struct Values3 {
    array<float,3> values{};
    float&operator[](uint i){return values.at(i);}const float&operator[](uint i)const{return values.at(i);}
    Float3 as_vec3()const{return{values[0],values[1],values[2]};}
    void set(Float3 v){values={v.x,v.y,v.z};}
};
struct SurfacePoint {
    Values3 pos,ng;
    SurfacePoint*operator->(){return this;}const SurfacePoint*operator->()const{return this;}
    Float3 normal()const{return ng.as_vec3();}void set_normal(Float3 n){ng.set(n);}
};
struct GISample {SurfacePoint sp;Values3 Lo;float age{};};
__STORAGE__
__RESERVOIR__
using RtxptGIStorageVar=RtxptGIStorage;using RtxptGIReservoirVar=RtxptGIReservoir;
}
__PRODUCTION_HELPERS__
using namespace vision;using namespace vision::rtxpt_gi;
void require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
void near(float actual,double expected,double absolute,const char*message){
    if(!std::isfinite(actual)||std::abs(double(actual)-expected)>absolute){
        std::cerr<<message<<": actual="<<actual<<" expected="<<expected<<'\n';
        throw std::runtime_error(message);
    }
}
uint bits(float v){return std::bit_cast<uint>(v);}
RtxptGIReservoir source(float lo=1,float pdf=1,uint mass=1){
    RtxptGIReservoir r;r.M=mass;r.weight=1;r.sample.Lo.set(make_float3(lo/pdf));
    r.sample.sp.pos.set({0,0,2});r.sample.sp.ng.set({0,0,-1});return r;
}
void codec_test(){
    static_assert(sizeof(RtxptGIStorage)==32,"native reservoir bandwidth ABI");
    require(encode_radiance(make_float3(0))==0,"black must pack exactly zero");
    require(encode_radiance(make_float3(1e-8f))==0,"below native HDR range must pack black");
    auto black=decode_radiance(0);require(black.x==0&&black.y==0&&black.z==0,"black must decode exactly zero");
    // Log-luminance uses midpoint reconstruction with a 1/409.6 octave step.
    const double luminance_bound=std::exp2(.5/409.6)-1+8e-6;
    for(float scale:{1e-5f,.0001f,.1f,1.f,100.f,10000.f,100000.f}){
        for(Float3 chroma:{make_float3(1),make_float3(.2f,.6f,1.f),make_float3(1.f,.4f,.3f)}){
            auto rgb=chroma*scale;auto decoded=decode_radiance(encode_radiance(rgb));
            require(decoded.x>=0&&decoded.y>=0&&decoded.z>=0,"HDR roundtrip produced negative radiance");
            const double relative=std::abs(double(luminance(decoded))/luminance(rgb)-1);
            require(relative<=luminance_bound,"LogLuv luma error exceeded its quantization bound");
        }
    }
    auto r=source(10,.2f,1000);r.age=900;r.weight=std::bit_cast<float>(0x7fc12345u);
    r.sample.sp.pos[0]=std::bit_cast<float>(0x80000000u);
    r.sample.sp.pos[1]=std::bit_cast<float>(0x00800001u);
    r.sample.sp.pos[2]=std::bit_cast<float>(0x7f7fffffu);
    r.sample.sp.ng.set({1,0,0});auto packed=pack(r);auto v=unpack(packed);
    require(packed.words[3]==0xffffu&&v.M==255&&v.age==255,"M/age must saturate independently to uint8");
    require(packed.words[7]==0,"native unused word must remain zero");
    require(bits(v.weight)==bits(r.weight),"reservoir weight must preserve raw float bits");
    for(uint i=0;i<3;++i)require(bits(v.sample.sp.pos[i])==bits(r.sample.sp.pos[i]),"secondary position must preserve raw float bits");
    RtxptGIReservoir empty;auto e=unpack(pack(empty));
    require(e.M==0&&e.weight==0,"invalidity is M0, not the decoded normal sentinel");
    require(encode_normal({0,0,1})==0,"native +Z oct normal must encode zero");
    auto z=decode_normal(0);near(z.z,1,1e-7,"native zero encoded normal means +Z");
    float max_error=0;uint random=0x20261108u;
    for(uint i=0;i<10006;++i){
        auto next=[&](){random=random*1664525u+1013904223u;return float(int(random>>8)-8388608)/8388608.f;};
        Float3 n=make_float3(next(),next(),next());
        if(i<6){n=make_float3(0);(&n.x)[i/2]=(i&1)?-1.f:1.f;}
        n=normalize(n);auto d=decode_normal(encode_normal(n));max_error=std::max(max_error,length(n-d));
        require(length(n-d)<.00008f,"oct-snorm16 normal angular precision");
    }
    std::cout<<"PASS codec: native32 ABI, black/HDR, bit fields and 10006 normals; max normal error="<<max_error<<'\n';
}
void jacobian_test(){
    const Float3 y{0,0,0},n{0,0,1},near_receiver{0,0,1},far_receiver{0,0,2};
    near(jacobian(near_receiver,far_receiver,y,n),4,1e-6,"planar solid-angle gain must scale as inverse distance squared");
    near(jacobian(far_receiver,near_receiver,y,n),.25,1e-7,"reverse planar Jacobian");
    const Float3 a{1,0,2},b{0,0,2};const float j=jacobian(a,b,y,n);
    near(j,8/(5*std::sqrt(5.)),2e-7,"off-axis solid-angle Jacobian includes emission cosine");
    near(j*jacobian(b,a,y,n),1,3e-7,"unclamped reconnect Jacobians must be reciprocal");
    near(jacobian(a*13,b*13,y,n),j,3e-7,"solid-angle Jacobian must be scale invariant");
    near(jacobian({0,0,-1},b,y,n),0,0,"back-facing receiver has zero target solid angle");
    near(jacobian(a,{0,0,-1},y,n),0,0,"zero source cosine must not produce infinity");
    near(jacobian(y,b,y,n),0,0,"zero-length connection must not produce NaN");
    for(auto fixture:std::array<std::array<float,3>,8>{{{.0999f,0.f,1.f/3},{.1f,1.f,1.f/3},{.2f,1.f,1.f/3},{1.f,1.f,1.f},{4.f,1.f,3.f},{10.f,1.f,3.f},{10.001f,0.f,3.f},{0.f,0.f,1.f/3}}}){
        Float value=fixture[0];bool accepted=validate_jacobian(value);
        require(accepted==bool(fixture[1]),"native Jacobian acceptance interval must be inclusive [0.1,10]");
        if(accepted)near(value,fixture[2],1e-7,"accepted Jacobian must clamp to [1/3,3]");
    }
    std::cout<<"PASS jacobian: planar/off-axis geometry, reciprocity, scale, degeneracy and acceptance\n";
}
void gain_test(){
    // For cosine proposals, F=rho*p. RTXPT embeds 1/p only in Lbar;
    // the physical contribution rho*Lo must not depend on proposal density.
    RtxptGIReservoir combined;float target=0;uint total=0;constexpr float rho=.7f,Lo=2.f;
    for(uint i=0;i<16;++i){
        const float pdf=.002f+.019f*float(i);auto v=source(Lo,pdf,1u+i%3);
        v.sample.sp.pos[0]=float(i);v.age=i;const float F=rho*pdf;
        target=F*v.sample.Lo.as_vec3().x;
        combine(combined,v,.371f,target);total+=v.M;
    }
    require(combined.M==total,"effective confidence must sum all source masses");
    finalize(combined,1.f,target*float(combined.M));
    near(combined.weight*target,rho*Lo,5e-7,"constant transport gain depends on candidate PDF or stream count");
    // A black realization still consumes one sample in the Monte Carlo average.
    RtxptGIReservoir masked;auto bright=source(Lo,.2f);float bright_target=rho*.2f*bright.sample.Lo.as_vec3().x;
    combine(masked,bright,.5f,bright_target);
    auto black=source(0,.2f);for(uint i=0;i<3;++i)combine(masked,black,.5f,0.f);
    require(masked.M==4,"black M1 candidates must not be dropped");
    finalize(masked,1.f,bright_target*float(masked.M));
    near(masked.weight*bright_target,rho*Lo/4,2e-7,"black stream mass was lost from normalization");
    // Native basic correction depends on the selected source's target, not
    // automatically on the center receiver target (a common erroneous gain).
    RtxptGIReservoir a=source(),b=source();a.sample.sp.pos[0]=1;b.sample.sp.pos[0]=2;b.M=2;b.weight=4;
    for(bool choose_b:{false,true}){
        RtxptGIReservoir r;combine(r,a,.5f,2.f);combine(r,b,choose_b?0.f:.9999f,3.f);
        const float selected_target=choose_b?3.f:2.f;
        const float source_target=choose_b?5.f:2.f;
        const float pi_sum=choose_b?13.f:16.f;
        finalize(r,source_target,selected_target*pi_sum);
        near(r.weight*selected_target,choose_b?10.:3.25,2e-6,"basic correction selected-source gain");
    }
    RtxptGIReservoir pooled=source();pooled.weight=1;
    auto donor=source();donor.weight=4;donor.M=2;donor.age=7;donor.sample.sp.pos[0]=99;
    require(combine(pooled,donor,.5f,.125f),"RIS equality boundary must select the incoming sample");
    require(pooled.M==3&&pooled.weight==2&&pooled.age==7&&pooled.sample.sp.pos[0]==99,"RIS must copy selected payload/age while retaining pooled mass/weight");
    RtxptGIReservoir invalid;invalid.weight=9;finalize(invalid,1,0);near(invalid.weight,0,0,"zero normalization must yield zero weight");
    std::cout<<"PASS gain: cosine/pdf cancellation, confidence mass, black candidates, selected-source basic correction\n";
}
void mis_test(){
    for(float response:{.001f,.1f,1.f,10000.f}){
        const float k=mis_initial_weight(make_float3(response),make_float3(response));
        near(k,.125,1e-7,"equal true/rough Lambert response must give native fresh weight1/8");
        near(1-k,.875,1e-7,"native reused Lambert weight must be7/8");
        near((1-k)*3.f+k*3.f,3,1e-6,"equal fresh/reused estimators must conserve transport gain");
    }
    near(mis_initial_weight(make_float3(0),make_float3(0)),0,0,"black true BSDF has zero initial MIS weight");
    near(mis_initial_weight(make_float3(-1),make_float3(-1)),0,0,"negative true BSDF must clamp to black");
    near(mis_initial_weight(make_float3(1e8f),make_float3(1e8f)),.125,1e-7,"native BRDF upper clamp is applied before ratio");
    near(mis_initial_weight(make_float3(1e-5f),make_float3(0)),1./1331,2e-9,"rough BRDF floor protects the native cubic MIS ratio");
    // Native GI target/boiling use Rec.709, while final MIS/material similarity
    // use the historical projection. Gray tests cannot distinguish the two.
    for(auto c:std::array<std::array<double,5>,3>{{
        {1,0,0,.2126,.299},{0,1,0,.7152,.587},{0,0,1,.0722,.114}}}){
        Float3 rgb{float(c[0]),float(c[1]),float(c[2])};
        near(luminance_709(rgb),c[3],3e-8,"target/boiling must retain exact native Rec.709 coefficients");
        near(luminance_legacy(rgb),c[4],3e-8,"final MIS/similarity must retain native historical coefficients");
    }
    require(luminance_legacy({1,0,0})>.25f&&luminance_709({1,0,0})<.25f,
            "red-versus-black similarity fixture must distinguish the native 0.25 threshold");
    const std::array<std::array<Float3,2>,4> colored{
        std::array<Float3,2>{Float3{1,0,0},Float3{0,1,0}},
        std::array<Float3,2>{Float3{0,0,1},Float3{1,0,0}},
        std::array<Float3,2>{Float3{4,.05f,.05f},Float3{.05f,4,.05f}},
        std::array<Float3,2>{Float3{.6f,.4f,.2f},Float3{.15f,.25f,.8f}}};
    const std::array<double,4> expected{
        std::pow(.299/(.299+.587+.0001*(.299+.114)),3),
        std::pow(.114/(.114+.299+.0001*(.587+.114)),3),
        std::pow(1.23105/3.5997,3),std::pow(.437/.7198,3)};
    for(size_t i=0;i<colored.size();++i){
        const auto exact=clamp(colored[i][0],0.f,1e4f),rough=clamp(colored[i][1],1e-4f,1e4f);
        const float actual=mis_initial_weight(colored[i][0],colored[i][1]);
        near(actual,expected[i],8e-8,"colored final MIS must match historical native luminance");
        const double rec709_weight=std::pow(double(luminance_709(exact))/luminance_709(exact+rough),3);
        require(std::abs(actual-rec709_weight)>1e-3,"colored fixture must detect accidental Rec.709 final MIS");
    }
    std::cout<<"PASS mis: Lambert partition, gain, black/clamps, distinct target/legacy luminance and four colored native MIS fixtures\n";
}
void depth_guide_test(){
    const Float3 physical{2,-1,3},virtual_position{6,4,8},approximate_position{9,11,12};
    auto depth=[](Float3 p,Float3 origin,Float3 forward){return dot(p-origin,forward);};
    auto accepts=[](float predicted,float previous){return std::abs(predicted-previous)<=.1f*std::max(predicted,previous);};
    auto same=[](Float3 actual,Float3 expected){return actual.x==expected.x&&actual.y==expected.y&&actual.z==expected.z;};
    const auto selected=depth_guide_position(physical,virtual_position,approximate_position,true,false);
    require(accepts(depth(selected,{0,0,0},{0,0,1}),8.f),
            "static mirror must compare virtual guide z8 with history z8, not physical z3");
    require(!accepts(depth(physical,{0,0,0},{0,0,1}),8.f),"static mirror fixture must reject the old physical-position guide");
    require(same(selected,virtual_position),"eligible replacement must choose its complete virtual position");
    const Float3 translated_camera{1,2,1},rotated_camera{1,0,0},rotated_forward{.6f,0,.8f};
    near(depth(selected,translated_camera,{0,0,1}),7.,1e-6,"previous-camera translation must apply to the virtual guide");
    near(depth(selected,rotated_camera,rotated_forward),9.4,2e-6,"previous-camera rotation must transform the chosen world position");
    require(accepts(depth(selected,rotated_camera,rotated_forward),9.4f)&&
            !accepts(depth(physical,rotated_camera,rotated_forward),9.4f),
            "rotated mirror fixture must distinguish virtual and physical depth domains");
    for(bool use_virtual:{false,true}){
        const auto approximate=depth_guide_position(physical,virtual_position,approximate_position,use_virtual,true);
        require(same(approximate,approximate_position),"approximate depth position must take priority over physical and virtual positions");
        near(depth(approximate,translated_camera,{0,0,1}),11.,1e-6,"approximate guide must use previous-camera translation");
        near(depth(approximate,rotated_camera,rotated_forward),14.4,2e-6,"approximate guide must use previous-camera rotation");
    }
    const auto primary=depth_guide_position(physical,virtual_position,approximate_position,false,false);
    require(same(primary,physical),"non-replaced or invalid-branch receiver must ignore the virtual position");
    near(depth(primary,translated_camera,{0,0,1}),2.,1e-6,"physical primary depth must remain in its own domain");
    // The selector returns a guide only; it must not modify transport coordinates.
    require(same(physical,{2,-1,3})&&same(virtual_position,{6,4,8})&&same(approximate_position,{9,11,12}),
            "depth guide selection changed its source positions");
    std::cout<<"PASS depth guide: static/translated/rotated mirrors, approximate priority and physical fallback\n";
}
int main(int argc,char**argv){
    try{
        require(argc==2,"choose analytical fixture");std::string which=argv[1];
        if(which=="codec")codec_test();else if(which=="jacobian")jacobian_test();
        else if(which=="gain")gain_test();else if(which=="mis")mis_test();
        else if(which=="depth")depth_guide_test();
        else require(false,"unknown fixture");return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
'''


@lru_cache(maxsize=1)
def _binary():
    assert HEADER.exists() and SOURCE.exists(), 'Production RTXPT GI component is absent'
    source = SOURCE.read_text(encoding='utf-8-sig')
    start = source.index('namespace vision::rtxpt_gi {')
    end_token = '}// namespace vision::rtxpt_gi'
    end = source.index(end_token, start) + len(end_token)
    helpers = source[start:end]
    header = HEADER.read_text(encoding='utf-8-sig')
    program = SHIM.replace('__PRODUCTION_HELPERS__', helpers)
    program = program.replace('__STORAGE__', _struct(header, 'RtxptGIStorage'))
    program = program.replace('__RESERVOIR__', _struct(header, 'RtxptGIReservoir'))
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        raise SkipTest('RTXPT GI analytical CPU tests require g++ or clang++')
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = tempfile.TemporaryDirectory(prefix='production-', dir=TEMP)
    path = Path(directory.name)
    cpp, binary = path / 'native_math.cpp', path / 'native_math.exe'
    cpp.write_text(program, encoding='utf-8')
    result = subprocess.run([compiler, '-std=c++20', '-O0', str(cpp), '-o', str(binary)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return directory, binary


def _run(fixture):
    _, binary = _binary()
    result = subprocess.run([str(binary), fixture], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.startswith('PASS '), result.stdout
    print(result.stdout.strip())


def test_rtxpt_native32_storage_and_hdr_quantization():
    _run('codec')


def test_rtxpt_solid_angle_reconnection_geometry():
    _run('jacobian')


def test_rtxpt_transport_gain_and_black_stream_mass():
    _run('gain')


def test_rtxpt_final_mis_partition_and_clamp_boundaries():
    _run('mis')


def test_rtxpt_temporal_depth_guide_uses_the_receiver_guide_domain():
    _run('depth')
