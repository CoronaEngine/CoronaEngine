#pragma once

// Web -> Python bridge worker.
//
// `bridge.call` used to run the whole service call on the CEF UI thread, so waiting for
// Python froze the renderer for as long as the service ran. The worker keeps that wait
// off the owner thread: the V8 promise stays pending, the owner thread stays free to
// answer the editor callbacks the Python service makes (they marshal to it), and the
// reply resolves from the worker when the service answers. The Python runtime itself is
// single-consumer, so bridge calls stay serialized.
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace Corona::Systems::UI {

class BridgeWorker {
   public:
    static BridgeWorker& instance();

    /// Enqueue a bridge call. False once the worker has been stopped (shutdown).
    bool post(std::function<void()> job);

    /// Stop before CEF shuts down: clears queued work and joins. An in-flight call ends
    /// when the Python runtime answers or cancels it.
    void stop();

    BridgeWorker(const BridgeWorker&) = delete;
    BridgeWorker& operator=(const BridgeWorker&) = delete;

   private:
    BridgeWorker() = default;
    ~BridgeWorker();
    void run();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::thread thread_;
    bool started_ = false;
    bool stopping_ = false;
};

}  // namespace Corona::Systems::UI
