"""Actual public ReSTIR depth API and GUI controls; PT zero-depth preserved."""
from pathlib import Path
import json
import re
from unittest import SkipTest
from _restir_cpu import ROOT, body, compile_run

API_CPP = r'''

#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
using DataWrap=nlohmann::json;using uint=unsigned;
#define OC_ERROR_IF(condition,message) do{if(condition)throw std::runtime_error(message);}while(false)
template<class T>struct Param{T value;T hv()const{return value;}Param&operator=(T v){value=v;return*this;}};
struct Stream{template<class T>Stream&operator<<(T){return*this;}};
int synchronize(){return 0;}int commit(){return 0;}
struct Pipeline{Stream&stream(){static Stream s;return s;}};
Pipeline*pipeline(){static Pipeline p;return&p;}
struct Resource{int device(){return 0;}};
struct Geometry{int bindless_array(){return 0;}Resource*gpu_resource(){static Resource r;return&r;}};
struct Scene{Geometry&geometry(){static Geometry g;return g;}};
Scene scene(){return{};}
namespace Global{struct SceneGpuContextScope{SceneGpuContextScope(int,int){}};}
struct Node{DataWrap data={{"max_recursion",5u},{"M_light",10u},{"open",true}};
    DataWrap settings()const{return data;}uint max_recursion()const{return data["max_recursion"].get<uint>();}
    bool set_settings(const DataWrap& d){data=d;return false;}void compile(){}void set_max_recursion(uint){};};
class Harness {
public:
    Param<uint> max_depth_{16},min_depth_{5};Param<float>rr_threshold_{1};uint apply_count=0;
    bool falcor_di_needs_compile_=false,reference_gi_needs_compile_=false;
    std::shared_ptr<Node>direct_=std::make_shared<Node>(),indirect_=std::make_shared<Node>(),stable_planes_=std::make_shared<Node>();
    DataWrap scene_restir_settings_;
    explicit Harness(uint depth){max_depth_=depth;__CONSTRUCTOR__ scene_restir_settings_=settings();}
    void update_data(){apply_count++;}void upload_immediately(){}
    DataWrap illumination_settings()const __ILLUMINATION_SETTINGS__
    static bool merge_illumination_setting(DataWrap& candidate,const std::string& key,const DataWrap& value) __COMMON_MERGE__
    void apply_illumination_settings(const DataWrap&candidate) __COMMON_APPLY__
    static bool merge_restir_settings(DataWrap& settings,const DataWrap& overrides) __RT_MERGE__
    DataWrap settings()const __SETTINGS__
    bool set_settings(const DataWrap& overrides) __SETTER__
};
void require(bool ok,const char*msg){if(!ok)throw std::runtime_error(msg);}
int main(){
    Harness h(16);
    require(h.max_depth_.hv()==15&&h.settings()["max_depth"]==16,"Constructor/getter public total-depth mismatch");
    require(!h.set_settings({{"max_depth",16},{"min_depth",5},{"rr_threshold",1}}),"Identical public settings should be no-op");
    for(uint depth=1;depth<=128;++depth){
        h.reference_gi_needs_compile_=false;
        h.set_settings({{"max_depth",depth}});
        require(h.reference_gi_needs_compile_,"Changed RT settings did not invalidate the reference GI program");
        require(h.max_depth_.hv()==depth-1&&h.settings()["max_depth"]==depth,"Setter/getter failed total-to-suffix roundtrip");
        require(h.min_depth_.hv()==5&&h.rr_threshold_.hv()==1,"Unrelated RR/depth setting changed");
    }
    h.set_settings({{"max_depth",16},{"direct",{{"M_light",2}}}});
    const auto before=h.settings();const auto writes=h.apply_count;
    for(const auto& bad:std::vector<DataWrap>{0,-1,129,1.5,"16",true}){
        require(!h.set_settings({{"max_depth",bad},{"direct",{{"M_light",1}}}}),"Invalid RT depth accepted");
        require(h.settings()==before&&h.apply_count==writes,"Rejected request changed live state");
    }
    require(h.set_settings({{"max_depth",1}})&&h.max_depth_.hv()==0&&h.settings()["max_depth"]==1,"RT depth one underflowed");
    bool rejected=false;try{Harness zero(0);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"Zero constructor depth must be rejected before unsigned subtraction");
    DataWrap pt={{"max_depth",16u},{"min_depth",5u},{"rr_threshold",1.f}};
    require(Harness::merge_illumination_setting(pt,"max_depth",0)&&pt["max_depth"]==0,"PT zero-depth validation contract changed");
    require(Harness::merge_illumination_setting(pt,"max_depth",128)&&pt["max_depth"]==128,"PT maximum-depth contract changed");
    std::cout<<"PASS 128 total-depth roundtrips, no-op16/5/1, six atomic rejections, RT1/0 constructor, PT0/128\n";
}
'''

