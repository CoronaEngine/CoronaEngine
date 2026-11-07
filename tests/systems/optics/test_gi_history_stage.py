"""Execute the production GI spatial/shading tail against CPU buffer boundaries.

The real buffer accessors and statements run unchanged. A deterministic spatial
neighbor supplies a distinguishable sample, so the next frame can detect whether
spatial output was fed back as temporal history. No renderer or GPU is used.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[3]


def extract_function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@pytest.fixture(scope="module")
def history_harness():
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("Standalone GI history regression requires g++")
    source = (ROOT / "vision/src/render_core/integrator/ReSTIR/indirect.cpp").read_text(encoding="utf-8")
    header = (ROOT / "vision/src/render_core/integrator/ReSTIR/indirect.h").read_text(encoding="utf-8")
    start = source.index("GIReservoirVar rsv = passthrough_reservoirs().read(dispatch_id());",
                         source.index("void ReSTIRGI::compile_spatial_shading()"))
    tail = source[start:source.index("\n    };", start)]
    accessors = "\n".join(extract_function(header, f"[[nodiscard]] auto {name}() const noexcept")
                           for name in ("prev_reservoirs", "cur_reservoirs", "passthrough_reservoirs"))
    program = r'''
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
using std::addressof;
#define $if if
#define $else else
using Float = double;
using Float3 = double;
struct Float4 { double value, distance; };
double make_float3(double value) { return value; }
Float4 make_float4(double value,double distance) { return {value,distance}; }
int make_int2(int value) { return value; }
struct GIReservoir {
    int sample, generation;
    double C, W, weight_sum;
    bool operator==(const GIReservoir&) const = default;
};
using GIReservoirVar = GIReservoir;
std::array<GIReservoir,2> storage[2];
struct Buffer {
    int slice;
    GIReservoir read(int i) const { return storage[slice][i]; }
    void write(int i,GIReservoir value) const { storage[slice][i]=value; }
};
// This history-stage fixture models the storage adapter boundary. The actual
// codec and physical slices are exercised by test_gi_reservoir_storage.py.
struct GIReservoirStorage {};
struct GIReservoirStorageView {
    Buffer buffer;
    GIReservoir read(int i) const { return buffer.read(i); }
    void write(int i, GIReservoir value) { buffer.write(i, value); }
};
struct Bindless { template<typename T> Buffer buffer_var(int i) { return {i}; } } bindless;
struct Interaction{bool has_material()const{return true;}int material_id()const{return 0;}};
struct MaterialEvaluator{};
struct Material{MaterialEvaluator create_evaluator(const Interaction&,int)const{return{};}};
struct Materials{template<class F>void dispatch(int,F f){Material m;f(&m);}};
struct Scene{Materials materials(){return{};}};Scene scene(){return{};}
int sampled_wavelengths(){return 0;}double cur_view_pos(bool){return 0;}
struct Surface{int hit{};bool is_replaced{};operator int()const{return 0;}};
struct Geometry{Interaction compute_surface_interaction(int,double){return{};}};
struct Pipeline {Geometry geometry(){return{};} Bindless& bindless_array() { return bindless; } } pipe;
Pipeline* pipeline() { return &pipe; }
struct Specular { void write(int,Float4) {} } specular_buffer;
struct FrameBuffer { Specular& denoiser_gi_specular() { return specular_buffer; } } fb;
FrameBuffer& frame_buffer() { return fb; }
struct Param { bool spatial; unsigned precise_lobes; };
struct Index { int value; int xy() const { return value; } };
struct Harness {
    int index{}, spatial_calls{}, shade_calls{}, lobe_calls{};
    std::array<double,2> radiance{};
    int reservoir_base() const { return 0; }
    ACCESSORS
    int dispatch_id() const { return index; }
    Index dispatch_idx() const { return {index}; }
    GIReservoir spatial_reuse(GIReservoir canonical,int,int,Param param) {
        ++spatial_calls;
        if(!param.spatial) return canonical;
        auto result=passthrough_reservoirs().read(1-index);
        ++result.generation;
        result.C+=3; result.W*=2; result.weight_sum+=17;
        return result;
    }
    double shading(GIReservoir rsv,int,double* specular,double* distance) {
        ++shade_calls;
        if(specular) *specular=rsv.sample;
        if(distance) *distance=2;
        return rsv.sample+100*rsv.generation;
    }
    GIReservoir spatial_reuse_cached(GIReservoir rsv,Surface surf,int pixel,Param param,
                                      const Interaction*,MaterialEvaluator*) {
        return spatial_reuse(rsv,surf,pixel,param);
    }
    void write_radiance(int i,Float4 value) { radiance[i]=value.value; }
    void run(int pixel,Param param,bool lobes) {
        index=pixel;
        Surface surf{};
        auto finish_lobes=[&](double,double,double) { ++lobe_calls; };
        TAIL
    }
};
int main(int argc,char** argv) {
    bool spatial=std::atoi(argv[1]);
    int lobe_mode=std::atoi(argv[2]);
    Param param{spatial,unsigned(lobe_mode==2)};
    Harness harness;
    storage[1]={GIReservoir{11,0,2,3,6},GIReservoir{22,0,4,5,20}};
    for(int frame=0;frame<2;++frame) {
        auto canonical=storage[1];
        for(int pixel=0;pixel<2;++pixel) harness.run(pixel,param,lobe_mode!=0);
        for(int pixel=0;pixel<2;++pixel) {
            bool exact_history=harness.prev_reservoirs().read(pixel)==canonical[pixel];
            bool scratch_unchanged=harness.passthrough_reservoirs().read(pixel)==canonical[pixel];
            double expected_shading=canonical[spatial?1-pixel:pixel].sample+(spatial?100:0);
            std::cout<<frame<<' '<<pixel<<' '<<exact_history<<' '<<scratch_unchanged<<' '
                     <<harness.radiance[pixel]<<' '<<expected_shading<<'\n';
        }
        // Next temporal stage consumes the actual production history binding.
        for(int pixel=0;pixel<2;++pixel)
            storage[1][pixel]=harness.prev_reservoirs().read(pixel);
    }
    std::cout<<harness.spatial_calls<<' '<<harness.shade_calls<<' '<<harness.lobe_calls<<'\n';
}
'''.replace("ACCESSORS", accessors).replace("TAIL", tail)
    temporary_root = ROOT / ".workspace/tmp/restir-blackspots/audit/gi-history-test"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as temporary:
        directory = Path(temporary)
        source, executable = directory / "harness.cpp", directory / "harness.exe"
        source.write_text(program, encoding="utf-8")
        built = subprocess.run([compiler, "-std=c++20", "-O0", str(source), "-o", str(executable)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        yield executable


@pytest.mark.parametrize("spatial", [False, True])
@pytest.mark.parametrize("lobe_mode", [0, 1, 2], ids=["raw", "legacy_lobes", "precise_lobes"])
def test_history_retains_temporal_result_while_shading_uses_spatial_result(history_harness, spatial, lobe_mode):
    result = subprocess.run([str(history_harness), str(int(spatial)), str(lobe_mode)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert len(lines) == 5
    for line in lines[:4]:
        frame, pixel, exact_history, scratch_unchanged, radiance, expected = map(float, line.split())
        assert exact_history == 1, f"frame {frame}, pixel {pixel}: spatial feedback entered next-frame history"
        assert scratch_unchanged == 1, "spatial reads must not overwrite another pixel's canonical input"
        assert radiance == expected, "this frame must still shade the spatially reused reservoir"
    assert list(map(int, lines[-1].split())) == [4, 4, 4 if lobe_mode == 2 else 0]
