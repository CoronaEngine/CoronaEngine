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
    const auto first = storage.allocate();
    const auto second = storage.allocate();

    const auto handles =
        Corona::Systems::GeometryInternal::snapshot_storage_handles(storage);
    assert(handles.size() == 2);

    // A snapshot must release the iterator's read lock before returning.
    {
        auto value = storage.try_acquire_write_nowait(first);
        assert(value);
        value->number = 7;
    }
    {
        auto value = storage.try_acquire_write_nowait(second);
        assert(value);
        value->number = 11;
    }

    storage.deallocate(first);
    storage.deallocate(second);
    return 0;
}
