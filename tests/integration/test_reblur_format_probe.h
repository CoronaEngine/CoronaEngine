#pragma once

// Compare native and EDSL typed-image stores, plus the explicit half encoding
// and decoding used by PackData2/UnpackData2. Inputs are raw float32 values.
#include "render_core/denoiser/ReBLUR/edsl_nrd_resources.h"
#include "render_core/denoiser/ReBLUR/edsl_nrd_common.h"
#include "render_core/denoiser/ReBLUR/native_nrd.h"
#include "rhi/context.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace vision::reblur::edsl {
namespace format_probe_detail {
using Bytes = std::vector<uint8_t>;

inline float positive_half_value(uint bits) {
    const uint exponent = bits >> 10u, mantissa = bits & 1023u;
    return exponent == 0u ? std::ldexp(float(mantissa), -24)
                          : std::ldexp(float(1024u + mantissa), int(exponent) - 25);
}

inline std::vector<float4> make_samples() {
    std::vector<float4> samples;
    samples.reserve(12000u);
    auto add = [&](float value) { samples.push_back(make_float4(value)); };
    auto neighborhood = [&](float value) {
        add(std::nextafter(value, -std::numeric_limits<float>::infinity()));
        add(value);
        add(std::nextafter(value, std::numeric_limits<float>::infinity()));
    };
    // Cover every UNORM10 and UNORM8 transition, plus all alpha2 transitions.
    // The rounded float32 midpoint and both adjacent float32 values are used.
    for (uint denominator : {1023u, 255u, 3u})
        for (uint code = 0u; code != denominator; ++code)
            neighborhood(float((double(code) + .5) / double(denominator)));
    for (float value : {.3318897784f, .5303149223f, .7287402153f}) add(value);

    // Every positive/negative binary16 subnormal boundary, including zero to
    // the smallest subnormal and the largest subnormal to the smallest normal.
    for (uint code = 0u; code != 1024u; ++code) {
        const float midpoint = (positive_half_value(code) + positive_half_value(code + 1u)) * .5f;
        neighborhood(midpoint); neighborhood(-midpoint);
    }
    // Normal half binades, mantissa carry boundaries and the overflow midpoint.
    constexpr std::array<uint, 7> mantissas{0u, 1u, 255u, 511u, 512u, 1022u, 1023u};
    for (uint exponent = 1u; exponent != 31u; ++exponent) for (uint mantissa : mantissas) {
        const uint code = (exponent << 10u) | mantissa;
        if (code == 0x7bffu) continue;
        const float midpoint = (positive_half_value(code) + positive_half_value(code + 1u)) * .5f;
        neighborhood(midpoint); neighborhood(-midpoint);
    }
    neighborhood(65520.f); neighborhood(-65520.f);
    for (float value : {0.f, -0.f, 1.f, -1.f, .6f, -1.2f,
                        65504.f, -65504.f, 65536.f, -65536.f, 1e10f, -1e10f,
                        std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(),
                        std::numeric_limits<float>::min(), -std::numeric_limits<float>::min(),
                        std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()}) add(value);
    // Include mixed channels so component order and alpha cannot pass by
    // accidentally duplicating one channel across a vector store.
    samples.push_back(make_float4(.3318897784f, .5303149223f, .7287402153f, 1.f));
    samples.push_back(make_float4(-0.f, .6f, -1.2f, .375f));
    if (samples.size() > 16000u) throw std::runtime_error("REBLUR format probe exceeds 16000 samples");
    return samples;
}

inline std::filesystem::path path_with_suffix(const std::filesystem::path &prefix, const std::string &suffix) {
    auto path = prefix; path += suffix; return path;
}

inline Bytes read_bytes(const std::filesystem::path &path, size_t expected_size) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != std::streampos(expected_size))
        throw std::runtime_error("REBLUR format probe missing/wrong-size native file: " + path.string());
    Bytes bytes(expected_size);
    input.seekg(0);
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("REBLUR format probe cannot read " + path.string());
    return bytes;
}

inline void write_bytes(const std::filesystem::path &path, const Bytes &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("REBLUR format probe cannot write " + path.string());
}
}

