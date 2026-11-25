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
[[nodiscard]] inline Uint vertex_hash(const Uint2 &pixel, const Uint &vertex) noexcept {
    return native_combine(native_hash(vertex + 0x035f9f29u), (pixel.x << 16u) | pixel.y);
}
[[nodiscard]] inline Float hash_float(const Uint &bits) noexcept { return cast<float>(bits >> 8u) / 16777216.f; }
}// namespace detail
[[nodiscard]] inline Uint resampling_seed(const Uint2 &pixel, const Uint &frame, const Uint &pass) noexcept {
    Uint base = detail::vertex_hash(pixel, pass);
    return detail::native_combine(detail::native_combine(base, Uint{0u}), frame);
}
[[nodiscard]] inline Float resampling_next(Uint &state) noexcept {
    state = detail::native_hash(state);
    return detail::hash_float(state);
}
}// namespace vision::restir_gi
