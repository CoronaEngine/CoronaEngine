"""Execute production Falcor DI sample/PDF helpers in a standalone CPU shim.

These fixtures verify the new importance contract. The previous uniform sampler
with its uniform PDF is valid; it does not satisfy this new conditional-PDF input.
No engine build, GPU, scene mutation or simulated convergence is involved.
"""
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = next(p for p in Path(__file__).resolve().parents if (p/'vision/src').is_dir())
COMMON = ROOT/'vision/src/render_core/integrator/falcor_di/common.h'
TEMP = ROOT/'.workspace/tmp/gi-di-fix-20261115/production-helper-tests'


def definition(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth, end = 1, begin+1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


SHIM = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using uint=std::uint32_t;using Uint=uint;using Float=float;using Bool=bool;
#define $if if
#define $else else
struct Float2{float x{},y{};};
struct Float3{float x{},y{},z{};};
Float3 make_float3(float v){return{v,v,v};}
Float3 make_float3(float x,float y,float z){return{x,y,z};}
Float3 operator-(Float3 a,Float3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 operator/(Float3 a,float b){return{a.x/b,a.y/b,a.z/b};}
Float2 operator*(Float2 a,float b){return{a.x*b,a.y*b};}
Float2 operator+(Float2 a,float b){return{a.x+b,a.y+b};}
Float2 operator/(Float2 a,float b){return{a.x/b,a.y/b};}
Float2 floor(Float2 a){return{std::floor(a.x),std::floor(a.y)};}
Float2 saturate(Float2 a){return{std::clamp(a.x,0.f,1.f),std::clamp(a.y,0.f,1.f)};}
float dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float length(Float3 a){return std::sqrt(dot(a,a));}
float uniform_sphere_PDF(){return 1.f/(4.f*3.14159265358979323846f);}
constexpr uint InvalidUI32=0xffffffffu;
namespace vision{float PDF_wi(float p,Float3 n,Float3 delta){return p*dot(delta,delta)/std::abs(dot(n,delta)/length(delta));}}
struct Context{uint environment_index{5u};};
struct LightEvalContext{Float3 pos{},ng{};float PDF_pos{};};
struct Surface{Float3 position{};};
struct TriangleSample{Float3 direction{};float distance{};Float3 radiance{};float pdf{};Float3 target{};bool infinite=false;};
bool finite(float x){return std::isfinite(x);}
__CANONICAL__
__EVALUATE__
void require(bool okay,const char*message){if(!okay)throw std::runtime_error(message);}
void near(float actual,float expected,const char*message){
 if(!std::isfinite(actual)||std::abs(actual-expected)>2e-5f*std::max(1.f,std::abs(expected))){
  std::cerr<<message<<": actual="<<actual<<" expected="<<expected<<'\n';throw std::runtime_error(message);}}
void pdf_test(){
 Context ctx;Surface receiver{{-5.f,2.f,9.f}};Float3 emission{1.f,2.f,3.f};
 for(float pdf:{.000001f,.01f,.37f,2.f,1000.f}){
  LightEvalContext point{{.6f,.8f,0.f},{},pdf};
  auto sample=evaluate_light_point(ctx,5u,emission,point,receiver);
  near(sample.pdf,pdf,"environment conditional PDF must come from resolved point");
  near(sample.radiance.x,emission.x/pdf,"environment radiance divided by conditional PDF once");
  near(sample.radiance.y,emission.y/pdf,"environment green conditional estimator");
  near(sample.direction.x,.6f,"stored environment direction is receiver independent");
 }
 for(float pdf:{0.f,-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){
  LightEvalContext point{{1,0,0},{},pdf};
  auto sample=evaluate_light_point(ctx,5u,emission,point,receiver);
  require(sample.pdf==0&&sample.distance==0&&sample.radiance.x==0,"invalid environment density must not divide or trace");
 }
 // This control locks down the original area-light geometry conversion.
 Surface origin;LightEvalContext area{{0,0,2},{0,0,-1},.25f};
 auto a=evaluate_light_point(ctx,2u,emission,area,origin);
 near(a.pdf,1.f,"area PDF conversion unchanged");near(a.radiance.z,3.f,"area radiance unchanged");
 std::cout<<"PASS actual production conditional PDF, invalid density and area controls\n";
}
__CANONICAL_TEST__
int main(int argc,char**argv){try{__MAIN__return 0;}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''


@lru_cache(None)
def executable(canonical=False):
    source = COMMON.read_text(encoding='utf-8-sig')
    evaluate = definition(source,'[[nodiscard]] inline TriangleSample evaluate_light_point(')
    # Reuse the exact production distant endpoint constant.
    start = source.index('inline constexpr float environment_ray_distance')
    constant = source[start:source.index(';',start)+1]
    program = SHIM.replace('__EVALUATE__',constant+'\n'+evaluate)
    if canonical:
        implementation = definition(source,'[[nodiscard]] inline Float2 canonical_environment_uv(')
        fixture = r'''
void canonical_test(){
 for(uint i=0;i<65536u;++i){
  float u=float(i)/65535.f;auto once=canonical_environment_uv({u,1.f-u});
  auto twice=canonical_environment_uv(once);
  require(once.x==twice.x&&once.y==twice.y,"UV16 canonicalization must be idempotent");
  near(once.x,u,"canonical UV is same16bit reservoir coordinate");
 }
 auto edge=canonical_environment_uv({-1.f,2.f});near(edge.x,0,"lower clamp");near(edge.y,1,"upper clamp");
 auto u=canonical_environment_uv({.1234567f,.9876543f});
 require(std::uint32_t(u.x*65535.f+.5f)==std::uint32_t(.1234567f*65535.f+.5f),"same packing as reservoir");
 std::cout<<"PASS actual production UV16 canonicalization for65536 coordinates\n";
}'''
        program=program.replace('__CANONICAL__',implementation).replace('__CANONICAL_TEST__',fixture).replace('__MAIN__','canonical_test();')
    else:
        program=program.replace('__CANONICAL__','').replace('__CANONICAL_TEST__','').replace('__MAIN__','pdf_test();')
    compiler=shutil.which('g++')
    if not compiler: pytest.skip('g++ required for standalone CPU production-helper fixture')
    TEMP.mkdir(parents=True,exist_ok=True)
    suffix='canonical' if canonical else 'pdf'
    path,binary=TEMP/f'{suffix}.cpp',TEMP/f'{suffix}.exe'
    path.write_text(program,encoding='utf8')
    result=subprocess.run([compiler,'-std=c++20','-O2',str(path),'-o',str(binary)],text=True,capture_output=True)
    assert result.returncode==0,result.stdout+result.stderr
    return binary


def run(canonical=False):
    result=subprocess.run([str(executable(canonical))],text=True,capture_output=True)
    assert result.returncode==0,result.stdout+result.stderr


def test_production_environment_sample_consumes_conditional_pdf():
    run()


def test_production_environment_uv16_is_canonical_before_target():
    run(True)


def test_production_delta_environment_keeps_legacy_uv_without_hdr_warp():
    source=COMMON.read_text(encoding='utf-8-sig')
    begin=source.index('inline constexpr float environment_ray_distance')
    constant=source[begin:source.index(';',begin)+1]
    helper='\n'.join(definition(source,marker) for marker in (
        '[[nodiscard]] inline Float2 environment_uv(',
        '[[nodiscard]] inline Float2 canonical_environment_uv(',
        '[[nodiscard]] inline Float2 sample_environment_uv('))
    mocks=r'''
constexpr float Pi=3.14159265358979323846f;
using std::clamp;using std::atan2;
Float2 make_float2(float x,float y){return{x,y};}
Float3 normalize(Float3 v){return v/length(v);}
template<class T>T select(bool c,T a,T b){return c?a:b;}
enum class LightType{DeltaDirection};
struct LightSampleContext{Float3 pos{};};
struct Environment{
 bool delta{};mutable uint samples{},contexts{};
 bool match(LightType)const{return delta;}
 Float2 sample_only(Float2 u)const{++samples;if(delta)throw std::runtime_error("delta entered HDR CDF");return u;}
 LightEvalContext compute_light_eval_context(const LightSampleContext&,Float2)const{++contexts;return{{1,0,0},{},0};}
};
struct LightSampler{
 Environment env;uint dispatches{};
 const Environment*env_light()const{return &env;}
 template<class F>void dispatch_environment(F f){++dispatches;f(&env);}
};
struct Renderer{LightSampler lights;LightSampler*light_sampler(){return &lights;}};
struct Toolkit{static Renderer&renderer(){static Renderer value;return value;}};
'''
    fixture=r'''
void delta_test(){
 auto&lights=Toolkit::renderer().lights;lights.env.delta=true;
 Float2 random{.1234567f,.9876543f};auto unchanged=sample_environment_uv(random);
 require(unchanged.x==random.x&&unchanged.y==random.y,"delta keeps original uncanonicalized world UV");
 require(lights.dispatches==0&&lights.env.samples==0&&lights.env.contexts==0,"delta bypasses unsupported HDR warp");
 lights.env.delta=false;auto sampled=sample_environment_uv(random);
 auto expected=canonical_environment_uv({.5f,0.f});
 near(sampled.x,expected.x,"continuous environment uses canonical sampled world direction");
 near(sampled.y,expected.y,"continuous azimuth uses sampled world direction");
 require(lights.dispatches==1&&lights.env.samples==1&&lights.env.contexts==1,"one public direction-only HDR warp");
 std::cout<<"PASS actual production delta bypass and continuous HDR warp boundary\n";
}'''
    program=SHIM.replace('__CANONICAL__',mocks+'\n'+helper).replace('__EVALUATE__',constant+'\n'+definition(source,'[[nodiscard]] inline TriangleSample evaluate_light_point('))
    program=program.replace('__CANONICAL_TEST__',fixture).replace('__MAIN__','delta_test();')
    compiler=shutil.which('g++')
    if not compiler:pytest.skip('g++ required for standalone CPU production-helper fixture')
    TEMP.mkdir(parents=True,exist_ok=True)
    path,binary=TEMP/'delta.cpp',TEMP/'delta.exe'
    path.write_text(program,encoding='utf8')
    compiled=subprocess.run([compiler,'-std=c++20','-O2',str(path),'-o',str(binary)],text=True,capture_output=True)
    assert compiled.returncode==0,compiled.stdout+compiled.stderr
    result=subprocess.run([str(binary)],text=True,capture_output=True)
    assert result.returncode==0,result.stdout+result.stderr
