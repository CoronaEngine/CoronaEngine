#include <corona/systems/script/python_runtime_coordinator.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Corona::Script::Python::PythonRuntimeCoordinator;
using Corona::Script::Python::PythonRuntimeRequest;
using Corona::Script::Python::PythonRuntimeRequestKind;
using Corona::Script::Python::PythonRuntimeResponse;
using Corona::Script::Python::PythonRuntimeResponseStatus;
using Corona::Script::Python::PythonRuntimeState;

namespace {
bool require(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

PythonRuntimeRequest request(std::string payload = {}) {
    PythonRuntimeRequest value;
    value.kind = PythonRuntimeRequestKind::ServiceCall;
    value.payload_json = std::move(payload);
    return value;
}

PythonRuntimeResponse callback_handler(const PythonRuntimeRequest& value) {
    return PythonRuntimeResponse::success("callback:" + value.payload_json);
}
}  // namespace

int main() {
    {
        PythonRuntimeCoordinator coordinator(2);
        if (!require(coordinator.bind_consumer_thread(),
                     "first thread should bind as the Python consumer")) return 1;
        std::atomic<bool> second_thread_bound{true};
        std::thread other([&] { second_thread_bound.store(coordinator.bind_consumer_thread()); });
        other.join();
        if (!require(!second_thread_bound.load(),
                     "another thread must not replace the Python consumer")) return 1;
    }

    {
        PythonRuntimeCoordinator coordinator(2);
        auto callback = request("payload");
        callback.kind = PythonRuntimeRequestKind::Callback;
        callback.handler = &callback_handler;
        auto ticket = coordinator.submit(std::move(callback));
        auto queued = coordinator.wait_pop(50ms);
        if (!require(queued.has_value(), "callback request should reach the consumer") ||
            !require(queued->handler != nullptr, "callback handler should survive queueing")) return 1;
        const auto active = coordinator.snapshot();
        if (!require(active.current_request.has_value(),
                     "snapshot should identify the request running on the consumer") ||
            !require(active.current_request->request_id == queued->request_id,
                     "snapshot request id should match the running request") ||
            !require(active.current_request->kind == PythonRuntimeRequestKind::Callback,
                     "snapshot should preserve the running request kind") ||
            !require(active.pending_count == 1 && active.queued_count == 0,
                     "snapshot should distinguish pending from queued work") ||
            !require(active.consumer_thread_bound,
                     "snapshot should report the bound consumer thread")) return 1;
        coordinator.set_execution_phase("callback:running");
        if (!require(coordinator.snapshot().execution_phase == "callback:running",
                     "snapshot should preserve the current Python execution phase")) return 1;
        auto response = queued->handler(*queued);
        coordinator.complete(queued->request_id, response);
        if (!require(!coordinator.snapshot().current_request.has_value(),
                     "completing a request should clear the active snapshot")) return 1;
        response = ticket.wait(50ms);
        if (!require(response.status == PythonRuntimeResponseStatus::Success,
                     "callback handler should complete") ||
            !require(response.payload_json == "callback:payload",
                     "callback handler should receive the plain payload")) return 1;
    }

    {
        PythonRuntimeCoordinator coordinator(2);
        std::thread consumer([&] {
            auto item = coordinator.wait_pop(100ms);
            if (item) coordinator.complete(item->request_id, PythonRuntimeResponse::success(item->payload_json));
        });
        auto response = coordinator.submit_and_wait(request("first"), 500ms);
        consumer.join();
        if (!require(response.status == PythonRuntimeResponseStatus::Success, "request should complete") ||
            !require(response.payload_json == "first", "response payload should match")) return 1;
    }

    {
        PythonRuntimeCoordinator coordinator(1);
        auto first = coordinator.submit(request("one"));
        auto second = coordinator.submit(request("two"));
        if (!require(first.accepted, "first request should be accepted") ||
            !require(!second.accepted && second.response.status == PythonRuntimeResponseStatus::QueueFull,
                     "request above capacity should return queue_full")) return 1;
    }

    {
        PythonRuntimeCoordinator coordinator(1);
        auto ordinary = coordinator.submit(request("ordinary"));
        auto shutdown = request("shutdown");
        shutdown.kind = PythonRuntimeRequestKind::LifecycleControl;
        shutdown.function = "shutdown";
        auto shutdown_ticket = coordinator.submit(std::move(shutdown));
        if (!require(ordinary.accepted, "ordinary request should occupy the bounded capacity") ||
            !require(shutdown_ticket.accepted,
                     "lifecycle control must bypass ordinary queue saturation")) return 1;
        auto next = coordinator.wait_pop(50ms);
        if (!require(next.has_value() && next->kind == PythonRuntimeRequestKind::LifecycleControl,
                     "lifecycle control must run before queued ordinary requests")) return 1;
        coordinator.complete(next->request_id, PythonRuntimeResponse::success());
        coordinator.begin_quiescing();
    }

    {
        PythonRuntimeCoordinator coordinator(4);
        auto response = coordinator.submit_and_wait(request("late"), 5ms);
        if (!require(response.status == PythonRuntimeResponseStatus::Timeout, "request should time out") ||
            !require(!coordinator.wait_pop(5ms).has_value(), "timed-out request must be discarded")) return 1;
    }

    {
        PythonRuntimeCoordinator coordinator(4);
        auto expired = request("expired-before-dispatch");
        expired.deadline = std::chrono::steady_clock::now() - 1ms;
        auto ticket = coordinator.submit(std::move(expired));
        if (!require(ticket.accepted, "an expiring request should still receive a completion ticket") ||
            !require(!coordinator.wait_pop(5ms).has_value(),
                     "an expired request must never enter the Python consumer") ||
            !require(ticket.wait(50ms).status == PythonRuntimeResponseStatus::Timeout,
                     "an expired queued request should complete as timeout")) return 1;
    }

    {
        PythonRuntimeCoordinator coordinator(4);
        std::mutex mutex;
        std::condition_variable cv;
        bool request_popped = false;
        bool allow_handler = false;
        std::atomic<bool> side_effect{false};
        std::thread consumer([&] {
            auto item = coordinator.wait_pop(100ms);
            if (!item) return;
            {
                std::lock_guard lock(mutex);
                request_popped = true;
            }
            cv.notify_one();
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return allow_handler; });
            }
            if (!item->cancelled()) {
                side_effect.store(true);
            }
            coordinator.complete(item->request_id, PythonRuntimeResponse::success());
        });

        std::thread producer([&] {
            const auto response = coordinator.submit_and_wait(request("in-flight"), 20ms);
            if (response.status != PythonRuntimeResponseStatus::Timeout) {
                side_effect.store(true);
            }
        });
        {
            std::unique_lock lock(mutex);
            if (!cv.wait_for(lock, 100ms, [&] { return request_popped; })) return 1;
        }
        producer.join();
        {
            std::lock_guard lock(mutex);
            allow_handler = true;
        }
        cv.notify_one();
        consumer.join();
        if (!require(!side_effect.load(),
                     "timed out in-flight requests must not execute their side effects")) return 1;
    }

    {
        PythonRuntimeCoordinator coordinator(8);
        auto ticket = coordinator.submit(request("shutdown"));
        coordinator.begin_quiescing();
        const auto snapshot = coordinator.snapshot();
        auto response = ticket.wait(50ms);
        if (!require(coordinator.state() == PythonRuntimeState::Quiescing, "state should be quiescing") ||
            !require(snapshot.state == PythonRuntimeState::Quiescing,
                     "snapshot should expose the coordinator lifecycle state") ||
            !require(snapshot.pending_count == 0 && snapshot.queued_count == 0,
                     "quiescing snapshot should show that pending work was cancelled") ||
            !require(response.status == PythonRuntimeResponseStatus::RuntimeStopping,
                     "pending request should complete as runtime_stopping") ||
            !require(!coordinator.submit(request("rejected")).accepted,
                     "new requests must be rejected")) return 1;
    }

    {
        PythonRuntimeCoordinator coordinator(64);
        constexpr int producer_count = 4;
        constexpr int per_producer = 8;
        std::atomic<int> completed{0};
        std::atomic<bool> duplicate_completed{false};
        std::thread consumer([&] {
            while (completed.load() < producer_count * per_producer) {
                auto item = coordinator.wait_pop(100ms);
                if (!item) continue;
                if (coordinator.complete(item->request_id, PythonRuntimeResponse::success(item->payload_json))) {
                    completed.fetch_add(1);
                }
                if (coordinator.complete(item->request_id, PythonRuntimeResponse::success("duplicate"))) {
                    duplicate_completed.store(true);
                }
            }
        });
        std::vector<std::thread> producers;
        std::atomic<int> successful{0};
        for (int producer = 0; producer < producer_count; ++producer) {
            producers.emplace_back([&, producer] {
                for (int index = 0; index < per_producer; ++index) {
                    auto response = coordinator.submit_and_wait(
                        request(std::to_string(producer) + ":" + std::to_string(index)), 1s);
                    if (response.status == PythonRuntimeResponseStatus::Success) successful.fetch_add(1);
                }
            });
        }
        for (auto& producer : producers) producer.join();
        consumer.join();
        if (!require(successful.load() == producer_count * per_producer, "all requests should complete") ||
            !require(!duplicate_completed.load(), "request must only complete once")) return 1;
    }
    return 0;
}
