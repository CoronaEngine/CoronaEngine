"""Run the production private-GI guide emitter with checked scalar buffers.

This catches missing directional relaxation, a first-vertex/invalid-guide leak,
wrong same-plane or clamped neighbor addressing, and destruction of motion bits.
The native fixtures have hand-derived R8 expectations; no test copy of the
conditioning algorithm computes the expected values. GPU integration remains a
separate check of the EDSL compiler and production Pack consumer.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "vision/src/render_core/integrator/ReSTIR/rtxpt_gi_denoiser_prepare.h"
MATH = SOURCE.with_name("rtxpt_gi_denoiser.h")
TEMP = ROOT / ".workspace/tmp/rtxpt-glass-opt-20261115/guide/tests"


def _function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_private_gi_guides_follow_native_terminal_ray_relaxation():
    motion_flags = _function(MATH.read_text(encoding="utf-8-sig"), "Uint native_reblur_motion_flags(")
    # Before this stage exists, Build directly publishes its motion flags.
    # Running that actual producer is the behaviorally failing pre-change case.
    helper = _function(SOURCE.read_text(encoding="utf-8-sig"),
                       "void prepare_native_reblur_guides(") if SOURCE.exists() else ""
    program = r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using uint=std::uint32_t;using Uint=uint;using Int=int;using Float=float;using Bool=bool;
#define $if if
#define $else else
struct Float3 {float x{},y{},z{};};
struct Float4 {float x{},y{},z{},w{};Float3 xyz()const{return{x,y,z};}};
struct Uint2 {uint x{},y{};};
struct Int2 {int x{},y{};};
struct Uint4 {
    uint x{},y{},z{},w{};
    uint&operator[](uint i){return (&x)[i];}
    uint operator[](uint i)const{return (&x)[i];}
};
using uint2=Uint2;using int2=Int2;using uint4=Uint4;
Int2 make_int2(int x,int y){return{x,y};}
Int2 make_int2(int v){return{v,v};}
Int2 make_int2(Uint2 v){return{int(v.x),int(v.y)};}
Int2 operator+(Int2 a,Int2 b){return{a.x+b.x,a.y+b.y};}
Int2 operator-(Int2 a,int b){return{a.x-b,a.y-b};}
Int2 clamp(Int2 v,Int2 lo,Int2 hi){return{std::clamp(v.x,lo.x,hi.x),std::clamp(v.y,lo.y,hi.y)};}
using std::clamp;
float dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Float3 normalize(Float3 v){float l=std::sqrt(dot(v,v));return{v.x/l,v.y/l,v.z/l};}
float saturate(float v){return std::clamp(v,0.f,1.f);}
template<class T,class V>T cast(V v){return static_cast<T>(v);}
// The selected input depths are exact half powers of two. This narrow shim
// rejects any unexpected input instead of substituting a different codec.
uint f32tof16(float v){
    if(v==0.f)return 0u;if(v==2.f)return 0x4000u;if(v==-2.f)return 0xc000u;
    throw std::runtime_error("unexpected motion depth in checked half shim");
}
template<class T>struct Buffer {
    std::vector<T> values;
    mutable uint reads{};uint writes{};
    explicit Buffer(uint count):values(count){}
    T read(uint i)const{++reads;return values.at(i);}
    void write(uint i,const T&v){++writes;values.at(i)=v;}
};
struct RtxptGIPrefixPlane {Float4 direction_vertex;};
using RtxptGIPrefixPlaneVar=RtxptGIPrefixPlane;
namespace reblur {
struct Guide {Float4 position;Uint4 identity;Float4 normal_roughness,diffuse_factor,specular_factor;};
using GuideVar=Guide;
}
constexpr uint PrefixInvalid=0xffffffffu;
MOTION_FLAGS
PRODUCTION_HELPER
static_assert(sizeof(reblur::Guide)==80u);
void require(bool value,const std::string&message){if(!value)throw std::runtime_error(message);}
void equal_bytes(const void*a,const void*b,std::size_t count,const char*message){
    require(std::memcmp(a,b,count)==0,message);
}
constexpr uint width=3u,height=3u,pixels=width*height;
struct Fixture {
    Buffer<RtxptGIPrefixPlane> planes{pixels*3u};
    Buffer<Uint4> headers{pixels};
    Buffer<reblur::Guide> guides{pixels*3u};
    Fixture(){
        for(uint i=0;i<pixels;++i)headers.values[i]={7u,19u,31u,0u};
        for(uint p=0;p<3;++p)for(uint i=0;i<pixels;++i){
            uint a=p*pixels+i;
            planes.values[a].direction_vertex={0.f,0.f,2.f,2.f};
            guides.values[a]={
                {float(i),float(p),9.f,10.f},
                {10u+i,20u+p,headers.values[i][p],native_reblur_motion_flags(-2.f)|0xacu},
                {.1f,.2f,.3f,.03162278f+float(p)*.1f},
                {.4f,.5f,.6f,-2.f}, {.7f,.8f,.9f,2.f}};
        }
    }
    void run(uint x=1u,uint y=1u){
        RUN_HELPER
    }
    uint mix(uint p=1u,uint i=4u)const{return(guides.values[p*pixels+i].identity.w>>8u)&255u;}
    void expect(uint expected,const char*label,uint p=1u,uint i=4u)const{
        require(mix(p,i)==expected,std::string(label)+": native R8 expected="+std::to_string(expected)+
            " actual="+std::to_string(mix(p,i)));
    }
};
int main(){
    try{
        uint cases=0u;
        {
            Fixture f;f.headers.values[3].y=PrefixInvalid;f.run();
            // One invalid neighbor contributes .02: (.02-.00002)*25=.4995 ->127.
            f.expect(127u,"one invalid neighbor");++cases;
        }
        {
            Fixture f;f.run();f.expect(0u,"flat terminal rays");++cases;
        }
        {
            Fixture f;f.headers.values[3].y=f.headers.values[5].y=PrefixInvalid;f.run();
            f.expect(255u,"two invalid neighbors saturate after R8 rounding");++cases;
        }
        {
            Fixture f;for(uint i:{1u,3u,5u,7u})f.headers.values[i].y=PrefixInvalid;
            f.run();f.expect(255u,"four invalid neighbors clamp");++cases;
        }
        {
            Fixture f;f.planes.values[pixels+3].direction_vertex={.6f,0.f,.8f,2.f};
            f.run();f.expect(255u,"divergent ray saturates");++cases;
        }
        {
            Fixture f;f.planes.values[pixels+3].direction_vertex={1.f,0.f,7.f,2.f};
            f.run();
            // Neighbor cosine=7/sqrt(50), relaxation=.2507626 -> R8=64.
            f.expect(64u,"small terminal angular divergence");++cases;
        }
        {
            Fixture f;for(uint i:{1u,3u,5u,7u})f.planes.values[pixels+i].direction_vertex={0.f,0.f,13.f,2.f};
            f.run();f.expect(0u,"nonunit ray normalization");++cases;
        }
        {
            Fixture f;f.headers.values[3].y=PrefixInvalid;
            f.planes.values[pixels+4].direction_vertex.w=1.f;
            f.guides.values[pixels+4].identity.w|=255u<<8u;f.run();
            f.expect(0u,"first vertex never relaxes");++cases;
        }
        {
            Fixture f;f.headers.values[3].y=PrefixInvalid;
            auto&g=f.guides.values[pixels+4];g.identity.w&=~1u;g.identity.w|=47u<<8u;
            auto before=g;f.run();equal_bytes(&before,&g,sizeof(g),"invalid guide changed");++cases;
        }
        {
            Fixture f;f.headers.values[4].y=PrefixInvalid;
            auto before=f.guides.values[pixels+4];f.run();
            equal_bytes(&before,&f.guides.values[pixels+4],sizeof(before),"invalid center branch changed");++cases;
        }
        {
            Fixture f;f.guides.values[pixels+3].identity.w=0u;
            f.guides.values[pixels+3].position.w=0.f;
            f.planes.values[pixels+3].direction_vertex={1.f,0.f,7.f,2.f};f.run();
            f.expect(64u,"valid sky branch uses its terminal direction");++cases;
        }
        {
            Fixture f;f.headers.values[3].y=91u;f.run();
            f.expect(0u,"unequal valid branches add no edge penalty");++cases;
        }
        for(auto pixel:std::array<Uint2,8>{{{0,0},{1,0},{2,0},{0,1},{2,1},{0,2},{1,2},{2,2}}}){
            Fixture f;uint i=pixel.y*width+pixel.x;
            // Each clamped outside neighbor repeats the valid center. Exactly
            // one in-bounds horizontal neighbor is invalid for all 8 cases.
            uint n=pixel.x==0u?i+1u:i-1u;
            f.headers.values[n].y=PrefixInvalid;f.run(pixel.x,pixel.y);
            f.expect(127u,"screen boundary clamps instead of adding an edge",1u,i);++cases;
        }
        {
            Fixture f;f.headers.values[3].x=PrefixInvalid;f.headers.values[5].z=PrefixInvalid;
            auto original=f.guides.values;auto planes=f.planes.values;auto headers=f.headers.values;
            f.run();f.expect(127u,"plane zero has its own neighbors",0u);
            f.expect(0u,"plane one unaffected by other planes",1u);
            f.expect(127u,"plane two has its own neighbors",2u);
            for(uint a=0;a<original.size();++a){
                auto actual=f.guides.values[a];auto expected=original[a];
                actual.identity.w&=~0xff00u;expected.identity.w&=~0xff00u;
                equal_bytes(&actual,&expected,sizeof(actual),"conditioning changed guide data outside mix bits");
            }
            equal_bytes(f.planes.values.data(),planes.data(),planes.size()*sizeof(planes[0]),"terminal planes mutated");
            equal_bytes(f.headers.values.data(),headers.data(),headers.size()*sizeof(headers[0]),"prefix headers mutated");
            auto first=f.guides.values;f.run();
            equal_bytes(f.guides.values.data(),first.data(),first.size()*sizeof(first[0]),"conditioning is not idempotent");++cases;
        }
        std::cout<<"PASS native guide conditioning: "<<cases
            <<" ray/edge/validity/plane cases, immutable guides and binary16 motion flags\n";
        return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
'''
    program = program.replace("MOTION_FLAGS", motion_flags).replace("PRODUCTION_HELPER", helper)
    call = "prepare_native_reblur_guides(planes, headers, guides, Uint2{x,y}, Uint2{width,height});"
    program = program.replace("RUN_HELPER", call if helper else "(void)x; (void)y;")
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise SkipTest("Private GI guide production-source regression requires g++ or clang++")
    TEMP.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=TEMP) as temporary:
        directory = Path(temporary)
        cpp, exe = directory / "guide_conditioning.cpp", directory / "guide_conditioning.exe"
        cpp.write_text(program, encoding="utf-8")
        result = subprocess.run([compiler, "-std=c++20", "-O0", str(cpp), "-o", str(exe)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.startswith("PASS native guide conditioning:"), result.stdout
        print(result.stdout.strip())


if __name__ == "__main__":
    test_private_gi_guides_follow_native_terminal_ray_relaxation()
