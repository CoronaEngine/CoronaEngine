"""Execute the actual GI prefix classifier and guarded buffer-read block on CPU.

The fixture encodes branches using the real scatter_path expression, including
its root sentinel. Host buffers reject forbidden reads and count valid accesses.
"""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from toolchain import find_compiler  # noqa: E402
import re
import shutil
import subprocess
import tempfile
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[3]


def _body(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_transmission_prefix_only_masks_temporal_and_guards_metadata_reads():
    path = ROOT / "vision/src/render_core/integrator/ReSTIR/indirect.cpp"
    source = path.read_text(encoding="utf-8")
    assert "Bool gi_prefix_has_transmission(" in source, "Missing SP transmission-prefix policy"
    helper = _body(source, "Bool gi_prefix_has_transmission(")
    temporal = _body(source, "void ReSTIRGI::compile_temporal_reuse()")
    start = temporal.index("Bool temporal_allowed = param.temporal != 0u;")
    block = temporal[start:temporal.index("passthrough_reservoirs().write(dispatch_id(), rsv);", start)]
    assert "rsv = temporal_reuse(rsv, surf, motion_vec, ss, param);" in block
    assert "gi_prefix_has_transmission" not in _body(source, "GIReservoirVar ReSTIRGI::spatial_reuse(")
    stable = (ROOT / "vision/src/render_core/integrator/ReSTIR/stable_planes.cpp").read_text(encoding="utf-8-sig")
    encoding = re.search(r"path\.sequence\s*=\s*([^;]+);", stable).group(1).replace("path.sequence", "sequence")
    sentinel = re.search(r"Uint sequence\{(\d+)u\}", stable).group(1)
    sample = (ROOT / "vision/src/base/scattering/sample.h").read_text(encoding="utf-8-sig")
    transmission = re.search(r"transmission_index\s*=\s*(\d+)u", sample).group(1)
    reflection = re.search(r"reflection_index\s*=\s*(\d+)u", sample).group(1)
    program = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
using uint=unsigned; using Uint=uint; using Bool=bool;
using std::min;
#define $if if
constexpr uint StablePlaneCount=3;
constexpr uint InvalidUI32=~0u;
uint select(bool condition,uint yes,uint no){return condition?yes:no;}
HELPER
struct Plane {
    uint branch_sequence=1,depth=0,flags=3;
    uint valid()const{return flags&1u;}
    uint reservoir_eligible()const{return(flags>>1u)&1u;}
    const Plane*operator->()const{return this;}
};
using StablePlaneDataVar=Plane;
struct Param {uint temporal=1,spatial=1,di=1,denoiser_layered=1;};
struct Surface {bool is_replaced=true;};
struct Integrator {bool enabled=true;bool stable_planes_enabled()const{return enabled;}};
struct Dim{uint x=2,y=3;};
struct Harness;
struct DominantBuffer {Harness*owner;uint read(uint)const;};
struct PlaneBuffer {Harness*owner;Plane read(uint)const;};
struct Harness {
    Integrator owner; Surface surf; Plane plane;
    uint dominant=1,dominant_reads=0,plane_reads=0,temporal_calls=0;
    bool forbidden=false;
    Integrator*integrator(){return &owner;}
    Harness&frame_buffer(){return *this;}
    DominantBuffer stable_dominant(){return{this};}
    PlaneBuffer stable_planes(){return{this};}
    uint dispatch_id()const{return 4;}
    Dim dispatch_dim()const{return{};}
    uint temporal_reuse(uint rsv,Surface,uint,uint,const Param&){++temporal_calls;return rsv+100;}
    uint run(const Param&param){
        uint rsv=17,motion_vec=0,ss=0;
        BLOCK
        return rsv;
    }
};
uint DominantBuffer::read(uint pixel)const{
    if(owner->forbidden||!owner->owner.enabled||pixel!=4)throw std::runtime_error("forbidden dominant read");
    ++owner->dominant_reads;return owner->dominant;
}
Plane PlaneBuffer::read(uint address)const{
    if(owner->forbidden||!owner->owner.enabled||owner->dominant>=3||address!=owner->dominant*6+4)
        throw std::runtime_error("forbidden plane read or wrong layered address");
    ++owner->plane_reads;return owner->plane;
}
void require(bool condition,const char*message){if(!condition)throw std::runtime_error(message);}
uint encode(uint choices,uint depth){
    uint sequence=SENTINEL;
    for(uint i=0;i<depth;++i){uint branch=(choices&(1u<<i))?TRANSMISSION:REFLECTION;sequence=ENCODING;}
    return sequence;
}
int main(){
    uint sequence_cases=0,guard_cases=0;
    // All branch combinations through the full 15-bounce representable prefix.
    for(uint depth=0;depth<=15;++depth){
        for(uint choices=0;choices<(1u<<depth);++choices){
            uint sequence=encode(choices,depth);
            require(gi_prefix_has_transmission(sequence,depth)==(choices!=0),"prefix classification");
            ++sequence_cases;
        }
    }
    // Sentinels and the 32-bit shift boundary remain safe at the max depth.
    require(!gi_prefix_has_transmission(1,0),"root sentinel misclassified");
    require(!gi_prefix_has_transmission(encode(0,15),15),"max-depth reflections misclassified");
    for(uint mode=0;mode<10;++mode){
        for(uint choices: {0u,1u,2u,3u}){
            Harness h; h.plane.branch_sequence=encode(choices,2);h.plane.depth=2;
            Param input;
            if(mode==1){input.denoiser_layered=0;h.owner.enabled=false;h.forbidden=true;}
            if(mode==2){h.surf.is_replaced=false;h.forbidden=true;}
            if(mode==3){input.temporal=0;h.forbidden=true;}
            if(mode==4)h.dominant=InvalidUI32;
            if(mode==5)h.dominant=3;
            if(mode==6)h.plane.flags=0;
            if(mode==7)h.plane.flags=1;
            if(mode==8){h.plane.depth=0;h.plane.branch_sequence=1;}
            if(mode==9){h.dominant=2;h.plane.depth=15;h.plane.branch_sequence=encode(choices,15);}
            uint output=h.run(input);
            bool usable=(mode==0||mode==9);
            bool expected_temporal=input.temporal!=0&&!(usable&&choices!=0);
            require(h.temporal_calls==uint(expected_temporal),"temporal gate policy");
            require(output==(expected_temporal?117u:17u),"fresh canonical reservoir not retained");
            require(input.spatial==1&&input.di==1,"spatial or DI changed");
            require(input.temporal==(mode==3?0u:1u),"host dispatch parameters mutated");
            require(h.dominant_reads==((mode==1||mode==2||mode==3)?0u:1u),"dominant read guard");
            require(h.plane_reads==((mode>=1&&mode<=5)?0u:1u),"plane read guard");
            ++guard_cases;
        }
    }
    std::cout<<sequence_cases<<" sequences; "<<guard_cases<<" guarded policy cases passed\n";
}
'''
    program = program.replace("HELPER", helper).replace("BLOCK", block)
    program = program.replace("ENCODING", encoding).replace("SENTINEL", sentinel)
    program = program.replace("TRANSMISSION", transmission).replace("REFLECTION", reflection)
    compiler = find_compiler(("g++", "clang++"))
    if not compiler:
        raise SkipTest("Standalone CPU regression requires g++ or clang++")
    temporary_root = ROOT / ".workspace/tmp/restir-blackspots/prefix-gate-tests"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
        directory = Path(temporary)
        cpp, exe = directory / "harness.cpp", directory / "harness.exe"
        cpp.write_text(program, encoding="utf-8")
        result = subprocess.run([compiler,"-std=c++20","-O0",str(cpp),"-o",str(exe)],capture_output=True,text=True)
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(exe)],capture_output=True,text=True)
        assert result.returncode == 0, result.stderr
        assert result.stdout.strip() == "65535 sequences; 40 guarded policy cases passed"
        print(result.stdout.strip())


if __name__ == "__main__":
    test_transmission_prefix_only_masks_temporal_and_guards_metadata_reads()