UI_CPP = r'''

#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
using uint=unsigned;
namespace ocarina {std::string format(const char*,const char*name){return std::string(name)+" integrator";}}
struct Widgets {
 bool open=true,recompile=false,toggle_sp=false;std::optional<uint>depth_edit,min_edit;std::optional<float>rr_edit;
 std::optional<uint>shown_depth,lower_depth,upper_depth;std::vector<std::string>calls;
 template<class F>bool use_folding_header(const std::string&label,F f){calls.push_back(label);if(open)f();return open;}
 template<class F>bool button_click(const char*label,F f){calls.push_back(label);if(recompile)f();return recompile;}
 bool input_uint_limit(const char*label,uint*value,uint lower,uint upper,uint,uint){
  calls.push_back(label);auto edit=std::string(label)=="max depth"?depth_edit:min_edit;
  if(std::string(label)=="max depth"){shown_depth=*value;lower_depth=lower;upper_depth=upper;}
  if(!edit)return false;uint next=std::clamp(*edit,lower,upper);bool changed=next!=*value;*value=next;return changed;
 }
 bool drag_float(const char*label,float*value,double,float lower,float upper){calls.push_back(label);if(!rr_edit)return false;float next=std::clamp(*rr_edit,lower,upper);bool changed=next!=*value;*value=next;return changed;}
 bool check_box(const char*label,bool*value){calls.push_back(label);if(toggle_sp)*value=!*value;return toggle_sp;}
};
struct Node{std::string name;uint custom=0,calls=0;void render_UI(Widgets*w){++calls;w->calls.push_back(name);}};
template<class T>struct Param{T value;uint writes=0;T&hv(){return value;}const T&hv()const{return value;}Param&operator=(T x){value=x;++writes;return *this;}};
class Base {
public:
 Param<uint>max_depth_{15},min_depth_{5};Param<float>rr_threshold_{1};bool changed_=false;uint compile_count=0;
 std::shared_ptr<Node>denoiser_=std::make_shared<Node>(Node{"denoiser",37});
 std::string impl_type()const{return "ReSTIR";}void compile(){++compile_count;}
 virtual bool render_UI(Widgets*widgets) noexcept __BASE_UI__
 virtual void render_sub_UI(Widgets*) noexcept {}
 virtual ~Base()=default;
};
class Harness:public Base{
public:
 std::shared_ptr<Node>direct_=std::make_shared<Node>(Node{"DI",2}),indirect_=std::make_shared<Node>(Node{"GI",9});
 bool sp=true;uint sp_changes=0,set_settings_calls=0;
 bool stable_planes_enabled()const{return sp;}
 void set_stable_planes_enabled(bool v){sp=v;++sp_changes;}
 void set_settings(){++set_settings_calls;direct_->custom=10;indirect_->custom=50;}
 __RT_UI__
 void render_sub_UI(Widgets*widgets) noexcept override __SUB_UI__
};
void require(bool ok,const char*m){if(!ok)throw std::runtime_error(m);}
void unchanged_parameters(Harness&h){require(h.direct_->custom==2&&h.indirect_->custom==9&&h.denoiser_->custom==37&&h.set_settings_calls==0,"UI depth edit reset unrelated runtime settings");}
int main(){
 uint cases=0;
 for(uint initial:{1u,16u,20u,128u})for(int edit:{-1,0,1,16,20,128,129}){
  Harness h;h.max_depth_.value=initial-1;Widgets w;if(edit>=0)w.depth_edit=uint(edit);
  Base*slot=&h;bool open=slot->render_UI(&w);
  require(open&&w.shown_depth==initial,"RT UI shows suffix depth instead of public total");
  require(w.lower_depth==1&&w.upper_depth==128,"RT UI total depth bounds differ from API");
  uint expected=edit<0?initial:std::clamp(uint(edit),1u,128u);
  require(h.max_depth_.hv()==expected-1,"RT UI did not convert total depth to suffix");
  require(h.changed_==(initial!=expected),"RT UI changed flag differs from actual edit");
  require(h.max_depth_.writes==(initial!=expected?1u:0u),"Unedited depth was written back");
  require(h.min_depth_.hv()==5&&h.rr_threshold_.hv()==1,"Depth edit changed min/RR settings");
  require(h.direct_->calls==1&&h.indirect_->calls==1&&h.denoiser_->calls==1,"Existing child/denoiser UI omitted");
  require(w.calls==std::vector<std::string>{"ReSTIR integrator","recompile","max depth","min depth","rr threshold","Stable Plane","DI","GI","denoiser"},"UI order changed");
  unchanged_parameters(h);++cases;
 }
 Harness closed;Widgets wc;wc.open=false;wc.depth_edit=20;closed.render_UI(&wc);
 require(!wc.shown_depth&&closed.max_depth_.hv()==15&&closed.direct_->calls==0&&closed.indirect_->calls==0&&closed.denoiser_->calls==1,"Folded UI behavior changed");unchanged_parameters(closed);++cases;
 Harness h;Widgets w;w.depth_edit=20;w.min_edit=7;w.rr_edit=.25f;w.recompile=true;w.toggle_sp=true;h.render_UI(&w);
 require(h.max_depth_.hv()==19&&h.min_depth_.hv()==7&&h.rr_threshold_.hv()==.25f&&h.compile_count==1&&!h.sp&&h.sp_changes==1&&h.changed_,"Other UI controls changed behavior");unchanged_parameters(h);++cases;
 Harness dirty;dirty.changed_=true;Widgets wd;dirty.render_UI(&wd);require(dirty.changed_&&dirty.max_depth_.writes==0,"Preexisting dirty flag lost");++cases;
 std::cout<<"PASS "<<cases<<" actual UI cases; total/suffix mapping, unchanged runtime settings, existing virtual dispatch and controls\n";
}
'''


