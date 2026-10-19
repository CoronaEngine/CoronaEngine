#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace Corona::Systems::Detail {

// Coalesce UI-only changes with an actively advancing scene. This stores only
// receipt/timing metadata, never an extra image. A stopped/slow scene must not
// stop UI redraws, including first-present acknowledgements and editor input.
class ScenePresentCadence {
public:
    using Clock = std::chrono::steady_clock;
    using Key = std::array<std::uint64_t, 2>; // image allocation, producer receipt

    // Submit composition immediately, then release its existing output on a
    // regular producer cadence. A small initial phase offset absorbs variable
    // CPU recording time without an extra queued image. Never wait more than
    // 8 ms, replay stale frames, or accumulate catch-up deadlines after a stall.
    [[nodiscard]] Clock::time_point present_deadline(const Key& scene, int fps,
                                                     Clock::time_point now) noexcept {
        if (!valid(scene) || fps <= 0) {
            paced_scene_ = {};
            return now;
        }
        if (scene == paced_scene_) return now;
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
        if (!valid(paced_scene_) || scene[0] != paced_scene_[0] || fps != paced_fps_) {
            present_deadline_ = now + std::min(period / 4,
                std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(4)));
        } else {
            present_deadline_ += period;
            present_deadline_ = std::clamp(present_deadline_, now,
                now + std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(8)));
        }
        paced_scene_ = scene;
        paced_fps_ = fps;
        return present_deadline_;
    }

    [[nodiscard]] bool defer_ui(const Key& scene, Clock::time_point now) const noexcept {
        if (!valid(scene) || scene != last_scene_ || !has_scene_) return false;
        // A small grace period absorbs producer scheduling jitter. Cap the
        // delay at 1.25 native 60 Hz periods even when the scene is much slower.
        const auto grace_ms = std::min(period_ms_ * 1.25, 1000.0 / 48.0);
        return std::chrono::duration<double, std::milli>(now - last_present_).count() < grace_ms;
    }

    void presented(const Key& scene, Clock::time_point now) noexcept {
        if (!valid(scene)) {
            has_scene_ = false;
            return;
        }
        if (has_scene_ && scene == last_scene_) return; // UI-only present
        if (has_scene_ && scene[0] == last_scene_[0]) {
            const double elapsed_ms = std::chrono::duration<double, std::milli>(now - last_present_).count();
            if (elapsed_ms > 0.0 && elapsed_ms < 100.0)
                period_ms_ = std::clamp(period_ms_ * 0.9 + elapsed_ms * 0.1, 1000.0 / 240.0, 1000.0 / 60.0);
        } else {
            period_ms_ = 1000.0 / 60.0;
        }
        last_scene_ = scene;
        last_present_ = now;
        has_scene_ = true;
    }

private:
    static bool valid(const Key& key) noexcept { return key[0] != 0 && key[1] != 0; }
    Key last_scene_{};
    Clock::time_point last_present_{};
    double period_ms_ = 1000.0 / 60.0;
    bool has_scene_ = false;
    Key paced_scene_{};
    Clock::time_point present_deadline_{};
    int paced_fps_ = 0;
};

} // namespace Corona::Systems::Detail
