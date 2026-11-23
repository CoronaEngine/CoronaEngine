"""Run the production register-only GI guide conditioning with scalar buffers.

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
SOURCE = ROOT / "vision/src/render_core/integrator/ReSTIR/restir_gi_denoiser_prepare.h"
MATH = SOURCE.with_name("restir_gi_denoiser.h")
TEMP = ROOT / ".workspace/tmp/gi-guide-reuse-20261119/conditioning-tests"


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
    helper = "template<typename Planes, typename Headers>\n" + _function(
        SOURCE.read_text(encoding="utf-8-sig"), "void condition_native_reblur_guide(")
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
struct ReSTIRGIPrefixPlane {Float4 direction_vertex;};
using ReSTIRGIPrefixPlaneVar=ReSTIRGIPrefixPlane;
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
    Buffer<ReSTIRGIPrefixPlane> planes{pixels*3u};
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
    call = r'''
        uint i=y*width+x;
        for(uint plane=0u;plane<3u;++plane){
            uint address=plane*pixels+i;
            auto guide=guides.read(address);
            auto terminal=planes.read(address);
            condition_native_reblur_guide(planes,headers,guide,terminal.direction_vertex.xyz(),
                uint(terminal.direction_vertex.w),headers.read(i)[plane],
                Uint2{x,y},Uint2{width,height},plane);
            guides.write(address,guide);
        }
    '''
    program = program.replace("RUN_HELPER", call)
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


def test_pt_guide_loader_gates_stale_planes_and_reads_valid_miss_neighbors():
    from test_restir_gi_guide_reuse import _run_overlay_codec

    source = SOURCE.read_text(encoding="utf-8-sig")
    condition = "template<typename Planes, typename Headers>\n" + _function(
        source, "void condition_native_reblur_guide(")
    loader_signature = "reblur::GuideVar load_pt_reblur_guide("
    loader_start = source.index(loader_signature)
    loader = source[source.rfind("template<", 0, loader_start):loader_start] + _function(source, loader_signature)
    shim = r'''
#include <array>
#include <cmath>
using Int=int;
#define $else else
struct Uint2 {uint x{},y{};};
struct Int2 {int x{},y{};};
using int2=Int2;
Int2 make_int2(int x,int y){return{x,y};}
Int2 make_int2(int v){return{v,v};}
Int2 make_int2(Uint2 v){return{int(v.x),int(v.y)};}
Int2 operator+(Int2 a,Int2 b){return{a.x+b.x,a.y+b.y};}
Int2 operator-(Int2 a,int b){return{a.x-b,a.y-b};}
Int2 clamp(Int2 v,Int2 lo,Int2 hi){return{std::clamp(v.x,lo.x,hi.x),std::clamp(v.y,lo.y,hi.y)};}
float dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Float3 normalize(Float3 v){float l=std::sqrt(dot(v,v));return{v.x/l,v.y/l,v.z/l};}
float saturate(float v){return std::clamp(v,0.f,1.f);}
constexpr uint PrefixInvalid=0xffffffffu,PrefixQueued=0xfffffffeu;
template<class T>struct CheckedBuffer {
    std::array<T,27u> values{};
    std::array<bool,27u> forbidden{};
    mutable uint reads{};
    T read(uint i)const{
        if(forbidden.at(i))throw std::runtime_error("stale or cross-plane read");
        ++reads;return values.at(i);
    }
};
'''
    checks = r'''
        {
            constexpr uint width=3u,height=3u,pixels=width*height,center=4u;
            CheckedBuffer<ReSTIRGIPrefixPlane> planes;
            CheckedBuffer<Uint4> headers;
            CheckedBuffer<float> motion_y;
            for(auto&h:headers.values)h={7u,19u,31u,0u};
            planes.forbidden.fill(true);
            motion_y.forbidden.fill(true);
            reblur::Guide empty;
            empty.normal_roughness={0.f,0.f,1.f,1.f};
            uint material_calls=0u;
            auto material=[&](uint instance){
                ++material_calls;
                require(instance==17u,"material lookup used stale instance");return 73u;
            };
            uint loader_cases=0u;
            for(uint plane=0u;plane<3u;++plane){
                const uint address=plane*pixels+center;
                // Poisoned previous-frame bytes deliberately look eligible.
                // Current invalid/queued headers must suppress even the read.
                planes.values[address].hit_words.x=0xffffffffu;
                planes.values[address].interiors_branch.w=0xffffffffu;
                for(uint branch:{PrefixInvalid,PrefixQueued}){
                    headers.values[center][plane]=branch;
                    planes.reads=0u;motion_y.reads=0u;material_calls=0u;
                    const auto actual=load_pt_reblur_guide(planes,headers,motion_y,Uint2{1u,1u},
                        Uint2{width,height},plane,material);
                    equal(&actual,&empty,sizeof(actual),"stale header did not return exact empty Guide80");
                    require(planes.reads==0u&&motion_y.reads==0u&&material_calls==0u,"stale header touched terminal/motionY/material data");
                    ++loader_cases;
                }
                // A current miss or ineligible consumed secondary may read its
                // current record, but its poisoned instance must stay unused.
                headers.values[center][plane]=7u+plane*12u;
                planes.forbidden[address]=false;
                pt_prefix_store_controls(planes.values[address],2u,true,true);
                planes.reads=0u;motion_y.reads=0u;material_calls=0u;
                auto actual=load_pt_reblur_guide(planes,headers,motion_y,Uint2{1u,1u},
                    Uint2{width,height},plane,material);
                equal(&actual,&empty,sizeof(actual),"ineligible terminal did not return exact empty Guide80");
                require(planes.reads==1u&&motion_y.reads==0u&&material_calls==0u,"ineligible terminal performed motionY/material/neighbor work");
                ++loader_cases;

                reblur::Guide guide;
                guide.position={.12345679f,-7.f,13.f,19.f};
                guide.identity={17u,73u,headers.values[center][plane],0xc0000003u};
                guide.normal_roughness={.1f,.2f,.3f,.03162278f};
                guide.diffuse_factor={.4f,.5f,.6f,std::bit_cast<float>(0x39007000u)};
                guide.specular_factor={.7f,.8f,.9f,std::bit_cast<float>(0xb9007001u)};
                auto&node=planes.values[address];
                node.hit_words.x=17u;node.virtual_position=guide.position;
                node.direction_vertex={0.f,0.f,2.f,1.f};
                pt_prefix_store_controls(node,1u,true,true);
                pt_prefix_store_guide(node,guide);
                motion_y.values[address]=guide.specular_factor.w;
                motion_y.forbidden[address]=false;
                const auto immutable=node;
                const auto immutable_motion_y=motion_y.values;
                planes.reads=0u;motion_y.reads=0u;material_calls=0u;
                actual=load_pt_reblur_guide(planes,headers,motion_y,Uint2{1u,1u},
                    Uint2{width,height},plane,material);
                equal(&actual,&guide,sizeof(actual),"first-vertex or consumed-primary Guide80 changed");
                equal(&node,&immutable,sizeof(node),"loader mutated immutable terminal source");
                equal(motion_y.values.data(),immutable_motion_y.data(),sizeof(immutable_motion_y),"loader mutated motionY sidecar");
                require(planes.reads==1u&&motion_y.reads==1u&&material_calls==1u,"first vertex read neighbors or skipped material/motionY");
                ++loader_cases;
                planes.forbidden[address]=true;
                motion_y.forbidden[address]=true;
            }
            // Guide-ineligible neighbors with current branch records still
            // contribute their terminal rays to the native divergence test.
            constexpr uint plane=1u,address=plane*pixels+center;
            for(uint i:{center,1u,3u,5u,7u}){
                planes.forbidden[plane*pixels+i]=false;
                planes.values[plane*pixels+i].direction_vertex={0.f,0.f,2.f,.1f};
            }
            planes.values[plane*pixels+3u].direction_vertex={1.f,0.f,7.f,.1f};
            planes.values[plane*pixels+3u].interiors_branch.w=0u;
            headers.values[center].y=19u;
            pt_prefix_store_controls(planes.values[address],2u,false,false);
            reblur::Guide guide;
            guide.position={2.f,3.f,5.f,7.f};
            guide.identity={17u,73u,19u,0xc0000003u};
            guide.normal_roughness={.1f,.2f,.3f,.03162278f};
            guide.diffuse_factor={.4f,.5f,.6f,std::bit_cast<float>(0x39007000u)};
            guide.specular_factor={.7f,.8f,.9f,std::bit_cast<float>(0xb9007001u)};
            planes.values[address].virtual_position=guide.position;
            pt_prefix_store_guide(planes.values[address],guide);
            motion_y.values[address]=guide.specular_factor.w;
            motion_y.forbidden[address]=false;
            const auto before_planes=planes.values;const auto before_headers=headers.values;
            const auto before_motion_y=motion_y.values;
            planes.reads=0u;headers.reads=0u;motion_y.reads=0u;material_calls=0u;
            auto actual=load_pt_reblur_guide(planes,headers,motion_y,Uint2{1u,1u},
                Uint2{width,height},plane,material);
            guide.identity.w|=64u<<8u;
            equal(&actual,&guide,sizeof(actual),"valid miss-neighbor ray was dropped from native R8=64 relaxation");
            require(planes.reads==5u&&headers.reads==5u&&motion_y.reads==1u&&material_calls==1u,"loader did not bound current-plane motionY/material reads");
            equal(planes.values.data(),before_planes.data(),sizeof(before_planes),"loader mutated terminal neighbors");
            equal(headers.values.data(),before_headers.data(),sizeof(before_headers),"loader mutated current headers");
            equal(motion_y.values.data(),before_motion_y.data(),sizeof(before_motion_y),"loader mutated motionY sidecar neighbors");
            ++loader_cases;
            headers.values[center].y=PrefixInvalid;
            planes.forbidden[address]=true;motion_y.forbidden[address]=true;
            planes.reads=0u;motion_y.reads=0u;material_calls=0u;
            actual=load_pt_reblur_guide(planes,headers,motion_y,Uint2{1u,1u},
                Uint2{width,height},plane,material);
            equal(&actual,&empty,sizeof(actual),"vanished plane retained previous-frame guide");
            require(planes.reads==0u&&motion_y.reads==0u&&material_calls==0u,"vanished plane read previous-frame source/motionY");
            ++loader_cases;
            std::cout<<"PASS PT guide loader: "<<loader_cases<<" stale/queued/ineligible/consumed/miss-neighbor cases\n";
        }
'''
    _run_overlay_codec(prefix_source=SOURCE.with_name("restir_gi_prefix_storage.h"),
                       extra_helpers=shim + condition + loader, extra_checks=checks)


if __name__ == "__main__":
    test_private_gi_guides_follow_native_terminal_ray_relaxation()
    test_pt_guide_loader_gates_stale_planes_and_reads_valid_miss_neighbors()
