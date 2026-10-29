#pragma once

#include "horizon.h"
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include "native_lighting.h"
#include "native_render_policy.h"

namespace Corona::Systems::OpticsDetail {

// Exact input key for the single shared solar RSM allocation. Retaining both
// source and mesh buffers prevents address reuse from producing a false hit.
// Do not key this per camera: another view can overwrite the same GPU images.
struct NativeLightFieldInputs {
    std::vector<std::uint32_t> words;
    std::vector<Horizon::HardwareBuffer> meshes;
    std::vector<Horizon::HardwareImage> images;

    void add(std::uint32_t value) { words.push_back(value); }
    void add(float value) { add(std::bit_cast<std::uint32_t>(value)); }
    bool operator==(const NativeLightFieldInputs& other) const {
        if (words != other.words || meshes.size() != other.meshes.size() || images.size()!=other.images.size()) return false;
        for (std::size_t i = 0; i < meshes.size(); ++i)
            if (!meshes[i].same_resource(other.meshes[i])) return false;
        for (std::size_t i = 0; i < images.size(); ++i)
            if (!images[i].same_resource(other.images[i])) return false;
        return true;
    }
};

class NativeLightFieldCache {
    std::shared_ptr<NativeLighting> source_;
    std::optional<NativeLightFieldInputs> inputs_;
    std::uintptr_t scene_{}, camera_{};
public:
    const std::shared_ptr<NativeLighting>& source() const { return source_; }
    bool owned_by(std::uintptr_t scene, std::uintptr_t camera) const {
        return source_ && scene_ == scene && camera_ == camera;
    }
    void begin_update() { inputs_.reset(); }
    bool matches(const std::shared_ptr<const void>& source,
                 const NativeLightFieldInputs& inputs) const {
        return source_ && source_ == source && inputs_ && *inputs_ == inputs;
    }
    void invalidate() { source_.reset(); inputs_.reset(); }
    // Call only after successful submission. Invalidate before recording any
    // writes, including diagnostic shadow passes.
    void submitted(std::shared_ptr<NativeLighting> source, NativeLightFieldInputs inputs,
                   std::uintptr_t scene, std::uintptr_t camera) {
        source_ = std::move(source);
        inputs_ = std::move(inputs);
        scene_=scene;camera_=camera;
    }
};
} // namespace Corona::Systems::OpticsDetail
