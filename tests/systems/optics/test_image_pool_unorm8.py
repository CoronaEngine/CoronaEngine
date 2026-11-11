"""CPU checks of ImagePool's actual lossless UNORM8 admission helper.

Only the helper is extracted and compiled; no engine build, image loader or GPU
is involved. Fixtures cover the input precision and color-space boundaries that
decide whether a decoded FLOAT4 image can retain its values in BYTE4 storage.
"""
from functools import lru_cache
from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'vision/src').is_dir())
SOURCE = ROOT / 'vision/src/base/mgr/image_pool.cpp'
TEMP = ROOT / '.workspace/tmp/rtxpt-corona-dsl-20261108/image-pool-unorm8-tests'


def _helper():
    source = SOURCE.read_text(encoding='utf-8-sig')
    signature = 'bool pack_lossless_unorm8('
    assert signature in source, 'Production pack_lossless_unorm8 helper is absent'
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PROGRAM = r'''
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
using std::size_t;
using std::uint8_t;
__PRODUCTION_HELPER__
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void rgba_test() {
    // Odd affine permutations put every byte code in every RGBA channel.
    std::vector<float> rgba;
    std::vector<uint8_t> expected;
    for (unsigned pixel = 0; pixel < 256; ++pixel) {
        for (unsigned code : {pixel, 255u - pixel, (73u * pixel) & 255u,
                              (137u * pixel + 19u) & 255u}) {
            rgba.push_back(float(code) / 255.f);
            expected.push_back(uint8_t(code));
        }
    }
    require(pack_lossless_unorm8(rgba.data(), rgba.size()), "UNORM8 scan rejected an exact RGBA image");
    std::vector<uint8_t> packed(rgba.size() + 2, 0xa5u);
    require(pack_lossless_unorm8(rgba.data(), rgba.size(), packed.data() + 1), "UNORM8 pack rejected exact RGBA");
    require(packed.front() == 0xa5u && packed.back() == 0xa5u, "pack wrote outside the requested byte range");
    for (size_t i = 0; i < rgba.size(); ++i) {
        require(packed[i + 1] == expected[i], "an RGBA channel packed the wrong code");
        float decoded = float(packed[i + 1]) / 255.f;
        require(std::bit_cast<uint32_t>(decoded) == std::bit_cast<uint32_t>(rgba[i]),
                "UNORM8 decode changed a source float bit");
    }
    require(pack_lossless_unorm8(nullptr, 0), "empty source should require no reads");
    std::cout << "PASS all 256 codes in every RGBA channel, exact decode and write bounds\n";
}
void srgb_test() {
    unsigned rejected = 0;
    for (unsigned code = 0; code < 256; ++code) {
        const float encoded = float(code) / 255.f;
        const float linear = encoded <= .04045f ? encoded / 12.92f
            : std::pow((encoded + .055f) / 1.055f, 2.4f);
        const std::array<float, 4> rgba{linear, linear, linear, 1.f};
        std::array<uint8_t, 4> packed{};
        const bool scan = pack_lossless_unorm8(rgba.data(), rgba.size());
        const bool result = pack_lossless_unorm8(rgba.data(), rgba.size(), packed.data());
        require(result == scan, "scan and pack modes disagreed on sRGB-decoded data");
        if (code == 0 || code == 255) {
            require(result && packed[0] == code && packed[3] == 255, "sRGB endpoints are exactly representable");
        } else {
            require(!result, "non-grid sRGB-decoded interior must keep float/half fallback");
            ++rejected;
        }
    }
    require(rejected == 254, "sRGB fixture did not cover every interior byte code");
    std::cout << "PASS 254 sRGB-decoded interiors fall back; endpoints remain exact\n";
}
void unorm16_test() {
    unsigned accepted = 0;
    for (unsigned code = 0; code < 65536; ++code) {
        const float value = float(code) / 65535.f;
        uint8_t packed = 0;
        const bool scan = pack_lossless_unorm8(&value, 1);
        const bool result = pack_lossless_unorm8(&value, 1, &packed);
        require(result == scan, "scan and pack modes disagreed on UNORM16 input");
        // 65535 = 257 * 255: only these original 16-bit codes lie on the 8-bit grid.
        require(result == (code % 257u == 0u), "UNORM16 precision was silently discarded");
        if (result) {
            require(packed == code / 257u, "exact UNORM16 multiple packed incorrectly");
            require(float(packed) / 255.f == value, "accepted UNORM16 value changed on decode");
            ++accepted;
        }
    }
    require(accepted == 256, "UNORM16 grid intersection must contain exactly 256 values");
    std::cout << "PASS all 65536 UNORM16 values; exactly the 256 multiples of 257 pass\n";
}
void invalid_test() {
    const float infinity = std::numeric_limits<float>::infinity();
    const std::array<float, 9> invalid{
        -1.f, -std::numeric_limits<float>::min(), std::nextafter(1.f, infinity),
        2.f, std::numeric_limits<float>::max(), infinity, -infinity,
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::signaling_NaN()
    };
    for (float value : invalid) {
        for (size_t channel = 0; channel < 4; ++channel) {
            std::array<float, 4> rgba{0.f, 64.f / 255.f, 128.f / 255.f, 1.f};
            rgba[channel] = value;
            std::array<uint8_t, 4> packed{};
            require(!pack_lossless_unorm8(rgba.data(), rgba.size()), "invalid/HDR channel passed admission scan");
            require(!pack_lossless_unorm8(rgba.data(), rgba.size(), packed.data()), "invalid/HDR channel was packed");
        }
    }
    for (unsigned code = 0; code < 256; ++code) {
        const float exact = float(code) / 255.f;
        for (float direction : {-infinity, infinity}) {
            const float neighbor = std::nextafter(exact, direction);
            require(!pack_lossless_unorm8(&neighbor, 1), "one-ULP non-grid value must fall back");
        }
    }
    // Admission promises numeric equality; the signed zero distinction is not retained.
    const float negative_zero = -0.f;
    uint8_t zero = 255;
    require(pack_lossless_unorm8(&negative_zero, 1, &zero) && zero == 0, "numeric negative zero should pack as zero");
    std::cout << "PASS invalid/HDR values in each channel, one-ULP grid neighbors and signed zero\n";
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "one fixture name is required");
        const std::string name = argv[1];
        if (name == "rgba") rgba_test();
        else if (name == "srgb") srgb_test();
        else if (name == "unorm16") unorm16_test();
        else if (name == "invalid") invalid_test();
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
        raise SkipTest('g++ is required for the standalone ImagePool CPU fixture')
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='run-', dir=TEMP))
    source = directory / 'image_pool_unorm8.cpp'
    executable = directory / 'image_pool_unorm8.exe'
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


def test_all_unorm8_codes_round_trip_exactly_in_every_rgba_channel():
    _run('rgba')


def test_srgb_decoding_requires_fallback_for_nonrepresentable_interiors():
    _run('srgb')


def test_unorm16_admits_only_exact_multiples_of_257():
    _run('unorm16')


def test_invalid_hdr_and_one_ulp_neighbors_require_fallback():
    _run('invalid')
