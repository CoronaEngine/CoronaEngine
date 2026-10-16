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
    compile_atrous(device);
    reset();
}

size_t Filter::allocated_bytes() const noexcept {
    size_t bytes = guide_.size_in_byte() + temporal_data_.size_in_byte() + tiles_.size_in_byte() +
        prepass_diffuse_.size_in_byte() + prepass_specular_.size_in_byte();
    for (uint i = 0; i < 2u; ++i) bytes += diffuse_[i].size_in_byte() + specular_[i].size_in_byte();
    for (const auto &layer : layers_) bytes += layer.geometry.size_in_byte() + layer.history.size_in_byte();
    return bytes;
}

CommandBatch Filter::dispatch(FilterParam p, uint layer, PassSubmit submit) {
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
    auto emit = [&](Command *command, string_view name) {
        if (submit) { CommandBatch batch; batch << command; submit(batch, name); }
        else commands << command;
    };
    bind(1u, 0u);
    emit(pack_(p).dispatch(size_), "Pack"); emit(classify_(p).dispatch(tiles_.size()), "ClassifyTiles");
    uint src = 0u;
    if (settings.reconstruction != 0u) {
        bind(src, src ^ 1u); emit(reconstruct_(p).dispatch(size_), "HitDistReconstruction"); src ^= 1u;
    }
    bind(src, src ^ 1u);
    emit(prepass_(p).dispatch(size_), "PrePass");
    p.src_diffuse = prepass_diffuse_.descriptor(); p.src_specular = prepass_specular_.descriptor();
    p.dst_diffuse = diffuse_[0].descriptor(); p.dst_specular = specular_[0].descriptor();
    emit(temporal_(p).dispatch(size_), "TemporalAccumulation");
    bind(0u, 1u);
    emit(fix_(p).dispatch(size_), "HistoryFix"); emit(clamp_(p).dispatch(size_), "HistoryClamping");
    src = 0u;
    if (settings.anti_firefly != 0u) {
        emit(firefly_(p).dispatch(size_), "AntiFirefly"); src = 1u;
    }
    bind(src, src ^ 1u);
    emit(publish_(p).dispatch(size_), "PublishHistory");
    const uint iterations = std::clamp(settings.atrous_iterations, 2u, 8u);
    for (uint i = 0u; i < iterations; ++i) {
        bind(src, src ^ 1u); p.step = 1u << i; p.first = i == 0u; p.last = i + 1u == iterations;
        const string name = i == 0u ? "AtrousFirst" : "Atrous" + std::to_string(p.step);
        emit((i == 0u ? atrous_first_ : atrous_)(p).dispatch(size_), name);
        src ^= 1u;
    }
    bind(src, src ^ 1u);
    emit(unpack_(p).dispatch(size_), "Unpack");
    state.frame = p.frame;
    return commands;
}
}
