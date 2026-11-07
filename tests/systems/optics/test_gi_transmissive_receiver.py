"""Execute the production GI receiver classifier and both reuse guards on CPU.

Scalar/lobe host fixtures test the policy boundary, not GPU code generation,
BSDF sampling or image quality. Temporary compiler outputs stay in .workspace.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "vision/src/render_core/integrator/ReSTIR/indirect.cpp"


def _balanced_end(source, start, opening, closing):
    depth = 0
    for index in range(start, len(source)):
        if source[index] == opening:
            depth += 1
        elif source[index] == closing:
            depth -= 1
            if depth == 0:
                return index + 1
    raise AssertionError("Unbalanced production source")


def _function(source, name):
    start = source.index(name)
    body = source.index("{", start)
    return source[start:_balanced_end(source, body, "{", "}")]


def test_transmissive_receiver_bypasses_both_reuse_stages():
    source = SOURCE.read_text(encoding="utf8")
    assert "Bool gi_receiver_has_transmission(" in source, "Missing deterministic receiver classifier"
    helper = _function(source, "Bool gi_receiver_has_transmission(")
    guards = {}
    spatial_setup = ""
    for stage in ("temporal", "spatial"):
        cached = stage == "spatial" and "ReSTIRGI::spatial_reuse_cached(" in source
        body = _function(source, f"GIReservoirVar ReSTIRGI::{stage}_reuse_cached(" if cached
                         else f"GIReservoirVar ReSTIRGI::{stage}_reuse(")
        if cached:
            spatial_setup = body[body.index("    Bool reuse_allowed"):body.index("    $if(param.spatial")]
            assert "receiver_reuse_allowed(cur_surf)" in spatial_setup
            assert "!gi_receiver_has_transmission(*cached_bsdf" in spatial_setup
        for match in re.finditer(r"\$if\(", body):
            start = match.end() - 1
            end = _balanced_end(body, start, "(", ")")
            condition = body[start + 1:end - 1]
            if "param." + stage in condition:
                guards[stage] = condition
                break
        assert stage in guards, f"Missing {stage} reuse condition"
        expected_gate = "reuse_allowed" if cached else "receiver_reuse_allowed(cur_surf)"
        assert expected_gate in guards[stage], f"{stage} does not gate receiver reuse"

    cpp=r'''
    #include <algorithm>
    #include <array>
    #include <cstdlib>
    #include <functional>
    #include <iostream>
    #include <vector>
    using uint=unsigned;using Float=float;using Bool=bool;
    struct Float2{};
    struct Spectrum{uint dimension()const{return 3;}};Spectrum*spectrum(){static Spectrum s;return &s;}
    struct SampledSpectrum { float v{}; explicit SampledSpectrum(uint){} float max()const{return v;} };
    struct Lobe {
     virtual ~Lobe()=default;virtual bool is_multi()const{return false;}
     virtual void reuse_material(SampledSpectrum&d,SampledSpectrum&s,Float2&)const{d.v=1;s.v=0;}
    };
    struct DielectricLobe: Lobe {
     float body; explicit DielectricLobe(float b):body(b){}
     void reuse_material(SampledSpectrum&d,SampledSpectrum&s,Float2&)const override{d.v=body;s.v=1-body;}
    };
    struct WeightedLobe {const Lobe* p;float sw,w;const Lobe* get()const{return p;}float sample_weight()const{return sw;}float weight()const{return w;}};
    struct LobeSet:Lobe {
     std::vector<WeightedLobe> children;
     explicit LobeSet(std::vector<WeightedLobe> a):children(std::move(a)){}
     bool is_multi()const override{return true;}
     void for_each(const std::function<void(const WeightedLobe&)>&v)const{for(auto&c:children)v(c);}
    };
    struct MaterialEvaluator {const Lobe*root;template<class F>void dispatch(F&& f)const{f(root);}};
    '''+helper+r'''
    struct Reservoir {float C,W,Lo;bool operator==(const Reservoir&)const=default;};
    const Reservoir fresh{1,2,3}, reused{11,17,19};
    struct Param {bool temporal,spatial;};
    bool inside=true;
    bool in_screen(int,int){return inside;}
    bool receiver_reuse_allowed(const MaterialEvaluator &m){return !gi_receiver_has_transmission(m,3);}
    int failures=0;
    void expect(bool b,const char*m){if(!b){std::cerr<<m<<'\n';++failures;}}
    void check(const Lobe&l,bool allowed,const char*label) {
     MaterialEvaluator cur_surf{&l};int prev_p=0,res=1;
     expect(receiver_reuse_allowed(cur_surf)==allowed,label);
     for(bool cached:{false,true}) {
      const void*cached_it=cached?&cur_surf:nullptr;
      MaterialEvaluator*cached_bsdf=cached?&cur_surf:nullptr;
      SPATIAL_SETUP
     for(bool requested:{false,true})for(bool screen:{false,true}) {
      Param param{requested,requested};inside=screen;
      Reservoir temporal=fresh,spatial=fresh;int temporal_calls=0,spatial_calls=0;
      if(TEMPORAL_GUARD){temporal=reused;++temporal_calls;}
      if(SPATIAL_GUARD){spatial=reused;++spatial_calls;}
      bool t=requested&&screen&&allowed,sp=requested&&allowed;
      expect(temporal==(t?reused:fresh),"temporal changed canonical or opaque behavior");
      expect(spatial==(sp?reused:fresh),"spatial changed canonical or opaque behavior");
      expect(temporal_calls==int(t)&&spatial_calls==int(sp),"bypass must not consume a reuse stream");
     }
     }
    }
    int main(){
     Lobe opaque;DielectricLobe glass(.96f),black(0);
     LobeSet principled({{&opaque,.1f,1},{&glass,.9f,1}});
     LobeSet zero_sample({{&opaque,1,1},{&glass,0,1}});
     LobeSet zero_weight({{&opaque,1,1},{&glass,1,0}});
     LobeSet opaque_set({{&opaque,1,1},{&black,1,1}});
     LobeSet tiny_active({{&opaque,1,1},{&glass,1e-30f,1e-30f}});
     check(opaque,true,"top opaque");check(glass,false,"top dielectric");check(black,true,"zero-transmission dielectric");
     check(principled,false,"active Principled dielectric");check(zero_sample,true,"inactive sample weight");
     check(zero_weight,true,"zero energy weight");check(opaque_set,true,"opaque LobeSet");check(tiny_active,false,"nonzero active weights must not underflow in a product");
     if(failures)return 1;std::cout<<"PASS: actual classifier + actual temporal/spatial guards, 8 material cases x 4 switch/screen cases\n";
    }
    '''
    cpp = cpp.replace("SPATIAL_SETUP", spatial_setup).replace("TEMPORAL_GUARD", guards["temporal"]).replace("SPATIAL_GUARD", guards["spatial"])
    compiler = shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        raise SkipTest("A C++20 host compiler is required for the production receiver-policy test")
    temporary_root = ROOT / ".workspace/tmp/restir-blackspots/receiver-gate-tests"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cpu-", dir=temporary_root) as directory:
        work = Path(directory).resolve()
        assert work.parent == temporary_root.resolve()
        harness = work / "receiver-gate.cpp"
        executable = work / "receiver-gate.exe"
        harness.write_text(cpp, encoding="utf8")
        build = subprocess.run([compiler, "-std=c++20", "-O0", str(harness), "-o", str(executable)],
                               capture_output=True, text=True, timeout=60)
        assert build.returncode == 0, build.stdout + build.stderr
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "8 material cases x 4 switch/screen cases" in result.stdout
