"""Production Falcor DI environment helpers, compiled on CPU without the engine.

Direction/PDF and light-mass helpers run unchanged. Independent quadrature checks
the 10-light/1-BSDF estimator's measure; receiver-motion and UV16 fixtures specify
the directional environment history contract without simulating GPU resources.
"""
from functools import lru_cache
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'vision/src').is_dir())
COMMON = ROOT / 'vision/src/render_core/integrator/falcor_di/common.h'
CANDIDATES = COMMON.with_name('candidates.h')
TEMP = ROOT / '.workspace/tmp/rtxpt-corona-dsl-20261108/falcor-environment-tests'


def _definition(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def test_production_environment_helpers_are_available():
    source = COMMON.read_text(encoding='utf-8-sig')
    assert 'Float3 environment_direction(' in source, 'Production Falcor DI environment direction helper is absent'
    assert 'Float2 environment_uv(' in source, 'Production Falcor DI environment UV helper is absent'
    assert 'light_selection_probabilities(' in source, 'Production Falcor DI environment light-mass helper is absent'


PROGRAM = r'''
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
using uint=std::uint32_t; using Uint=uint; using Float=float; using Bool=bool;
using std::vector; using std::max; using std::min; using std::clamp; using std::sqrt;
using std::sin; using std::cos; using std::atan2; using std::acos;
constexpr float Pi=3.14159265358979323846f, Inv2Pi=1.f/(2.f*Pi), Inv4Pi=1.f/(4.f*Pi);
struct Float2 { float x{},y{}; float operator[](unsigned i)const{return i?y:x;} };
struct Float3 { float x{},y{},z{}; };
Float2 make_float2(float x,float y){return{x,y};}
Float3 make_float3(float x,float y,float z){return{x,y,z};}
Float3 operator+(Float3 a,Float3 b){return{a.x+b.x,a.y+b.y,a.z+b.z};}
Float3 operator-(Float3 a,Float3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 operator*(Float3 a,float s){return{a.x*s,a.y*s,a.z*s};}
float dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float length(Float3 a){return std::sqrt(dot(a,a));}
Float3 normalize(Float3 a){float s=length(a);return{a.x/s,a.y/s,a.z/s};}
template<class T,class U>T select(bool b,T yes,U no){return b?yes:T(no);}
template<class T,class U>T cast(U value){return static_cast<T>(value);}
float fract(float v){return v-std::floor(v);}
namespace ocarina {using ::Inv4Pi;using ::Pi;using ::Inv2Pi;}
enum class EPort {D,H};
template<EPort>using oc_float=float;
template<EPort>using oc_float2=Float2;
template<EPort>using oc_float3=Float3;
__SPHERE_IMPLEMENTATION__
Float3 square_to_sphere(const Float2 &u){return square_to_sphere_impl<EPort::D>(u);}
__PDF__
namespace vision::falcor_di {
__CONSTANTS__
__HELPERS__
}
__ALIAS_CLASS__
using namespace vision::falcor_di;
void require(bool ok,const char*message){if(!ok)throw std::runtime_error(message);}
void near(double actual,double expected,double tolerance,const char*message){
    if(!std::isfinite(actual)||std::abs(actual-expected)>tolerance){
        std::cerr<<message<<": actual="<<actual<<" expected="<<expected<<'\n';throw std::runtime_error(message);
    }
}
void directions_test(){
    const auto north=environment_direction({0.f,0.f}),south=environment_direction({1.f,.73f});
    near(north.z,1,1e-7,"north pole");near(south.z,-1,1e-7,"south pole");
    require(std::isfinite(environment_uv(north).y)&&std::isfinite(environment_uv(south).y),"pole inverse must remain finite");
    for(float z:{0.f,.00001f,.1f,.5f,.9f,.99999f,1.f})for(float azimuth:{0.f,.00001f,.2f,.75f,.99999f,1.f}){
        const auto direction=environment_direction({z,azimuth});
        near(length(direction),1,3e-7,"uniform-sphere direction must be unit length");
        const auto inverse=environment_uv(direction);
        require(inverse.x>=0&&inverse.x<=1&&inverse.y>=0&&inverse.y<=1,"environment UV inverse left its unit square");
        near(length(environment_direction(inverse)-direction),0,2e-6,"UV/direction inverse changed a direction at pole or seam");
    }
    near(length(environment_direction({.37f,0.f})-environment_direction({.37f,1.f})),0,4e-7,"azimuth seam must be periodic");
    for(unsigned i=0;i<2000;++i){
        Float2 uv{float((i*997u)%65536u)/65535.f,float((i*811u+7u)%65536u)/65535.f};
        const auto direction=environment_direction(uv);
        const auto stored=environment_uv(direction);
        Float2 packed{std::round(stored.x*65535.f)/65535.f,std::round(stored.y*65535.f)/65535.f};
        near(length(environment_direction(packed)-direction),0,.0056,"UV16 history exceeded the polar quantization bound");
    }
    const Float2 stored_uv{.31f,.73f};
    const auto stored_direction=environment_direction(stored_uv);
    for(Float3 receiver:std::array<Float3,3>{{{0,0,0},{100,-30,7},{-7000,1200,-43}}}){
        const auto recovered=environment_direction(environment_uv(stored_direction));
        const auto endpoint=receiver+recovered*environment_ray_distance;
        near(length(normalize(endpoint-receiver)-stored_direction),0,2e-6,"environment history changed direction with receiver translation");
    }
    require(environment_ray_distance>=1e15f,"environment shadow ray must reach the distant-light endpoint");
    std::cout<<"PASS sphere UV inverse, poles/seam, 2000 UV16 histories and receiver translation\n";
}
void sphere_test(){
    double x=0,y=0,z=0,xx=0,yy=0,zz=0;
    constexpr unsigned nz=512,np=128;
    for(unsigned i=0;i<nz;++i)for(unsigned j=0;j<np;++j){
        const auto d=environment_direction({(i+.5f)/nz,(j+.5f)/np});
        x+=d.x;y+=d.y;z+=d.z;xx+=d.x*d.x;yy+=d.y*d.y;zz+=d.z*d.z;
    }
    const double n=nz*np;
    near(x/n,0,1e-6,"sphere mean x");near(y/n,0,1e-6,"sphere mean y");near(z/n,0,1e-6,"sphere mean z");
    near(xx/n,1./3,3e-6,"sphere second moment x");near(yy/n,1./3,3e-6,"sphere second moment y");near(zz/n,1./3,3e-6,"sphere second moment z");
    near(double(uniform_sphere_PDF())*4*3.14159265358979323846,1,1e-7,"uniform sphere PDF must integrate to one, not hemisphere mass or doubled4pi");
    std::cout<<"PASS 65536 uniform-sphere moment samples and solid-angle PDF normalization\n";
}
void probabilities_test(){
    for(unsigned count:{0u,1u,8u,17u,67u,20594u})for(bool env:{false,true}){
        vector<float> weights(count);double total=0;
        for(unsigned i=0;i<count;++i){weights[i]=float((i*37u)%101u+1u);total+=weights[i];}
        if(count==0&&!env){bool rejected=false;try{(void)light_selection_probabilities(weights,env);}catch(const std::runtime_error&){rejected=true;}
            require(rejected,"empty emitter set without environment must be rejected");continue;}
        const auto probabilities=light_selection_probabilities(weights,env);
        require(probabilities.size()==count+unsigned(env),"environment mass must append one light entry");
        double cdf=0,areas=0;
        for(unsigned i=0;i<probabilities.size();++i){
            require(std::isfinite(probabilities[i])&&probabilities[i]>=0,"light mass must be finite and nonnegative");
            const double next=cdf+probabilities[i];require(next>=cdf,"CDF must be monotone");cdf=next;
            if(i<count){areas+=probabilities[i];near(probabilities[i],weights[i]/total*(env?.5:1.),2e-7,"existing relative area-light mass changed");}
        }
        near(cdf,1,2e-7,"arbitrary-count light masses do not normalize");
        if(count)near(areas,env?.5:1.,2e-7,"positive area-light family must retain its assigned total mass");
        if(env)near(probabilities.back(),count?.5:1.,0,"environment-only/area-mixture mass is incorrect");
    }
    const auto sparse=light_selection_probabilities({0,1,0,3},true);
    require(sparse[0]==0&&sparse[2]==0,"zero-area lights gained selection support");
    near(sparse[1],.125,0,"sparse light mass1");near(sparse[3],.375,0,"sparse light mass3");
    const auto dark=light_selection_probabilities({0,0,0},true);
    require(dark.size()==4&&dark[0]==0&&dark[1]==0&&dark[2]==0&&dark[3]==1,"zero area weights must leave full environment mass");
    for(float bad:{-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})for(bool env:{false,true}){
        bool rejected=false;try{(void)light_selection_probabilities({1,bad,2},env);}catch(const std::runtime_error&){rejected=true;}
        require(rejected,"invalid area-light weight was silently admitted");
    }
    bool rejected=false;try{(void)light_selection_probabilities({0,0},false);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"all-zero area weights without environment must be rejected");
    std::cout<<"PASS normalized area/environment masses for0/1/8/17/67/20594lights, sparse support and invalid input rejection\n";
}
void mixture_test(){
    const float conditional=uniform_sphere_PDF();
    near(initial_mixture_pdf(1.f,0.f,conditional),10./11.,5e-8,"10 light proposals out of11 must retain their balance weight");
    near(initial_mixture_pdf(.5f,0.f,conditional),5./11.,5e-8,"50-percent environment selection mass was doubled");
    near(initial_mixture_pdf(0.f,.25f,conditional),(.25/conditional)/11.,5e-8,"BSDF density must convert to conditional-light measure once");
    near(initial_mixture_pdf(.5f,-1.f,conditional),5./11.,5e-8,"negative BSDF density must not subtract proposal mass");
    constexpr double pi=3.14159265358979323846;
    for(float select_env:{.5f,1.f})for(double albedo:{.1,.7,1.})for(double radiance:{.25,2.}){
        double from_light=0,from_bsdf=0;constexpr unsigned n=16384;
        for(unsigned i=0;i<n;++i){
            const double z=1.-2.*(i+.5)/n;
            const double bsdf=std::max(0.,z)/pi;
            const double integrand=albedo*radiance*bsdf;
            const double mixture=initial_mixture_pdf(select_env,float(bsdf),conditional)*conditional;
            const double d_omega=4*pi/n;
            from_light+=(10./11.)*select_env*conditional*integrand/mixture*d_omega;
            from_bsdf+=(1./11.)*bsdf*integrand/mixture*d_omega;
        }
        near(from_light+from_bsdf,albedo*radiance,2e-7,"two-technique Lambert environment estimator lost/gained4pi or selection mass");
    }
    std::cout<<"PASS actual10/11 mixture measure and12 deterministic Lambert environment expectations\n";
}
void alias_test(){
    double worst=0;
    for(unsigned n:{1u,8u,67u,20595u})for(bool env:{false,true}){
        const unsigned areas=n-unsigned(env);
        vector<float> weights(areas);
        for(unsigned i=0;i<areas;++i)weights[i]=(i%5u==0u)?0.f:float((i*31u)%73u+1u);
        if(areas==1u)weights[0]=3.f;
        const auto expected=light_selection_probabilities(weights,env);
        vision::AliasTable table;
        table.build(expected);
        const auto&entries=table.host_table();
        require(entries.size()==n,"alias table must retain every area/environment entry");
        vector<double> recovered(n,0.);
        for(unsigned i=0;i<n;++i){
            const auto&e=entries[i];
            require(std::isfinite(e.prob)&&e.prob>=0.f&&e.prob<=1.f&&e.alias<n,"alias cell has invalid mass or destination");
            recovered[i]+=double(e.prob)/n;
            recovered[e.alias]+=(1.-e.prob)/n;
        }
        const double normalization=std::accumulate(expected.begin(),expected.end(),0.);
        near(std::accumulate(recovered.begin(),recovered.end(),0.),1.,2e-12,"reconstructed alias PMF lost total mass");
        double error=0;
        for(unsigned i=0;i<n;++i){
            const double desired=expected[i]/normalization;
            error=std::max(error,std::abs(recovered[i]-desired));
            if(expected[i]==0.f)near(recovered[i],0.,2e-12,"alias table selected a zero-power emitter");
        }
        worst=std::max(worst,error);
        near(error,0.,2e-6,"alias PMF disagrees with normalized area/environment selection masses");
        if(env)near(recovered.back(),areas?.5:1.,2e-6,"alias table changed the environment family probability");
    }
    std::cout<<"PASS production AliasTable PMF reconstruction for1/8/67/20595cells with zero weights; max error="<<worst<<'\n';
}
int main(int argc,char**argv){try{require(argc==2,"choose a fixture");std::string test=argv[1];
    if(test=="directions")directions_test();else if(test=="sphere")sphere_test();
    else if(test=="probabilities")probabilities_test();else if(test=="mixture")mixture_test();
    else if(test=="alias")alias_test();
    else throw std::runtime_error("unknown fixture");return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''


@lru_cache(maxsize=1)
def _executable():
    test_production_environment_helpers_are_available()
    common = COMMON.read_text(encoding='utf-8-sig')
    candidates = CANDIDATES.read_text(encoding='utf-8-sig')
    warp = (ROOT / 'vision/src/math/warp.h').read_text(encoding='utf-8-sig')
    helpers = '\n'.join(_definition(common, signature) for signature in (
        'Float3 environment_direction(', 'Float2 environment_uv(', 'vector<float> light_selection_probabilities('))
    helpers += '\n' + _definition(candidates, 'Float initial_mixture_pdf(')
    constants = '\n'.join(re.search(pattern, source).group(0) for source, pattern in (
        (common, r'inline constexpr float environment_ray_distance\s*=\s*[^;]+;'),
        (candidates, r'inline constexpr uint initial_light_count\s*=\s*[^;]+;'),
        (candidates, r'inline constexpr uint initial_candidate_count\s*=\s*[^;]+;')))
    sphere = 'template<EPort p = EPort::D>\n' + _definition(warp, '[[nodiscard]] oc_float3<p> square_to_sphere_impl(')
    program = PROGRAM.replace('__SPHERE_IMPLEMENTATION__', sphere)
    program = program.replace('__PDF__', _definition(warp, '[[nodiscard]] constexpr float uniform_sphere_PDF('))
    program = program.replace('__CONSTANTS__', constants).replace('__HELPERS__', helpers)
    alias = (ROOT / 'vision/src/render_core/warper/alias.h').read_text(encoding='utf-8-sig')
    alias_class = r'''
namespace vision {
__ENTRY__;
template<class T>struct HostData {
    vector<T> values;
    void set_host(vector<T> value){values=std::move(value);}
    const vector<T>&host_buffer()const{return values;}
};
size_t next_pow2(size_t n){return std::bit_ceil(n);}
struct AliasTable {
    HostData<AliasEntry> table_;
    HostData<float> func_;
    double integral_{};
    void build(vector<float> weights) noexcept;
    __GETTER__
};
__BUILD__
}
'''
    alias_class = alias_class.replace('__ENTRY__', _definition(alias, 'struct AliasEntry {'))
    alias_class = alias_class.replace('__GETTER__', _definition(alias, '[[nodiscard]] const vector<AliasEntry> &host_table()'))
    alias_class = alias_class.replace('__BUILD__', _definition(alias, 'void AliasTable::build(vector<float> weights)'))
    program = program.replace('__ALIAS_CLASS__', alias_class)
    compiler = shutil.which('g++')
    if not compiler:
        raise SkipTest('g++ is required for the standalone Falcor environment fixture')
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='run-', dir=TEMP))
    source, binary = directory / 'falcor_environment.cpp', directory / 'falcor_environment.exe'
    source.write_text(program, encoding='utf-8')
    result = subprocess.run([compiler, '-std=c++20', '-O2', str(source), '-o', str(binary)],
                            text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    return binary


def _run(fixture):
    result = subprocess.run([str(_executable()), fixture], text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout, end='')


def test_environment_direction_history_roundtrip_and_uv16_bounds():
    _run('directions')


def test_uniform_sphere_moments_and_pdf_normalization():
    _run('sphere')


def test_area_environment_probabilities_and_invalid_light_sets():
    _run('probabilities')


def test_initial_mixture_pdf_preserves_lambert_environment_expectation():
    _run('mixture')


def test_actual_alias_table_recovers_area_environment_pmf_at_full_light_count():
    _run('alias')
