"""Run native history image layout through CPU typed-store boundaries."""
from pathlib import Path
import shutil
import subprocess

from test_restir_gi_compact_output import _definition


ROOT = Path(__file__).resolve().parents[3]


def test_history_native_image_layout():
    source = (ROOT/'vision/src/render_core/integrator/restir_gi/history.h').read_text(encoding='utf-8')
    history = _definition(source, 'class HistoryImages {', True)
    device = (ROOT/'vision/src/render_core/integrator/restir_gi/device.h').read_text(encoding='utf-8')
    codec = '\n'.join(_definition(device, name) for name in (
        'inline Uint pack_image_half2(', 'inline Float2 unpack_image_half2('))
    half_source = (ROOT/'Horizon/src/math/half.h').read_text(encoding='utf-8')
    half = '\n'.join(_definition(half_source, name) for name in (
        'static constexpr uint16_t float_to_half(', 'static constexpr float half_to_float('))
    program = r'''
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
using uint=uint32_t;using Uint=uint;
struct Float2{float x,y;};
struct Float3{float x,y,z;};
struct Float4{float x,y,z,w;Float3 xyz()const{return{x,y,z};}};
using float4=Float4;
struct Uint2{uint x,y;};using uint2=Uint2;
Float2 make_float2(float x,float y){return{x,y};}
Float4 make_float4(Float3 xyz,float w){return{xyz.x,xyz.y,xyz.z,w};}
Uint2 make_uint2(uint x,uint y){return{x,y};}
Float3 operator+(Float3 a,Float3 b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 operator-(Float3 a,Float3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 operator*(Float3 a,float b){return{a.x*b,a.y*b,a.z*b};}
Float3 operator+(Float3 a,float b){return{a.x+b,a.y+b,a.z+b};}
Float3 operator-(Float3 a,float b){return{a.x-b,a.y-b,a.z-b};}
namespace ocarina {
using std::bit_cast;
ACTUAL_HALF
uint f32tof16(float value){return float_to_half(value);}
float f16tof32(uint value){return half_to_float(uint16_t(value));}
}
ACTUAL_CODEC
enum class PixelStorage{FLOAT4,HALF4,UINT2};
struct Texture2D{
    struct Storage{
        PixelStorage format;
        uint width;
        std::vector<Float4> floats;
        std::vector<Uint2> uints;
    };
    std::shared_ptr<Storage> storage;
    template<class T>T read_native(Uint2 pixel)const{
        const uint i=pixel.x+pixel.y*storage->width;
        if constexpr(std::is_same_v<T,Float4>)return storage->floats.at(i);
        else return storage->uints.at(i);
    }
    void write_native(Uint2 pixel,Float4 value)const{
        if(storage->format==PixelStorage::HALF4){
            auto half=[](float f){return ocarina::half_to_float(ocarina::float_to_half(f));};
            value={half(value.x),half(value.y),half(value.z),half(value.w)};
        }
        storage->floats.at(pixel.x+pixel.y*storage->width)=value;
    }
    void write_native(Uint2 pixel,Uint2 value)const{
        storage->uints.at(pixel.x+pixel.y*storage->width)=value;
    }
};
struct Device{
    uint bytes_per_cell=0;
    Texture2D create_texture2d(uint2 extent,PixelStorage format,const std::string&){
        const uint bytes=format==PixelStorage::FLOAT4?16u:8u;
        bytes_per_cell+=bytes;
        return{std::make_shared<Texture2D::Storage>(Texture2D::Storage{
            format,extent.x,std::vector<Float4>(extent.x*extent.y),std::vector<Uint2>(extent.x*extent.y)})};
    }
};
struct SampleVar{
    Float4 hit_distance{},origin_cosine{},radiance_count{},normal_weight{};
    uint light_seed=0;
};
ACTUAL_HISTORY
void require(bool value,const char*why){if(!value)throw std::runtime_error(why);}
void near(float value,float expected,float tolerance=1e-6f){require(std::abs(value-expected)<=tolerance,"native history numeric value");}
uint bits(float value){return std::bit_cast<uint>(value);}
void exact_xyz(Float4 a,Float4 b){require(bits(a.x)==bits(b.x)&&bits(a.y)==bits(b.y)&&bits(a.z)==bits(b.z),"FP32 origin changed");}
int main(){
    Device device;HistoryImages history;history.allocate(device,{3,2},"test");
    require(device.bytes_per_cell==48,"history logical size/format changed");
    SampleVar sample;
    sample.origin_cosine={1000.f,-2000.f,3000.f,-.625f};
    sample.hit_distance={1001.001f,-2002.003f,3000.3f,7.25f};
    sample.radiance_count={.1f,.2f,.3f,20.f};
    sample.normal_weight={.6f,.8f,-1.f,1.5f};
    sample.light_seed=0x7fc12345u;
    history.write({2,1},sample);
    auto restored=history.read({2,1});
    exact_xyz(restored.origin_cosine,sample.origin_cosine);
    near(restored.origin_cosine.w,-.625f);near(restored.hit_distance.w,7.25f);
    near(restored.hit_distance.x,sample.hit_distance.x,.002f);
    near(restored.hit_distance.y,sample.hit_distance.y,.002f);
    near(restored.hit_distance.z,sample.hit_distance.z,.002f);
    near(restored.radiance_count.x,.0999755859375f);
    near(restored.radiance_count.y,.199951171875f);
    near(restored.radiance_count.z,.300048828125f);
    near(restored.radiance_count.w,20.f);near(restored.normal_weight.w,1.5f);
    near(restored.normal_weight.x,.599609375f);near(restored.normal_weight.y,.7998046875f);
    near(restored.normal_weight.z,-1.f);
    require(restored.light_seed==sample.light_seed,"seed field/bit pattern changed");
    for(uint seed:{0u,0xffffffffu,0x7f800001u,0x80000000u}){
        sample.light_seed=seed;history.write({2,1},sample);
        require(history.read({2,1}).light_seed==seed,"seed not stored as integer");
    }
    sample.origin_cosine={-0.f,std::bit_cast<float>(1u),1.f,.5f};
    sample.hit_distance={1.f,2.f,3.f,4.f};
    history.write({1,0},sample);
    exact_xyz(history.read({1,0}).origin_cosine,sample.origin_cosine);
    history.write({0,0},SampleVar{});
    const auto zero=history.read({0,0});
    require(zero.light_seed==0&&zero.radiance_count.w==0&&zero.normal_weight.w==0,"zero/background metadata");
    near(zero.hit_distance.x,0);near(zero.normal_weight.x,0);
    std::cout<<"PASS native48 history origin/offset/radiance/normal/cosine/MW/seed and formats\n";
}
'''.replace('ACTUAL_HALF', half).replace('ACTUAL_CODEC', codec).replace('ACTUAL_HISTORY', history)
    folder = ROOT/'.workspace/tmp/restirgi-final-parity-20261107/history-images'
    folder.mkdir(parents=True, exist_ok=True)
    cpp, exe = folder/'history.cpp', folder/'history.exe'
    cpp.write_text(program, encoding='utf-8')
    compiler = shutil.which('g++') or shutil.which('clang++')
    assert compiler, 'Native history CPU check requires a C++ compiler'
    built = subprocess.run([compiler, '-std=c++20', '-O0', str(cpp), '-o', str(exe)], capture_output=True, text=True)
    assert built.returncode == 0, built.stdout+built.stderr
    checked = subprocess.run([str(exe)], capture_output=True, text=True)
    assert checked.returncode == 0, checked.stdout+checked.stderr
    print(checked.stdout, end='')


if __name__ == '__main__':
    test_history_native_image_layout()
