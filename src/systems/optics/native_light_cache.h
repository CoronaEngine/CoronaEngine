#pragma once

#include "horizon.h"
#include <bit>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Corona::Systems::OpticsDetail {

// Exact input snapshot for the single shared RSM/LPV allocation. Retaining both
// source and mesh buffers prevents address reuse from producing a false hit.
// Do not key this per camera: another view can overwrite the same GPU images.
struct NativeLightFieldInputs {
    std::vector<std::uint32_t> words;
    std::vector<Horizon::HardwareBuffer> meshes;

    void add(std::uint32_t value) { words.push_back(value); }
    void add(float value) { add(std::bit_cast<std::uint32_t>(value)); }
    bool operator==(const NativeLightFieldInputs& other) const {
        if (words != other.words || meshes.size() != other.meshes.size()) return false;
        for (std::size_t i = 0; i < meshes.size(); ++i)
            if (!meshes[i].same_resource(other.meshes[i])) return false;
        return true;
    }
};

class NativeLightFieldCache {
    std::shared_ptr<const void> source_;
    std::optional<NativeLightFieldInputs> inputs_;
public:
    bool matches(const std::shared_ptr<const void>& source,
                 const NativeLightFieldInputs& inputs) const {
        return source_ && source_ == source && inputs_ && *inputs_ == inputs;
    }
    void invalidate() { source_.reset(); inputs_.reset(); }
    // Call only after successful submission. Invalidate before recording any
    // writes, including diagnostic shadow passes and interrupted propagation.
    void submitted(std::shared_ptr<const void> source, NativeLightFieldInputs inputs) {
        source_ = std::move(source);
        inputs_ = std::move(inputs);
    }
};
} // namespace Corona::Systems::OpticsDetail
