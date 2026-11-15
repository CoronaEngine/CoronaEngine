"""Compare actual framework sampling and ReSTIR classification wrapper on CPU."""
from pathlib import Path
import shutil
import subprocess
import pytest
from test_shadow_ray_endpoint import extract_function

ROOT=Path(__file__).resolve().parents[3]


def test_actual_shadow_wrapper_preserves_framework_proposal_draws_and_classifies_infinite():
    header=(ROOT/'vision/src/base/integral/restir_shadow.h').read_text(encoding='utf-8-sig')
    framework=(ROOT/'vision/src/base/illumination/lightsampler.cpp').read_text(encoding='utf-8-sig')
    old=extract_function(framework,'LightSample LightSampler::sample_wi(const LightSampleContext &lsc, TSampler &sampler,')
    old=old.replace('LightSampler::sample_wi','sample_wi',1)
    helpers='\n'.join(extract_function(header,s) for s in (
        '[[nodiscard]] inline Bool is_infinite_light(', '[[nodiscard]] inline LightSample sample_light('))
    program=r'''
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>
using Uint=unsigned;using Float=float;using Bool=bool;
struct Float2{float x,y;};struct LightSampleContext{};struct SampledWavelengths{};
struct Sampler{std::vector<int>*events;unsigned draws=0;Sampler*operator->(){return this;}
 float next_1d(){events->push_back(1);++draws;return .125f;}
 Float2 next_2d(){events->push_back(2);draws+=2;return{.25f,.75f};}};
using TSampler=Sampler*;
enum LightType{Infinite=1};
struct Light{bool infinite;bool match(LightType)const{return infinite;}};
struct SampledLight{Uint light_index;float PMF;};
struct LightSample{float radiance,pdf;};
struct LightSampler{
 std::vector<int>*events;Uint selected;
 SampledLight select_light(const LightSampleContext&,float u)const{
  events->push_back(3);if(u!=.125f)throw std::runtime_error("selector changed");return{selected,.375f};}
 LightSample sample_wi(const SampledLight&s,const LightSampleContext&,Float2 u,const SampledWavelengths&)const{
  events->push_back(4);return{float(s.light_index)+u.x+u.y,s.PMF*.625f};}
 std::pair<Uint,Uint>extract_light_id(Uint index)const{return{9,index};}
 void dispatch_light(Uint,Uint instance,const std::function<void(const Light*)>&f)const{
  events->push_back(5);Light l{instance!=0};f(&l);}
 __ORIGINAL__
};
__HELPERS__
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
int main(){try{
 for(Uint selected:{0u,1u,2u}){
  std::vector<int>old_events,new_events;Sampler old_s{&old_events},new_s{&new_events};
  TSampler old_ptr=&old_s,new_ptr=&new_s;LightSampler old_l{&old_events,selected},new_l{&new_events,selected};
  LightSampleContext lsc;SampledWavelengths swl;
  auto old=old_l.sample_wi(lsc,old_ptr,swl);bool infinite=false;
  auto current=sample_light(new_l,lsc,new_ptr,swl,infinite);
  require(old_events==std::vector<int>{1,2,3,4},"framework source draw/call order control changed");
  require(new_events==std::vector<int>{1,2,3,4,5},"wrapper must classify only after identical 1D/2D/select/sample sequence");
  require(old_s.draws==3&&new_s.draws==3,"classification must not consume extra RNG");
  require(current.radiance==old.radiance&&current.pdf==old.pdf,"wrapper changed sampled radiance/PDF");
  require(infinite==(selected!=0),"finite area, continuous environment and delta environment classification");
 }
 std::cout<<"PASS real framework versus wrapper: exact draws/proposal/PDF, finite+two infinite categories\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''.replace('__ORIGINAL__',old).replace('__HELPERS__',helpers)
    compiler=shutil.which('g++')
    if not compiler:pytest.skip('g++ required for production sampling fixture')
    temp=ROOT/'.workspace/tmp/restir-shadow-audit-20261115/production-tests';temp.mkdir(parents=True,exist_ok=True)
    cpp=temp/'sampling.cpp';exe=temp/'sampling.exe';cpp.write_text(program)
    built=subprocess.run([compiler,'-std=c++20','-O0',str(cpp),'-o',str(exe)],capture_output=True,text=True)
    assert built.returncode==0,built.stdout+built.stderr
    result=subprocess.run([str(exe)],capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
