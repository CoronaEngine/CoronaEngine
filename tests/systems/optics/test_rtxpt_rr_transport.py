"""Production RTXPT roulette probability and analytical transport protocols.

The probability helper is extracted unchanged and compiled with CPU scalar DSL
shims. Transport fixtures enumerate Bernoulli outcomes rather than sampling:
current endpoint emission is unconditional, while downstream work receives the
survivor weight. The fixtures specify that protocol; they do not run the PT loop.
"""
from functools import lru_cache
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'vision/src').is_dir())
SOURCE = ROOT / 'vision/src/base/integral/integrator.cpp'
TEMP = ROOT / '.workspace/tmp/rtxpt-corona-dsl-20261108/rr-transport-tests'


def _helper():
    source = SOURCE.read_text(encoding='utf-8-sig')
    signature = 'Float rtxpt_rr_probability('
    assert signature in source, 'Production rtxpt_rr_probability helper is absent'
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PROGRAM = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
using Float = float;
using Uint = std::uint32_t;
using std::max;
using std::sqrt;
struct Float3 { float x, y, z; };
Float3 make_float3(float value) { return {value, value, value}; }
Float3 operator/(Float3 v, float d) { return {v.x / d, v.y / d, v.z / d}; }
// Match Horizon/src/math/base.h's RGB luminance coefficients.
float luminance(const Float3 &v) { return .212671f * v.x + .715160f * v.y + .072169f * v.z; }
float saturate(float value) { return std::clamp(value, 0.f, 1.f); }
template<class T, class U> T cast(U value) { return static_cast<T>(value); }
namespace ocarina { using ::luminance; using ::saturate; using ::cast; using ::max; using ::sqrt; }
__PRODUCTION_HELPER__
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char *message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        std::cerr << message << ": actual=" << actual << " expected=" << expected << '\n';
        throw std::runtime_error(message);
    }
}
void probability_test() {
    near(rtxpt_rr_probability(make_float3(0.f), 4u, 16u), .7225, 2e-7, "black early path probability");
    near(rtxpt_rr_probability(make_float3(.25f), 8u, 16u), .1225, 2e-7, "quarter-throughput analytic probability");
    near(rtxpt_rr_probability(make_float3(1.f), 8u, 16u), 0., 0., "bright path before tail ramp");
    near(rtxpt_rr_probability(make_float3(1.f), 12u, 16u), .3, 1e-7, "three-quarter depth tail ramp");
    near(rtxpt_rr_probability(make_float3(.25f), 12u, 16u), .4225, 2e-7, "brightness and tail terms combine");
    near(rtxpt_rr_probability(make_float3(0.f), 12u, 16u), 1., 0., "dark late path saturates at certain termination");
    near(rtxpt_rr_probability(make_float3(4.f), 16u, 16u), .6, 1e-7, "bright terminal depth probability helper");
    near(rtxpt_rr_probability(make_float3(-1.f), 4u, 16u), .7225, 2e-7, "negative luminance clamps before sqrt");
    const auto red = rtxpt_rr_probability({.25f / .212671f, 0.f, 0.f}, 8u, 16u);
    const auto green = rtxpt_rr_probability({0.f, .25f / .715160f, 0.f}, 8u, 16u);
    near(red, green, 2e-7, "equal-luminance colors must have equal roulette probability");
    near(red, .1225, 2e-7, "roulette must use luminance rather than maximum component");
    std::cout << "PASS analytic probability, luminance, depth ramp and clamp boundaries\n";
}
void range_test() {
    unsigned cases = 0;
    for (Uint total : {1u, 2u, 8u, 16u, 32u, 128u}) {
        for (float brightness : {0.f, 1e-12f, .001f, .025f, .25f, 1.f, 10000.f}) {
            float previous = -1.f;
            for (Uint depth = 1u; depth <= total; ++depth) {
                const auto q = rtxpt_rr_probability(make_float3(brightness), depth, total);
                require(std::isfinite(q) && q >= 0.f && q <= 1.f, "roulette probability left [0,1]");
                require(q >= previous, "fixed-throughput termination must not decrease with depth");
                previous = q;
                ++cases;
            }
        }
        for (Uint depth = 1u; depth <= total; ++depth) {
            float previous = 1.f;
            for (float brightness : {0.f, 1e-12f, .001f, .025f, .25f, 1.f, 10000.f}) {
                const auto q = rtxpt_rr_probability(make_float3(brightness), depth, total);
                require(q <= previous + 1e-7f, "brighter camera throughput must not terminate more often");
                previous = q;
            }
        }
    }
    std::cout << "PASS " << cases << " probability range/depth cases and brightness monotonicity\n";
}
struct Outcome { double radiance; bool downstream; };
Outcome endpoint_first(double q, double uniform, double endpoint, double continuation) {
    // Endpoints reached by the existing ray precede its RR decision. Only a
    // survivor executes NEE/scattering and supplies the next ray's weight.
    if (uniform < q || q == 1.) return {endpoint, false};
    return {endpoint + continuation / (1. - q), true};
}
void expectation_test() {
    unsigned fixtures = 0;
    for (float brightness : {0.f, .025f, .25f, 1.f}) {
        for (Uint depth : {4u, 8u, 9u, 11u}) {
            const double q = rtxpt_rr_probability(make_float3(brightness), depth, 16u);
            for (double endpoint : {0., .2, 3.}) for (double continuation : {0., .5, 7.}) {
                if (q == 1.) continue;
                const auto killed = endpoint_first(q, q * .5, endpoint, continuation);
                const auto alive = endpoint_first(q, (1. + q) * .5, endpoint, continuation);
                if (q > 0.) require(!killed.downstream && killed.radiance == endpoint, "terminated path lost/boosted endpoint or evaluated downstream");
                require(alive.downstream, "surviving path omitted downstream work");
                const double mean = q * killed.radiance + (1. - q) * alive.radiance;
                const double expected = endpoint + continuation;
                near(mean, expected, 2e-13 * max(1., expected), "endpoint-first RR changed transport expectation");
                const double variance = q * std::pow(killed.radiance - mean, 2.)
                    + (1. - q) * std::pow(alive.radiance - mean, 2.);
                const double theory = q * continuation * continuation / (1. - q);
                near(variance, theory, 3e-12 * max(1., theory), "endpoint-first RR variance is incorrect");
                const double immediate_variance = q * expected * expected / (1. - q);
                require(variance <= immediate_variance + 1e-11, "preserving endpoint introduced avoidable RR variance");
                if (q > 0. && endpoint > 0.) require(variance < immediate_variance, "endpoint variance must differ from immediate roulette");
                ++fixtures;
            }
        }
    }
    std::cout << "PASS " << fixtures << " exhaustive two-outcome means and variances versus immediate RR\n";
}
void boundaries_test() {
    const double zero = rtxpt_rr_probability(make_float3(1.f), 8u, 16u);
    const double one = rtxpt_rr_probability(make_float3(0.f), 12u, 16u);
    require(zero == 0. && one == 1., "fixtures must exercise exact probability endpoints");
    for (double uniform : {0., .25, .5, .999999999999}) {
        const auto a = endpoint_first(zero, uniform, 2., 5.);
        require(a.downstream && a.radiance == 7., "zero probability should preserve all work without compensation");
        const auto b = endpoint_first(one, uniform, 2., 5.);
        require(!b.downstream && b.radiance == 2., "certain termination must retain only the unboosted endpoint");
    }
    const double q = rtxpt_rr_probability(make_float3(.25f), 8u, 16u);
    require(!endpoint_first(q, std::nextafter(q, 0.), 2., 5.).downstream, "sample below threshold should terminate");
    require(endpoint_first(q, q, 2., 5.).downstream, "strict-less RR threshold equality should survive");
    // Four leaves cover two decisions. Reweighted throughput determines the
    // second probability; expectation remains unchanged at both vertices.
    const double q0 = rtxpt_rr_probability(make_float3(.1f), 6u, 16u);
    const double q1 = rtxpt_rr_probability(make_float3(.1f) / float(1. - q0), 10u, 16u);
    double mean = 0.;
    for (bool alive0 : {false, true}) for (bool alive1 : {false, true}) {
        const double probability = (alive0 ? 1. - q0 : q0) * (alive1 ? 1. - q1 : q1);
        double radiance = 2.;
        if (alive0) radiance += (3. + (alive1 ? 7. / (1. - q1) : 0.)) / (1. - q0);
        mean += probability * radiance;
    }
    near(mean, 12., 1e-12, "two-vertex endpoint-first transport lost expected radiance");
    std::cout << "PASS q0/q1, strict threshold, no downstream at q1 and two-decision expectation\n";
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "one fixture name is required");
        const std::string name = argv[1];
        if (name == "probability") probability_test();
        else if (name == "range") range_test();
        else if (name == "expectation") expectation_test();
        else if (name == "boundaries") boundaries_test();
        else throw std::runtime_error("unknown fixture");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
