#include <corona/resource/resource.h>
#include <corona/resource/resource_cache.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::fflush(stderr);
    std::exit(1);
}

/// Minimal concrete resource: the cache hands out a handle only when the entry is
/// Ready *and* carries a resource, so the contract under test needs a real object.
class DummyResource : public Corona::Resource::IResource {
   public:
    DummyResource() : IResource("dummy") {}
};

void install_ready_resource(Corona::Resource::ResourceCache& cache,
                            Corona::Resource::TResourceID rid) {
    auto [entry, created] = cache.get_or_create_entry(rid);
    require(static_cast<bool>(entry), "entry must be created");
    // add_resource() refuses an existing entry, so publish the resource directly.
    entry->resource = std::make_shared<DummyResource>();
    entry->state = Corona::Resource::LoadState::Ready;
}

}  // namespace

// The editor UI thread paints the editor and dispatches its input, so any read it
// performs must be bounded.  A system thread holds a resource entry's read lock
// across GPU work (seconds to minutes when a submit stalls), and a blocking lock
// there froze another system permanently.  This pins the best-effort contract: a
// busy entry yields no handle immediately instead of waiting, and an idle entry
// still yields one so periodic work resumes on the next cycle.
int main() {
    using Corona::Resource::ResourceCache;
    using Corona::Resource::TResourceID;

    ResourceCache cache;
    const TResourceID rid = 4242;
    install_ready_resource(cache, rid);

    require(cache.try_acquire_read_nowait(rid).valid(),
            "an idle entry must hand out a read handle");

    std::thread loader([&cache, rid] {
        auto lock = cache.acquire_write(rid);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    const auto started = std::chrono::steady_clock::now();
    auto handle = cache.try_acquire_read_nowait(rid);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    const auto write_started = std::chrono::steady_clock::now();
    auto write_handle = cache.try_acquire_write_nowait(rid);
    const auto write_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - write_started)
                                      .count();
    loader.join();

    require(elapsed_ms < 100, "best-effort read waited for a busy entry");
    require(!handle.valid(), "a busy entry must not hand out a read handle");
    require(write_elapsed_ms < 100, "best-effort write waited for a busy entry");
    require(!write_handle.valid(), "a busy entry must not hand out a write handle");

    require(!cache.try_acquire_read_nowait(rid + 1).valid(),
            "an unknown resource must not hand out a read handle");
    require(!cache.try_acquire_write_nowait(rid + 1).valid(),
            "an unknown resource must not hand out a write handle");

    // GeometrySystem's periodic RAM residency pass depends on this: once the slow
    // holder is gone the exclusive handle must be available again, otherwise a
    // skipped cycle would mean a permanently skipped pass.
    require(cache.try_acquire_write_nowait(rid).valid(),
            "an idle entry must hand out a write handle");
    return 0;
}
