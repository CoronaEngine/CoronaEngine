#pragma once

#include "reservoir.h"

namespace vision {
// Only these fields survive a dispatch. Streaming weights and the initial
// candidate's visibility endpoint remain in the full logical reservoir.
struct DIReservoirStorage {
    static constexpr const char *Name = "ReSTIRDI::reservoirs_ di32-v1 x 2";
    array<uint, 8> words{0u, 0u, 0u, InvalidUI32, 0u, 0u, 0u, 0u};
};
static_assert(sizeof(DIReservoirStorage) == 32u);
}// namespace vision

OC_STRUCT(vision, DIReservoirStorage, words) {};

namespace vision {
[[nodiscard]] inline bool di_reservoir_storage_matches(const ocarina::string &name,
                                                       const ocarina::ByteBufferDesc &history,
                                                       const ocarina::ByteBufferDesc &scratch,
                                                       size_t count) noexcept {
    const size_t bytes = count * sizeof(DIReservoirStorage);
    return name == DIReservoirStorage::Name && history.head() != 0u &&
           history.head() == scratch.head() && history.offset == 0u &&
           history.size == bytes && scratch.offset == bytes && scratch.size == bytes;
}

class DIReservoirStorageView {
private:
    ocarina::BindlessArrayBuffer<DIReservoirStorage> buffer_;

public:
    explicit DIReservoirStorageView(ocarina::BindlessArrayBuffer<DIReservoirStorage> buffer)
        : buffer_(ocarina::move(buffer)) {}

    [[nodiscard]] DIReservoirVar read(const Uint &index) const noexcept {
        DIReservoirStorageVar stored = buffer_.read(index);
        DIReservoirVar value;
        value.W = ocarina::as<float>(stored.words[0]);
        value.C = ocarina::as<float>(stored.words[1]);
        value.sample.p_hat = ocarina::as<float>(stored.words[2]);
        value.sample.light_index = stored.words[3];
        value.sample.prim_id = stored.words[4];
        value.sample.bary = make_float2(ocarina::as<float>(stored.words[5]), ocarina::as<float>(stored.words[6]));
        value.sample.age = stored.words[7];
        return value;
    }

    void write(const Uint &index, const DIReservoirVar &value) noexcept {
        DIReservoirStorageVar stored;
        stored.words[0] = ocarina::as<uint>(value.W);
        stored.words[1] = ocarina::as<uint>(value.C);
        stored.words[2] = ocarina::as<uint>(value.sample.p_hat);
        stored.words[3] = value.sample.light_index;
        stored.words[4] = value.sample.prim_id;
        stored.words[5] = ocarina::as<uint>(value.sample.bary.x);
        stored.words[6] = ocarina::as<uint>(value.sample.bary.y);
        stored.words[7] = value.sample.age;
        buffer_.write(index, stored);
    }
};
}// namespace vision
