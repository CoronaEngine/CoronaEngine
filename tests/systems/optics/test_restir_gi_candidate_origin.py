"""Check side-bit candidate origin reconstruction against production helpers.

Runs the real host offset implementation and Interaction two/three-argument
wrappers. This is CPU FP32 evidence; rendering still validates GPU lowering.
"""
from pathlib import Path
import shutil
import subprocess

from test_restir_gi_compact_output import _definition


ROOT = Path(__file__).resolve().parents[3]


def test_candidate_side_preserves_trace_origin():
    interaction = (ROOT/'vision/src/base/scattering/interaction.h').read_text(encoding='utf-8')
    rtx = (ROOT/'Horizon/src/dsl/types/rtx.h').read_text(encoding='utf-8')
    offset = _definition(rtx, 'inline float3 offset_ray_origin(')
    oriented = _definition(interaction, 'inline auto offset_ray_origin(Pos &&p_in, Normal n_in, W w)')
    two = _definition(interaction, 'static auto custom_offset_ray_origin(Pos &&p_in, Normal &&n_in)')
    three = _definition(interaction, 'static auto custom_offset_ray_origin(Pos &&p_in, Normal &&n_in, W w)')
    half_source = (ROOT/'Horizon/src/math/half.h').read_text(encoding='utf-8')
    half = '\n'.join(_definition(half_source, name) for name in (
        'static constexpr uint16_t float_to_half(', 'static constexpr float half_to_float('))
    program = r'''
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>
#define OC_FORWARD(x) std::forward<decltype(x)>(x)
namespace ocarina {
using std::bit_cast;
struct float3{float x,y,z;};
struct int3{int x,y,z;};
struct bool3{bool x,y,z;};
float3 make_float3(float x,float y,float z){return{x,y,z};}
int3 make_int3(int x,int y,int z){return{x,y,z};}
float3 operator-(float3 a){return{-a.x,-a.y,-a.z};}
float3 operator+(float3 a,float3 b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
float3 operator*(float3 a,float b){return{a.x*b,a.y*b,a.z*b};}
float3 operator*(float b,float3 a){return a*b;}
bool3 operator<(float3 a,float b){return{a.x<b,a.y<b,a.z<b};}
float3 abs(float3 a){return{std::abs(a.x),std::abs(a.y),std::abs(a.z)};}
template<class T>T select(bool condition,T yes,T no){return condition?yes:no;}
float3 select(bool3 c,float3 yes,float3 no){return{c.x?yes.x:no.x,c.y?yes.y:no.y,c.z?yes.z:no.z};}
float dot(float3 a,float3 b){return(a.x*b.x+a.y*b.y)+a.z*b.z;}
ACTUAL_OFFSET
ACTUAL_HALF
}
namespace vision {
using namespace ocarina;
template<typename Pos,typename Normal,typename W>
ACTUAL_ORIENTED
struct Interaction{
    inline static float factor=1.f;
    static float ray_offset_factor(){return factor;}
    template<typename Pos,typename Normal>
    ACTUAL_TWO
    template<typename Pos,typename Normal,typename W>
    ACTUAL_THREE
};
}
using namespace ocarina;
void require(bool value,const char*why){if(!value)throw std::runtime_error(why);}
bool same(float a,float b){return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);}
uint32_t cases=0,fronts=0,backs=0,grazing=0;
void check(float3 position,float3 normal,float3 direction,float factor){
    vision::Interaction::factor=factor;
    auto original=vision::Interaction::custom_offset_ray_origin(position,normal,direction);
    const float d=dot(direction,normal*factor);
    const float side=d>0.f?1.f:-1.f;
    const float stored_side=half_to_float(float_to_half(side));
    require(same(side,stored_side),"FP16 side is not exact");
    auto restored=vision::Interaction::custom_offset_ray_origin(position,normal*stored_side);
    require(same(original.x,restored.x)&&same(original.y,restored.y)&&same(original.z,restored.z),
            "candidate side changed trace origin bits");
    ++cases;fronts+=d>0.f;backs+=d<0.f;grazing+=d==0.f;
}
int main(){
    const float tiny=std::bit_cast<float>(1u);
    const float3 positions[]={
        {0,0,0},{-0.f,0,-0.f},{tiny,-tiny,0},{.03125f,-.03125f,.031249998f},
        {1.f,-1.f,16.f},{1000.f,-2000.f,3000.f},{1e8f,-1e8f,1e-20f},
    };
    const float3 normals[]={
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},
        {.5f,.5f,0},{-.5f,-.5f,0},{.3f,-.4f,.5f},{0,0,0},{-0.f,1,-0.f},
    };
    const float3 directions[]={
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},
        {1,-1,0},{-1,1,0},{1,-1,tiny},{0,0,0},{tiny,-tiny,tiny},
    };
    const float factors[]={-8.f,-1.f,-.125f,-1e-10f,-0.f,0.f,1e-10f,.125f,1.f,8.f};
    for(auto p:positions)for(auto n:normals)for(auto d:directions)for(float f:factors)check(p,n,d,f);
    require(fronts&&backs&&grazing,"missing sided or grazing coverage");
    std::cout<<"PASS candidate side origin "<<cases<<" bit-identical cases, "<<fronts
             <<" front / "<<backs<<" back / "<<grazing<<" grazing\n";
}
'''.replace('ACTUAL_OFFSET', offset).replace('ACTUAL_HALF', half).replace('ACTUAL_ORIENTED', oriented).replace('ACTUAL_TWO', two).replace('ACTUAL_THREE', three)
    folder = ROOT/'.workspace/tmp/restirgi-final-parity-20261107/candidate-origin'
    folder.mkdir(parents=True, exist_ok=True)
    cpp, exe = folder/'origin.cpp', folder/'origin.exe'
    cpp.write_text(program, encoding='utf-8')
    compiler = shutil.which('g++') or shutil.which('clang++')
    assert compiler, 'Candidate origin CPU check requires a C++ compiler'
    for optimization in ('-O0', '-O2'):
        built = subprocess.run([compiler, '-std=c++20', optimization, str(cpp), '-o', str(exe)], capture_output=True, text=True)
        assert built.returncode == 0, built.stdout+built.stderr
        checked = subprocess.run([str(exe)], capture_output=True, text=True)
        assert checked.returncode == 0, checked.stdout+checked.stderr
        print(optimization, checked.stdout, end='')


if __name__ == '__main__':
    test_candidate_side_preserves_trace_origin()
