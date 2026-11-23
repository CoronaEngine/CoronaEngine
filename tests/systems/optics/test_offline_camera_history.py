"""Execute the actual offline render loop against a CPU pipeline boundary.

The boundary models Sensor::after_render updating host previous-camera data,
and Pipeline::upload_data publishing that data to GPU-visible storage. The
production sync prefix and render loop are compiled unchanged; no GPU runs.
"""
from pathlib import Path
import csv
import math
import os
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from toolchain import find_compiler  # noqa: E402
import shutil
import subprocess
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture(scope="module")
def offline_history_harness():
    compiler = find_compiler()
    if not compiler:
        pytest.skip("Standalone offline camera regression requires g++")
    source = (ROOT / "src/systems/optics/optics_system.cpp").read_text(encoding="utf-8")
    prefix_start = source.index("Vision::sync_vision_camera(*pipeline, camera);")
    prefix_end = source.index("const bool diagnostic =", prefix_start)
    prefix = source[prefix_start:prefix_end]
    check_start = source.index("const auto incompatible = [&]")
    check_end = source.index("if (incompatible()) return;", check_start)
    compatibility = source[check_start:check_end]
    loop_start = source.index("const auto render_frames = capture ? capture->render_frames : 1u;", prefix_end)
    opening = source.index("{", source.index("for (unsigned frame = 1;", loop_start))
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            break
    loop = source[loop_start:end + 1]
    code = r'''
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>
struct Capture {
    unsigned render_frames;
    std::chrono::steady_clock::time_point expires_at=std::chrono::steady_clock::time_point::max();
    std::string timings_path, file_path;
};
struct StageProfile {
    bool enabled=false;
    double gbuffer_ms=0, sampling_mask_ms=0, path_tracing_ms=0, stable_build_ms=0, stable_fill_ms=0;
    double restir_di_ms=0, restir_gi_ms=0, spatial_angular_ms=0, temporal_ms=0;
    double combine_ms=0, postprocess_ms=0, render_final_ms=0;
};
struct Integrator {
    const char *reason=nullptr;
    const char *render_incompatibility() const { return reason; }
    bool allows_path_tracing_fallback() const { return false; }
    double cur_denoise_time() { return 0; }
    StageProfile cur_stage_profile() { return {}; }
} integrator;
struct Renderer { Integrator* integrator() { return &::integrator; } } renderer;
namespace ktm { struct fvec3 { float x{}, y{}, z{}; }; }
enum class CameraVisionRenderMode { ReSTIR };
struct CameraOutputMode { enum { FinalColor }; };
struct Camera {
    ktm::fvec3 position{42.f,0.f,0.f}, forward{0.f,0.f,-1.f}, world_up{0.f,1.f,0.f};
    int output_mode=0;
    int vision_restir_settings=0;
    bool surface=false;
};
struct Sensor {
    unsigned camera_revision=1, projection_revision=17;
    unsigned external_camera_revision() const { return camera_revision; }
    unsigned external_projection_revision() const { return projection_revision; }
};
struct Scene { Sensor value; Sensor* sensor() { return &value; } };
struct Pipeline {
    Camera current_camera, host_previous{ktm::fvec3{-7.f,0.f,0.f}}, gpu_previous=host_previous;
    int uploads=0, sync_calls=0;
    int expire_after=-1;
    Capture* capture=nullptr;
    Scene scene_value;
    std::vector<Camera> observed_previous, observed_current;
    Scene& scene() { return scene_value; }
    void upload_data() { gpu_previous=host_previous; ++uploads; }
    void display(double) {
        observed_previous.push_back(gpu_previous);
        observed_current.push_back(current_camera);
        host_previous=current_camera;
        if (std::getenv("TEST_INCOMPATIBLE_AFTER_DISPLAY")) integrator.reason="unsupported camera";
        if (capture && static_cast<int>(observed_previous.size())==expire_after)
            capture->expires_at=std::chrono::steady_clock::time_point::min();
    }
    unsigned frame_index() { return static_cast<unsigned>(observed_previous.size()); }
    double cur_render_time() { return 0; }
    Renderer& renderer() { return ::renderer; }
};
namespace Vision {
void sync_vision_camera(Pipeline& pipeline,const Camera& camera) {
    auto differs=[](ktm::fvec3 a,ktm::fvec3 b) { return a.x!=b.x || a.y!=b.y || a.z!=b.z; };
    const auto& before=pipeline.current_camera;
    if (differs(before.position,camera.position) || differs(before.forward,camera.forward) || differs(before.world_up,camera.world_up))
        ++pipeline.scene_value.value.camera_revision;
    pipeline.current_camera=camera;
    ++pipeline.sync_calls;
}
struct VisionDiagnosticAdapter {
    bool render(Pipeline& pipeline,int) { pipeline.display(0); return true; }
};
}
struct Timings : std::ostringstream { bool is_open() const { return false; } };
struct Phase { void set(const char*) {} };
int failures=0;
void fail_pending_screenshots(int) { ++failures; }
void execute(Pipeline* pipeline,unsigned count,bool capturing,bool diagnostic,int expire_after,const char* output_path) {
    Camera camera;
    Phase vision_phase;
    SYNC_PREFIX
    std::optional<Capture> capture;
    if (capturing) capture=Capture{count};
    if (capture) capture->file_path=output_path;
    pipeline->capture=capture ? &*capture : nullptr;
    pipeline->expire_after=expire_after;
    if (capture && expire_after==0) capture->expires_at=std::chrono::steady_clock::time_point::min();
    int cam_handle=1;
    struct Runtime {
        std::unordered_map<int,std::unique_ptr<Vision::VisionDiagnosticAdapter>> diagnostics;
        std::unordered_map<int,std::pair<std::string,bool>> incompatible_views;
        CameraVisionRenderMode mode=CameraVisionRenderMode::ReSTIR;
    } runtime;
    struct View { CameraVisionRenderMode vision_mode=CameraVisionRenderMode::ReSTIR; } view;
    std::optional<int> fallback_settings;
    auto has_pending_screenshot=[&](int) { return bool(capture); };
#define CFW_LOG_WARNING(...) ((void)0)
#define CFW_LOG_ERROR(...) ((void)0)
    COMPATIBILITY_CHECK
    Timings frame_timings, stage_timings;
    unsigned long long capture_qpc_frequency=1;
    auto capture_profile_qpc=[] { return 0u; };
    RENDER_LOOP
}
int main(int argc,char** argv) {
    Pipeline pipeline;
    execute(&pipeline,std::atoi(argv[1]),std::atoi(argv[2])!=0,std::atoi(argv[3])!=0,std::atoi(argv[4]),argc>5?argv[5]:"");
    bool after_first_current=true;
    for (size_t i=1;i<pipeline.observed_previous.size();++i)
        after_first_current &= pipeline.observed_previous[i].position.x==42;
    std::cout << pipeline.uploads << ' ' << pipeline.observed_previous.size() << ' '
              << failures << ' ' << after_first_current << '\n';
    if (argc>5) {
        std::cout << std::setprecision(std::numeric_limits<float>::max_digits10);
        auto write_pose=[](const Camera& pose) {
            std::cout << ',' << pose.position.x << ',' << pose.position.y << ',' << pose.position.z
                      << ',' << pose.forward.x << ',' << pose.forward.y << ',' << pose.forward.z
                      << ',' << pose.world_up.x << ',' << pose.world_up.y << ',' << pose.world_up.z;
        };
        for (size_t i=0;i<pipeline.observed_current.size();++i) {
            std::cout << "H," << i;
            write_pose(pipeline.observed_current[i]);write_pose(pipeline.observed_previous[i]);
            std::cout << '\n';
        }
        std::cout << "S," << pipeline.sync_calls << '\n';
    }
}
'''.replace("SYNC_PREFIX", prefix).replace("COMPATIBILITY_CHECK", compatibility).replace("RENDER_LOOP", loop)
    temporary_root = ROOT / ".workspace/tmp/gi-guide-reuse-20261119/offline-camera-history-test"
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
    (192, True, False, -1, 192, 0),
    (1, False, False, -1, 1, 0),
    (64, True, False, 0, 0, 1),
    (64, True, False, 1, 1, 1),
    (1, False, True, -1, 1, 0),
])
def test_each_render_observes_published_previous_camera(offline_history_harness, count, capturing,
                                                       diagnostic, expire_after, rendered, failed):
    result = subprocess.run([str(offline_history_harness), str(count), str(int(capturing)),
                             str(int(diagnostic)), str(expire_after)], capture_output=True, text=True,
                            env=dict(os.environ, VISION_GI_GUIDE_REUSE_CAMERA_SEQUENCE=""))
    assert result.returncode == 0, result.stderr
    uploads, actual_rendered, actual_failed, after_first_current = map(int, result.stdout.split())
    assert actual_rendered == rendered
    assert actual_failed == failed
    assert after_first_current == 1, "The second and subsequent frames must see the last rendered camera"
    assert uploads == rendered, "Publish once per rendered frame, after checking capture timeout"


