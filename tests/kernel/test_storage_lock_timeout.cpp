// Compiled with CFW_ENABLE_LOCK_TIMEOUT=1 (see tests/kernel/CMakeLists.txt).
//
// Horizon's Storage reports a slot lock that stays held longer than
// CFW_LOCK_TIMEOUT_MS with a CRITICAL log plus a captured stack and then throws,
// instead of waiting forever. That branch is compiled out of the default build
// because it pulls in <dbghelp.h> and costs a timeout on every acquisition - which
// is exactly why a stuck engine lock looks like an idle system in production logs.
// This test pins the contract of the diagnostics build so the switch stays usable:
// a blocked acquisition fails loudly and promptly, a free one is unaffected, and
// the non-blocking accessors keep working either way.
#include <horizon/core/storage.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>

#ifndef CFW_ENABLE_LOCK_TIMEOUT
#error "this test must be compiled with CFW_ENABLE_LOCK_TIMEOUT=1"
#endif
#if !CFW_ENABLE_LOCK_TIMEOUT
#error "this test must be compiled with CFW_ENABLE_LOCK_TIMEOUT=1"
#endif

namespace {

struct Value {
    int number = 0;
};

using Storage = Corona::Kernel::Utils::Storage<Value, 2, 1>;

}  // namespace

int main() {
    Storage storage;
    const auto busy_slot = storage.allocate();
    const auto free_slot = storage.allocate();
    assert(busy_slot != 0 && free_slot != 0 && busy_slot != free_slot);

    std::atomic<bool> holder_ready{false};
    std::atomic<bool> release_holder{false};
    std::thread holder([&] {
        auto lock = storage.acquire_write(busy_slot);
        assert(lock.valid());
        holder_ready.store(true, std::memory_order_release);
        while (!release_holder.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    while (!holder_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // A blocked acquisition must fail loudly instead of waiting forever. It must
    // also fail *within* a bounded time: the whole point of the switch is to turn a
    // silent hang into a report.
    const auto started = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        auto blocked = storage.acquire_write(busy_slot);
        (void)blocked;
    } catch (const std::runtime_error&) {
        threw = true;
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    assert(threw);
    assert(elapsed_ms >= CFW_LOCK_TIMEOUT_MS / 2);
    assert(elapsed_ms < CFW_LOCK_TIMEOUT_MS * 4 + 5000);

    release_holder.store(true, std::memory_order_release);
    holder.join();

    // Free slots are unaffected, and the non-blocking accessors never wait (they
    // are the escape hatch for latency-critical threads).
    assert(storage.acquire_write(free_slot).valid());
    assert(storage.acquire_read(free_slot).valid());
    auto nowait = storage.try_acquire_read_nowait(free_slot);
    assert(nowait.valid());

    return 0;
}