'''


@lru_cache(maxsize=1)
def _executable():
    helper = _helper()
    compiler = shutil.which('g++')
    if not compiler:
        raise SkipTest('g++ is required for the standalone RTXPT RR fixture')
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='run-', dir=TEMP))
    source = directory / 'rtxpt_rr_transport.cpp'
    executable = directory / 'rtxpt_rr_transport.exe'
    source.write_text(PROGRAM.replace('__PRODUCTION_HELPER__', helper), encoding='utf-8')
    result = subprocess.run(
        [compiler, '-std=c++20', '-O2', str(source), '-o', str(executable)],
        text=True, capture_output=True, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return executable


def _run(fixture):
    result = subprocess.run([str(_executable()), fixture], text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout, end='')


def test_production_probability_matches_analytic_luminance_and_depth_cases():
    _run('probability')


def test_probability_range_and_depth_brightness_monotonicity():
    _run('range')


def test_endpoint_first_roulette_preserves_expectation_and_changes_variance():
    _run('expectation')


def test_probability_endpoints_threshold_and_repeated_transport():
    _run('boundaries')


def test_native_transport_requires_rtxpt_backend_and_exact_explicit_opt_in():
    source = (ROOT / 'vision/src/render_core/integrator/ReSTIR/indirect.cpp').read_text(encoding='utf-8-sig')
    declarations = re.findall(r'const bool native_transport\s*=\s*[^;]+;', source)
    assert len(declarations) == 2, 'Candidate and compile-log transport decisions must both be tested'
    program = r'''
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
using std::string_view;
bool candidate_route(bool rtxpt_backend_, const char *transport_override) {
    __CANDIDATE_DECLARATION__
    return native_transport;
}
bool log_route(bool rtxpt_backend_, const char *transport_override) {
    __LOG_DECLARATION__
    return native_transport;
}
int main() {
    try {
        struct Case { const char *value; bool enabled; };
        const std::array<Case, 14> cases{{
            {nullptr, false}, {"", false}, {"0", false}, {"true", false},
            {"10", false}, {"01", false}, {"1", true}, {"1 ", false},
            {" 1", false}, {"TRUE", false}, {"false", false}, {"-1", false},
            {"1.0", false}, {"+1", false}}};
        unsigned checked = 0;
        for (bool backend : {false, true}) for (const auto test : cases) {
            const bool expected = backend ? test.enabled : false;
            if (candidate_route(backend, test.value) != expected)
                throw std::runtime_error("candidate transport opt-in accepted a non-exact value or wrong backend");
            if (log_route(backend, test.value) != expected)
                throw std::runtime_error("compile-log transport mode disagrees with the exact opt-in contract");
            ++checked;
        }
        std::cout << "PASS " << checked << " exact opt-in/backend cases through both production transport declarations\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
'''
    program = program.replace('__CANDIDATE_DECLARATION__', declarations[0])
    program = program.replace('__LOG_DECLARATION__', declarations[1])
    compiler = shutil.which('g++')
    if not compiler:
        raise SkipTest('g++ is required for the standalone RTXPT transport opt-in fixture')
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='opt-in-', dir=TEMP))
    cpp, binary = directory / 'transport_opt_in.cpp', directory / 'transport_opt_in.exe'
    cpp.write_text(program, encoding='utf-8')
    result = subprocess.run([compiler, '-std=c++20', '-O2', str(cpp), '-o', str(binary)],
                            text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout, end='')
