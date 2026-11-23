"""Execute the shared PT segment-selection code with a cached GI camera hit."""
from _restir_cpu import ROOT, compile_run


def test_gi_reuses_only_the_physical_camera_segment():
    source = (ROOT / 'vision/src/base/integral/integrator.cpp').read_text(encoding='utf-8-sig')
    init_start = source.index('optional<Bool> camera_hit_pending;')
    init_end = source.index('auto mis_bsdf =', init_start)
    query_start = source.index('auto trace_segment =', init_end)
    query_end = source.index('if (hc.path_capture && hc.path_capture->on_signal', query_start)
    fill = (ROOT / 'vision/src/render_core/integrator/ReSTIR/restir_gi.cpp').read_text(encoding='utf-8-sig')
    fill = fill[fill.index('PTPathCapture capture;'):]
    assert 'capture.reuse_camera_hit = true;' in fill[:fill.index('integrator()->Li(')]
    assert 'frame_buffer().rays().read(dispatch_id())->to_ray_state()' in fill[:fill.index('integrator()->Li(')]
    program = r'''
#include <cassert>
#include <iostream>
#include <optional>
using std::optional;
using Bool = bool;
#define $if if
#define $else else

struct Capture { bool reuse_camera_hit; };
struct Counters { struct { unsigned x{}; } trace; };
struct Context { Capture* path_capture; Counters* work_counters; int medium_events{}; };
struct RayState { int ray; };
struct Geometry {
    int closest{}, media{};
    int trace_closest(int ray) { ++closest; return 100 + ray; }
    int trace_transport(int, int, int, RayState rs, int&, int&, int) { ++media; return 200 + rs.ray; }
};
struct FrameBuffer {
    int frame{}, pixel{}, hit{}, reads{};
    FrameBuffer& cur_visibility_buffer_var(int f) { assert(frame == f); return *this; }
    int read(int p) { assert(pixel == p); ++reads; return hit; }
};
struct RenderEnv { int frame; int frame_index() const { return frame; } };

void check_path(Capture* capture, bool use_media, int stored_hit, bool terminal_first, int frame) {
    Counters counts{};
    Context hc{capture, &counts};
    Geometry geometry;
    RenderEnv render_env{frame};
    FrameBuffer fb{frame, 37, stored_hit};
    auto frame_buffer = [&]() -> FrameBuffer& { return fb; };
    auto dispatch_id = [] { return 37; };
    auto scene = [] { return 0; };
    RayState rs{};
    int hit{}, sampler{}, swl{}, it{}, throughput{};
    __PENDING_STATE__
    auto segment = [&](int ray, unsigned bounces, bool inner) {
        rs.ray = ray;
        __SELECT_OR_TRACE__
        return hit;
    };
    const bool reuse = capture && capture->reuse_camera_hit && !use_media;
    const int traced = use_media ? 200 : 100;
    int queries = 0;
    if (terminal_first) {
        // Even with a pending hit, the direct-light supplement traces its own ray.
        assert(segment(9, 0, false) == traced + 9);
        ++queries;
    }
    assert(segment(1, 0, true) == (reuse ? stored_hit : traced + 1));
    queries += !reuse;
    if (stored_hit >= 0) {
        // A material-free boundary restarts bounce zero with a new origin.
        assert(segment(2, 0, true) == traced + 2);
        assert(segment(3, 1, true) == traced + 3);
        assert(segment(4, 2, false) == traced + 4);
        queries += 3;
    }
    assert(fb.reads == int(reuse));
    assert(geometry.closest == (use_media ? 0 : queries));
    assert(geometry.media == (use_media ? queries : 0));
    assert(counts.trace.x == unsigned(queries));
}
int main() {
    Capture enabled{true}, disabled{false};
    for (int frame : {0, 1, 8, 191}) {
        check_path(&enabled, false, -1, false, frame);
        check_path(&enabled, false, 70, false, frame);
        check_path(&enabled, false, 70, true, frame);
        check_path(&enabled, true, 70, false, frame);
        check_path(&disabled, false, 70, false, frame);
        check_path(nullptr, false, 70, true, frame);
    }
    std::cout << "PASS GI first query, miss, boundary restart, secondary/terminal rays, PT and media\n";
}
'''
    compile_run(program.replace('__PENDING_STATE__', source[init_start:init_end])
                .replace('__SELECT_OR_TRACE__', source[query_start:query_end]), 'gi-camera-hit')
