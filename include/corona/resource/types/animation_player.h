#pragma once
#include "corona/resource/types/animation.h"
#include "corona/resource/types/animation_pose.h"
#include "corona/resource/resource_manager.h"
#include <mutex>

namespace Corona::Resource {

struct AnimationClipInfo {
    std::string id, name, source;
    std::size_t source_index{};
    double duration_seconds{};
};
struct AnimationSnapshot {
    std::vector<std::string> sources, requested_sources;
    std::vector<AnimationClipInfo> clips;
    std::string current, target, load_status{"loading"}, load_error;
    double time_seconds{}, transition_weight{};
    bool paused{};
    std::uint64_t revision{};
};

// Per-geometry ownership. APIs enqueue clip requests or set the pause flag under
// the player lock; mechanics commits catalogs and advances playback. Import jobs own values,
// never actor pointers, so destruction/replacement cannot write into a new actor.
class AnimationPlayer {
public:
    void set_sources(std::vector<std::string> sources, std::vector<std::filesystem::path> paths);
    void request_switch(std::string_view animation, double blend_seconds = 0.2);
    // Freeze both clip clocks and an in-flight blend; changing clips keeps this flag.
    void set_paused(bool paused);
    AnimationSnapshot snapshot() const;
    std::uint64_t revision() const;
    void reset_model();
    void unavailable(std::string error);
    // Returns true for a discontinuity that should release automatic IK anchors.
    bool evaluate(std::uint64_t model, const SkeletonData& skeleton,
                  const std::vector<AnimationClip>& embedded, std::string_view model_path,
                  double dt, std::vector<std::array<float, 16>>& locals);

private:
    struct BoundClip {
        AnimationClipInfo info;
        std::shared_ptr<const AnimationSource> owner;
        std::vector<int> channels; // target node -> source channel
        const AnimationClip& clip() const { return owner->clips[info.source_index]; }
    };
    struct Switch { std::string id; double seconds{}; };
    std::vector<BoundClip> bind(std::shared_ptr<const AnimationSource> source,
                                const std::string& label, bool embedded) const;
    std::vector<LocalTransform> sample(const std::string& id, double seconds) const;
    const BoundClip* find(const std::string& id) const;
    void ensure_not_removing(const std::vector<std::string>& sources) const;
    void initialize(std::uint64_t model, const SkeletonData&, const std::vector<AnimationClip>&,
                    std::string_view model_path);
    mutable std::mutex mutex_;
    std::uint64_t model_{}, revision_{};
    SkeletonData skeleton_;
    std::vector<LocalTransform> reference_, last_pose_, frozen_from_;
    std::vector<BoundClip> embedded_, clips_;
    std::vector<std::string> sources_, requested_sources_;
    std::vector<std::filesystem::path> paths_, requested_paths_;
    std::shared_future<AnimationSources> pending_;
    std::optional<Switch> command_;
    std::string current_, target_, status_{"loading"}, error_;
    double time_{}, target_time_{}, elapsed_{}, duration_{};
    bool paused_{};
};
} // namespace Corona::Resource
