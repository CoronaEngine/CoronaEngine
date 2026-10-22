#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace Corona::Systems::Vision {

// Coalesce bursts of edits without requiring moving/animated geometry to stop.
// The first dirty timestamp survives subsequent revisions; at most 250 ms of
// debounce is added before attempting the next complete geometry snapshot.
struct VisionGeometrySyncDebounce {
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> first_dirty;
    std::uint32_t stable_frames{};

    void reset() noexcept {
        first_dirty.reset();
        stable_frames = 0;
    }

    [[nodiscard]] bool poll(bool revision_changed, Clock::time_point now) noexcept {
        if (!first_dirty) first_dirty = now;
        stable_frames = revision_changed ? 0u : stable_frames + 1u;
        return stable_frames >= 3u || now - *first_dirty >= std::chrono::milliseconds(250);
    }
};

}  // namespace Corona::Systems::Vision
