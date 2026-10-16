#include "filter.h"
#include <algorithm>

namespace vision::relax {
void Filter::reset() noexcept {
    for (auto &layer : layers_) layer.frame = ~0u;
}

void Filter::prepare(Device &device, uint2 size, uint layer_count) {
    const uint n = size.x * size.y;
    if (size_.x != size.x || size_.y != size.y) {
        size_ = size;
        reset();
        guide_ = device.create_buffer<Guide>(n, "RELAX current guides");
        temporal_data_ = device.create_buffer<TemporalData>(n, "RELAX current temporal metadata");
        prepass_diffuse_ = device.create_buffer<float4>(n, "RELAX prepass diffuse");
        prepass_specular_ = device.create_buffer<float4>(n, "RELAX prepass specular");
        tiles_ = device.create_buffer<uint>(((size.x + 15u) / 16u) * ((size.y + 15u) / 16u), "RELAX tiles");
        for (uint i = 0; i < 2; ++i) {
            diffuse_[i] = device.create_buffer<float4>(n, "RELAX diffuse scratch");
            specular_[i] = device.create_buffer<float4>(n, "RELAX specular scratch");
        }
        for (auto &layer : layers_) { layer.geometry = {}; layer.history = {}; }
    }
    for (uint i = 0; i < layers_.size(); ++i) {
        auto &layer = layers_[i];
        if (i >= std::min(layer_count, 3u)) {
            layer.geometry = {}; layer.history = {}; layer.frame = ~0u;
        } else if (layer.geometry.size() != n) {
            layer.geometry = device.create_buffer<GeometryHistory>(n, "RELAX previous layer geometry");
            layer.history = device.create_buffer<PersistentHistory>(n, "RELAX previous layer histories");
            layer.frame = ~0u;
        }
    }
}

void Filter::compile(Device &device) {
    compile_modulation(device);
    compile_prepass(device);
    compile_temporal(device);
    compile_history_fix(device);
    compile_history_clamping(device);
    reset();
}

CommandBatch Filter::dispatch(FilterParam p, uint layer) {
    auto &state = layers_[layer];
    p.size = size_; p.settings = settings;
    p.disocclusion_threshold += (1.f + p.jitter_delta) / float(size_.y);
    p.history_valid &= state.frame != ~0u && state.frame + 1u == p.frame;
    p.guide = guide_.descriptor(); p.prev_guide = state.geometry.descriptor();
    p.prev_history = state.history.descriptor(); p.history = temporal_data_.descriptor();
    p.tiles = tiles_.descriptor();
    p.prepass_diffuse = prepass_diffuse_.descriptor(); p.prepass_specular = prepass_specular_.descriptor();
    auto bind = [&](uint src, uint dst) {
        p.src_diffuse = diffuse_[src].descriptor(); p.src_specular = specular_[src].descriptor();
        p.dst_diffuse = diffuse_[dst].descriptor(); p.dst_specular = specular_[dst].descriptor();
    };
    CommandBatch commands;
    bind(1u, 0u);
    commands << pack_(p).dispatch(size_) << classify_(p).dispatch(tiles_.size());
    uint src = 0u;
    if (settings.reconstruction != 0u) {
        bind(src, src ^ 1u); commands << reconstruct_(p).dispatch(size_); src ^= 1u;
    }
    bind(src, src ^ 1u);
    commands << prepass_(p).dispatch(size_);
    p.src_diffuse = prepass_diffuse_.descriptor(); p.src_specular = prepass_specular_.descriptor();
    p.dst_diffuse = diffuse_[0].descriptor(); p.dst_specular = specular_[0].descriptor();
    commands << temporal_(p).dispatch(size_);
    bind(0u, 1u);
    commands << fix_(p).dispatch(size_) << clamp_(p).dispatch(size_);
    src = 0u;
    if (settings.anti_firefly != 0u) {
        commands << firefly_(p).dispatch(size_); src = 1u;
    }
    bind(src, src ^ 1u);
    commands << publish_(p).dispatch(size_);
    commands << unpack_(p).dispatch(size_);
    state.frame = p.frame;
    return commands;
}
}
