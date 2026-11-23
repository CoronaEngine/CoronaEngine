#include "bridge_worker.h"

namespace Corona::Systems::UI {

BridgeWorker& BridgeWorker::instance() {
    static BridgeWorker worker;
    return worker;
}

BridgeWorker::~BridgeWorker() {
    stop();
}

bool BridgeWorker::post(std::function<void()> job) {
    if (!job) return false;
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    if (!started_) {
        started_ = true;
        thread_ = std::thread([this] { run(); });
    }
    queue_.push_back(std::move(job));
    cv_.notify_one();
    return true;
}

void BridgeWorker::stop() {
    std::thread worker;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        // Queued calls will never run; their endpoints are going away with the browser.
        queue_.clear();
        started_ = false;
        worker = std::move(thread_);
    }
    cv_.notify_all();
    if (worker.joinable()) worker.join();
}

void BridgeWorker::run() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            job();
        } catch (...) {
            // The job reports its own failures through the endpoint; a stray exception
            // must not kill the worker.
        }
    }
}

}  // namespace Corona::Systems::UI
