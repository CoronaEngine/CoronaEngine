#include "storage_snapshot.h"

#include <cassert>

namespace {
struct Value {
    int number = 0;
};
}  // namespace

int main() {
    using Storage = Corona::Kernel::Utils::Storage<Value, 2, 1>;
    Storage storage;
    const auto id = storage.allocate();
    {
        auto value = storage.try_acquire_write_nowait(id);
        assert(value);
        value->number = 7;
    }

    const auto snapshot =
        Corona::Systems::OpticsDetail::snapshot_storage<Value, 2, 1>(storage);
    assert(snapshot.size() == 1);
    assert(snapshot.front().number == 7);

    // The copied value must not keep the storage slot locked.
    {
        auto value = storage.try_acquire_write_nowait(id);
        assert(value);
        value->number = 11;
    }
    assert(snapshot.front().number == 7);

    const auto value_snapshot =
        Corona::Systems::OpticsDetail::snapshot_storage_value<Value, 2, 1>(storage, id);
    assert(value_snapshot);
    assert(value_snapshot->number == 11);
    storage.deallocate(id);
    return 0;
}
