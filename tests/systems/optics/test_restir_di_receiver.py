"""Run the production ReSTIR DI receiver gates and tiled history addressing on the CPU."""
from _restir_cpu import ROOT, body, compile_run


def test_restir_di_receiver_history():
    base = ROOT / "vision/src/render_core/integrator/restir/di"
    common = (base / "common.h").read_text(encoding="utf-8")
    reuse = (base / "reuse.h").read_text(encoding="utf-8")
    reservoir = (base / "reservoir.h").read_text(encoding="utf-8")
    functions = [body(src, token) for src, token in [
        (common, "inline Bool valid_surface("),
        (reuse, "inline Bool reuse_similar("),
        (reuse, "inline Bool same_shadow_receiver("),
        (reservoir, "inline Uint reservoir_index("),
    ]]
    program = r"""
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

using Uint = unsigned;
using uint = unsigned;
using Bool = bool;
using std::abs;
using std::max;
using float3 = std::array<float, 3>;
using uint3 = std::array<uint, 3>;
using Float = float;
struct uint2 { uint x, y; };
using Uint2 = uint2;
Uint2 operator/(Uint2 v, uint d) { return {v.x / d, v.y / d}; }
Uint2 operator%(Uint2 v, uint d) { return {v.x % d, v.y % d}; }
struct Surface { float3 position{}, normal{0, 0, 1}, geometric_normal{0, 0, 1}; float depth{10}; };
float dot(const float3 &a, const float3 &b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
template<class T> auto as(const float3 &v) { return std::bit_cast<T>(v); }
bool all(bool value) { return value; }
void check(bool x, const char *msg) { if (!x) throw std::runtime_error(msg); }
__FUNCTIONS__
int main() {
    Surface receiver, history;
    check(reuse_similar(receiver, history), "same receiver must reuse history");
    for (float angle : {34.f, 36.f}) {
        const float radians = angle * 3.14159265f / 180.f;
        history.normal = {std::sin(radians), 0.f, std::cos(radians)};
        check(reuse_similar(receiver, history) == (angle < 35.f), "receiver normal gate");
    }
    history = receiver;
    for (float depth : {-1.f, 8.99f, 9.f, 11.f, 11.01f}) {
        history.depth = depth;
        check(reuse_similar(receiver, history) == (depth >= 9.f && depth <= 11.f), "receiver depth gate");
    }
    history = receiver;
    check(same_shadow_receiver(receiver, history), "identical shadow endpoints");
    history.position[0] = .1f;
    check(!same_shadow_receiver(receiver, history), "moving receiver must refresh visibility");
    history = receiver;
    history.geometric_normal[0] = .1f;
    check(!same_shadow_receiver(receiver, history), "changed geometric normal must refresh visibility");
    for (uint2 extent : {uint2{1,1}, {16,16}, {17,31}, {33,17}}) {
        const uint slice_size = ((extent.x + 15) / 16) * ((extent.y + 15) / 16) * 256;
        std::vector<bool> seen(slice_size * 3, false);
        for (uint slice = 0; slice < 3; ++slice) {
            for (uint y = 0; y < extent.y; ++y) for (uint x = 0; x < extent.x; ++x) {
                const uint index = reservoir_index(extent, {x,y}, slice);
                check(index >= slice * slice_size && index < (slice+1) * slice_size && !seen[index],
                      "history pixel or slice alias");
                seen[index] = true;
            }
        }
    }
    std::cout << "PASS ReSTIR DI receiver rejection, exact visibility reuse and three tiled slices";
}
"""
    compile_run(program.replace("__FUNCTIONS__", "\n".join(head + block for head, block in functions)),
                "restir-di-receiver")
