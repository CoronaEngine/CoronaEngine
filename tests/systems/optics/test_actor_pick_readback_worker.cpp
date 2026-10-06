#include <corona/systems/optics/actor_pick_readback_worker.h>
#include "actor_pick_result.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <vector>

using namespace std::chrono_literals;

namespace {

void expect(bool condition) {
    if (!condition) {
        std::abort();
    }
}

}  // namespace

int main() {
    using Corona::Systems::OpticsDetail::ActorPickReadbackWorker;
    using Corona::Systems::OpticsDetail::resolve_actor_pick_result;

    {
        const std::vector<std::uintptr_t> actors{0x10, 0x20, 0x30};
        const auto success = resolve_actor_pick_result(true, 2, actors);
        expect(success.status == "success");
        expect(success.actor_handle == 0x20);

        const auto miss = resolve_actor_pick_result(true, 0, actors);
        expect(miss.status == "miss");
        expect(miss.actor_handle == 0);

        const auto stale_instance = resolve_actor_pick_result(true, 9, actors);
        expect(stale_instance.status == "miss");
        expect(stale_instance.actor_handle == 0);

        const auto error = resolve_actor_pick_result(false, 2, actors);
        expect(error.status == "error");
        expect(error.actor_handle == 0);
    }

    {
        ActorPickReadbackWorker worker(64);
        worker.start();

        std::promise<void> release_work;
        auto release_future = release_work.get_future().share();
        std::atomic<bool> work_started{false};

        auto reservation = worker.reserve();
        expect(reservation.has_value());
        const auto submit_start = std::chrono::steady_clock::now();
        expect(worker.submit(std::move(*reservation), [&] {
            work_started.store(true, std::memory_order_release);
            release_future.wait();
        }));
        const auto submit_elapsed = std::chrono::steady_clock::now() - submit_start;
        expect(submit_elapsed < 100ms);

        for (int i = 0; i < 100 && !work_started.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(1ms);
        }
        expect(work_started.load(std::memory_order_acquire));
        release_work.set_value();
        worker.shutdown();
        expect(worker.pending_count() == 0);
    }

    {
        ActorPickReadbackWorker worker(64);
        worker.start();
        std::vector<ActorPickReadbackWorker::Reservation> reservations;
        reservations.reserve(64);
        for (std::size_t i = 0; i < 64; ++i) {
            auto reservation = worker.reserve();
            expect(reservation.has_value());
            reservations.push_back(std::move(*reservation));
        }
        expect(!worker.reserve().has_value());

        reservations.pop_back();
        expect(worker.reserve().has_value());
        worker.shutdown();
    }

    {
        ActorPickReadbackWorker worker(4);
        worker.start();
        std::mutex values_mutex;
        std::vector<int> values;

        for (int value : {1, 0, 3}) {
            auto reservation = worker.reserve();
            expect(reservation.has_value());
            expect(worker.submit(std::move(*reservation), [&, value] {
                std::lock_guard lock(values_mutex);
                values.push_back(value);
            }));
        }

        worker.shutdown();
        expect(values == (std::vector<int>{1, 0, 3}));
        expect(worker.pending_count() == 0);
    }

    return 0;
}
