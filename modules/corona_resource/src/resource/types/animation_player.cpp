#include "corona/resource/types/animation_player.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace Corona::Resource {
namespace {
using NodeMap = std::unordered_map<std::string, int>;
NodeMap unique_nodes(const SkeletonData& skeleton) {
    NodeMap result;
    for (std::size_t i = 0; i < skeleton.nodes.size(); ++i)
        if (!result.emplace(skeleton.nodes[i].name, static_cast<int>(i)).second)
            result[skeleton.nodes[i].name] = -1;
    return result;
}
void validate_clip(const AnimationClip& clip) {
    if (!std::isfinite(clip.duration) || clip.duration < 0 ||
        !std::isfinite(clip.ticks_per_second) || clip.ticks_per_second <= 0)
        throw std::invalid_argument("INVALID_ANIMATION: invalid duration or tick rate");
    const auto keys_valid = [](const auto& keys, bool quaternion) {
        float previous = -1;
        for (const auto& [time, value] : keys) {
            if (!std::isfinite(time) || time < 0 || time <= previous)
                throw std::invalid_argument("INVALID_ANIMATION: key times must increase");
            previous = time;
            double norm = 0;
            for (auto component : value) {
                if (!std::isfinite(component)) throw std::invalid_argument("INVALID_ANIMATION: non-finite key");
                norm += double(component) * component;
            }
            if (quaternion && norm < 1e-16) throw std::invalid_argument("INVALID_ANIMATION: zero quaternion");
        }
    };
    for (const auto& ch : clip.channels) {
        keys_valid(ch.positions, false); keys_valid(ch.rotations, true); keys_valid(ch.scales, false);
    }
}
double wrap_time(double time, double dt, double duration) {
    return duration > 0 ? std::fmod(time + dt, duration) : 0;
}
}

std::vector<AnimationPlayer::BoundClip> AnimationPlayer::bind(
    std::shared_ptr<const AnimationSource> source, const std::string& label, bool embedded) const {
    const auto targets = unique_nodes(skeleton_);
    const auto origins = unique_nodes(source->skeleton);
    std::vector<BoundClip> result;
    for (std::size_t ci = 0; ci < source->clips.size(); ++ci) {
        const auto& clip = source->clips[ci];
        validate_clip(clip);
        BoundClip binding;
        binding.info = {(embedded ? "model:" : "source:" + label + ":") + std::to_string(ci) + ":" + clip.name,
                        clip.name.empty() ? "Animation " + std::to_string(ci) : clip.name,
                        label, ci, clip.duration / double(clip.ticks_per_second)};
        binding.owner = source;
        binding.channels.assign(skeleton_.nodes.size(), -1);
        for (std::size_t ch = 0; ch < clip.channels.size(); ++ch) {
            const auto& name = clip.channels[ch].bone_name;
            const auto target = targets.find(name), origin = origins.find(name);
            if (target == targets.end() || target->second < 0 || origin == origins.end() || origin->second < 0)
                throw std::invalid_argument("INCOMPATIBLE_SKELETON: missing or ambiguous node " + name);
            if (binding.channels[target->second] >= 0)
                throw std::invalid_argument("INVALID_ANIMATION: duplicate channel " + name);
            if (!embedded) {
                int a = target->second, b = origin->second;
                std::size_t depth = 0;
                while (a >= 0 && b >= 0) {
                    if (++depth > skeleton_.nodes.size() || a >= static_cast<int>(skeleton_.nodes.size()) ||
                        b >= static_cast<int>(source->skeleton.nodes.size()))
                        throw std::invalid_argument("INCOMPATIBLE_SKELETON: invalid hierarchy");
                    const auto& ta = skeleton_.nodes[a];
                    const auto& sb = source->skeleton.nodes[b];
                    // Exporter scene-root names may differ; their transforms must not.
                    if (ta.name != sb.name && (ta.parent >= 0 || sb.parent >= 0))
                        throw std::invalid_argument("INCOMPATIBLE_SKELETON: parent differs for " + name);
                    for (std::size_t i = 0; i < ta.local.size(); ++i)
                        if (!std::isfinite(sb.local[i]) || std::abs(ta.local[i] - sb.local[i]) >
                            1e-4f * std::max({1.0f, std::abs(ta.local[i]), std::abs(sb.local[i])}))
                            throw std::invalid_argument("RETARGET_REQUIRED: reference pose differs for " + name);
                    a = ta.parent; b = sb.parent;
                }
                if (a != b) throw std::invalid_argument("INCOMPATIBLE_SKELETON: hierarchy depth differs for " + name);
            }
            binding.channels[target->second] = static_cast<int>(ch);
        }
        result.push_back(std::move(binding));
    }
    return result;
}

const AnimationPlayer::BoundClip* AnimationPlayer::find(const std::string& id) const {
    const auto it = std::find_if(clips_.begin(), clips_.end(), [&](const auto& clip) { return clip.info.id == id; });
    return it == clips_.end() ? nullptr : &*it;
}

