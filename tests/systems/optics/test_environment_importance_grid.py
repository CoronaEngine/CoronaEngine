"""CPU area/mass checks of the actual environment-importance grid reducer."""
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'vision/src').is_dir())
SOURCE = ROOT / 'vision/src/render_core/light/environments/spherical.cpp'
TEMP = ROOT / '.workspace/tmp/rtxpt-corona-dsl-20261108/environment-importance-tests'


def _helper():
    source = SOURCE.read_text(encoding='utf-8-sig')
    signature = 'vector<float> aggregate_environment_importance('
    assert signature in source, 'Production aggregate_environment_importance helper is absent'
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PROGRAM = r'''
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
using uint = std::uint32_t;
using std::vector;
using std::min;
using std::max;
struct uint2 { uint x, y; };
__PRODUCTION_HELPER__
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double value, double expected, double tolerance, const char *message) {
    if (!std::isfinite(value) || std::abs(value - expected) > tolerance) {
        std::cerr << message << ": actual=" << value << " expected=" << expected << '\n';
        throw std::runtime_error(message);
    }
}
double mass(const vector<float> &grid) {
    return std::accumulate(grid.begin(), grid.end(), 0.);
}
void check(const vector<float> &result, const vector<float> &expected) {
    require(result.size() == expected.size(), "coarse grid has incorrect size");
    for (size_t i = 0; i < result.size(); ++i)
        near(result[i], expected[i], 2e-6 * max(1.f, expected[i]), "unexpected bin mass");
}
void conservation_test() {
    const uint2 source{17u, 11u};
    vector<float> weights(source.x * source.y);
    uint random = 0x20261108u;
    const float scales[]{0.f, 1e-12f, 1e-4f, .7f, 10.f, 1e7f};
    for (size_t i = 0; i < weights.size(); ++i) {
        random = random * 1664525u + 1013904223u;
        weights[i] = scales[i % 6] * float((random >> 16u) + 1u) / 65536.f;
    }
    const double total = mass(weights);
    for (uint2 target : {uint2{1u, 1u}, uint2{7u, 5u}, uint2{8u, 3u}, uint2{13u, 8u}, source}) {
        const auto result = aggregate_environment_importance(weights, source, target);
        require(result.size() == size_t(target.x) * target.y, "coarse dimensions were not honored");
        for (float value : result) require(std::isfinite(value) && value >= 0.f, "positive input produced invalid mass");
        near(mass(result), total, total * 2e-7, "importance mass was lost or duplicated");
        if (target.x == source.x && target.y == source.y)
            for (size_t i = 0; i < result.size(); ++i)
                require(result[i] == weights[i], "identity reduction changed input weights");
    }
    std::cout << "PASS mass conservation, nonnegative HDR range and identity grid\n";
}
void uniform_test() {
    for (const auto dimensions : {std::pair{uint2{5u, 3u}, uint2{2u, 2u}},
                                  std::pair{uint2{7u, 11u}, uint2{3u, 4u}},
                                  std::pair{uint2{19u, 13u}, uint2{8u, 5u}}}) {
        const auto [source, target] = dimensions;
        for (float value : {0.f, .5f, 2.3f}) {
            const vector<float> weights(source.x * source.y, value);
            const auto result = aggregate_environment_importance(weights, source, target);
            const double expected = double(value) * source.x * source.y / (target.x * target.y);
            require(result.size() == size_t(target.x) * target.y, "uniform grid dimensions changed");
            for (float bin : result)
                near(bin, expected, max(1., expected) * 2e-7, "equal-area UV bins must receive equal mass");
        }
    }
    std::cout << "PASS uniform mass per coarse cell for three nondivisible 2D grids\n";
}
void impulse_test() {
    check(aggregate_environment_importance({0.f, 1.f, 0.f}, {3u, 1u}, {2u, 1u}), {.5f, .5f});
    check(aggregate_environment_importance({0.f, 1.f, 0.f}, {1u, 3u}, {1u, 2u}), {.5f, .5f});
    vector<float> square(9u); square[4] = 1.f;
    check(aggregate_environment_importance(square, {3u, 3u}, {2u, 2u}), {.25f, .25f, .25f, .25f});
    vector<float> rectangle(15u); rectangle[7] = 1.f;
    check(aggregate_environment_importance(rectangle, {5u, 3u}, {2u, 2u}), {.25f, .25f, .25f, .25f});
    rectangle.assign(15u, 0.f); rectangle[14] = 7.f;
    check(aggregate_environment_importance(rectangle, {5u, 3u}, {3u, 2u}), {0.f, 0.f, 0.f, 0.f, 0.f, 7.f});
    rectangle.assign(15u, 0.f); rectangle[0] = 9.f;
    check(aggregate_environment_importance(rectangle, {5u, 3u}, {3u, 2u}), {9.f, 0.f, 0.f, 0.f, 0.f, 0.f});
    std::cout << "PASS horizontal/vertical and four-way impulse overlap; corner support\n";
}
void edges_test() {
    // Last column: 3 units. Last row except its corner: 8 units. No cropped remainder.
    vector<float> edge(15u);
    edge[4] = edge[9] = edge[14] = 1.f;
    edge[10] = edge[11] = edge[12] = edge[13] = 2.f;
    check(aggregate_environment_importance(edge, {5u, 3u}, {2u, 2u}), {0.f, 1.5f, 5.f, 4.5f});
    auto one = aggregate_environment_importance(edge, {5u, 3u}, {1u, 1u});
    check(one, {11.f});
    vector<float> baseline(15u, 1.f), combined = baseline;
    for (size_t i = 0; i < edge.size(); ++i) combined[i] += edge[i];
    const auto sum = aggregate_environment_importance(combined, {5u, 3u}, {2u, 2u});
    const auto a = aggregate_environment_importance(baseline, {5u, 3u}, {2u, 2u});
    const auto b = aggregate_environment_importance(edge, {5u, 3u}, {2u, 2u});
    for (size_t i = 0; i < sum.size(); ++i)
        near(sum[i], double(a[i]) + b[i], 1e-6, "aggregation must be linear in pixel mass");
    std::cout << "PASS nondivisible trailing row/column, complete 1x1 integral and linearity\n";
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "one fixture name is required");
        const std::string name = argv[1];
        if (name == "mass") conservation_test();
        else if (name == "uniform") uniform_test();
        else if (name == "impulse") impulse_test();
        else if (name == "edges") edges_test();
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
        raise SkipTest('g++ is required for the standalone environment-importance fixture')
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='run-', dir=TEMP))
    source = directory / 'environment_importance.cpp'
    executable = directory / 'environment_importance.exe'
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


def test_positive_importance_mass_is_conserved_across_grid_shapes():
    _run('mass')


def test_uniform_fine_mass_produces_uniform_coarse_mass():
    _run('uniform')


def test_impulses_split_by_area_without_changing_support():
    _run('impulse')


def test_nondivisible_edges_are_retained_and_aggregation_is_linear():
    _run('edges')
