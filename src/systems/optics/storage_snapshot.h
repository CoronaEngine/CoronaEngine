#pragma once

#include <corona/kernel/entity_storage.h>

#include <optional>
#include <vector>

namespace Corona::Systems::OpticsDetail {

template <typename T, std::size_t BufferCapacity, std::size_t InitialBuffers>
[[nodiscard]] std::vector<T> snapshot_storage(
    const Corona::Kernel::Utils::EntityStorage<T, BufferCapacity, InitialBuffers>& storage) {
    std::vector<T> snapshot;
    snapshot.reserve(storage.count());
    for (const auto& value : storage) {
        snapshot.push_back(value);
    }
    return snapshot;
}

template <typename T, std::size_t BufferCapacity, std::size_t InitialBuffers>
[[nodiscard]] std::optional<T> snapshot_storage_value(
    Corona::Kernel::Utils::EntityStorage<T, BufferCapacity, InitialBuffers>& storage,
    typename Corona::Kernel::Utils::EntityStorage<T, BufferCapacity, InitialBuffers>::ObjectId id) {
    if (auto value = storage.try_acquire_read(id)) {
        return *value;
    }
    return std::nullopt;
}

}  // namespace Corona::Systems::OpticsDetail