std::vector<LocalTransform> AnimationPlayer::sample(const std::string& id, double seconds) const {
    auto pose = reference_;
    if (const auto* binding = find(id)) {
        const auto& clip = binding->clip();
        const float ticks = static_cast<float>(seconds * clip.ticks_per_second);
        for (std::size_t i = 0; i < pose.size(); ++i)
            if (binding->channels[i] >= 0)
                pose[i] = sample_channel_trs(clip.channels[binding->channels[i]], ticks, reference_[i]);
    }
    return pose;
}

void AnimationPlayer::ensure_not_removing(const std::vector<std::string>& sources) const {
    for (const auto& id : {current_, target_, command_ ? command_->id : std::string{}}) {
        const auto* clip = find(id);
        if (clip && clip->info.id.starts_with("source:") &&
            std::find(sources.begin(), sources.end(), clip->info.source) == sources.end())
            throw std::invalid_argument("ANIMATION_IN_USE: switch away before removing " + clip->info.source);
    }
}

void AnimationPlayer::set_sources(std::vector<std::string> sources, std::vector<std::filesystem::path> paths) {
    if (sources.size() != paths.size()) throw std::invalid_argument("Animation source/path count mismatch");
    std::set<std::string> unique;
    for (const auto& source : sources)
        if (source.empty() || !unique.insert(source).second) throw std::invalid_argument("Duplicate or empty animation source");
    std::lock_guard lock(mutex_);
    ensure_not_removing(sources);
    if (sources == requested_sources_ && status_ != "error") return;
    auto pending = ResourceManager::get_instance().import_animations_async(paths);
    requested_sources_ = std::move(sources);
    requested_paths_ = std::move(paths);
    pending_ = std::move(pending);
    status_ = "loading"; error_.clear(); ++revision_;
}

void AnimationPlayer::request_switch(std::string_view animation, double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) throw std::invalid_argument("blend_seconds must be finite and non-negative");
    std::lock_guard lock(mutex_);
    const BoundClip* selected = find(std::string(animation));
    if (!selected) {
        for (const auto& clip : clips_) if (clip.info.name == animation) {
            if (selected) throw std::invalid_argument("AMBIGUOUS_ANIMATION: use the clip id");
            selected = &clip;
        }
    }
    if (!selected) throw std::invalid_argument(status_ == "loading" ? "ANIMATION_NOT_READY" : "ANIMATION_NOT_FOUND");
    if (pending_.valid() && selected->info.id.starts_with("source:") &&
        std::find(requested_sources_.begin(), requested_sources_.end(), selected->info.source) == requested_sources_.end())
        throw std::invalid_argument("ANIMATION_SOURCE_REMOVAL_PENDING");
    command_ = Switch{selected->info.id, seconds};
}

void AnimationPlayer::set_paused(bool paused) {
    std::lock_guard lock(mutex_);
    if (current_.empty() && target_.empty())
        throw std::invalid_argument(status_ == "loading" ? "ANIMATION_NOT_READY" : "ACTOR_HAS_NO_ANIMATION");
    if (paused_ == paused) return;
    paused_ = paused;
    ++revision_;
}

void AnimationPlayer::reset_model() {
    std::lock_guard lock(mutex_);
    if (!pending_.valid()) {
        requested_sources_ = sources_;
        requested_paths_ = paths_;
        if (!requested_paths_.empty()) pending_ = ResourceManager::get_instance().import_animations_async(requested_paths_);
    }
    model_ = 0; clips_.clear(); embedded_.clear(); reference_.clear(); last_pose_.clear(); frozen_from_.clear();
    current_.clear(); target_.clear(); command_.reset(); sources_.clear(); paths_.clear();
    time_ = target_time_ = elapsed_ = duration_ = 0;
    paused_ = false;
    status_ = "loading"; error_.clear(); ++revision_;
}

void AnimationPlayer::unavailable(std::string error) {
    std::lock_guard lock(mutex_);
    if (status_ != "error" || error_ != error) { status_ = "error"; error_ = std::move(error); ++revision_; }
}

void AnimationPlayer::initialize(std::uint64_t model, const SkeletonData& skeleton,
                                 const std::vector<AnimationClip>& embedded, std::string_view path) {
    // A model can change before its GPU skin cache has ever been materialized.
    // Rebind the last committed source set even when no reset_model call arrived.
    if (model_ != 0 && !pending_.valid() && !paths_.empty()) {
        requested_sources_ = sources_;
        requested_paths_ = paths_;
        pending_ = ResourceManager::get_instance().import_animations_async(requested_paths_);
    }
    skeleton_ = skeleton;
    reference_.clear();
    for (const auto& node : skeleton.nodes) reference_.push_back(decompose_local(node.local));
    auto source = std::make_shared<AnimationSource>();
    source->skeleton = skeleton;
    source->clips = embedded;
    auto bindings = bind(source, std::string(path), true);
    embedded_ = bindings; clips_ = std::move(bindings);
    current_ = clips_.empty() ? "" : clips_.front().info.id;
    target_.clear(); command_.reset(); time_ = target_time_ = elapsed_ = duration_ = 0;
    paused_ = false;
    last_pose_ = sample(current_, 0); frozen_from_.clear();
    sources_.clear(); paths_.clear(); model_ = model;
    status_ = pending_.valid() ? "loading" : "ready"; error_.clear(); ++revision_;
}

