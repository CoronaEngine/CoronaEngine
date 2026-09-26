#pragma once

#include <horizon/core/storage.h>

#include <vector>

namespace Corona::Systems::GeometryInternal {

template <typename Storage>
[[nodiscard]] std::vector<typename Storage::ObjectId>
snapshot_storage_handles(const Storage& storage) {
    using ObjectId = typename Storage::ObjectId;
    std::vector<ObjectId> handles;
    handles.reserve(storage.count());
    for (auto it = storage.cbegin(); it != storage.cend(); ++it) {
        handles.push_back(reinterpret_cast<ObjectId>(&*it));
    }
    return handles;
}

}  // namespace Corona::Systems::GeometryInternal
