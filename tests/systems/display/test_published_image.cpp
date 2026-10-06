#include <horizon/core/storage.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <stdexcept>

#include "corona/systems/display/published_image.h"

namespace {
using Corona::Systems::Detail::ImageFrameMetadata;
using Corona::Systems::Detail::PublishedImage;
using Storage = Corona::Kernel::Utils::Storage<int, 2, 1>;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void idle_eviction_rejects_cached_and_reused_handles() {
    Storage storage;
    const auto handle = storage.allocate();
    PublishedImage producer(handle);
    const auto cached = producer;
    const auto snapshot = cached;
    *storage.acquire_write(handle) = 42;
    for (int idle_frames = 1; idle_frames <= 240; ++idle_frames) {
        auto access = snapshot.acquire_write(storage);
        expect(access && *access->images() == 42, "last frame remains accessible before eviction");
    }
    producer.retire().wait();
    storage.deallocate(handle);
    expect(!cached.acquire_write(storage), "cached layer must reject an evicted slot");
    expect(!snapshot.acquire_write(storage), "copied snapshot must reject an evicted slot");

    const auto reused = storage.allocate();
    expect(reused == handle, "test must exercise numeric handle reuse");
    *storage.acquire_write(reused) = 99;
    PublishedImage reactivated(reused);
    expect(!snapshot.acquire_write(storage), "old snapshot must not read another allocation");
    {
        auto access = reactivated.acquire_write(storage);
        expect(access && *access->images() == 99, "reactivated image should be accessible");
    }
    reactivated.retire().wait();
    storage.deallocate(reused);
}

void retirement_waits_for_image_access_and_receipt_writeback() {
    Storage storage;
    const auto handle = storage.allocate();
    PublishedImage producer(handle);
    auto access = producer.acquire_write(storage);
    auto retirement = producer.retire();
    expect(!producer.acquire_write(storage), "retirement must reject new acquisitions immediately");
    std::promise<void> waiting;
    auto entered = waiting.get_future();
    auto completed = std::async(std::launch::async, [&]() {
        retirement.wait([&]() { waiting.set_value(); });
        // Acquiring here also proves the storage lock died before the lease.
        expect(*storage.acquire_write(handle) == 7, "retirement must observe final consumed receipt");
        storage.deallocate(handle);
    });
    entered.wait();
    expect(completed.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
           "retirement must wait while Display holds the image");
    *access->images() = 7;
    access.reset();
    completed.get();
}

void layer_retirement_does_not_retire_the_surface_or_ui() {
    Storage optics_storage;
    Storage ui_storage;
    const auto optics_id = optics_storage.allocate();
    PublishedImage optics(optics_id);
    const auto ui_id = ui_storage.allocate();
    PublishedImage ui(ui_id);
    Corona::Systems::Detail::SurfaceFrameCoordinator surface;
    expect(surface.activate(1), "surface should activate");
    auto snapshot = surface.capture(1);
    optics.retire().wait();
    optics_storage.deallocate(optics_id);
    auto frame = surface.begin_frame(snapshot, [&]() { return ui.acquire_write(ui_storage); });
    expect(frame && frame->images(), "UI must remain accessible on an optics-idle surface");
    frame.reset();
    auto removed = surface.retire(1);
    bool attempted_image_access = false;
    auto stale_frame = surface.begin_frame(snapshot, [&]() {
        attempted_image_access = true;
        return ui.acquire_write(ui_storage);
    });
    expect(!stale_frame && !attempted_image_access, "surface removal must still guard image access");
    removed.wait();
    ui.retire().wait();
    ui_storage.deallocate(ui_id);
}

void partial_image_acquisition_releases_image_and_surface_leases() {
    Storage storage;
    const auto optics_id = storage.allocate();
    const auto ui_id = storage.allocate();
    PublishedImage optics(optics_id);
    PublishedImage ui(ui_id);
    struct FailingStorage {
        using WriteHandle = Storage::WriteHandle;
        Storage& storage;
        std::uintptr_t failing_id;
        WriteHandle acquire_write(std::uintptr_t id) {
            if (id == failing_id) {
                throw std::runtime_error("injected second acquisition failure");
            }
            return storage.acquire_write(id);
        }
    } failing{storage, ui_id};
    using Access = decltype(optics.acquire_write(failing));
    struct Images {
        Access optics;
        Access ui;
    };
    Corona::Systems::Detail::SurfaceFrameCoordinator surface;
    expect(surface.activate(1), "surface should activate");
    bool caught = false;
    try {
        (void)surface.begin_frame(surface.capture(1), [&]() -> Images {
            return {optics.acquire_write(failing), ui.acquire_write(failing)};
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    expect(caught, "second image acquisition must throw in this test");
    expect(static_cast<bool>(storage.try_acquire_write_nowait(optics_id)),
           "partial acquisition must release the first storage lock");
    optics.retire().wait();
    ui.retire().wait();
    surface.retire(1).wait();
    storage.deallocate(optics_id);
    storage.deallocate(ui_id);
    expect(storage.count() == 0, "retirement must leave no occupied slots");
}

void acquired_image_replaces_stale_frame_metadata() {
    struct Frame {
        int image = 0;
        ImageFrameMetadata metadata;
    };
    Corona::Kernel::Utils::Storage<Frame, 2, 1> storage;
    const auto handle = storage.allocate();
    PublishedImage producer(handle);

    // Display has copied an event, but has not acquired its image yet.
    // Exercise shrinking, growing, and a viewport change without resizing.
    const ImageFrameMetadata updates[] = {
        {8, 960, 540, 20, 30, 900, 480},
        {9, 2560, 1440, 40, 50, 2400, 1300},
        {10, 1920, 1080, 60, 70, 1800, 900},
    };
    for (const auto& updated : updates) {
        struct Layer : ImageFrameMetadata {
            uint64_t first_present_boundary = 19;
        } snapshot;
        static_cast<ImageFrameMetadata&>(snapshot) = {7, 1920, 1080, 0, 0, 1920, 1080};
        {
            auto write = storage.acquire_write(handle);
            write->image = 7;
            write->metadata = snapshot;
        }
        const auto cached_publication = producer;

        // The producer replaces the frame in the same allocation before Display
        // acquires it. The cached publication remains valid across this update.
        {
            auto write = storage.acquire_write(handle);
            write->image = 42;
            write->metadata = updated;
        }
        auto access = cached_publication.acquire_write(storage, snapshot);
        expect(access && access->images()->image == 42, "Display should acquire the updated image");
        expect(snapshot.width == updated.width && snapshot.height == updated.height,
               "acquired image must not use the cached event's dimensions");
        expect(snapshot.frame_index == updated.frame_index,
               "consumption must identify the acquired frame rather than the cached event");
        expect(snapshot.viewport_x == updated.viewport_x &&
                   snapshot.viewport_y == updated.viewport_y &&
                   snapshot.viewport_width == updated.viewport_width &&
                   snapshot.viewport_height == updated.viewport_height,
               "acquired image must use its own viewport");
        expect(snapshot.first_present_boundary == 19,
               "refreshing image metadata must preserve the captured UI acknowledgement boundary");
        expect(!storage.try_acquire_write_nowait(handle),
               "image and metadata must remain protected through Display receipt writeback");
    }
    producer.retire().wait();
    storage.deallocate(handle);
}

void missing_images_clear_cached_frame_metadata() {
    struct Frame {
        ImageFrameMetadata metadata;
    };
    Corona::Kernel::Utils::Storage<Frame, 2, 1> storage;
    const auto handle = storage.allocate();
    PublishedImage producer(handle);
    const auto cached = producer;
    producer.retire().wait();
    storage.deallocate(handle);

    for (const auto& publication : {cached, PublishedImage{}}) {
        ImageFrameMetadata metadata{7, 1920, 1080, 20, 30, 1800, 900};
        expect(!publication.acquire_write(storage, metadata), "missing image must reject acquisition");
        expect(metadata.frame_index == 0 && metadata.width == 0 && metadata.height == 0 &&
                   metadata.viewport_x == 0 && metadata.viewport_y == 0 &&
                   metadata.viewport_width == 0 && metadata.viewport_height == 0,
               "missing image must not leave stale dimensions or viewport coordinates");
    }
}
}  // namespace

int main() {
    idle_eviction_rejects_cached_and_reused_handles();
    retirement_waits_for_image_access_and_receipt_writeback();
    layer_retirement_does_not_retire_the_surface_or_ui();
    partial_image_acquisition_releases_image_and_surface_leases();
    acquired_image_replaces_stale_frame_metadata();
    missing_images_clear_cached_frame_metadata();
    Storage storage;
    expect(!PublishedImage{}.acquire_write(storage), "unpublished layers have no access");
    std::cout << "Published image lifecycle tests passed\n";
}
