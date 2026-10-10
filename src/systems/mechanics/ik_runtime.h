#pragma once

#include <corona/resource/types/animation_pose.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Corona::Systems::MechanicsInternal {

inline bool finite_ik_point(const std::array<float, 3>& point) {
    return std::all_of(point.begin(), point.end(), [](float v) { return std::isfinite(v); });
}

inline std::array<float, 3> ik_transform_point(const std::array<float, 16>& m,
                                             const std::array<float, 3>& p) {
    return {m[0]*p[0] + m[4]*p[1] + m[8]*p[2] + m[12],
            m[1]*p[0] + m[5]*p[1] + m[9]*p[2] + m[13],
            m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14]};
}

// A bone origin is C * G * origin; offset matrices transform bind vertices,
// and must not be used for joint positions. C includes import normalization.
inline std::optional<std::array<float, 3>> ik_bone_model_position(
    const Resource::SkeletonData& skeleton,
    const std::vector<std::array<float, 16>>& locals, int node) {
    if (locals.size() != skeleton.nodes.size() || node < 0 ||
        node >= static_cast<int>(locals.size())) return std::nullopt;
    std::vector<int> path;
    for (int current = node; current >= 0;) {
        if (current >= static_cast<int>(locals.size()) || path.size() >= locals.size())
            return std::nullopt;
        path.push_back(current);
        current = skeleton.nodes[static_cast<std::size_t>(current)].parent;
    }
    auto global = Resource::compose_trs({0,0,0}, {0,0,0,1}, {1,1,1});
    for (auto it = path.rbegin(); it != path.rend(); ++it)
        global = Resource::mat4_mul(global, locals[static_cast<std::size_t>(*it)]);
    const auto point = Resource::mat4_translation(Resource::mat4_mul(skeleton.global_inverse, global));
    return finite_ik_point(point) ? std::optional(point) : std::nullopt;
}

inline bool is_automatic_ik(const Resource::IkChain& chain) {
    return chain.mode == Resource::IkChain::Mode::FootPlant || chain.contact_driven;
}

// Replacing a fully weighted anchor in one frame would teleport the foot. Loss of
// support, excessive drift, or a lifted animation foot first releases the old IK.
inline bool foot_can_plant(Resource::IkChain& chain, bool found, bool keep_anchor, float gap) {
    if (chain.runtime.has_target && !keep_anchor) chain.runtime.releasing = true;
    return found && !chain.runtime.releasing &&
        (keep_anchor || gap <= std::max(0.0f, chain.plant_threshold));
}

// Exactly one activation update per animation frame, regardless of physics substeps.
// No target has ever been found => no IK, even if the caller's maximum weight is 1.
inline void advance_ik_activation(Resource::IkChain& chain,
                                  const std::optional<std::array<float, 3>>& target,
                                  float dt) {
    if (!chain.enabled || !std::isfinite(chain.weight) || chain.weight <= 0) {
        chain.runtime = {};
        return;
    }
    dt = std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;
    const float decay = std::isfinite(chain.contact_weight_decay)
        ? std::max(0.0f, chain.contact_weight_decay) : 0.0f;
    const float rise = std::isfinite(chain.contact_weight_rise) && chain.contact_weight_rise > 0
        ? chain.contact_weight_rise : decay;
    chain.runtime.grounded = target.has_value() && finite_ik_point(*target);
    if (chain.runtime.grounded) {
        chain.runtime.has_target = true;
        chain.runtime.target = *target;
        chain.runtime.weight = std::min(1.0f, chain.runtime.weight + rise * dt);
    } else if (chain.runtime.has_target) {
        chain.runtime.weight = std::max(0.0f, chain.runtime.weight - decay * dt);
        if (chain.runtime.weight == 0) chain.runtime = {};
    } else {
        chain.runtime = {};
    }
}

inline std::optional<Resource::IkChain> ik_solver_parameters(const Resource::IkChain& chain) {
    if (!chain.enabled || !std::isfinite(chain.weight) || chain.weight <= 0) return std::nullopt;
    auto result = chain;
    if (is_automatic_ik(chain)) {
        if (!chain.runtime.has_target || chain.runtime.weight <= 0) return std::nullopt;
        result.target = chain.runtime.target;
        result.weight = std::clamp(chain.weight, 0.0f, 1.0f) * chain.runtime.weight;
    }
    return finite_ik_point(result.target) ? std::optional(result) : std::nullopt;
}

inline bool commit_ik_runtime(std::vector<Resource::IkChain>& destination,
                              std::uint64_t current_revision,
                              const std::vector<Resource::IkChain>& snapshot,
                              std::uint64_t snapshot_revision) {
    if (current_revision != snapshot_revision || destination.size() != snapshot.size()) return false;
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        if (destination[i].id != snapshot[i].id || destination[i].end_node != snapshot[i].end_node ||
            destination[i].mode != snapshot[i].mode) return false;
    }
    for (std::size_t i = 0; i < snapshot.size(); ++i) destination[i].runtime = snapshot[i].runtime;
    return true;
}

} // namespace Corona::Systems::MechanicsInternal
