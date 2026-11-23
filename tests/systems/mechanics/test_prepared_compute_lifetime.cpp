#include "gpu_skinning.h"

#include <atomic>
#include <cstdio>
#include <future>
#include <thread>

int main() {
    using Pipeline = Corona::Systems::GpuSkinningPipeline;
    for (int iteration = 0; iteration < 8; ++iteration) {
        std::atomic<int> prepared{};
        std::array<std::thread, 2> workers;
        std::array<std::future<std::unique_ptr<Pipeline>>, 2> results;
        for (int slot = 0; slot < 2; ++slot) {
            std::packaged_task<std::unique_ptr<Pipeline>()> prepare([&prepared] {
                auto pipeline = std::make_unique<Pipeline>();
                const std::array<Corona::Resource::Vertex, 2> vertices{};
                const std::array<Corona::Resource::BoneWeights, 2> weights{};
                Pipeline::Request request;
                request.key = 1;
                request.revision = 1;
                request.vertices = vertices;
                request.weights = weights;
                request.readback = false;
                (void)pipeline->update(std::span(&request, 1), {});
                pipeline->reset();
                ++prepared;
                return pipeline;
            });
            results[slot] = prepare.get_future();
            workers[slot] = std::thread(std::move(prepare));
        }
        for (auto& worker : workers) worker.join();
        if (prepared != 2) {
            // Surface preparation exceptions instead of mistaking a future
            // containing an exception for a successfully prepared pipeline.
            for (auto& result : results) (void)result.get();
            return 1;
        }
        // Match shutdown of an unused preparation result: its compiler owner
        // must remain valid after the preparing thread and its TLS are gone.
        std::puts("Destroying pipeline after its compiler thread exited");
        std::fflush(stdout);
        for (auto& result : results) result = {};
    }
    std::puts("PASS: prepared compute pipelines outlive their compiler threads");
}
