#pragma once

#include "dsl/dsl.h"
#include "base/using.h"
#include "math/half.h"

namespace vision {
// Quantize only at storage boundaries. Consumers continue computing in FP32.
[[nodiscard]] inline Uint pack_storage_half2(const Float2 &value) noexcept {
    return ocarina::as<uint>(ocarina::make_half2(value));
}
[[nodiscard]] inline Float2 unpack_storage_half2(const Uint &value) noexcept {
    return ocarina::make_float2(ocarina::as<ocarina::half2>(value));
}
[[nodiscard]] inline Uint2 pack_storage_half4(const Float4 &value) noexcept {
    return ocarina::make_uint2(pack_storage_half2(value.xy()), pack_storage_half2(value.zw()));
}
[[nodiscard]] inline Float4 unpack_storage_half4(const Uint2 &value) noexcept {
    return ocarina::make_float4(unpack_storage_half2(value.x), unpack_storage_half2(value.y));
}
[[nodiscard]] inline uint pack_storage_half2_host(float2 value) noexcept {
    return uint(ocarina::float_to_half(value.x)) | (uint(ocarina::float_to_half(value.y)) << 16u);
}
[[nodiscard]] inline float2 unpack_storage_half2_host(uint value) noexcept {
    return ocarina::make_float2(ocarina::half_to_float(uint16_t(value)),
                              ocarina::half_to_float(uint16_t(value >> 16u)));
}
[[nodiscard]] inline uint2 pack_storage_half4_host(float4 value) noexcept {
    return ocarina::make_uint2(pack_storage_half2_host(value.xy()), pack_storage_half2_host(value.zw()));
}
[[nodiscard]] inline float4 unpack_storage_half4_host(uint2 value) noexcept {
    return ocarina::make_float4(unpack_storage_half2_host(value.x), unpack_storage_half2_host(value.y));
}
}// namespace vision