inline void check_reblur_format_contract(Device &device, Stream &stream,
                                         const std::filesystem::path &probe_spirv,
                                         const std::filesystem::path &output_prefix) {
    using namespace format_probe_detail;
    const auto samples = make_samples();
    const auto count = static_cast<uint>(samples.size());
    const uint2 extent = make_uint2(count, 1u);
    std::vector<uint> encoded_samples(count);
    for (uint i = 0u; i != count; ++i) {
        uint bits = (i * 65521u) & 65535u;
        if ((bits & 0x7c00u) == 0x7c00u && (bits & 1023u) != 0u)
            bits = (bits & 0x8000u) | 0x7c00u;
        encoded_samples[i] = bits;
    }
    NativeReblur reference(device.context()->runtime_directory() / "reblur-native");
    auto source = device.create_buffer<float4>(count, "REBLUR format contract source float32 bits");
    auto encoded_source = device.create_texture2d(extent, PixelStorage::UINT1);
    std::array<Texture2D, 6> textures{
        device.create_texture2d(extent, PixelStorage::RGB10A2),
        device.create_texture2d(extent, PixelStorage::HALF4),
        device.create_texture2d(extent, PixelStorage::HALF1),
        device.create_texture2d(extent, PixelStorage::UNORM1),
        device.create_texture2d(extent, PixelStorage::UINT1),
        device.create_texture2d(extent, PixelStorage::FLOAT1)};
    constexpr std::array<const char *, 6> names{"rgb10a2", "rgba16f", "r16f", "r8", "f32tof16", "f16tof32"};
    constexpr std::array<size_t, 6> pixel_bytes{4u, 8u, 2u, 1u, 4u, 4u};
    Dispatch parameters{};
    parameters.external.raw_diffuse = source.descriptor();
    parameters.inputs[0] = ImageDesc(encoded_source, extent);
    std::array<Bytes, 6> actual;
    for (uint index = 0u; index != textures.size(); ++index) {
        parameters.outputs[index] = ImageDesc(textures[index], extent);
        actual[index].resize(samples.size() * pixel_bytes[index]);
    }
    Kernel probe = [](Var<Dispatch> p) {
        configure_block(64u);
        Uint i = dispatch_id();
        $if(i < p.outputs.t0.size.x) {
            Float4 value = p.external.raw_diffuse.read(i);
            Int2 pixel = make_int2(i, 0u);
            Image<float4, Format::RGB10A2Unorm> rgb10a2(p.outputs.t0);
            Image<float4, Format::RGBA16Float> rgba16f(p.outputs.t1);
            Image<float, Format::R16Float> r16f(p.outputs.t2);
            Image<float, Format::R8Unorm> r8(p.outputs.t3);
            Image<uint, Format::R32Uint> f32tof16(p.outputs.t4);
            Image<float, Format::R32Float> f16tof32(p.outputs.t5);
            Image<uint, Format::R32Uint> encoded(p.inputs.t0);
            rgb10a2.write(pixel, value);
            rgba16f.write(pixel, value);
            r16f.write(pixel, Float(value.x));
            r8.write(pixel, Float(value.x));
            // Exercise the exact production PackData2/UnpackData2 path. The
            // native oracle above uses NRD's f32tof16/f16tof32 intrinsics.
            f32tof16.write(pixel, PackData2(0.f, value.x, 0.f, Bool{false}) >> 16u);
            Uint bits = encoded.read(pixel) & 65535u;
            Uint flags; Bool catrom;
            Float2 decoded = UnpackData2(bits << 16u, flags, catrom);
            f16tof32.write(pixel, Float(decoded.y));
        };
    };
    auto shader = device.compile(probe, "REBLUR-EDSL-format-store-contract");
    stream << source.upload(samples.data())
           << encoded_source.upload(encoded_samples.data())
           << reference.format_store_probe(samples, probe_spirv, output_prefix)
           << shader(parameters).dispatch(count);
    for (uint index = 0u; index != textures.size(); ++index) stream << textures[index].download(actual[index].data());
    stream << synchronize() << commit();

    // The native callback writes files after its own event wait. Stream
    // synchronization above also completes all EDSL texture readbacks before
    // any CPU file access or comparison begins.
    const auto native_input = read_bytes(path_with_suffix(output_prefix, "-inputs.bin"), samples.size() * sizeof(float4));
    if (std::memcmp(native_input.data(), samples.data(), native_input.size()) != 0)
        throw std::runtime_error("REBLUR native/EDSL format probe source float32 bits differ");
    const auto native_encoded = read_bytes(path_with_suffix(output_prefix, "-decode-inputs.bin"), encoded_samples.size() * sizeof(uint));
    if (std::memcmp(native_encoded.data(), encoded_samples.data(), native_encoded.size()) != 0)
        throw std::runtime_error("REBLUR native/EDSL half decode source bits differ");
    size_t total_mismatch = 0u;
    std::cout << std::setprecision(9);
    for (uint index = 0u; index != textures.size(); ++index) {
        const auto expected = read_bytes(path_with_suffix(output_prefix, std::string("-") + names[index] + ".bin"), actual[index].size());
        write_bytes(path_with_suffix(output_prefix, std::string("-edsl-") + names[index] + ".bin"), actual[index]);
        size_t mismatch = 0u, first = actual[index].size();
        for (size_t byte = 0u; byte != actual[index].size(); ++byte) {
            if (actual[index][byte] == expected[byte]) continue;
            if (mismatch++ == 0u) first = byte;
        }
        total_mismatch += mismatch;
        std::cout << "FORMAT " << names[index] << " samples=" << count << " bytes=" << actual[index].size()
                  << " mismatched_bytes=" << mismatch;
        if (mismatch) {
            const size_t sample = first / pixel_bytes[index];
            uint component = index == 1u ? uint(first % pixel_bytes[index]) / 2u : 0u;
            if (index == 0u) {
                uint32_t native{}, edsl{};
                std::memcpy(&native, expected.data() + sample * 4u, 4u);
                std::memcpy(&edsl, actual[index].data() + sample * 4u, 4u);
                const uint32_t difference = native ^ edsl;
                component = (difference & 1023u) ? 0u : (difference & (1023u << 10u)) ? 1u :
                            (difference & (1023u << 20u)) ? 2u : 3u;
            }
            std::cout << " first_byte=" << first << " first_sample=" << sample << " component=" << component
                      << " native_byte=" << unsigned(expected[first]) << " edsl_byte=" << unsigned(actual[index][first])
                      << " input=(" << samples[sample].x << ',' << samples[sample].y << ',' << samples[sample].z << ',' << samples[sample].w << ')';
        }
        std::cout << '\n';
    }
    if (total_mismatch) throw std::runtime_error("REBLUR native/EDSL format store bytes differ: " + std::to_string(total_mismatch));
    std::cout << "PASS: native/EDSL RGB10A2, RGBA16F, R16F, R8 stores and explicit f32tof16/f16tof32 conversions are byte-identical across " << count << " samples\n";
}
}
