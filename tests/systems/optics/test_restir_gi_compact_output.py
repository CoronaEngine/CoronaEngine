"""Execute the production compact GI codec against native-format golden words.

The actual codec bodies and Horizon half conversion run through CPU scalar/vector
boundaries. This catches field order, half rounding and UNORM truncation errors;
it does not test DSL lowering or GPU half conversion.
"""
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[3]


def _definition(source, signature, trailing_semicolon=False):
    assert signature in source, f'Missing production codec: {signature}'
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + (';' if trailing_semicolon else '')


def test_compact_temporal_output_native_words():
    source = (ROOT/'vision/src/render_core/integrator/restir_gi/device.h').read_text(encoding='utf-8-sig')
    codec = '\n'.join([
        _definition(source, 'struct TemporalOutput {', True),
        *[_definition(source, signature) for signature in (
            'inline Uint pack_normal_11_10_11(',
            'inline Float3 unpack_normal_11_10_11(',
            'inline Uint pack_image_half2(',
            'inline Float2 unpack_image_half2(',
            'inline Uint pack_pixel(',
            'inline Uint2 unpack_pixel(',
            'inline Uint4 pack_temporal_output(',
            'inline TemporalOutput unpack_temporal_output(',
        )],
    ])
    half_source = (ROOT/'Horizon/src/math/half.h').read_text(encoding='utf-8-sig')
    half = '\n'.join(_definition(half_source, signature) for signature in (
        'static constexpr uint16_t float_to_half(', 'static constexpr float half_to_float('))
    program = r'''
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
using uint=uint32_t; using Uint=uint; using Float=float;
struct float2 {float x,y;}; using Float2=float2;
struct float3 {float x,y,z;}; using Float3=float3;
struct uint4 {uint x,y,z,w;}; using Uint4=uint4;
struct uint2 {uint x,y;}; using Uint2=uint2;
float2 make_float2(float x,float y){return{x,y};}
float3 make_float3(float x,float y,float z){return{x,y,z};}
float3 make_float3(float2 xy,float z){return{xy.x,xy.y,z};}
uint4 make_uint4(uint x,uint y,uint z,uint w){return{x,y,z,w};}
uint2 make_uint2(uint x,uint y){return{x,y};}
float3 operator*(float3 v,float s){return{v.x*s,v.y*s,v.z*s};}
float3 operator-(float3 v,float s){return{v.x-s,v.y-s,v.z-s};}
float3 normalize(float3 v){float r=1.f/std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);return v*r;}
template<class T,class U>T cast(U x){return static_cast<T>(x);}
namespace ocarina {
using std::bit_cast;using std::clamp;using std::isnan;using std::max;using ::make_float2;using ::normalize;using ::cast;
template<class T,class U>T as(U x){return std::bit_cast<T>(x);}
template<class T>T select(bool condition,T yes,T no){return condition?yes:no;}
ACTUAL_HALF
uint f32tof16(float value){return float_to_half(value);}
float f16tof32(uint value){return half_to_float(uint16_t(value));}
}
ACTUAL_CODEC
void require(bool pass,const char*why){if(!pass)throw std::runtime_error(why);}
void near(float actual,float expected){require(std::abs(actual-expected)<2e-6f,"normal decode normalization/bit allocation");}
int main(){
    static_assert(sizeof(uint4)==16);
    struct NormalCase{float3 input;uint packed;float3 decoded;};
    const NormalCase normals[]={
        {{1,0,0},0x7fefffffu,{.999999403f,-.000977516f,-.000488519f}},
        {{-1,0,0},0x7feff800u,{-.999999403f,-.000977516f,-.000488519f}},
        {{0,1,0},0x7ffffbffu,{-.000488520f,.999999761f,-.000488520f}},
        {{0,-1,0},0x7fe003ffu,{-.000488520f,-.999999761f,-.000488520f}},
        {{0,0,1},0xffeffbffu,{-.000488519f,-.000977516f,.999999403f}},
        {{0,0,-1},0x000ffbffu,{-.000488519f,-.000977516f,-.999999403f}},
        {{0,0,0},0x7feffbffu,{-.408115494f,-.816629345f,-.408115494f}},
        {{.6f,.8f,0},0x7ffcc665u,{.600282014f,.799788262f,-.000489227f}},
        {{2,-2,0},0x7fe007ffu,{.707106739f,-.707106739f,-.000345436f}},
        {{.5f,-.5f,.5f},0xbfe7fdffu,{.576691564f,-.578665430f,.576691564f}},
        {{-.5f,.5f,-.5f},0x3ff7f9ffu,{-.577819751f,.576410159f,-.577819751f}},
    };
    for(const auto&c:normals){
        require(pack_normal_11_10_11(c.input)==c.packed,"native normal word changed (truncate, no encode normalization)");
        auto d=unpack_normal_11_10_11(c.packed);near(d.x,c.decoded.x);near(d.y,c.decoded.y);near(d.z,c.decoded.z);
    }
    // Distinct field values catch component swaps. Depth is raw linear FP32.
    const auto raw=pack_temporal_output(3.25f,{1.f,-2.f,.5f},4.f,{0,1,0});
    require(raw.x==0x40500000u&&raw.y==0xc0003c00u&&raw.z==0x44003800u&&raw.w==0x7ffffbffu,"compact field order/width");
    const auto decoded=unpack_temporal_output(raw);
    require(decoded.depth==3.25f&&decoded.hit_offset.x==1.f&&decoded.hit_offset.y==-2.f&&decoded.hit_offset.z==.5f&&decoded.luminance==4.f,"compact decode components");
    // Fixed half cases include ties-to-even on both sides, gradual underflow,
    // maximum finite and overflow. No hidden clamping may alter native words.
    struct HalfCase{float value;uint half;};
    const HalfCase halves[]={
        {0.f,0x0000u},{-0.f,0x8000u},{1.f,0x3c00u},{-2.f,0xc000u},
        {0x1.002p0f,0x3c00u},{0x1.006p0f,0x3c02u},
        {0x1p-25f,0x0000u},{0x1.8p-24f,0x0002u},{0x1p-24f,0x0001u},
        {0x1p-14f,0x0400u},{65504.f,0x7bffu},{65520.f,0x7c00u},
    };
    for(const auto&c:halves){
        auto p=pack_temporal_output(1.f,{c.value,c.value,c.value},c.value,{0,0,1});
        require(p.y==(c.half|(c.half<<16u))&&p.z==p.y,"native half conversion/word order");
    }
    const uint depth_bits[]={0u,0x80000000u,0x00000001u,0x3c23d70au,0x461c4000u,0x7fc12345u};
    for(uint bits:depth_bits){
        auto p=pack_temporal_output(std::bit_cast<float>(bits),{0,0,0},0.f,{0,1,0});
        require(p.x==bits&&std::bit_cast<uint>(unpack_temporal_output(p).depth)==bits,"FP32 depth must bitcast without reciprocal/quantization");
    }
    // Decode zeros has native normal(-1,-1,-1), not an invalid sentinel.
    auto zero=unpack_temporal_output({0,0,0,0});near(zero.hit_normal.x,-.577350269f);
    require(zero.depth==0&&zero.luminance==0,"zero data fields");
    require(pack_pixel({0,0})==0&&pack_pixel({1919,1079})==0x0437077fu,"pixel payload layout");
    for(auto pixel : {uint2{0,0},uint2{1919,1079},uint2{65535,65535}}){
        auto decoded_pixel=unpack_pixel(pack_pixel(pixel));
        require(decoded_pixel.x==pixel.x&&decoded_pixel.y==pixel.y,"pixel payload roundtrip");
    }
    std::cout<<"PASS compact GI16: normal, half, depth and XY payload layout\n";
}
'''.replace('ACTUAL_HALF', half).replace('ACTUAL_CODEC', codec)
    output = ROOT/'.workspace/tmp/restirgi-textures-20261107/cpu-packing'
    output.mkdir(parents=True, exist_ok=True)
    cpp, binary = output/'packing.cpp', output/'packing.exe'
    cpp.write_text(program, encoding='utf-8')
    compiler = shutil.which('g++') or shutil.which('clang++')
    assert compiler, 'CPU packing test needs a host C++ compiler'
    built = subprocess.run([compiler, '-std=c++20', '-O0', str(cpp), '-o', str(binary)], capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.startswith('PASS compact GI16:')
    print(result.stdout, end='')


if __name__ == '__main__':
    test_compact_temporal_output_native_words()
