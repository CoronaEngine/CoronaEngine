#pragma once

#include "dsl/dsl.h"
#include "base/using.h"
#include <bit>

namespace vision {
// Raw output only: three signed floats with their original 8-bit exponent and
// 12 mantissa bits, plus the producer's 0/1 alpha. Denoisers retain full float4.
[[nodiscard]] inline Uint encode_di_radiance_channel(const Float &value) noexcept {
    Uint bits = ocarina::as<uint>(value);
    Uint magnitude = bits & 0x7fffffffu;
    Uint rounded = min((magnitude + 0x3ffu + ((magnitude >> 11u) & 1u)) >> 11u, 0xfefffu);
    Uint special = (magnitude >> 11u) | select(magnitude > 0x7f800000u, 0x800u, 0u);
    return ((bits >> 11u) & 0x100000u) | select(magnitude >= 0x7f800000u, special, rounded);
}

[[nodiscard]] inline Uint2 pack_di_radiance(const Float4 &value) noexcept {
    Uint r = encode_di_radiance_channel(value.x);
    Uint g = encode_di_radiance_channel(value.y);
    Uint b = encode_di_radiance_channel(value.z);
    return make_uint2(r | (g << 21u), (g >> 11u) | (b << 10u) | (cast<uint>(value.w != 0.f) << 31u));
}

[[nodiscard]] inline Float4 unpack_di_radiance(const Uint2 &words) noexcept {
    Uint r = words.x & 0x1fffffu;
    Uint g = (words.x >> 21u) | ((words.y & 0x3ffu) << 11u);
    Uint b = (words.y >> 10u) & 0x1fffffu;
    return make_float4(ocarina::as<float>(r << 11u), ocarina::as<float>(g << 11u),
                       ocarina::as<float>(b << 11u), cast<float>(words.y >> 31u));
}

[[nodiscard]] inline float4 unpack_di_radiance_host(uint2 words) noexcept {
    const uint r = words.x & 0x1fffffu;
    const uint g = (words.x >> 21u) | ((words.y & 0x3ffu) << 11u);
    const uint b = (words.y >> 10u) & 0x1fffffu;
    return make_float4(std::bit_cast<float>(r << 11u), std::bit_cast<float>(g << 11u),
                       std::bit_cast<float>(b << 11u), float(words.y >> 31u));
}
}// namespace vision
