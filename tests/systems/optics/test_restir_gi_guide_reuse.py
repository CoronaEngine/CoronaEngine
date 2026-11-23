"""Behavioral checks for the real private-PT terminal guide overlay codec.

The production helpers compile with scalar host types. No alternate codec,
renderer build, shader compiler or GPU is used by this test.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "vision/src/render_core/integrator/restir/gi/restir_gi_prefix_storage.h"
TEMP = ROOT / ".workspace/tmp/gi-guide-reuse-20261119/overlay-tests"
HELPERS = (
    "pt_prefix_depth", "pt_prefix_consumed", "pt_prefix_ideal_path",
    "pt_prefix_guide_valid", "pt_prefix_store_controls", "pt_prefix_store_guide",
    "pt_prefix_decode_guide",
)


def _production_helpers(source_path=None):
    source = (source_path or SOURCE).read_text(encoding="utf-8-sig")
    bodies = []
    for name in HELPERS:
        needle = name + "("
        assert needle in source, f"Missing production guide overlay helper: {name}"
        name_start = source.index(needle)
        start = source.rfind("\n", 0, name_start) + 1
        opening = source.index("{", name_start)
        depth, end = 1, opening + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        bodies.append(source[start:end])
    constants = "\n".join(line for line in source.splitlines()
                          if line.startswith("constexpr uint PtPrefix"))
    return constants + "\n" + "\n".join(bodies)


def _run_overlay_codec(*, prefix_source=None, extra_helpers="", extra_checks=""):
    helpers = _production_helpers(prefix_source)
    program = r'''
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
using uint=std::uint32_t;using Uint=uint;using Float=float;using Bool=bool;
#define $if if
struct Float3 {float x{},y{},z{};};
struct Float4 {float x{},y{},z{},w{};Float3 xyz()const{return{x,y,z};}};
struct Uint4 {
    uint x{},y{},z{},w{};
    uint&operator[](uint i){return (&x)[i];}
    uint operator[](uint i)const{return (&x)[i];}
};
Float4 make_float4(Float3 v,float w){return{v.x,v.y,v.z,w};}
Float4 make_float4(float x,float y,float z,float w){return{x,y,z,w};}
Float4 make_float4(float v){return{v,v,v,v};}
Uint4 make_uint4(uint x,uint y,uint z,uint w){return{x,y,z,w};}
Uint4 make_uint4(uint v){return{v,v,v,v};}
template<class T,class V>T cast(V v){return static_cast<T>(v);}
template<class T>T select(bool p,T a,T b){return p?a:b;}
namespace ocarina {
template<class T,class V>T as(V v){return std::bit_cast<T>(v);}
using ::select;
}
namespace reblur {
struct Guide {
    Float4 position{};Uint4 identity{};Float4 normal_roughness{};
    Float4 diffuse_factor{1.f,1.f,1.f,0.f},specular_factor{1.f,1.f,1.f,0.f};
};using GuideVar=Guide;
}
struct ReSTIRGIPrefixPlane {
    Uint4 hit_words{};Float4 origin_distance{},direction_vertex{},throughput_flags{};
    Uint4 interiors_branch{};Float4 virtual_position{};
};using ReSTIRGIPrefixPlaneVar=ReSTIRGIPrefixPlane;
PRODUCTION_HELPERS
EXTRA_HELPERS
void require(bool value,const char*message){if(!value)throw std::runtime_error(message);}
void equal(const void*a,const void*b,std::size_t n,const char*message){require(std::memcmp(a,b,n)==0,message);}
uint finite_bits(uint bits){return (bits&0x7f800000u)==0x7f800000u?bits^0x00800000u:bits;}
static_assert(sizeof(reblur::Guide)==80u&&sizeof(ReSTIRGIPrefixPlane)==96u);
int main(){
    try{
        uint64_t cases=0u;
        // Motion Z was explicitly binary16-packed by the original producer.
        // Motion XY must retain arbitrary finite FP32 producer bits, including
        // values whose nominal shader half roundtrip the driver eliminated.
        for(uint h=0u;h<65536u;++h){
            if((h&0x7c00u)==0x7c00u)continue;
            for(uint depth=0u;depth<=5u;++depth)for(uint control=0u;control<4u;++control){
                ReSTIRGIPrefixPlane node;
                node.hit_words={0x12345678u,0xabcdef01u,0x3eaaaaabu,0x80000000u};
                node.origin_distance={91.f,92.f,93.f,-17.123456f};
                node.direction_vertex={.2534567f,-.501234f,3.1234f,float(depth)};
                node.interiors_branch={0xdeadbeefu,0x87654321u,0x11111111u,0xffffffffu};
                node.virtual_position={.12345679f,std::bit_cast<float>(0x80000000u),123456.789f,31.123456f};
                const auto transport=node;
                pt_prefix_store_controls(node,depth,(control&1u)!=0u,(control&2u)!=0u);
                require(!pt_prefix_guide_valid(node),"fresh terminal controls leaked valid guide");
                require(pt_prefix_depth(node)==depth,"terminal depth changed");
                require(pt_prefix_consumed(node)==((control&1u)!=0u),"consumed flag changed");
                require(pt_prefix_ideal_path(node)==((control&2u)!=0u),"ideal flag changed");
                const uint controls=node.interiors_branch.w;
                const auto empty=pt_prefix_decode_guide(node,0x1234u,0x5678u,std::bit_cast<float>(0x7fc00001u));
                reblur::Guide default_guide;
                default_guide.normal_roughness={0.f,0.f,1.f,1.f};
                equal(&empty,&default_guide,sizeof(empty),"invalid guide defaults changed");
                reblur::Guide guide;
                guide.position=node.virtual_position;
                guide.identity={node.hit_words.x,0x713579bdu,0x13579bdfu,3u|(h<<16u)};
                guide.normal_roughness={std::bit_cast<float>(0x80000000u),std::bit_cast<float>(1u),-.33333334f,.03162278f};
                const uint motion_x_bits=h==0u?0x39007000u:finite_bits(h*0x9e3779b9u);
                const uint motion_y_bits=h==0u?0xb9007001u:finite_bits(h*0x85ebca6bu+0x27d4eb2du);
                guide.diffuse_factor={.040000003f,65504.f,std::bit_cast<float>(0x3f800001u),std::bit_cast<float>(motion_x_bits)};
                guide.specular_factor={-.125f,std::bit_cast<float>(0x80000000u),std::bit_cast<float>(0x3eaaaaabu),std::bit_cast<float>(motion_y_bits)};
                const float motion_y=guide.specular_factor.w;
                pt_prefix_store_guide(node,guide);
                require(pt_prefix_guide_valid(node),"valid guide flag lost");
                require((node.interiors_branch.w&PtPrefixControlMask)==controls,"guide clobbered terminal controls");
                equal(&node.hit_words,&transport.hit_words,sizeof(node.hit_words),"terminal hit changed");
                equal(&node.origin_distance.w,&transport.origin_distance.w,sizeof(float),"travelled length changed");
                equal(&node.direction_vertex,&transport.direction_vertex,sizeof(float)*3u,"terminal direction changed");
                equal(&node.virtual_position,&transport.virtual_position,sizeof(Float4),"virtual position/depth changed");
                const auto immutable=node;
                const auto decoded=pt_prefix_decode_guide(node,guide.identity.z,guide.identity.y,motion_y);
                equal(&guide,&decoded,sizeof(guide),"Guide80 roundtrip changed bits");
                equal(&node,&immutable,sizeof(node),"guide decode mutated terminal source");
                ++cases;
            }
        }
        // Relaxation belongs to the later register-only conditioning stage.
        // Storing a guide must keep all transport controls and strip only the
        // nonpersistent reserved/relaxation bits from its identity flags.
        for(uint relaxation=0u;relaxation<256u;++relaxation){
            ReSTIRGIPrefixPlane node;
            node.hit_words.x=17u;
            pt_prefix_store_controls(node,5u,true,true);
            reblur::Guide guide;
            guide.identity={17u,19u,23u,0x80000003u|0xfcu|(relaxation<<8u)};
            pt_prefix_store_guide(node,guide);
            require(pt_prefix_depth(node)==5u&&pt_prefix_consumed(node)&&pt_prefix_ideal_path(node),
                    "nonpersistent flags changed transport controls");
            require(pt_prefix_decode_guide(node,23u,19u,guide.specular_factor.w).identity.w==0x80000003u,
                    "register-only relaxation leaked into terminal source");
        }
        EXTRA_CHECKS
        std::cout<<"PASS PT terminal overlay: "<<cases<<" exact Guide80/control roundtrips; 256 flag cases\n";
    }catch(const std::exception&error){std::cerr<<error.what()<<'\n';return 1;}
}
'''.replace("PRODUCTION_HELPERS", helpers).replace("EXTRA_HELPERS", extra_helpers).replace("EXTRA_CHECKS", extra_checks)
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise SkipTest("A C++20 compiler is required for the real overlay codec test")
    TEMP.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=TEMP) as temporary:
        directory = Path(temporary)
        cpp, exe = directory / "overlay.cpp", directory / "overlay.exe"
        cpp.write_text(program, encoding="utf-8")
        result = subprocess.run([compiler, "-std=c++20", "-O2", str(cpp), "-o", str(exe)],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "PASS PT terminal overlay:" in result.stdout, result.stdout
        print(result.stdout.strip())


def test_pt_terminal_overlay_preserves_guide_and_controls_exactly():
    _run_overlay_codec()


def test_pt_terminal_overlay_preserves_actual_gpu_fp32_motion():
    _run_overlay_codec(extra_checks=r'''
        uint fp32_cases=0u;
        for(uint motion_x_bits:{0u,0x80000000u,1u,0x80000001u,0x007fffffu,0x807fffffu,
            0x00800000u,0x80800000u,0x33800000u,0x33800001u,0x39007000u,0x39008000u,
            0xb9007000u,0x39007001u,0x3f800001u,0xbf800001u,0x477fe001u,0x47800000u,
            0x7f7fffffu,0xff7fffffu,0x3eaaaaabu,0xbeaaaaabu,0x40000000u,0xc0000000u})
        for(uint motion_y_bits:{0u,0x80000000u,1u,0x80000001u,0x007fffffu,0x807fffffu,
            0x00800000u,0x80800000u,0x33800000u,0x33800001u,0x39007000u,0x39008000u,
            0xb9007000u,0x39007001u,0x3f800001u,0xbf800001u,0x477fe001u,0x47800000u,
            0x7f7fffffu,0xff7fffffu,0x3eaaaaabu,0xbeaaaaabu,0x40000000u,0xc0000000u}){
            ReSTIRGIPrefixPlane node;
            node.hit_words.x=17u;
            node.virtual_position={1.f,2.f,3.f,4.f};
            pt_prefix_store_controls(node,2u,false,true);
            reblur::Guide guide;
            guide.position=node.virtual_position;
            guide.identity={17u,19u,23u,0xc0000003u};
            guide.normal_roughness={.1f,.2f,.3f,.03162278f};
            guide.diffuse_factor={.4f,.5f,.6f,std::bit_cast<float>(motion_x_bits)};
            guide.specular_factor={.7f,.8f,.9f,std::bit_cast<float>(motion_y_bits)};
            pt_prefix_store_guide(node,guide);
            const auto decoded=pt_prefix_decode_guide(node,23u,19u,guide.specular_factor.w);
            equal(&decoded,&guide,sizeof(guide),"actual GPU FP32 motion was requantized");
            ++fp32_cases;
        }
        std::cout<<"PASS FP32 XY motion: "<<fp32_cases<<" independent edge-pattern pairs\n";
    ''')


if __name__ == "__main__":
    test_pt_terminal_overlay_preserves_guide_and_controls_exactly()
    test_pt_terminal_overlay_preserves_actual_gpu_fp32_motion()