def _json_include():
    commands_path = ROOT/'cmake-build-relwithdebinfo/compile_commands.json'
    if commands_path.exists():
        commands = json.loads(commands_path.read_text(encoding='utf-8'))
        for command in commands:
            for include in re.findall(r'(?:-external:I|-I)(?:"([^"]+)"|([^ ]+))', command.get('command', '')):
                path = Path(include[0] or include[1])
                if (path/'nlohmann/json.hpp').is_file():
                    return path
    raise SkipTest('Depth API harness requires the configured nlohmann/json.hpp dependency')


def test_public_total_depth_api_roundtrip_and_atomic_rejection():
    source = (ROOT/'vision/src/render_core/integrator/rt.cpp').read_text(encoding='utf-8')
    base = (ROOT/'vision/src/base/integral/integrator.cpp').read_text(encoding='utf-8')
    replacements = {
        '__CONSTRUCTOR__': body(source, 'explicit RealTimeIntegrator(')[1],
        '__RT_MERGE__': body(source, 'static bool merge_restir_settings(')[1],
        '__SETTINGS__': body(source, 'DataWrap settings() const override')[1],
        '__SETTER__': body(source, 'bool set_settings(const DataWrap &overrides)')[1],
        '__ILLUMINATION_SETTINGS__': body(base, 'DataWrap IlluminationIntegrator::illumination_settings(')[1],
        '__COMMON_MERGE__': body(base, 'bool IlluminationIntegrator::merge_illumination_setting(')[1],
        '__COMMON_APPLY__': body(base, 'void IlluminationIntegrator::apply_illumination_settings(')[1]}
    program = API_CPP
    for key, value in replacements.items():
        program = program.replace(key, value)
    compile_run(program, 'depth-api', [_json_include()])


def test_public_total_depth_gui_and_runtime_parameters():
    rt = (ROOT/'vision/src/render_core/integrator/rt.cpp').read_text(encoding='utf-8')
    base = (ROOT/'vision/src/base/integral/integrator.cpp').read_text(encoding='utf-8')
    base_ui = body(base, 'bool IlluminationIntegrator::render_UI(')[1]
    token = '    bool render_UI(Widgets *widgets) noexcept override'
    ui = ''.join(body(rt, token)) if token in rt else ''
    sub_ui = body(rt, '    void render_sub_UI(Widgets *widgets) noexcept override')[1]
    compile_run(UI_CPP.replace('__BASE_UI__', base_ui).replace('__RT_UI__', ui)
                .replace('__SUB_UI__', sub_ui), 'depth-ui')


def test_shared_restir_validator_matches_public_total_depth_range():
    program = r'''
#include <nlohmann/json.hpp>
#include "src/systems/common/vision_restir_settings.h"
#include <iostream>
#include <stdexcept>
#include <vector>
using Json = nlohmann::json;
int main() {
    using Corona::Systems::Vision::validate_restir_settings;
    for (unsigned depth = 1; depth <= 128; ++depth) {
        if (!validate_restir_settings(Json{{"max_depth", depth}}).empty())
            throw std::runtime_error("valid total depth rejected by public validator");
    }
    for (const Json& depth : std::vector<Json>{0, -1, 129, 1.5, "16", true}) {
        if (validate_restir_settings(Json{{"max_depth", depth}}).empty())
            throw std::runtime_error("invalid total depth accepted by public validator");
    }
    std::cout << "PASS public ReSTIR validator: 128 valid depths and six rejections\n";
}
'''
    compile_run(program, 'depth-public-validator', [ROOT, _json_include()])
