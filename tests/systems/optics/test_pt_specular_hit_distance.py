"""Run the production PT ``route_spec`` lambda with scalar CPU shims.

The renderer, GPU, and engine build are not needed.  The lambda is extracted
from ``integrator.cpp`` unchanged apart from spelling DSL scalar operations as
ordinary C++; the fixtures then exercise its observable hit-distance result.
"""

from functools import lru_cache
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = next(parent for parent in Path(__file__).resolve().parents
            if (parent / "vision/src").is_dir())
SOURCE = ROOT / "vision/src/base/integral/integrator.cpp"
TEMP = ROOT / ".workspace/tmp/roaming-five-20261108/pt-hit-distance-test"


def _extract_route_spec(source: str) -> str:
    start = source.index("auto route_spec =")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    assert source[end:end + 1] == ";", "route_spec lambda must end with a semicolon"
    route_spec = source[start:end + 1]
    route_spec = route_spec.replace("ocarina::select", "select")
    route_spec = route_spec.replace("$if", "if").replace("Bool(", "bool(")
    route_spec = re.sub(
        r"\(([^()]+)\)\.cast<float>\(\)",
        r"static_cast<float>(\1)",
        route_spec,
    )
    return route_spec


SHIM = r'''
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

using Uint = unsigned;
using Float = float;
struct Bool2 { bool x, y; };
struct Float2 {
    float x{}, y{};
    Float2& operator+=(Float2 b) { x += b.x; y += b.y; return *this; }
    Float2& operator*=(float b) { x *= b; y *= b; return *this; }
};
struct Float3 {
    float x{}, y{}, z{};
    Float3& operator+=(Float3 b) { x += b.x; y += b.y; z += b.z; return *this; }
};
Float2 make_float2(float v) { return {v, v}; }
Float2 make_float2(float x, float y) { return {x, y}; }
Float3 make_float3(float v) { return {v, v, v}; }
Float2 operator+(Float2 a, Float2 b) { return {a.x + b.x, a.y + b.y}; }
Float2 operator*(Float2 a, float b) { return {a.x * b, a.y * b}; }
Float2 operator/(Float2 a, Float2 b) { return {a.x / b.x, a.y / b.y}; }
Bool2 operator>(Float2 a, float b) { return {a.x > b, a.y > b}; }
Float3 operator-(Float3 a, Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 operator*(Float3 a, Float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
Float3 operator*(Float3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
Float2 max(Float2 a, Float2 b) { return {std::max(a.x, b.x), std::max(a.y, b.y)}; }
Float3 max(Float3 a, Float3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
float max(float a, float b) { return std::max(a, b); }
float luminance(Float3 a) { return .2126f * a.x + .7152f * a.y + .0722f * a.z; }
template<class T> T select(bool predicate, T yes, T no) { return predicate ? yes : no; }
Float2 select(Bool2 predicate, Float2 yes, Float2 no) {
    return {predicate.x ? yes.x : no.x, predicate.y ? yes.y : no.y};
}
struct HitContext {
    Float3 *direct_specular{};
    Float2 *lobe_hitdist{};
    bool restir_direct_split{};
    bool split() const { return true; }
};

float run(std::string fixture) {
    Float3 ret_specular = make_float3(0.f);
    Float3 spec_route = make_float3(0.f);
    Float3 ret_direct_specular = make_float3(0.f);
    Float2 distance_sum = make_float2(0.f);
    Float2 distance_weight = make_float2(0.f);
    Float2 sampled_distance = make_float2(0.f);
    Float first_distance = 0.f;
    Float2 hit_distance = make_float2(0.f);
    HitContext hc{};
    hc.lobe_hitdist = &hit_distance;
__ROUTE_SPEC__
    if (fixture == "direct-plus-indirect-specular") {
        spec_route = make_float3(1.f);
        route_spec(make_float3(100.f), 0u, make_float3(1.f), 100.f, false);
        first_distance = 2.f;
        sampled_distance.y = 2.f;
        route_spec(make_float3(1.f), 1u, make_float3(0.f), 0.f, true);
    } else if (fixture == "pure-direct-specular") {
        spec_route = make_float3(1.f);
        route_spec(make_float3(100.f), 0u, make_float3(1.f), 100.f, false);
        sampled_distance.y = 7.f;
    } else if (fixture == "diffuse-control") {
        route_spec(make_float3(100.f), 0u, make_float3(0.f), 100.f, false);
        first_distance = 2.f;
        sampled_distance.x = 2.f;
        route_spec(make_float3(1.f), 1u, make_float3(0.f), 0.f, true);
    } else if (fixture == "direct-specular-diffuse-sample") {
        route_spec(make_float3(100.f), 0u, make_float3(1.f), 100.f, false);
    } else {
        throw std::runtime_error("unknown fixture");
    }
    *hc.lobe_hitdist = select(distance_weight > 1e-5f,
        distance_sum / max(distance_weight, make_float2(1e-5f)), sampled_distance);
    return fixture == "diffuse-control" ? hit_distance.x : hit_distance.y;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    try {
        std::cout << std::setprecision(9) << run(argv[1]);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
'''


@lru_cache(maxsize=1)
def _binary():
    compiler = shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        raise unittest.SkipTest("PT hit-distance CPU regression requires g++ or clang++")
    route_spec = _extract_route_spec(SOURCE.read_text(encoding="utf-8-sig"))
    program = SHIM.replace("__ROUTE_SPEC__", route_spec)
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = tempfile.TemporaryDirectory(prefix="production-", dir=TEMP)
    path = Path(directory.name)
    cpp = path / "pt_specular_hit_distance.cpp"
    binary = path / "pt_specular_hit_distance.exe"
    cpp.write_text(program, encoding="utf-8")
    result = subprocess.run(
        [compiler, "-std=c++20", "-O0", str(cpp), "-o", str(binary)],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return directory, binary


def _run(fixture: str) -> float:
    _, binary = _binary()
    result = subprocess.run([str(binary), fixture], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return float(result.stdout)


class PtSpecularHitDistanceTest(unittest.TestCase):
    def test_direct_nee_does_not_bias_indirect_specular_hit_distance(self):
        self.assertAlmostEqual(_run("direct-plus-indirect-specular"), 2.0, places=6)

    def test_pure_direct_specular_uses_the_sampled_specular_guide(self):
        self.assertAlmostEqual(_run("pure-direct-specular"), 7.0, places=6)

    def test_direct_diffuse_distance_weighting_is_unchanged(self):
        expected = (100.0 * 100.0 + 1.0 * 2.0) / 101.0
        self.assertAlmostEqual(_run("diffuse-control"), expected, places=5)

    def test_diffuse_primary_sample_leaves_direct_specular_for_guide_repair(self):
        self.assertEqual(_run("direct-specular-diffuse-sample"), 0.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
