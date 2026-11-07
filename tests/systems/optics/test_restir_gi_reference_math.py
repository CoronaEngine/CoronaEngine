"""Execute the production scalar core used by the GPU reference GI port.

These fixtures catch cosine/PDF double counting, discarded null-stream counts,
incorrect square-root Jacobians, and reuse after a reset. No GPU is required.
"""
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[3]


def test_reference_reservoir_math():
    output = ROOT / '.workspace/tmp/restirgi-port-20261107/cpu-math'
    output.mkdir(parents=True, exist_ok=True)
    source = output / 'math.cpp'
    binary = output / 'math.exe'
    source.write_text(r'''
#include "restir_gi/core.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace vision::restir_gi;
void eq(float actual, float expected, const char *label) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>1e-5f)
        throw std::runtime_error(label);
}
int main() {
    using M = Math<HostOps>;
    eq(M::interpolate(960.f, 970.f, .5f), 965.f, "screen lerp uses start/end/fraction order");
    eq(M::interpolate(.5f, .1f, .5f), .3f, "inverse-depth interpolation");
    eq(M::spatial_radius(0u, 0.f, 1u), 64.f, "full pass0 initial radius");
    eq(M::spatial_radius(0u, 15.f, 1u), 54.f, "full pass0 intermediate radius");
    eq(M::spatial_radius(0u, 20.f, 1u), 44.f, "full pass0 mature radius");
    eq(M::spatial_radius(1u, 0.f, 1u), 32.f, "full pass1 initial radius");
    eq(M::spatial_radius(1u, 160.f, 1u), 22.f, "full pass1 mature radius");
    eq(M::spatial_radius(0u, 0.f, 2u), 32.f, "half pass0 initial radius");
    eq(M::spatial_radius(1u, 160.f, 2u), 11.f, "half pass1 mature radius");
    // A zero-energy stream still contributes to the normalization.
    eq(M::finish_weight(6.f, 3.f, 2.f), 1.f, "null stream count lost");
    eq(M::finish_weight(0.f, 3.f, 0.f), 0.f, "zero stream is not finite");
    eq(M::finish_weight(1000.f, 1.f, 1.f), 10.f, "firefly weight clamp lost");
    eq(M::temporal_count(40.f, 0.f, .5f), 10.f, "history clamp/relevance");
    eq(M::temporal_count(40.f, 1.f, .5f), .5f, "invalid history clamp");
    eq(M::temporal_count(40.f, .25f, 1.f), 4.47213595f, "raw invalidity square root");
    eq(M::spatial_jacobian(4.f, 2.f, .5f, .125f), 1.f, "distance/cosine Jacobian");
    eq(M::spatial_jacobian(4.f, 2.f, .5f, .5f), 2.f, "spatial sqrt Jacobian");
    eq(M::spatial_jacobian(4.f, 2.f, .5f, -.5f), 0.f, "backside Jacobian");
    eq(M::validation_m_scale(1.f, 100.f), .03f, "bright refresh M clamp");
    eq(M::validation_w_scale(1.f, 100.f), .1f, "bright refresh W clamp");
    eq(M::inverse_depth_relative_difference(2.f, 1.f), .5f, "linear to inverse depth ratio");
    eq(M::inverse_depth_relative_difference(1.f, 2.f), 1.f, "inverse depth ratio is asymmetric");
    // Midpoint integration of uniform-hemisphere constant L must give albedo*L.
    float integral=0.f;
    for (int i=0;i<1000;++i) integral += .4f*3.f*M::hemisphere_factor((i+.5f)/1000.f)/1000.f;
    eq(integral, 1.2f, "uniform hemisphere pi convention");
    if (sampling_extent(1919,2)!=960 || sampling_extent(1081,2)!=541 || sampling_extent(17,1)!=17)
        throw std::runtime_error("odd extent loses border cells");
    if (consecutive_history(0xffffffffu,0) || consecutive_history(7,0) || consecutive_history(7,9) || !consecutive_history(7,8))
        throw std::runtime_error("history survived reset/discontinuity");
    std::cout << "PASS reference GI production math\n";
}
''', encoding='utf-8')
    compiler = shutil.which('g++') or shutil.which('clang++')
    assert compiler, 'A C++ compiler is required for the CPU reference math oracle'
    build = subprocess.run([compiler, '-std=c++20', '-O0', '-I'+str(ROOT/'vision/src/render_core/integrator'),
                            str(source), '-o', str(binary)], capture_output=True, text=True)
    assert build.returncode == 0, build.stdout + build.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.startswith('PASS reference GI production math')
    print(result.stdout, end='')


if __name__ == '__main__':
    test_reference_reservoir_math()