def test_incompatible_render_fails_pending_capture_before_the_next_frame(offline_history_harness):
    result = subprocess.run([str(offline_history_harness), "64", "1", "0", "-1"],
                            capture_output=True, text=True, check=True,
                            env=dict(os.environ, VISION_GI_GUIDE_REUSE_CAMERA_SEQUENCE="",
                                     TEST_INCOMPATIBLE_AFTER_DISPLAY="1"))
    assert list(map(int, result.stdout.split()))[:3] == [1, 1, 1]


def test_scheduled_camera_sequence_keeps_previous_pose_and_frame_history(offline_history_harness):
    directory = offline_history_harness.parent / "scheduled-sequence"
    directory.mkdir()
    sequence = directory / "sequence.csv"
    source = (42., 0., 0., 0., 0., -1., 0., 1., 0.)
    events = {0: source}
    for frame in range(64, 80):
        events[frame] = (42. + .01 * (frame - 63), *source[1:])
    events[80] = (42.16, 0., 0., math.sin(math.radians(25)), 0., -math.cos(math.radians(25)), 0., 1., 0.)
    events[96] = (100000., 100000., 100000., *source[3:])
    events[112] = source
    with sequence.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("frame", "pos_x", "pos_y", "pos_z", "forward_x", "forward_y", "forward_z", "up_x", "up_y", "up_z"))
        writer.writerows((frame, *pose) for frame, pose in events.items())
    result = subprocess.run([str(offline_history_harness), "192", "1", "0", "-1", str(directory / "image.png")],
                            capture_output=True, text=True,
                            env=dict(os.environ, VISION_GI_GUIDE_REUSE_CAMERA_SEQUENCE=str(sequence)))
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert list(map(int, lines[0].split()))[:3] == [192, 192, 0]
    assert lines[-1] == "S,193", "Initial sync plus one pose-only sync per scheduled capture frame"
    expected, pose = [], source
    for frame in range(192):
        pose = events.get(frame, pose)
        expected.append(pose)
    observed = [line.split(",") for line in lines if line.startswith("H,")]
    assert len(observed) == 192
    for frame, row in enumerate(observed):
        assert int(row[1]) == frame
        assert list(map(float, row[2:11])) == pytest.approx(expected[frame], abs=1e-5)
        if frame:
            assert list(map(float, row[11:20])) == pytest.approx(expected[frame - 1], abs=1e-5), "Publish the last rendered pose before each scheduled frame"
    with (directory / "camera-sequence-audit.csv").open(newline="", encoding="utf-8") as stream:
        audit = list(csv.DictReader(stream))
    assert len(audit) == 192
    for frame, row in enumerate(audit):
        assert int(row["frame"]) == frame + 1 and int(row["capture_frame"]) == frame
        assert int(row["event"]) == int(frame in events)
        assert int(row["integrator_before_sync"]) == int(row["integrator_before"]) == frame
        assert int(row["integrator_after"]) == frame + 1
        assert int(row["external_projection_revision"]) == 17
        if frame:
            changed = expected[frame] != expected[frame - 1]
            assert (row["external_camera_revision"] != audit[frame - 1]["external_camera_revision"]) == changed
