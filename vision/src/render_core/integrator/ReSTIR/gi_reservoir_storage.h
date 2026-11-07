#pragma once

#include "reservoir.h"
#include "math/base.h"
#include <bit>

namespace vision {
// Keep all estimator scalars, position, radiance and age bit-exact. Only the
// geometric unit normal is quantized, using two signed octahedral coordinates.
struct GIReservoirStorage {
    // Target-domain changes must reject older history even at the same byte ABI.
    static constexpr const char *Name = "ReSTIRGI::reservoirs_ gi44-oct16-pdf-target-v2 x 2";
    static constexpr uint InvalidNormal = 0x80008000u;
    array<uint, 11> words{0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, InvalidNormal, 0u};
};
static_assert(sizeof(GIReservoirStorage) == 44u);
}// namespace vision

OC_STRUCT(vision, GIReservoirStorage, words) {};

namespace vision {
[[nodiscard]] inline Uint pack_gi_normal(const Float3 &normal) noexcept {
    Uint packed = GIReservoirStorage::InvalidNormal;
    Float denominator = ocarina::abs(normal.x) + ocarina::abs(normal.y) + ocarina::abs(normal.z);
    $if(denominator > 0.f && !ocarina::isnan(denominator) && !ocarina::isinf(denominator)) {
        Float x = normal.x / denominator, y = normal.y / denominator;
        $if(normal.z < 0.f) {
            Float old_x = x;
            x = (1.f - ocarina::abs(y)) * ocarina::select(old_x >= 0.f, 1.f, -1.f);
            y = (1.f - ocarina::abs(old_x)) * ocarina::select(y >= 0.f, 1.f, -1.f);
        };
        // -32768 is never emitted; its pair is reserved for the zero normal.
        Int sx = cast<int>(ocarina::floor(ocarina::clamp(x, -1.f, 1.f) * 32767.f + 0.5f));
        Int sy = cast<int>(ocarina::floor(ocarina::clamp(y, -1.f, 1.f) * 32767.f + 0.5f));
        packed = (cast<uint>(sx) & 0xffffu) | ((cast<uint>(sy) & 0xffffu) << 16u);
    };
    return packed;
}

[[nodiscard]] inline Float3 unpack_gi_normal(const Uint &packed) noexcept {
    Float3 normal = ocarina::make_float3(0.f);
    $if(packed != GIReservoirStorage::InvalidNormal) {
        Int sx = cast<int>(packed & 0xffffu), sy = cast<int>(packed >> 16u);
        sx = ocarina::select(sx >= 32768, sx - 65536, sx);
        sy = ocarina::select(sy >= 32768, sy - 65536, sy);
        Float x = cast<float>(sx) / 32767.f, y = cast<float>(sy) / 32767.f;
        Float z = 1.f - ocarina::abs(x) - ocarina::abs(y);
        $if(z < 0.f) {
            Float old_x = x;
            x = (1.f - ocarina::abs(y)) * ocarina::select(old_x >= 0.f, 1.f, -1.f);
            y = (1.f - ocarina::abs(old_x)) * ocarina::select(y >= 0.f, 1.f, -1.f);
        };
        Float inverse_length = ocarina::rsqrt(x * x + y * y + z * z);
        normal = ocarina::make_float3(x * inverse_length, y * inverse_length, z * inverse_length);
    };
    return normal;
}

[[nodiscard]] inline float3 unpack_gi_normal_host(uint packed) noexcept {
    if (packed == GIReservoirStorage::InvalidNormal) return ocarina::make_float3(0.f);
    int x = int(packed & 0xffffu), y = int(packed >> 16u);
    x -= x >= 32768 ? 65536 : 0;
    y -= y >= 32768 ? 65536 : 0;
    return ocarina::octahedral_decode(ocarina::make_float2(float(x) / 32767.f, float(y) / 32767.f));
}

[[nodiscard]] inline GIReservoir unpack_gi_reservoir_host(const GIReservoirStorage &stored) noexcept {
    GIReservoir value;
    value.W = std::bit_cast<float>(stored.words[0]);
    value.C = std::bit_cast<float>(stored.words[1]);
    value.weight_sum = std::bit_cast<float>(stored.words[2]);
    for (uint i = 0u; i < 3u; ++i) {
        value.sample.sp.pos[i] = std::bit_cast<float>(stored.words[3u + i]);
        value.sample.Lo[i] = std::bit_cast<float>(stored.words[6u + i]);
    }
    value.sample.sp.set_normal(unpack_gi_normal_host(stored.words[9]));
    value.sample.age = std::bit_cast<float>(stored.words[10]);
    return value;
}

[[nodiscard]] inline bool gi_reservoir_storage_matches(const ocarina::string &name,
                                                       const ocarina::ByteBufferDesc &history,
                                                       const ocarina::ByteBufferDesc &scratch,
                                                       size_t count) noexcept {
    const size_t bytes = count * sizeof(GIReservoirStorage);
    return name == GIReservoirStorage::Name && history.head() != 0u &&
           history.head() == scratch.head() && history.offset == 0u &&
           history.size == bytes && scratch.offset == bytes && scratch.size == bytes;
}

class GIReservoirStorageView {
private:
    ocarina::BindlessArrayBuffer<GIReservoirStorage> buffer_;

public:
    explicit GIReservoirStorageView(ocarina::BindlessArrayBuffer<GIReservoirStorage> buffer)
        : buffer_(ocarina::move(buffer)) {}

    [[nodiscard]] GIReservoirVar read(const Uint &index) const noexcept {
        GIReservoirStorageVar stored = buffer_.read(index);
        GIReservoirVar value;
        value.W = ocarina::as<float>(stored.words[0]);
        value.C = ocarina::as<float>(stored.words[1]);
        value.weight_sum = ocarina::as<float>(stored.words[2]);
        for (uint i = 0u; i < 3u; ++i) {
            value.sample.sp.pos[i] = ocarina::as<float>(stored.words[3u + i]);
            value.sample.Lo[i] = ocarina::as<float>(stored.words[6u + i]);
        }
        value.sample.sp->set_normal(unpack_gi_normal(stored.words[9]));
        value.sample.age = ocarina::as<float>(stored.words[10]);
        return value;
    }

    void write(const Uint &index, const GIReservoirVar &value) noexcept {
        GIReservoirStorageVar stored;
        stored.words[0] = ocarina::as<uint>(value.W);
        stored.words[1] = ocarina::as<uint>(value.C);
        stored.words[2] = ocarina::as<uint>(value.weight_sum);
        for (uint i = 0u; i < 3u; ++i) {
            stored.words[3u + i] = ocarina::as<uint>(value.sample.sp.pos[i]);
            stored.words[6u + i] = ocarina::as<uint>(value.sample.Lo[i]);
        }
        stored.words[9] = pack_gi_normal(value.sample.sp->normal());
        stored.words[10] = ocarina::as<uint>(value.sample.age);
        buffer_.write(index, stored);
    }
};
}// namespace vision