bool AnimationPlayer::evaluate(std::uint64_t model, const SkeletonData& skeleton,
                                const std::vector<AnimationClip>& embedded, std::string_view path,
                                double dt, std::vector<std::array<float, 16>>& locals) {
    if (!std::isfinite(dt) || dt < 0) throw std::invalid_argument("Invalid animation timestep");
    std::lock_guard lock(mutex_);
    if (model_ != model) initialize(model, skeleton, embedded, path);
    if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            if (skeleton.bone_count <= 0 && !requested_sources_.empty()) throw std::invalid_argument("ACTOR_HAS_NO_SKIN");
            ensure_not_removing(requested_sources_);
            const auto sources = pending_.get();
            auto next = embedded_;
            for (std::size_t i = 0; i < sources.size(); ++i) {
                auto added = bind(sources[i], requested_sources_[i], false);
                next.insert(next.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
            }
            // Reject reimports that would silently retarget a live subresource.
            for (const auto& id : {current_, target_, command_ ? command_->id : std::string{}})
                if (!id.empty() && std::none_of(next.begin(), next.end(), [&](const auto& c) { return c.info.id == id; }))
                    throw std::invalid_argument("ANIMATION_IN_USE: active clip changed during import");
            clips_ = std::move(next); sources_ = requested_sources_; paths_ = requested_paths_;
            status_ = "ready"; error_.clear();
        } catch (const std::exception& error) { status_ = "error"; error_ = error.what(); }
        pending_ = {}; ++revision_;
    }
    // Imports and explicit clip selection still work while paused. Only animation
    // time is frozen; physics, actor transforms and IK remain independent.
    if (paused_) dt = 0;
    bool discontinuity = false;
    if (command_) {
        const auto command = std::move(*command_); command_.reset();
        if (command.id != (target_.empty() ? current_ : target_)) {
            if (command.seconds == 0) {
                current_ = command.id; time_ = 0; target_.clear(); frozen_from_.clear();
                duration_ = elapsed_ = 0; discontinuity = true;
            } else {
                // An interrupted fade starts at the last PRE-IK mixed pose.
                if (!target_.empty()) frozen_from_ = last_pose_;
                else frozen_from_.clear();
                target_ = command.id; target_time_ = 0; duration_ = command.seconds; elapsed_ = 0;
            }
            ++revision_;
            // The first sample after the command is exactly the requested start pose.
            dt = 0;
        }
    }
    if (auto* clip = find(current_)) time_ = wrap_time(time_, dt, clip->info.duration_seconds);
    auto pose = frozen_from_.empty() ? sample(current_, time_) : frozen_from_;
    if (!target_.empty()) {
        const auto* clip = find(target_);
        target_time_ = wrap_time(target_time_, dt, clip->info.duration_seconds);
        elapsed_ = std::min(duration_, elapsed_ + dt);
        const auto target_pose = sample(target_, target_time_);
        const float weight = static_cast<float>(elapsed_ / duration_);
        for (std::size_t i = 0; i < pose.size(); ++i) pose[i] = blend_local(pose[i], target_pose[i], weight);
        if (elapsed_ >= duration_) {
            current_ = std::move(target_); target_.clear(); time_ = target_time_;
            frozen_from_.clear(); duration_ = elapsed_ = 0; ++revision_;
        }
    }
    last_pose_ = std::move(pose);
    locals.resize(last_pose_.size());
    for (std::size_t i = 0; i < locals.size(); ++i) {
        const auto& p = last_pose_[i];
        locals[i] = compose_trs(p.translation, p.rotation, p.scale);
    }
    return discontinuity;
}

AnimationSnapshot AnimationPlayer::snapshot() const {
    std::lock_guard lock(mutex_);
    AnimationSnapshot result;
    result.sources = sources_; result.requested_sources = requested_sources_;
    for (const auto& clip : clips_) result.clips.push_back(clip.info);
    result.current = current_; result.target = target_; result.load_status = status_; result.load_error = error_;
    result.time_seconds = time_; result.transition_weight = duration_ > 0 ? elapsed_ / duration_ : 0;
    result.paused = paused_;
    result.revision = revision_;
    return result;
}
std::uint64_t AnimationPlayer::revision() const { std::lock_guard lock(mutex_); return revision_; }
} // namespace Corona::Resource
