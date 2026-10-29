#pragma once

#include "di_radiance_storage.h"

namespace vision {
// GI raw output has the same signed HDR range and 0/1 alpha contract as DI.
[[nodiscard]] inline Uint2 pack_gi_radiance(const Float4 &value) noexcept {
    return pack_di_radiance(value);
}
[[nodiscard]] inline Float4 unpack_gi_radiance(const Uint2 &value) noexcept {
    return unpack_di_radiance(value);
}
[[nodiscard]] inline float4 unpack_gi_radiance_host(uint2 value) noexcept {
    return unpack_di_radiance_host(value);
}
}// namespace vision
