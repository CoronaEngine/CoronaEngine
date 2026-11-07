"""Execute the production shadow-ray templates against CPU ray/plane fixtures.

The two templates, origin offset, and shadow epsilon are extracted unchanged
from their headers. Only vector/ray plumbing is supplied by the CPU harness;
no renderer, GPU, or engine build is needed.
"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[3]


def extract_function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for end in range(opening, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise AssertionError(f"Unclosed production function: {signature}")


@pytest.fixture(scope="module")
def shadow_ray_harness():
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("Standalone CPU shadow-ray regression requires g++")
    interaction = (ROOT / "vision/src/base/scattering/interaction.h").read_text(encoding="utf-8")
    rtx = (ROOT / "Horizon/src/dsl/types/rtx.h").read_text(encoding="utf-8")
    constants = (ROOT / "Horizon/src/math/constants.h").read_text(encoding="utf-8")
    functions = ["template<typename T>\n" + extract_function(interaction, signature)
                 for signature in (
                     "[[nodiscard]] ray_t<T> spawn_ray_to(T p_start, T n_start, T p_target)",
                     "[[nodiscard]] ray_t<T> spawn_ray_to(T p_start, T n_start, T p_target, T n_target)",
                 )]
    offset = extract_function(rtx, "inline float3 offset_ray_origin(")
    epsilon = re.search(r"constexpr float ShadowEpsilon\s*=\s*[^;]+;", constants).group()
    program = r'''
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
using std::bit_cast;
struct float3 { float x, y, z; };
struct int3 { int x, y, z; };
struct bool3 { bool x, y, z; };
float3 make_float3(float x,float y,float z) { return {x,y,z}; }
int3 make_int3(int x,int y,int z) { return {x,y,z}; }
float3 operator+(float3 a,float3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
float3 operator-(float3 a,float3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
float3 operator-(float3 a) { return {-a.x,-a.y,-a.z}; }
float3 operator*(float3 a,float s) { return {a.x*s,a.y*s,a.z*s}; }
float3 operator*(float s,float3 a) { return a*s; }
float3& operator*=(float3& a,float s) { a=a*s; return a; }
bool3 operator<(float3 a,float s) { return {a.x<s,a.y<s,a.z<s}; }
float3 abs(float3 a) { return {std::abs(a.x),std::abs(a.y),std::abs(a.z)}; }
float dot(float3 a,float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
template<typename T> T select(bool b,T yes,T no) { return b?yes:no; }
float3 select(bool3 b,float3 yes,float3 no) {
    return {b.x?yes.x:no.x,b.y?yes.y:no.y,b.z?yes.z:no.z};
}
struct Ray { float3 origin, direction; float tmax; };
template<typename T> using ray_t = Ray;
Ray make_ray(float3 origin,float3 direction,float tmax) { return {origin,direction,tmax}; }
EPSILON
OFFSET
FUNCTIONS
int main(int argc,char** argv) {
    int overload=std::atoi(argv[1]);
    float start=std::strtof(argv[2],nullptr), target=std::strtof(argv[3],nullptr);
    float sign=target>start?1.f:-1.f;
    float3 p={0,0,start}, q={0,0,target};
    // Off-axis origin displacement also checks that the ray still aims at q.
    float3 n={.6f,0,.8f*sign};
    Ray r=overload==3 ? spawn_ray_to(p,n,q) : spawn_ray_to(p,n,q,float3{0,0,-sign});
    float target_t=(target-r.origin.z)/r.direction.z;
    float blocker_t=((start+target)*.5f-r.origin.z)/r.direction.z;
    float3 endpoint=r.origin+r.direction;
    std::cout << std::setprecision(10) << target_t << ' ' << r.tmax << ' '
              << blocker_t << ' ' << endpoint.x << ' ' << endpoint.z;
}
'''.replace("EPSILON", epsilon).replace("OFFSET", offset).replace("FUNCTIONS", "\n".join(functions))
    temporary_root = ROOT / ".workspace/tmp/restir-blackspots/audit/shadow-ray-test"
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=temporary_root) as directory:
        directory = Path(directory)
        source, executable = directory / "harness.cpp", directory / "harness.exe"
        source.write_text(program, encoding="utf-8")
        built = subprocess.run([compiler, "-std=c++20", "-O0", str(source), "-o", str(executable)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        yield executable


@pytest.mark.parametrize("overload", [3, 4])
@pytest.mark.parametrize("start,target", [(0, .01), (1, 1.1), (10, 10.5),
                                          (0, -.01), (-1, -1.1), (-10, -10.5)])
def test_target_plane_does_not_self_occlude_but_intervening_plane_does(shadow_ray_harness, overload, start, target):
    run = subprocess.run([str(shadow_ray_harness), str(overload), str(start), str(target)],
                         capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    target_t, maximum_t, blocker_t, endpoint_x, endpoint_z = map(float, run.stdout.split())
    assert target_t > maximum_t, "The sampled target plane must not occlude its own shadow ray"
    assert 0 < blocker_t < maximum_t, "A true blocker between the surfaces must still occlude"
    assert endpoint_x == pytest.approx(0, abs=1e-9), "Origin offset must not displace the target"
    if overload == 3:
        assert endpoint_z == pytest.approx(target, abs=1e-6)
    else:
        assert min(start, target) < endpoint_z < max(start, target), "Target-normal offset must reach the ray"
