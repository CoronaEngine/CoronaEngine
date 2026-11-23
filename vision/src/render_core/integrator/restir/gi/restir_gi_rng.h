#pragma once
#include "base/using.h"

namespace vision::restir_gi {
namespace detail {
[[nodiscard]] inline Uint native_hash(const Uint &input) noexcept {
    Uint x = input;
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
[[nodiscard]] inline Uint native_combine(const Uint &seed, const Uint &value) noexcept {
    return seed ^ (native_hash(value) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u));
}
[[nodiscard]] inline Uint reverse_bits(const Uint &input) noexcept {
    Uint x = input;
    x = ((x & 0xaaaaaaaau) >> 1u) | ((x & 0x55555555u) << 1u);
    x = ((x & 0xccccccccu) >> 2u) | ((x & 0x33333333u) << 2u);
    x = ((x & 0xf0f0f0f0u) >> 4u) | ((x & 0x0f0f0f0fu) << 4u);
    x = ((x & 0xff00ff00u) >> 8u) | ((x & 0x00ff00ffu) << 8u);
    return (x >> 16u) | (x << 16u);
}
[[nodiscard]] inline Uint owen_scramble(const Uint &input, const Uint &seed) noexcept {
    Uint x = reverse_bits(input);
    x ^= x * 0x3d20adeau;
    x += seed;
    x *= (seed >> 16u) | 1u;
    x ^= x * 0x05526c56u;
    x ^= x * 0x53a22864u;
    return reverse_bits(x);
}
inline constexpr uint SobolDirections[5u][32u] = {
    {0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u, 0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u, 0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u, 0x00080000u, 0x00040000u, 0x00020000u, 0x00010000u, 0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u, 0x00000800u, 0x00000400u, 0x00000200u, 0x00000100u, 0x00000080u, 0x00000040u, 0x00000020u, 0x00000010u, 0x00000008u, 0x00000004u, 0x00000002u, 0x00000001u},
    {0x80000000u, 0xc0000000u, 0xa0000000u, 0xf0000000u, 0x88000000u, 0xcc000000u, 0xaa000000u, 0xff000000u, 0x80800000u, 0xc0c00000u, 0xa0a00000u, 0xf0f00000u, 0x88880000u, 0xcccc0000u, 0xaaaa0000u, 0xffff0000u, 0x80008000u, 0xc000c000u, 0xa000a000u, 0xf000f000u, 0x88008800u, 0xcc00cc00u, 0xaa00aa00u, 0xff00ff00u, 0x80808080u, 0xc0c0c0c0u, 0xa0a0a0a0u, 0xf0f0f0f0u, 0x88888888u, 0xccccccccu, 0xaaaaaaaau, 0xffffffffu},
    {0x80000000u, 0xc0000000u, 0x60000000u, 0x90000000u, 0xe8000000u, 0x5c000000u, 0x8e000000u, 0xc5000000u, 0x68800000u, 0x9cc00000u, 0xee600000u, 0x55900000u, 0x80680000u, 0xc09c0000u, 0x60ee0000u, 0x90550000u, 0xe8808000u, 0x5cc0c000u, 0x8e606000u, 0xc5909000u, 0x6868e800u, 0x9c9c5c00u, 0xeeee8e00u, 0x5555c500u, 0x8000e880u, 0xc0005cc0u, 0x60008e60u, 0x9000c590u, 0xe8006868u, 0x5c009c9cu, 0x8e00eeeeu, 0xc5005555u},
    {0x80000000u, 0xc0000000u, 0x20000000u, 0x50000000u, 0xf8000000u, 0x74000000u, 0xa2000000u, 0x93000000u, 0xd8800000u, 0x25400000u, 0x59e00000u, 0xe6d00000u, 0x78080000u, 0xb40c0000u, 0x82020000u, 0xc3050000u, 0x208f8000u, 0x51474000u, 0xfbea2000u, 0x75d93000u, 0xa0858800u, 0x914e5400u, 0xdbe79e00u, 0x25db6d00u, 0x58800080u, 0xe54000c0u, 0x79e00020u, 0xb6d00050u, 0x800800f8u, 0xc00c0074u, 0x200200a2u, 0x50050093u},
    {0x80000000u, 0x40000000u, 0x20000000u, 0xb0000000u, 0xf8000000u, 0xdc000000u, 0x7a000000u, 0x9d000000u, 0x5a800000u, 0x2fc00000u, 0xa1600000u, 0xf0b00000u, 0xda880000u, 0x6fc40000u, 0x81620000u, 0x40bb0000u, 0x22878000u, 0xb3c9c000u, 0xfb65a000u, 0xddb2d000u, 0x78022800u, 0x9c0b3c00u, 0x5a0fb600u, 0x2d0ddb00u, 0xa2878080u, 0xf3c9c040u, 0xdb65a020u, 0x6db2d0b0u, 0x800228f8u, 0x400b3cdcu, 0x200fb67au, 0xb00ddb9du},
};
[[nodiscard]] inline Uint sobol(const Uint &index, uint dimension) noexcept {
    Uint value = 0u;
    for (uint bit = 0u; bit < 32u; ++bit) value ^= ((index >> bit) & 1u) * SobolDirections[dimension][bit];
    return value;
}
[[nodiscard]] inline Uint vertex_hash(const Uint2 &pixel, const Uint &vertex) noexcept {
    return native_combine(native_hash(vertex + 0x035f9f29u), (pixel.x << 16u) | pixel.y);
}
[[nodiscard]] inline Float hash_float(const Uint &bits) noexcept { return cast<float>(bits >> 8u) / 16777216.f; }
[[nodiscard]] inline Float3 uniform3(const Uint &base, const Uint &frame, uint effect) noexcept {
    Uint state = native_combine(native_combine(base, Uint{effect}), frame);
    Float3 result = make_float3(0.f);
    for (uint i = 0u; i < 3u; ++i) { state = native_hash(state); result[i] = hash_float(state); }
    return result;
}
[[nodiscard]] inline Float3 low_discrepancy3(const Uint &base, const Uint &frame) noexcept {
    Uint state = native_combine(base, Uint{1u});
    Float3 result = make_float3(0.f);
    for (uint dimension = 0u; dimension < 3u; ++dimension) {
        Uint shuffle_seed = native_combine(state, Uint{0u});
        Uint dimension_seed = native_combine(state, Uint{1u + dimension});
        Uint shuffled = owen_scramble(frame, shuffle_seed);
        Uint value = dimension == 0u ? reverse_bits(shuffled) : sobol(shuffled, dimension);
        value = owen_scramble(value, dimension_seed);
        result[dimension] = hash_float(value);
    }
    return result;
}
}// namespace detail
[[nodiscard]] inline Float3 bsdf_random(const Uint2 &pixel, const Uint &vertex,
                                        const Uint &frame, const Uint &diffuse_bounces) noexcept {
    Uint base = detail::vertex_hash(pixel, vertex);
    Float3 result = make_float3(0.f);
    $if(diffuse_bounces < 1u) { result = detail::low_discrepancy3(base, frame); }
    $else { result = detail::uniform3(base, frame, 1u); };
    return result;
}
[[nodiscard]] inline Float4 nee_random(const Uint2 &pixel, const Uint &vertex, const Uint &frame) noexcept {
    Uint base = detail::vertex_hash(pixel, vertex);
    Float3 light = detail::uniform3(base, frame, 3u);
    Uint wrs = detail::native_hash(detail::native_combine(detail::native_combine(base, Uint{2u}), frame));
    return make_float4(light, detail::hash_float(wrs));
}
[[nodiscard]] inline Float roulette_random(const Uint2 &pixel, const Uint &vertex, const Uint &frame) noexcept {
    Uint base = detail::vertex_hash(pixel, vertex);
    Uint state = detail::native_combine(detail::native_combine(base, Uint{6u}), frame);
    return detail::hash_float(detail::native_hash(state));
}
[[nodiscard]] inline Uint resampling_seed(const Uint2 &pixel, const Uint2 &resolution,
                                           const Uint &frame, const Uint &pass) noexcept {
    (void)resolution;
    Uint base = detail::vertex_hash(pixel, pass);
    return detail::native_combine(detail::native_combine(base, Uint{0u}), frame);
}
[[nodiscard]] inline Float resampling_next(Uint &state) noexcept {
    state = detail::native_hash(state);
    return detail::hash_float(state);
}
[[nodiscard]] inline Float feedback_random(const Uint2 &pixel, const Uint &vertex, const Uint &frame) noexcept {
    Uint base = detail::vertex_hash(pixel, vertex);
    Uint state = detail::native_combine(detail::native_combine(base, Uint{5u}), frame);
    return resampling_next(state);
}
[[nodiscard]] inline Float bsdf_feedback_random(const Uint2 &pixel, const Uint &vertex,
                                                const Uint &frame, bool environment) noexcept {
    const uint salt = environment ? 0x0366fe2fu : 0x0367c2c7u;
    Uint value = detail::native_combine(detail::native_combine(detail::native_hash(vertex + salt),
        (pixel.x << 16u) | pixel.y), frame);
    return detail::hash_float(value);
}
}// namespace vision::restir_gi
