"""CPU fixtures compile the production final GI combination and roughening view.

They catch a constant mixture, swapped/per-direction weights, proposal-PDF
thresholds, and accidental changes to the captured receiver used for transport.
No engine build, shader compilation, rendering, or GPU work is performed.
"""
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'vision/src').is_dir())
BASE = ROOT / 'vision/src/render_core/integrator/ReSTIR'
TEMP = ROOT / '.workspace/tmp/rtxpt-gi-signal-mis-20261115/mis'


def _function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


SHIM = r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
using uint=std::uint32_t;using Float=float;using Bool=bool;using Uint=uint;
#define $if if
struct Float3{float x{},y{},z{};};
struct Float4{float x{},y{},z{},w{};};
Float3 make_float3(float v){return{v,v,v};}
Float3 make_float3(float x,float y,float z){return{x,y,z};}
Float3 operator+(Float3 a,Float3 b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 operator-(Float3 a,Float3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 operator*(Float3 a,Float3 b){return{a.x*b.x,a.y*b.y,a.z*b.z};}
Float3 operator*(Float3 a,float b){return{a.x*b,a.y*b,a.z*b};}
Float3 operator/(Float3 a,float b){return{a.x/b,a.y/b,a.z/b};}
Float3&operator*=(Float3&a,float b){return a=a*b;}
float dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float length_squared(Float3 a){return dot(a,a);}
float length(Float3 a){return std::sqrt(dot(a,a));}
Float3 normalize(Float3 a){return a/length(a);}
Float3 clamp(Float3 a,float lo,float hi){return{std::clamp(a.x,lo,hi),std::clamp(a.y,lo,hi),std::clamp(a.z,lo,hi)};}
float saturate(float a){return std::clamp(a,0.f,1.f);}
template<class T,class V>T cast(V v){return static_cast<T>(v);}
template<class T,class V>T as(V v){return std::bit_cast<T>(v);}
using std::max;
void near(float actual,double expected,const char*message,double eps=2e-6){
    if(!std::isfinite(actual)||std::abs(double(actual)-expected)>eps){
        std::cerr<<message<<": actual="<<actual<<" expected="<<expected<<'\n';
        throw std::runtime_error(message);
    }
}
void require(bool okay,const char*message){if(!okay)throw std::runtime_error(message);}
namespace vision::rtxpt_gi{
__MIS_HELPERS__
bool native_occluded(auto&&...){return false;}
}
namespace rtxpt_gi=vision::rtxpt_gi;
namespace gi_principled{
constexpr uint BaseNearBit=1u<<8u,CoatNearBit=1u<<9u,IdealGlassBit=1u<<11u;
}
struct SampledWavelengths{};
struct PTSurfaceView{
    std::array<Float4,6> parameters{};
    Float3 f(const Float3&,const SampledWavelengths&)const{
        float preserved=0;
        for(uint i=0;i<6;++i){
            preserved+=(i+1)*(parameters[i].x+2*parameters[i].y+3*parameters[i].z);
            if(i!=0&&i!=5)preserved+=(i+1)*4*parameters[i].w;
        }
        return{parameters[0].w,float(as<uint>(parameters[5].w)),preserved};
    }
__ROUGH_EVALUATOR__
};
struct Point{
    Float3 p{};
    const Point*operator->()const{return this;}
    Float3 position()const{return p;}
};
struct Spectrum{Float3 v{};Float3 as_vec3()const{return v;}};
struct Sample{Point sp;Spectrum Lo;};
struct Reservoir{Sample sample;float weight{};uint M{1};};
struct Interaction{Float3 pos{};};
struct Evaluation{Float3 diffuse{},specular{};};
struct Surface{
    Evaluation selected{make_float3(.7f),make_float3(.3f)};
    Evaluation fresh{make_float3(.25f),make_float3(.75f)};
    Float3 selected_rough{make_float3(1)},fresh_rough{make_float3(1)};
    Evaluation evaluate(Float3 direction,SampledWavelengths)const{return direction.x>.5f?fresh:selected;}
    Float3 f_roughness_clamp(float minimum,Float3 direction,SampledWavelengths)const{
        require(minimum==.2f,"native final MIS must clamp perceptual roughness at 0.2");
        return direction.x>.5f?fresh_rough:selected_rough;
    }
};
struct Params{uint rtxpt_final_visibility{},rtxpt_final_mis{1};};
struct Result{Float3 diffuse,specular;float distance;};
struct FinalShading{
    SampledWavelengths sampled_wavelengths()const{return{};}
    int rtxpt_materials()const{return 0;}
    Result combine_production(Surface surface,Reservoir selected,Reservoir fresh,Params param={})const{
        Interaction it;float hit_distance=0;
__FINAL_COMBINATION__
        hit_distance=hit_distance*selected_weight+initial_distance*initial_weight;
        return{diffuse_result,specular_result,hit_distance};
    }
};
Reservoir reservoir(Float3 position,float radiance,float weight){
    Reservoir r;r.sample.sp.p=position;r.sample.Lo.v=make_float3(radiance);r.weight=weight;return r;
}
void final_test(const std::string&which){
    Surface surface;auto selected=reservoir({0,0,2},4,.5f),fresh=reservoir({3,0,0},6,.5f);
    Params param;
    if(which=="directions"){
        surface.selected={make_float3(2),make_float3(2)};
        surface.fresh_rough=make_float3(4);
    }else if(which=="missing"){
        fresh=reservoir({3,0,0},0,0);
    }else if(which=="tiny"){
        fresh=reservoir({3,0,0},6e-30f,1e30f);
    }else if(which=="delta"){
        surface.selected={};surface.fresh={};
        surface.selected_rough=surface.fresh_rough=make_float3(0);
    }else if(which=="sharp"){
        surface.selected_rough=surface.fresh_rough=make_float3(0);
    }else if(which=="off")param.rtxpt_final_mis=0;
    auto r=FinalShading{}.combine_production(surface,selected,fresh,param);
    if(which=="lambert"){
        near(r.diffuse.x,1.31875,"equal true/rough final diffuse must use 7/8 reused plus 1/8 fresh");
        near(r.specular.x,.80625,"equal true/rough final specular split");
        near(r.distance,2.125,"native weighted hit distance");
    }else if(which=="directions"){
        near(r.diffuse.x,1.958,"reused and initial directions need separate cubic weights");
        near(r.specular.x,1.97,"full true specular response must remain in transport");
        near(r.distance,1.,"directional weights must also combine hit distances");
    }else if(which=="missing"){
        near(r.diffuse.x+r.specular.x,1.75,"missing fresh must retain the native reused directional weight");
        near(r.distance,1.75,"missing fresh cannot contribute a bogus hit distance");
    }else if(which=="tiny"){
        near(r.diffuse.x+r.specular.x,2.5,"tiny positive proposal PDFs must cancel in the estimator");
    }else if(which=="delta"){
        near(r.diffuse.x+r.specular.x,0,"zero continuous delta response remains finite and black");
        near(r.distance,2.,"zero true BRDF must give reusedWeight 1 and freshWeight 0");
    }else if(which=="sharp"){
        near(r.diffuse.x+r.specular.x,2.99970006,"zero rough response must use the native floor rather than suppress the true BSDF");
        near(r.distance,2.99970006,"near-delta hit distance uses the same native cubic weights");
    }else if(which=="off"){
        near(r.diffuse.x,1.4,"MIS disabled uses the unchanged true diffuse transport");
        near(r.specular.x,.6,"MIS disabled uses the unchanged true specular transport");
        near(r.distance,2.,"MIS disabled keeps reused hit distance");
    }else throw std::runtime_error("unknown final fixture");
}
void rough_test(){
    PTSurfaceView surface;
    for(uint i=0;i<6;++i)surface.parameters[i]={float(i+1),float(i+2),float(i+3),float(i+4)};
    surface.parameters[0].w=.03f;
    const uint flags=gi_principled::BaseNearBit|gi_principled::CoatNearBit|gi_principled::IdealGlassBit|63u;
    surface.parameters[5].w=as<float>(flags);
    const auto exact=surface.f({},{});
    const auto rough=surface.f_roughness_clamp(.2f,{},{});
    near(rough.x,.2,"roughened proxy clamps base perceptual roughness before evaluation");
    near(rough.y,gi_principled::CoatNearBit|63u,"roughened base cannot retain ideal-glass or near-specular classification");
    near(rough.z,exact.z,"roughened proxy must retain every other complete-closure parameter");
    const auto after=surface.f({},{});
    near(after.x,.03,"proxy evaluation must not change actual transport roughness");
    near(after.y,flags,"proxy evaluation must not change actual transport delta flags");
    surface.parameters[0].w=.7f;
    near(surface.f_roughness_clamp(.2f,{},{}).x,.7,"rough receiver above the floor must keep its roughness");
}
int main(int argc,char**argv){try{
    require(argc==2,"choose fixture");std::string which=argv[1];
    if(which=="rough")rough_test();else final_test(which);
    std::cout<<"PASS production final MIS "<<which<<'\n';return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''


@lru_cache(maxsize=1)
def _binary():
    source = (BASE / 'rtxpt_gi.cpp').read_text(encoding='utf-8-sig')
    pt = (BASE / 'rtxpt_gi_pt.h').read_text(encoding='utf-8-sig')
    start = source.index('Float3 direction = normalize(', source.index('void RtxptGI::compile_final()'))
    end = source.index('Float3 prefix =', start)
    helpers = '\n'.join(_function(source, name) for name in ('Float luminance_legacy(', 'Float mis_initial_weight('))
    rough = _function(pt, '[[nodiscard]] Float3 f_roughness_clamp(')
    program = ('constexpr bool half_mis = false;\n' + SHIM.replace('__MIS_HELPERS__', helpers)
               .replace('__ROUGH_EVALUATOR__', rough)
               .replace('__FINAL_COMBINATION__', source[start:end]))
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        pytest.skip('CPU production-fragment fixtures require g++ or clang++')
    TEMP.mkdir(parents=True, exist_ok=True)
    cpp, binary = TEMP / 'final_mis.cpp', TEMP / 'final_mis.exe'
    cpp.write_text(program, encoding='utf-8')
    result = subprocess.run([compiler, '-std=c++20', '-O0', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return binary


@pytest.mark.parametrize('fixture', ['lambert', 'directions', 'missing', 'tiny', 'delta', 'sharp', 'off', 'rough'])
def test_production_final_mis(fixture):
    result = subprocess.run([str(_binary()), fixture], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout.strip())
