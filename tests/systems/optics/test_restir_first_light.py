"""Execute production first-light support and ownership in a scalar CPU shim.

Before the new helpers exist, adapters execute the existing production gate,
target_at and prefix predicates. Red is an incorrect behavioral result, not a
missing-symbol compilation failure. Renderer integration is checked separately.
"""
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess
import pytest

ROOT = next(p for p in Path(__file__).resolve().parents if (p/'vision/src').is_dir())
FALCOR = ROOT/'vision/src/render_core/integrator/falcor_di'
POLICY = ROOT/'vision/src/base/scattering/gi_ideal_glass.h'
TEMP = ROOT/'.workspace/tmp/rtxpt-first-light-fix-20261115/production-tests'


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
#include <iostream>
#include <stdexcept>
#include <string>
using Float=float; using Bool=bool; using Uint=unsigned; using uint=unsigned;
#define $if if
#define $else else
struct Float3{float x{},y{},z{};};
Float3 operator*(Float3 a,Float3 b){return{a.x*b.x,a.y*b.y,a.z*b.z};}
float dot(Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float luminance(Float3 v){return .2126f*v.x+.7152f*v.y+.0722f*v.z;}
bool finite(float x){return std::isfinite(x);}
struct BxDFFlag{static constexpr uint Specular=32;static bool is_specular(uint f){return f&Specular;}};
enum MaterialEvalMode{F=1,PDF=2,All=3};
struct Spectrum{Float3 value;Float3 vec3()const{return value;}};
struct ScatterEval{Spectrum f;float density;float pdf()const{return density;}};
struct Frame{Float3 normal()const{return{0,0,1};}};
struct MaterialEvaluator{
 uint bits=16;uint flag()const{return bits;}Frame shading_frame()const{return{};}
 ScatterEval evaluate(Float3,Float3,MaterialEvalMode)const{return{{{2,2,2}},.25f};}
};
struct TriangleSample{Float3 direction;Float3 radiance{3,3,3};float pdf=1;float distance=1;};
constexpr float ray_epsilon=.001f;
template<class T>T select(bool b,T a,T c){return b?a:c;}
namespace ocarina{using ::select;}
__DEFINITIONS__
enum MISMode{EBoth=0,ELight=1,EBSDF=2};
struct Corrector{MISMode mis_mode_=EBoth; __CORRECTOR__};
void require(bool condition,const char*message){if(!condition)throw std::runtime_error(message);}
void near(float got,float want,const char*message){require(std::abs(got-want)<1e-6f,message);}
void support(){
 MaterialEvaluator bsdf;
 for(float wo:{-.7f,.7f})for(float wi:{-.4f,.4f}){
  TriangleSample light{{0,0,wi}};
  require(test_support(bsdf,{0,0,wo},light)==(wo*wi>0),"DI support must admit both reflection sides and reject transmission");
 }
 bsdf.bits=32;TriangleSample light{{0,0,1}};
 require(!test_support(bsdf,{0,0,1},light),"true discrete reflection must stay PT-owned");
}
void targets(){
 MaterialEvaluator bsdf;
 for(float wo:{-.7f,.7f})for(float wi:{-.4f,.4f}){
  TriangleSample light{{0,0,wi}};
  near(target_at(light,bsdf,{0,0,wo}),wo*wi>0?6.f:0.f,"reuse target must be reflection-only in either orientation");
 }
 bsdf.bits=32;TriangleSample light{{0,0,1}};
 near(target_at(light,bsdf,{0,0,1}),0,"discrete reflection is not estimated again by DI");
}
void ownership(){
 for(bool t:{false,true})for(bool covered:{false,true})for(uint bounce:{0u,1u,2u,3u}){
  bool want=bounce==0||(bounce==1&&covered&&!t);
  require(test_owned(t,covered,bounce)==want,"first transmission and uncovered reflection must remain PT-owned");
  float expected=bounce==1&&!want?1.f:.375f;
  near(test_mis(.375f,t,covered,bounce),expected,"unique first-light BSDF technique must have MIS one; later MIS unchanged");
 }
}
void prefix(){
 for(bool ideal:{false,true})for(uint depth:{1u,2u,3u,4u})for(uint branch:{5u,6u})for(bool covered:{false,true}){
  bool want=ideal&&(depth>2||(depth==2&&(branch==5||!covered)));
  require(test_prefix(ideal,depth,branch,covered)==want,"Build must own first transmission or uncovered delta reflection");
 }
}
void corrector(){
 Corrector c;
 for(uint b:{0u,1u,2u,3u}){
  near(c.correct_bsdf_weight(1.f,b),1.f,"Both mode must preserve unique-technique MIS one");
  near(c.correct_bsdf_weight(.375f,b),.375f,"Both mode must preserve existing reflection/later MIS");
 }
}
int main(int argc,char**argv){try{std::string mode=argv[1];
 if(mode=="support")support();else if(mode=="targets")targets();else if(mode=="ownership")ownership();else if(mode=="corrector")corrector();else prefix();
 std::cout<<"PASS production "<<mode<<'\n';return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''


@lru_cache(None)
def executable():
    brdf=(FALCOR/'brdf.h').read_text(encoding='utf-8-sig')
    candidates=(FALCOR/'candidates.h').read_text(encoding='utf-8-sig')
    reuse=(FALCOR/'reuse.h').read_text(encoding='utf-8-sig')
    policy=POLICY.read_text(encoding='utf-8-sig')
    parts=[definition(brdf,'[[nodiscard]] inline Float target_weight(')]
    if 'inline Bool reflection_supported(' in brdf:
        parts.append(definition(brdf,'[[nodiscard]] inline Bool reflection_supported('))
        parts.append('bool test_support(const MaterialEvaluator&bsdf,Float3 wo,TriangleSample light){return reflection_supported(bsdf,wo,light.direction);}')
        parts.append(definition(brdf,'[[nodiscard]] inline Float reflection_target_weight('))
    else:
        # Execute the exact existing candidate's support expression.
        marker='const Bool bsdf_supported = '
        start=candidates.index(marker)+len(marker)
        expression=candidates[start:candidates.index(';',start)]
        parts.append('bool test_support(const MaterialEvaluator&bsdf,Float3 wo,TriangleSample light){bool use_bsdf=!BxDFFlag::is_specular(bsdf.flag());float cosine=light.direction.z;return '+expression+';}')
    parts.append(definition(reuse,'[[nodiscard]] inline Float target_at('))
    for name, signature, fallback in (
        ('owned','[[nodiscard]] inline Bool gi_camera_emission_owned_by_di(',
         'bool gi_camera_emission_owned_by_di(bool,bool,uint bounce){return bounce<=1;}'),
        ('mis','[[nodiscard]] inline Float gi_camera_emission_bsdf_weight(',
         'float gi_camera_emission_bsdf_weight(float original,bool,bool,uint){return original;}')):
        parts.append(definition(policy,signature) if signature in policy else fallback)
    parts.extend(['bool test_owned(bool t,bool c,uint b){return gi_camera_emission_owned_by_di(t,c,b);}',
                  'float test_mis(float original,bool t,bool c,uint b){return gi_camera_emission_bsdf_weight(original,t,c,b);}'])
    prefix=definition(policy,'[[nodiscard]] inline Bool gi_prefix_stable_emission_allowed(')
    parts.append(prefix)
    argument='i,d,b,c' if 'first_reflection_covered_by_di' in prefix else 'i,d'
    parts.append('bool test_prefix(bool i,uint d,uint b,bool c){return gi_prefix_stable_emission_allowed('+argument+');}')
    corrector=(ROOT/'vision/src/base/integral/integrator.h').read_text(encoding='utf-8-sig')
    program=SHIM.replace('__DEFINITIONS__','\n'.join(parts)).replace('__CORRECTOR__',
        definition(corrector,'[[nodiscard]] Float correct_bsdf_weight('))
    compiler=shutil.which('g++')
    if not compiler:pytest.skip('g++ required for production CPU fixture')
    TEMP.mkdir(parents=True,exist_ok=True)
    source=TEMP/'first_light.cpp';binary=TEMP/'first_light.exe'
    source.write_text(program)
    result=subprocess.run([compiler,'-std=c++20','-O2',str(source),'-o',str(binary)],text=True,capture_output=True)
    assert result.returncode==0,result.stdout+result.stderr
    return binary


@pytest.mark.parametrize('mode',['support','targets','ownership','prefix','corrector'])
def test_production_first_light(mode):
    result=subprocess.run([str(executable()),mode],text=True,capture_output=True)
    assert result.returncode==0,result.stdout+result.stderr
