"""Run the production GI44 codec, views and allocation paths on CPU boundaries."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT=next(p for p in Path(__file__).resolve().parents if (p/'vision/src').is_dir())
HEADER=ROOT/'vision/src/render_core/integrator/ReSTIR/gi_reservoir_storage.h'
INDIRECT=ROOT/'vision/src/render_core/integrator/ReSTIR/indirect.h'


def _function(source,signature):
    start=source.index(signature);opening=source.index('{',start);end=opening+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]


def test_gi44_preserves_estimator_fields_and_history_slices():
    assert HEADER.exists(), 'Production GI44 storage header is absent'
    source=HEADER.read_text(encoding='utf-8-sig')
    source=re.sub(r'^#(?:include|pragma).*$', '',source,flags=re.M)
    source=source.replace('OC_STRUCT(vision, GIReservoirStorage, words) {};',
                          'namespace vision { using GIReservoirStorageVar=GIReservoirStorage; }')
    indirect=INDIRECT.read_text(encoding='utf-8-sig')
    accessors='\n'.join(_function(indirect,f'[[nodiscard]] auto {name}() const noexcept')
                        for name in ['prev_reservoirs','cur_reservoirs','passthrough_reservoirs'])
    implementation=INDIRECT.with_suffix('.cpp').read_text(encoding='utf-8-sig')
    allocations=[]
    for signature in ('void ReSTIRGI::prepare(', 'void ReSTIRGI::update_resolution('):
        method=_function(implementation,signature)
        allocations.append(_function(method, 'if (!rtxpt_backend_)'))
    oct_source=(ROOT/'Horizon/src/math/common_lib.inl.h').read_text(encoding='utf-8-sig')
    oct_decode=_function(oct_source,'[[nodiscard]]  inline auto octahedral_decode(float2 p)')
    program=r'''
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
#include <vector>
#define $if if
using uint=uint32_t;using Uint=uint;using Int=int;using Float=float;
struct float2{float x,y;};struct float3{float x,y,z;};using Float3=float3;
float2 make_float2(float x,float y){return{x,y};}
float3 make_float3(float x,float y,float z){return{x,y,z};}
float3 make_float3(float v){return{v,v,v};}
float3 operator/(float3 v,float s){return{v.x/s,v.y/s,v.z/s};}
float length(float3 v){return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z);}
float rsqrt(float x){return 1.f/std::sqrt(x);}
template<class T>T select(bool c,T a,T b){return c?a:b;}
template<class T>T sign(T x){return x>=0?T(1):T(-1);}
template<class T,class U>T cast(U x){return static_cast<T>(x);}
using std::abs;using std::floor;using std::clamp;
namespace ocarina {
using std::string;using std::move;using std::isnan;using std::isinf;
using ::make_float2;using ::make_float3;using ::rsqrt;using ::select;
using std::abs;using std::floor;using std::clamp;
template<class T,class U>T as(U x){return std::bit_cast<T>(x);}
struct ByteBufferDesc{uintptr_t address;size_t offset,size;uintptr_t head()const{return address;}};
template<class T>struct BindlessArrayBuffer {
    std::vector<T>*data;size_t offset,count;
    T read(uint i)const{if(i>=count)throw std::runtime_error("OOB read");return data->at(offset+i);}
    void write(uint i,const T&v){if(i>=count)throw std::runtime_error("OOB write");data->at(offset+i)=v;}
};
OCT_DECODE
}
namespace vision {
template<class T,size_t N>using array=std::array<T,N>;
struct SurfacePoint {
    array<float,3>pos{},ng{};
    void set_normal(float3 v){ng={v.x,v.y,v.z};}
    float3 normal()const{return{ng[0],ng[1],ng[2]};}
    SurfacePoint*operator->(){return this;}const SurfacePoint*operator->()const{return this;}
};
struct GISample{SurfacePoint sp;array<float,3>Lo{};float age{};};
struct GIReservoir{float weight_sum{},C{},W{};GISample sample;};
using GIReservoirVar=GIReservoir;
}
ACTUAL_STORAGE
using namespace vision;
using std::vector;
std::vector<GIReservoirStorage>storage;
size_t count=17;
struct Slice {size_t offset{},count{};};
std::array<Slice,2>registered;
struct Bindless {
    template<class T>ocarina::BindlessArrayBuffer<T>buffer_var(uint slot){
        const auto &view=registered.at(slot);return{&storage,view.offset,view.count};
    }
}bindings;
struct Pipeline{Bindless&bindless_array(){return bindings;}size_t pixel_num()const{return count;}}pipe;
Pipeline*pipeline(){return &pipe;}
struct HostAllocation{size_t elements{};std::string name;};
struct Device {
    template<class T>HostAllocation create_buffer(size_t n,const char*name){
        static_assert(std::is_same_v<T,GIReservoirStorage>);return{n,name};
    }
}device_boundary;
Device&device(){return device_boundary;}
struct RegisteredReservoirs {
    HostAllocation allocation;
    HostAllocation&super(){return allocation;}
    void register_self(size_t offset,size_t length){registered[0]={offset,length};}
    void register_view(size_t offset,size_t length){registered[1]={offset,length};}
    void register_view_index(uint slot,size_t offset,size_t length){registered.at(slot)={offset,length};}
    void upload_immediately(const GIReservoirStorage*values){storage.assign(values,values+allocation.elements);}
};
struct Harness {
    RegisteredReservoirs reservoirs_;
    bool rtxpt_backend_{};
    uint reservoir_base()const{return 0;}
    ACCESSORS
    void prepare(){Pipeline*rp=pipeline();PREPARE_ALLOCATION}
    void resize(){Pipeline*rp=pipeline();RESIZE_ALLOCATION}
};
void require(bool condition,const char*message){if(!condition)throw std::runtime_error(message);}
uint bits(float v){return std::bit_cast<uint>(v);}
void exact(const GIReservoir&a,const GIReservoir&b){
    require(bits(a.W)==bits(b.W)&&bits(a.C)==bits(b.C)&&bits(a.weight_sum)==bits(b.weight_sum),"estimator scalar bits changed");
    require(bits(a.sample.age)==bits(b.sample.age),"full-width age changed");
    for(uint i=0;i<3;++i){
        require(bits(a.sample.sp.pos[i])==bits(b.sample.sp.pos[i]),"position precision changed");
        require(bits(a.sample.Lo[i])==bits(b.sample.Lo[i]),"HDR radiance precision changed");
    }
}
int main(){
    Harness native;
    native.rtxpt_backend_=true;
    native.prepare();
    require(native.reservoirs_.allocation.elements==0&&storage.empty(),"RTXPT prepare allocated legacy storage");
    native.resize();
    require(native.reservoirs_.allocation.elements==0&&storage.empty(),"RTXPT resize allocated legacy storage");
    Harness h;h.prepare();
    require(storage.size()==count*2&&h.reservoirs_.allocation.name==GIReservoirStorage::Name,"prepare allocation ABI changed");
    require(registered[0].offset==0&&registered[0].count==count&&registered[1].offset==count&&registered[1].count==count,"prepare registered slices changed");
    GIReservoir zero=h.prev_reservoirs().read(0);
    require(length(zero.sample.sp.normal())==0&&zero.W==0&&zero.C==0&&zero.weight_sum==0,"invalid default changed");
    const uint patterns[]={0,0x80000000u,1,0x007fffffu,0x00800000u,0x3f800000u,0x7f7fffffu,0xff7fffffu,
                           0x7f800000u,0xff800000u,0x7fc12345u,0x47ffffffu,0x4f7fffffu};
    for(uint fixture=0;fixture<130;++fixture){
        GIReservoir v;uint expected[11]{};
        for(uint word=0;word<11;++word)expected[word]=patterns[(word+fixture)%13];
        v.W=std::bit_cast<float>(expected[0]);v.C=std::bit_cast<float>(expected[1]);v.weight_sum=std::bit_cast<float>(expected[2]);
        for(uint i=0;i<3;++i){v.sample.sp.pos[i]=std::bit_cast<float>(expected[3+i]);v.sample.Lo[i]=std::bit_cast<float>(expected[6+i]);}
        v.sample.age=std::bit_cast<float>(expected[10]);v.sample.sp.set_normal(make_float3(0,0,1));
        h.passthrough_reservoirs().write(3,v);
        for(uint word=0;word<11;++word)if(word!=9)require(storage[count+3].words[word]==expected[word],"physical word order/bit preservation");
        exact(v,h.passthrough_reservoirs().read(3));exact(v,unpack_gi_reservoir_host(storage[count+3]));
    }
    const float nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    for(float3 normal:{make_float3(0),make_float3(nan,0,1),make_float3(0,inf,1)}){
        auto packed=pack_gi_normal(normal);require(packed==GIReservoirStorage::InvalidNormal,"invalid normal lost sentinel");
        require(length(unpack_gi_normal(packed))==0&&length(unpack_gi_normal_host(packed))==0,"invalid normal decoded valid");
    }
    uint random=20261105;float max_error=0;
    for(uint i=0;i<10006;++i){
        auto next=[&](){random=random*1664525u+1013904223u;return float(int(random>>8)-8388608)/8388608.f;};
        float3 normal=make_float3(next(),next(),next());
        if(i<6){normal=make_float3(0);float*axis=&normal.x;axis[i/2]=(i%2)?-1.f:1.f;}
        normal=normal/length(normal);auto packed=pack_gi_normal(normal);
        require(packed!=GIReservoirStorage::InvalidNormal,"valid normal became sentinel");
        auto actual=unpack_gi_normal(packed),host=unpack_gi_normal_host(packed);
        float error=length(make_float3(normal.x-actual.x,normal.y-actual.y,normal.z-actual.z));max_error=std::max(error,max_error);
        require(error<0.00008f,"normal angular precision");
        require(length(make_float3(host.x-actual.x,host.y-actual.y,host.z-actual.z))<0.0000003f,"host/device decoding disagreement");
        require(pack_gi_normal(actual)==packed,"normal drifts on history roundtrip");
    }
    // Real accessors must keep A/history disjoint from B/scratch across frames.
    for(uint frame=0;frame<4;++frame){
        for(uint p=0;p<count;++p){GIReservoir v;v.W=100*frame+p;v.C=p+1;v.weight_sum=3*p+7;v.sample.age=frame;
            v.sample.sp.set_normal(make_float3(0,0,-1));h.passthrough_reservoirs().write(p,v);}
        auto before=storage;
        for(uint p=0;p<count;++p){auto temporal=h.passthrough_reservoirs().read(p);h.cur_reservoirs().write(p,temporal);
            auto donor=h.passthrough_reservoirs().read((p+1)%count);donor.W+=10000;
            exact(h.prev_reservoirs().read(p),temporal);}
        for(uint p=0;p<count;++p)require(storage[count+p].words==before[count+p].words,"A write overwrote scratch B");
    }
    ocarina::ByteBufferDesc a{1,0,count*44},b{1,count*44,count*44};
    require(gi_reservoir_storage_matches(GIReservoirStorage::Name,a,b,count),"current ABI rejected");
    require(!gi_reservoir_storage_matches("ReSTIRGI::reservoirs_ x 2",a,b,count),"old ABI accepted");
    auto bad=b;bad.offset=0;require(!gi_reservoir_storage_matches(GIReservoirStorage::Name,a,bad,count),"aliased slices accepted");
    bad=b;bad.size=count*52;require(!gi_reservoir_storage_matches(GIReservoirStorage::Name,a,bad,count),"old stride accepted");
    count=7;h.resize();a={2,0,count*44};b={2,count*44,count*44};
    require(storage.size()==count*2&&h.reservoirs_.allocation.name==GIReservoirStorage::Name,"resize allocation ABI changed");
    require(registered[0].offset==0&&registered[0].count==count&&registered[1].offset==count&&registered[1].count==count,"resize registered slices changed");
    require(gi_reservoir_storage_matches(GIReservoirStorage::Name,a,b,count),"resized ABI rejected");
    for(uint p=0;p<count;++p)require(h.prev_reservoirs().read(p).W==0&&length(h.passthrough_reservoirs().read(p).sample.sp.normal())==0,"resize defaults changed");
    std::cout<<"PASS GI44: 130 bit-pattern records, 10006 normals, invalid defaults, HDR/age, four-frame A/B, ABI and resize; max normal error="<<max_error<<'\n';
}
'''.replace('ACTUAL_STORAGE',source).replace('ACCESSORS',accessors).replace('OCT_DECODE',oct_decode)
    program=program.replace('PREPARE_ALLOCATION',allocations[0]).replace('RESIZE_ALLOCATION',allocations[1])
    compiler=shutil.which('g++') or shutil.which('clang++')
    if not compiler:raise SkipTest('GI44 CPU boundary regression requires g++ or clang++')
    temp_root=ROOT/'.workspace/tmp/restir-performance/tests/gi44-cpu';temp_root.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temp_root) as temporary:
        path=Path(temporary)
        assert path.resolve().is_relative_to(temp_root.resolve())
        cpp=path/'harness.cpp';exe=path/'harness.exe';cpp.write_text(program,encoding='utf-8')
        built=subprocess.run([compiler,'-std=c++20','-O0',str(cpp),'-o',str(exe)],capture_output=True,text=True)
        assert built.returncode==0,built.stderr
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        assert result.returncode==0,result.stderr
        assert result.stdout.startswith('PASS GI44:'),result.stdout
        print(result.stdout.strip())
