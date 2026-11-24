"""Exercise the shared PT/ReSTIR GI roulette step with a deterministic sampler."""
from _restir_cpu import ROOT, body, compile_run


def test_shared_immediate_roulette():
    source = (ROOT / 'vision/src/base/integral/integrator.cpp').read_text(encoding='utf-8-sig')
    declaration, block = body(source, '$if(eta_scale * rr_lum <')
    roulette = (declaration + block).replace('$if', 'if').replace('$break', 'break')
    program = r"""
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
using Float = float;
using std::min;
struct Sampler {
    float sample;
    unsigned draws{};
    float next_1d() { ++draws; return sample; }
};
struct Counters { struct { unsigned x{}, y{}; } rr; };
struct Capture { bool record_radiance, active; float beta{2.f}; };
struct HitContext { Counters *work_counters; Capture *path_capture; };
struct Result { bool survived; float throughput, beta; unsigned draws, attempts, deaths; };

Result step(float rr_lum, float sample, unsigned rr_depth = 3, float eta_scale = 1.f,
            bool record = true, bool active = true) {
    float threshold = 1.f;
    unsigned min_depth = 3;
    auto rr_threshold_ = &threshold;
    auto min_depth_ = &min_depth;
    Sampler rng{sample};
    auto sampler = &rng;
    Counters counters;
    Capture capture{record, active};
    HitContext hc{&counters, &capture};
    float throughput = 1.f;
    bool survived = false;
    do {
        __ROULETTE__
        survived = true;
    } while (false);
    return {survived, throughput, capture.beta, rng.draws, counters.rr.x, counters.rr.y};
}
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    for (auto result : {step(.25f, 1.f, 2), step(.5f, 1.f, 3, 2.f)}) {
        check(result.survived && result.draws == 0 && result.attempts == 0 && result.deaths == 0 &&
              result.throughput == 1.f && result.beta == 2.f, "inactive roulette must consume no random number");
    }
    const auto died = step(.25f, .3f);
    check(!died.survived && died.draws == 1 && died.attempts == 1 && died.deaths == 1 &&
          died.throughput == 1.f && died.beta == 2.f, "a losing draw must terminate before continuation");
    const auto alive = step(.25f, .25f);
    check(alive.survived && alive.draws == 1 && alive.attempts == 1 && alive.deaths == 0 &&
          alive.throughput == 4.f && alive.beta == 8.f, "equality must survive and reweight both throughputs");
    check(step(.25f, 0.f, 3, 1.f, false).beta == 2.f &&
          step(.25f, 0.f, 3, 1.f, true, false).beta == 2.f,
          "inactive or disabled path capture must retain its weight");
    const auto capped = step(1.f, .95f, 3, .5f);
    check(capped.survived && std::abs(capped.throughput * .95f - 1.f) < 1e-6f &&
          !step(1.f, .96f, 3, .5f).survived, "survival probability must be capped at 0.95");
    std::cout << "PASS shared immediate roulette thresholds, random draws, and transport compensation\n";
}
"""
    compile_run(program.replace('__ROULETTE__', roulette), 'gi-rr-transport')
