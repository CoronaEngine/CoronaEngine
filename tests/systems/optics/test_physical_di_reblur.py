"""Execute the physical-DI-only REBLUR policy with the real JSON implementation."""
from pathlib import Path
import shutil
import subprocess

import pytest
from test_shadow_ray_endpoint import extract_function

ROOT = Path(__file__).resolve().parents[3]
HEADER = ROOT / 'vision/src/render_core/integrator/ReSTIR/physical_di_reblur.h'


def test_physical_di_reblur_preserves_parent_and_other_settings():
    assert HEADER.exists(), 'Physical DI still inherits the GI diffuse prepass/radius unchanged'
    compiler = shutil.which('g++')
    if not compiler:
        pytest.skip('g++ required for the production JSON policy fixture')
    json_include = ROOT / 'cmake-build-relwithdebinfo/_deps/nlohmann_json-src/include'
    assert (json_include / 'nlohmann/json.hpp').exists()
    directory = ROOT / '.workspace/tmp/classroom-restir-fix-20261115/production-tests'
    directory.mkdir(parents=True, exist_ok=True)
    cpp = directory / 'physical_di_reblur.cpp'
    exe = directory / 'physical_di_reblur.exe'
    cpp.write_text(r'''
#include "ReSTIR/physical_di_reblur.h"
#include <iostream>
#include <stdexcept>
void require(bool v, const char *message) { if (!v) throw std::runtime_error(message); }
int main() { try {
    const nlohmann::json original{
        {"diffuse_prepass",15.f},{"specular_prepass",40.f},
        {"min_radius",1.f},{"max_radius",30.f},
        {"max_history",63.f},{"fast_history",6.f},{"stabilization",.984375f},
        {"anti_firefly",true},{"reconstruction",2u}};
    const auto before = original.dump();
    for (float max_radius : {0.f, 3.f, 10.f, 30.f, 100.f}) {
        for (float min_radius : {0.f, 1.f, 9.f, 30.f}) {
            auto parent = original;
            parent["max_radius"] = max_radius;
            parent["min_radius"] = std::min(min_radius, max_radius);
            const auto parent_before = parent.dump();
            auto child = vision::physical_di_reblur_settings(parent);
            require(parent.dump() == parent_before, "Policy changed the parent GI/PT settings");
            require(child["diffuse_prepass"] == 0.f, "Physical DI must bypass diffuse PrePass");
            require(child["max_radius"] == std::min(max_radius,10.f), "Must respect a smaller user radius");
            require(child["min_radius"] == std::min(parent["min_radius"].get<float>(), child["max_radius"].get<float>()),
                    "Minimum radius exceeded physical DI maximum");
            auto restored = child;
            for (const char *key : {"diffuse_prepass", "min_radius", "max_radius"}) restored[key] = parent[key];
            require(restored == parent, "Policy changed an unrelated REBLUR field");
            require(vision::physical_di_reblur_settings(child).dump() == child.dump(), "Policy must be idempotent");
            require(vision::physical_di_reblur_settings(nlohmann::json::parse(parent.dump())).dump() == child.dump(),
                    "Repeated target serialization would cause a per-frame sync/history reset");
        }
    }
    require(original.dump() == before, "Caller input mutated");
    std::cout << "PASS real policy: D0, R<=10, smaller user R/minR, all other fields, immutable parent, stable target\n";
    return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; } }
''', encoding='utf-8')
    build = subprocess.run([compiler, '-std=c++20', '-O0', '-I', str(json_include),
                            '-I', str(ROOT / 'vision/src/render_core/integrator'),
                            str(cpp), '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_actual_rt_sync_prepare_target_and_readonly_context_feedback():
    source = (ROOT / 'vision/src/render_core/integrator/rt.cpp').read_text(encoding='utf-8-sig')
    start = source.index('const auto physical_di_settings = physical_di_reblur_settings(')
    route = source[start:source.index('begin_denoise_timing(stream);', start)]
    getter = extract_function(source, '[[nodiscard]] DataWrap reblur_context_settings() const override')
    getter = getter.replace(' const override', ' const', 1)
    program = r'''
#include "ReSTIR/physical_di_reblur.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
using DataWrap=nlohmann::json;
using vision::physical_di_reblur_settings;
struct Filter {
    std::string value;
    int resets=0;
    std::string reblur_settings_json()const{return value;}
    void set(const std::string &target){if(value!=target){value=target;++resets;}}
};
enum Command{Sync,Commit}; Command synchronize(){return Sync;} Command commit(){return Commit;}
struct Stream{int syncs=0;Stream &operator<<(Command c){syncs+=c==Sync;return *this;}};
bool any(bool v){return v;}
struct Pipeline{int size=1920;int resolution()const{return size;}};
struct Fixture {
    Filter parent; Filter *denoiser_=&parent;
    std::unique_ptr<Filter> native_di_denoiser_;
    int native_denoiser_size_=0;
    Stream stream; Pipeline p; bool active=true;
    Pipeline *pipeline(){return &p;}
    bool native_reblur_requested()const{return active;}
    void prepare_native_di_denoiser(const std::string &settings){
        if(!native_di_denoiser_) native_di_denoiser_=std::make_unique<Filter>();
        native_denoiser_size_=p.size; native_di_denoiser_->set(settings);
    }
    void frame(){__ROUTE__}
    __GETTER__
};
void require(bool v,const char*m){if(!v)throw std::runtime_error(m);}
int main(){try{
    Fixture f;
    DataWrap parent{{"diffuse_prepass",15.f},{"specular_prepass",40.f},{"min_radius",1.f},
                    {"max_radius",30.f},{"max_history",63.f},{"anti_firefly",true}};
    f.parent.value=parent.dump();
    auto before=f.reblur_context_settings();
    require(!before["physical_di"]["ready"].get<bool>(),"Uncreated child must not claim ready");
    require(before["physical_di"]["settings"].empty(),"Uncreated child must not claim target is actual");
    f.frame();
    require(f.stream.syncs==1&&f.native_di_denoiser_->resets==1,"First preparation must synchronize/reset once");
    for(int i=0;i<10;++i)f.frame();
    require(f.stream.syncs==1&&f.native_di_denoiser_->resets==1,"Unchanged target caused per-frame barrier/reset");
    auto feedback=f.reblur_context_settings();
    require(feedback["policy"]=="physical-di-detail-preserving-v1","Missing explicit fixed policy");
    require(feedback["physical_di"]["ready"].get<bool>(),"Prepared child not ready");
    require(feedback["physical_di"]["settings"]==nlohmann::json::parse(f.native_di_denoiser_->value),"API child must be actual");
    require(feedback["gi"]["settings"]==parent&&feedback["gi"]["count"]==3,"GI parent settings changed");
    parent["diffuse_prepass"]=25.f;f.parent.value=parent.dump();f.frame();
    require(f.stream.syncs==1&&f.native_di_denoiser_->resets==1,"Parent-only diffuse radius reset physical DI");
    parent["max_radius"]=3.f;parent["min_radius"]=2.f;f.parent.value=parent.dump();
    require(!f.reblur_context_settings()["physical_di"]["ready"].get<bool>(),"Stale actual settings must not claim ready");
    f.frame();
    require(f.stream.syncs==2&&f.native_di_denoiser_->resets==2,"Changed derived target must synchronize once");
    require(nlohmann::json::parse(f.native_di_denoiser_->value)["max_radius"]==3.f,"Small user radius ignored");
    f.p.size=1080;f.frame();
    require(f.stream.syncs==3&&f.native_di_denoiser_->resets==2,"Resize must sync without settings reset");
    f.active=false;
    require(f.reblur_context_settings()==DataWrap::object(),"Inactive/private-less route leaked context settings");
    require(f.parent.value==parent.dump(),"Private preparation mutated parent settings");
    std::cout<<"PASS actual RT route/getter: one shared target, no repeated sync/reset, resize/settings invalidation, actual context readiness\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
'''.replace('__ROUTE__', route).replace('__GETTER__', getter)
    compiler = shutil.which('g++')
    if not compiler:
        pytest.skip('g++ required for actual host-route regression')
    directory = ROOT / '.workspace/tmp/classroom-restir-fix-20261115/production-tests'
    directory.mkdir(parents=True, exist_ok=True)
    cpp = directory / 'physical_di_route.cpp'
    exe = directory / 'physical_di_route.exe'
    cpp.write_text(program, encoding='utf-8')
    build = subprocess.run([compiler, '-std=c++20', '-O0',
                            '-I', str(ROOT / 'cmake-build-relwithdebinfo/_deps/nlohmann_json-src/include'),
                            '-I', str(ROOT / 'vision/src/render_core/integrator'),
                            str(cpp), '-o', str(exe)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
