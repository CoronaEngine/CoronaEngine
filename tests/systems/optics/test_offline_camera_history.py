"""Execute the actual offline render loop against a CPU pipeline boundary.

The boundary models Sensor::after_render updating host previous-camera data,
and Pipeline::upload_data publishing that data to GPU-visible storage. The
production sync prefix and render loop are compiled unchanged; no GPU runs.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope="module")
def offline_history_harness():
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("Standalone offline camera regression requires g++")
    source = (ROOT / "src/systems/optics/optics_system.cpp").read_text(encoding="utf-8")
    prefix_start = source.index("Vision::sync_vision_camera(*pipeline, camera);")
    prefix_end = source.index("const bool diagnostic =", prefix_start)
    prefix = source[prefix_start:prefix_end]
    loop_start = source.index("const auto render_frames = capture ? capture->render_frames : 1u;", prefix_end)
    opening = source.index("{", source.index("for (unsigned frame = 1;", loop_start))
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            break
    loop = source[loop_start:end + 1]
    code = r'''
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>
struct Capture {
    unsigned render_frames;
    std::chrono::steady_clock::time_point expires_at=std::chrono::steady_clock::time_point::max();
};
struct StageProfile {
    bool enabled=false;
    double gbuffer_ms=0, sampling_mask_ms=0, path_tracing_ms=0, stable_build_ms=0, stable_fill_ms=0;
    double restir_di_ms=0, restir_gi_ms=0, spatial_angular_ms=0, temporal_ms=0;
    double combine_ms=0, postprocess_ms=0, render_final_ms=0;
};
struct Integrator {
    double cur_denoise_time() { return 0; }
    StageProfile cur_stage_profile() { return {}; }
} integrator;
struct Renderer { Integrator* integrator() { return &::integrator; } } renderer;
struct Pipeline {
    int current_camera=0, host_previous=-7, gpu_previous=-7, uploads=0;
    int expire_after=-1;
    Capture* capture=nullptr;
    std::vector<int> observed_previous;
    void upload_data() { gpu_previous=host_previous; ++uploads; }
    void display(double) {
        observed_previous.push_back(gpu_previous);
        host_previous=current_camera;
        if (capture && static_cast<int>(observed_previous.size())==expire_after)
            capture->expires_at=std::chrono::steady_clock::time_point::min();
    }
    unsigned frame_index() { return static_cast<unsigned>(observed_previous.size()); }
    double cur_render_time() { return 0; }
    Renderer& renderer() { return ::renderer; }
};
struct Camera { int position=42, output_mode=0; };
namespace Vision {
void sync_vision_camera(Pipeline& pipeline,const Camera& camera) { pipeline.current_camera=camera.position; }
struct VisionDiagnosticAdapter {
    bool render(Pipeline& pipeline,int) { pipeline.display(0); return true; }
};
}
struct Timings : std::ostringstream { bool is_open() const { return false; } };
struct Phase { void set(const char*) {} };
int failures=0;
void fail_pending_screenshots(int) { ++failures; }
void execute(Pipeline* pipeline,unsigned count,bool capturing,bool diagnostic,int expire_after) {
    Camera camera;
    Phase vision_phase;
    SYNC_PREFIX
    std::optional<Capture> capture;
    if (capturing) capture=Capture{count};
    pipeline->capture=capture ? &*capture : nullptr;
    pipeline->expire_after=expire_after;
    if (capture && expire_after==0) capture->expires_at=std::chrono::steady_clock::time_point::min();
    int cam_handle=1;
    struct Runtime { std::unordered_map<int,std::unique_ptr<Vision::VisionDiagnosticAdapter>> diagnostics; } runtime;
    Timings frame_timings, stage_timings;
    unsigned long long capture_qpc_frequency=1;
    auto capture_profile_qpc=[] { return 0u; };
    RENDER_LOOP
}
int main(int argc,char** argv) {
    Pipeline pipeline;
    execute(&pipeline,std::atoi(argv[1]),std::atoi(argv[2])!=0,std::atoi(argv[3])!=0,std::atoi(argv[4]));
    bool after_first_current=true;
    for (size_t i=1;i<pipeline.observed_previous.size();++i)
        after_first_current &= pipeline.observed_previous[i]==42;
    std::cout << pipeline.uploads << ' ' << pipeline.observed_previous.size() << ' '
              << failures << ' ' << after_first_current;
}
'''.replace("SYNC_PREFIX", prefix).replace("RENDER_LOOP", loop)
    temporary_root = ROOT / ".workspace/tmp/restir-blackspots/audit/offline-camera-history-test"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as directory:
        directory = Path(directory)
        cpp, executable = directory / "harness.cpp", directory / "harness.exe"
        cpp.write_text(code, encoding="utf-8")
        built = subprocess.run([compiler, "-std=c++20", "-O0", str(cpp), "-o", str(executable)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        yield executable


@pytest.mark.parametrize("count,capturing,diagnostic,expire_after,rendered,failed", [
    (64, True, False, -1, 64, 0),
    (1, False, False, -1, 1, 0),
    (64, True, False, 0, 0, 1),
    (64, True, False, 1, 1, 1),
    (1, False, True, -1, 1, 0),
])
def test_each_render_observes_published_previous_camera(offline_history_harness, count, capturing,
                                                       diagnostic, expire_after, rendered, failed):
    result = subprocess.run([str(offline_history_harness), str(count), str(int(capturing)),
                             str(int(diagnostic)), str(expire_after)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    uploads, actual_rendered, actual_failed, after_first_current = map(int, result.stdout.split())
    assert actual_rendered == rendered
    assert actual_failed == failed
    assert after_first_current == 1, "The second and subsequent frames must see the last rendered camera"
    assert uploads == rendered, "Publish once per rendered frame, after checking capture timeout"
