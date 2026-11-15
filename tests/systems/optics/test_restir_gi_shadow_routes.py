"""Execute shared Li's actual route branches in a scalar CPU shim.

Full renderer DSL cannot execute on CPU; only the scalar host-choice blocks are
extracted. Geometry/light spies check opt-out, medium state and winner identity.
The independent float32 triangle witness and production helper tests cover rays.
"""
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT=next(p for p in Path(__file__).resolve().parents if (p/'vision/src').is_dir())
SOURCE=ROOT/'vision/src/base/integral/integrator.cpp'
TEMP=ROOT/'.workspace/tmp/restir-shadow-audit-20261115/GI-route-tests'


def braces(source,begin):
    depth,end=1,begin+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return end


def route(source,legacy,marker):
    if marker not in source:return legacy
    point=source.index(marker)
    start=source.rfind('if (hc.restir_shadow_connections)',0,point)
    assert start>=0
    end=braces(source,source.index('{',start))
    tail=source[end:]
    if tail.lstrip().startswith('else'):
        end=braces(source,source.index('{',end))
    return source[start:end]


CPP=r'''
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
using Bool=bool;
struct Float3{float x=0,y=0,z=0;};
Float3 operator-(Float3 a,Float3 b){return{a.x-b.x,a.y-b.y,a.z-b.z};}
Float3 normalize(Float3 a){float d=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);return{a.x/d,a.y/d,a.z/d};}
struct Ray{int kind=0;bool infinite=false;};
struct RayState{Ray ray{};int medium=-1;};
struct Interaction{Float3 pos{};RayState spawn_ray_state(Float3 d)const{return{{20,false},d.z>0?9:7};}};
struct LightSample{Float3 p_light{0,0,1000};int identity=0;};
struct HitContext{bool restir_shadow_connections=false;};
struct Wavelengths{};struct Sampler{};
int old_calls=0,trace_calls=0,typed_calls=0,legacy_samples=0;bool called_infinite=false;
struct LightSampler{LightSample sample_wi(Interaction,Sampler&,Wavelengths)const{legacy_samples++;return{{0,0,1000},10};}};
struct Geometry{
 bool occluded(Interaction it,Float3 target,RayState*out)const{old_calls++;*out=it.spawn_ray_state(target-it.pos);out->ray={10,false};return false;}
 bool trace_occlusion(Ray r)const{trace_calls++;called_infinite=r.infinite;return true;}
};
namespace restir_shadow{
 Ray visibility_ray(Interaction,Float3,Bool inf){return{30,inf};}
 LightSample sample_light(const LightSampler&,const Interaction&,Sampler&,Wavelengths,Bool&inf){typed_calls++;inf=true;return{{0,0,1000},30};}
}
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
void shadow(bool enabled,bool inf,bool secondary,float dz){
 HitContext hc{enabled};Interaction it;Geometry geometry;Bool occluded=false,blocked=false;
 LightSample light_sample{{0,0,dz}},selected{{0,0,dz}};
 std::optional<Bool> light_sample_infinite=inf,selected_infinite=inf;RayState shadow_ray,selected_shadow;
 if(secondary){__SECONDARY__}else{__PRIMARY__}
 RayState state=secondary?selected_shadow:shadow_ray;
 require(state.medium==(dz>0?9:7),"GI ray must retain inside/outside medium selection");
 if(enabled){require(old_calls==0&&trace_calls==1,"GI shadow must use private trace path");require(state.ray.kind==30,"GI must store private ray into the same Tr RayState");require(called_infinite==inf,"shadow must preserve selected light infinity identity");}
 else{require(old_calls==1&&trace_calls==0,"ordinary PT opt-out must keep exact legacy call");require(state.ray.kind==10,"ordinary PT ray cannot be overwritten");}
}
void sample(bool enabled){
 HitContext hc{enabled};LightSampler storage;auto*light_sampler=&storage;Interaction it;Sampler sampler;Wavelengths swl;
 LightSample light_sample;std::optional<Bool> light_sample_infinite=false;
 __SAMPLE__
 if(enabled){require(typed_calls==1&&legacy_samples==0,"GI NEE needs typed light sampler");require(*light_sample_infinite&&light_sample.identity==30,"GI sample must keep infinity identity");}
 else require(typed_calls==0&&legacy_samples==1&&light_sample.identity==10,"ordinary PT sampler/RNG route must stay unchanged");
}
void winner(){
 HitContext hc{true};LightSample candidate{{0,0,1000},30},selected;std::optional<Bool> candidate_infinite=true,selected_infinite=false;float target=2,selected_weight=0;
 __WINNER__
 require(selected.identity==30&&selected_weight==2,"original selected light/weight changed");
 require(*selected_infinite,"RIS-selected candidate must carry its infinity identity");
}
int main(int argc,char**argv){try{std::string mode=argv[1];
 if(mode=="ordinary")shadow(false,true,false,1000);
 else if(mode=="primary")shadow(true,true,false,-1000);
 else if(mode=="secondary")shadow(true,false,true,1000);
 else if(mode=="sample")sample(true);
 else if(mode=="sample-optout")sample(false);
 else winner();std::cout<<"PASS "<<mode<<'\n';return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''


@lru_cache(None)
def executable():
    text=SOURCE.read_text(encoding='utf-8-sig')
    first=route(text,'occluded = geometry.occluded(it, light_sample.p_light, &shadow_ray);',
                'shadow_ray.ray = restir_shadow::visibility_ray(')
    second=route(text,'Bool blocked = geometry.occluded(it, selected.p_light, &selected_shadow);',
                 'selected_shadow.ray = restir_shadow::visibility_ray(')
    sample=route(text,'light_sample = light_sampler->sample_wi(it, sampler, swl);',
                 'light_sample = restir_shadow::sample_light(')
    start=text.index('selected = candidate;');end=text.index('}',start)
    winner=text[start:end]
    program=CPP.replace('__PRIMARY__',first).replace('__SECONDARY__',second).replace('__SAMPLE__',sample).replace('__WINNER__',winner)
    compiler=shutil.which('g++')
    if not compiler:pytest.skip('g++ required for CPU host-route fixture')
    TEMP.mkdir(parents=True,exist_ok=True)
    path=TEMP/'routes.cpp';binary=TEMP/'routes.exe';path.write_text(program,encoding='utf8')
    result=subprocess.run([compiler,'-std=c++20','-O2',str(path),'-o',str(binary)],capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
    return binary


@pytest.mark.parametrize('mode',['ordinary','primary','secondary','sample','sample-optout','winner'])
def test_actual_shared_Li_GI_shadow_route(mode):
    result=subprocess.run([str(executable()),mode],capture_output=True,text=True)
    assert result.returncode==0,result.stdout+result.stderr
