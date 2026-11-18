"""CPU execution of production ReSTIR ray builders and local light payload.

Before the ReSTIR-only header exists the fixture executes the actual legacy
spawn_ray_to template, so red reports geometry/range errors, not missing symbols.
"""
from functools import lru_cache
from pathlib import Path
import re
import shutil
import subprocess
import pytest
from test_shadow_ray_endpoint import extract_function

ROOT=Path(__file__).resolve().parents[3]
HEADER=ROOT/'vision/src/base/integral/restir_shadow.h'
TEMP=ROOT/'.workspace/tmp/restir-shadow-audit-20261115/production-tests'

SHIM=r'''
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>
using std::bit_cast;using std::max;using uint=unsigned;using Uint=uint;using Float=float;using Bool=bool;
#define $if if
#define $else else
constexpr uint InvalidUI32=~0u;
struct float3{float x{},y{},z{};};using Float3=float3;
struct int3{int x,y,z;};struct bool3{bool x,y,z;};
struct Float2{float x{},y{};};
Float3 make_float3(float x,float y,float z){return{x,y,z};}Float3 make_float3(float a){return{a,a,a};}
int3 make_int3(int x,int y,int z){return{x,y,z};}
Float3 operator+(Float3 a,Float3 b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 operator-(Float3 a,Float3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 operator-(Float3 a){return{-a.x,-a.y,-a.z};}
Float3 operator*(Float3 a,float s){return{a.x*s,a.y*s,a.z*s};}
Float3 operator*(float s,Float3 a){return a*s;}
Float3 operator/(Float3 a,float s){return a*(1/s);}Float3&operator*=(Float3&a,float s){a=a*s;return a;}
bool3 operator<(Float3 a,float b){return{a.x<b,a.y<b,a.z<b};}
Float3 abs(Float3 a){return{std::abs(a.x),std::abs(a.y),std::abs(a.z)};}
float dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}float length(Float3 a){return std::sqrt(dot(a,a));}
Float3 normalize(Float3 a){return a/length(a);}
bool finite(float f){return std::isfinite(f);}
template<class T>T select(bool b,T a,T c){return b?a:c;}
Float3 select(bool3 b,Float3 a,Float3 c){return{b.x?a.x:c.x,b.y?a.y:c.y,b.z?a.z:c.z};}
struct Float4{float x{},y{},z{},w{};Float3 xyz()const{return{x,y,z};}};
Float4 make_float4(Float3 a,float w){return{a.x,a.y,a.z,w};}
struct RayVar{Float4 org_min,dir_max;};template<class T>using ray_t=RayVar;
RayVar make_ray(Float3 p,Float3 w,float t){return{make_float4(p,0),make_float4(w,t)};}
__OFFSET__
struct Interaction{Float3 pos,ng;
 static Float3 custom_offset_ray_origin(Float3 p,Float3 n,Float3 w){return offset_ray_origin(p,n*select(dot(n,w)>0,1.f,-1.f));}};
struct Context{uint environment_index=2;};struct Surface{Float3 position;};
struct LightEvalContext{Float3 pos,ng;float PDF_pos;};
struct TriangleSample{Float3 direction{};float distance{};Float3 radiance{};float pdf{};Float3 target{};bool infinite=false;bool delta=false;};
constexpr float environment_ray_distance=1e16f;
namespace vision{float PDF_wi(float pdf,Float3 normal,Float3 delta){return pdf*dot(delta,delta)/(std::abs(dot(normal,delta))/length(delta));}}
__EVALUATE_POINT__
__BUILDERS__
void require(bool b,const char*m){if(!b)throw std::runtime_error(m);}
void near(float a,float b,float tolerance,const char*m){require(std::abs(a-b)<tolerance,m);}
float plane_parameter(RayVar r,uint axis,float position){return (position-(axis==0?r.org_min.x:r.org_min.z))/(axis==0?r.dir_max.x:r.dir_max.z);}
void late_blocker(){auto r=test_finite({0,0,0},{0,0,100});float t=plane_parameter(r,2,99.9995f);
 require(t>r.org_min.w&&t<r.dir_max.w,"late real occluder must not be removed by a relative or .001 endpoint gap");}
void offset_target(){Float3 origin{.2f,0,.25f},target{10,0,.2505f};auto r=test_finite(origin,target);
 float target_t=plane_parameter(r,2,target.z);require(target_t>r.dir_max.w,"target light must not hit itself after origin offset");
 auto expected=(target-origin)/length(target-origin);
 require(bit_cast<uint>(r.dir_max.x)==bit_cast<uint>(expected.x)&&bit_cast<uint>(r.dir_max.z)==bit_cast<uint>(expected.z),"nonzero finite direction must remain bit-identical");
 auto reached=r.org_min.xyz()+r.dir_max.xyz()*target_t;near(reached.x,target.x,1e-5f,"ray must aim at the actual fixed endpoint");}
void environment(){Interaction it{{1000,0,1000},{0,0,1}};Float3 target=it.pos+normalize(Float3{1,0,1})*10;
 auto r=test_visibility(it,target,true);auto wanted=normalize(target-it.pos);
 near(r.dir_max.x,wanted.x,1e-7f,"environment direction x must stay at the shading direction");
 near(r.dir_max.z,wanted.z,1e-7f,"origin offset must not re-aim an infinite environment toward a fake endpoint");
 near(r.dir_max.w,100000.f,.01f,"environment uses native fixed directional travel");}
void near_segment(){auto r=test_finite({0,0,0},{0,0,.00005f});require(r.dir_max.w==0,"near segment must clamp absolute epsilon to zero");require(std::isfinite(r.dir_max.z),"near direction must remain finite");}
void zero_segment(){auto r=test_finite({2,3,4},{2,3,4});
 require(std::isfinite(r.dir_max.x)&&std::isfinite(r.dir_max.y)&&std::isfinite(r.dir_max.z),"zero segment direction must remain finite");
 near(length(r.dir_max.xyz()),1.f,1e-7f,"empty segment still needs a valid unit ray direction");
 require(r.org_min.w==0&&r.dir_max.w==0,"empty finite segment has zero interval");}
void payload(){Context c;Surface s{{-9358.03125f,6908.1474609375f,4564.7392578125f}};
 LightEvalContext point{{1096.217529296875f,-8255.5458984375f,1132.293212890625f},{-1,0,0},.25f};
 auto sample=evaluate_light_point(c,1u,{1,2,3},point,s);require(sample.pdf>0,"valid oblique area point control");
 require(sample.target.x==point.pos.x&&sample.target.y==point.pos.y&&sample.target.z==point.pos.z,"finite local payload must retain exact sampled point, not direction-distance roundtrip");
 require(!sample.infinite,"area point must stay finite");
 auto reconstructed=s.position+sample.direction*sample.distance;
 require(std::abs(reconstructed.x-point.pos.x)>.0001f,"float32 witness must expose a roundtrip error larger than endpoint epsilon");
 auto env=evaluate_light_point(c,2u,{1,2,3},{{.6f,.8f,0},{},.25f},s);
 require(env.infinite,"environment identity must be explicit in local payload");}
int main(int argc,char**argv){try{std::string mode=argv[1];if(mode=="late")late_blocker();else if(mode=="offset")offset_target();else if(mode=="env")environment();else if(mode=="near")near_segment();else if(mode=="zero")zero_segment();else payload();std::cout<<"PASS production shadow "<<mode<<'\n';return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''


@lru_cache(None)
def executable():
    interaction=(ROOT/'vision/src/base/scattering/interaction.h').read_text(encoding='utf-8-sig')
    horizon=(ROOT/'Horizon/src/dsl/types/rtx.h').read_text(encoding='utf-8-sig')
    common=(ROOT/'vision/src/render_core/integrator/falcor_di/common.h').read_text(encoding='utf-8-sig')
    offset=extract_function(horizon,'inline float3 offset_ray_origin(')
    evaluate=extract_function(common,'[[nodiscard]] inline TriangleSample evaluate_light_point(')
    if HEADER.exists():
        header=HEADER.read_text(encoding='utf-8-sig')
        parts=re.findall(r'inline constexpr float \w+\s*=\s*[^;]+;',header)
        parts.extend(extract_function(header,s) for s in ('[[nodiscard]] inline RayVar finite_ray(',
            '[[nodiscard]] inline RayVar direction_ray(', '[[nodiscard]] inline RayVar visibility_ray('))
        parts.extend(['RayVar test_finite(Float3 p,Float3 q){return finite_ray(p,q);}',
                      'RayVar test_visibility(const Interaction&it,Float3 target,bool infinite){return visibility_ray(it,target,infinite);}'])
    else:
        legacy='template<typename T>\n'+extract_function(interaction,'[[nodiscard]] ray_t<T> spawn_ray_to(T p_start, T n_start, T p_target)')
        parts=['constexpr float ShadowEpsilon=.0001f;',legacy,
               'RayVar test_finite(Float3 p,Float3 q){return spawn_ray_to(p,Float3{},q);}',
               'RayVar test_visibility(const Interaction&it,Float3 target,bool){return spawn_ray_to(it.pos,it.ng,target);}']
    program=SHIM.replace('__OFFSET__',offset).replace('__EVALUATE_POINT__',evaluate).replace('__BUILDERS__','\n'.join(parts))
    compiler=shutil.which('g++')
    if not compiler:pytest.skip('g++ required for production geometry fixture')
    TEMP.mkdir(parents=True,exist_ok=True);cpp=TEMP/'shadow.cpp';exe=TEMP/'shadow.exe';cpp.write_text(program)
    built=subprocess.run([compiler,'-std=c++20','-O0',str(cpp),'-o',str(exe)],capture_output=True,text=True)
    assert built.returncode==0,built.stdout+built.stderr
    return exe


@pytest.mark.parametrize('mode',['late','offset','env','near','payload','zero'])
def test_production_restir_shadow_geometry(mode):
    result=subprocess.run([str(executable()),mode],capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
