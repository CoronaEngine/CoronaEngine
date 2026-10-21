#include "filter.h"
#include <algorithm>

namespace vision::reblur {
void Filter::reset() noexcept {
    for (auto &layer : layers_) layer.frame = InvalidUI32;
}

void Filter::prepare(Device &device, uint2 size, uint layer_count) {
    const uint n = size.x * size.y;
    if (size_.x != size.x || size_.y != size.y) {
        size_ = size;
        reset();
        for (uint i = 0; i < 2; ++i) {
            diffuse_[i] = device.create_buffer<half4>(n, "REBLUR diffuse scratch");
            specular_[i] = device.create_buffer<half4>(n, "REBLUR specular scratch");
        }
        fast_ = device.create_buffer<half2>(n, "REBLUR fast scratch");
        tracking_ = device.create_buffer<float>(n, "REBLUR tracking scratch");
        tiles_ = device.create_buffer<uint>(((size.x + 15u) / 16u) * ((size.y + 15u) / 16u), "REBLUR tiles");
        guide_ = device.create_byte_buffer(n * sizeof(Guide), "REBLUR current geometry SOA");
        history_ = device.create_byte_buffer(n * sizeof(TemporalData), "REBLUR current history SOA");
        for (auto &layer : layers_) { layer.guide = {}; layer.history = {}; }
    }
    for (uint l = 0; l < std::min(layer_count, 3u); ++l) {
        if (layers_[l].guide.size() == n * sizeof(GeometryHistory)) continue;
        layers_[l].guide = device.create_byte_buffer(n * sizeof(GeometryHistory), "REBLUR layer geometry SOA");
        layers_[l].history = device.create_byte_buffer(n * sizeof(PersistentHistory), "REBLUR layer history SOA");
        layers_[l].frame = InvalidUI32;
    }
}

void Filter::compile(Device &device) {
    compile_modulation(device);
    compile_prepass(device);
    compile_temporal(device);
    compile_history_fix(device);
    compile_spatial(device);
    compile_stabilization(device);
    reset();
}

CommandBatch Filter::dispatch(FilterParam p, uint layer) {
    auto &state = layers_[layer];
    p.size = size_;
    p.history_valid &= state.frame != InvalidUI32 && state.frame + 1u == p.frame;
    p.guide = guide_.descriptor();
    p.prev_guide = state.guide.descriptor();
    p.history = history_.descriptor();
    p.prev_history = state.history.descriptor();
    p.fast = fast_.descriptor(); p.tracking = tracking_.descriptor(); p.tiles = tiles_.descriptor();
    p.max_history = settings.max_history; p.fast_history = settings.fast_history;
    p.fix_frames = settings.history_fix_frames; p.fix_stride = settings.history_fix_stride;
    p.diffuse_prepass = settings.diffuse_prepass; p.specular_prepass = settings.specular_prepass;
    p.min_radius = settings.min_radius; p.max_radius = settings.max_radius;
    p.lobe_fraction = settings.lobe_fraction * settings.lobe_fraction; p.roughness_fraction = settings.roughness_fraction;
    p.plane_sensitivity = settings.plane_sensitivity; p.min_hit_weight = settings.min_hit_weight;
    p.stabilization = settings.stabilization; p.antilag_sigma = settings.antilag_sigma;
    p.antilag_sensitivity = settings.antilag_sensitivity; p.anti_firefly = settings.anti_firefly;
    p.reconstruction = static_cast<uint>(settings.reconstruction);
    auto bind = [&](uint src, uint dst) {
        p.src_diffuse = diffuse_[src].descriptor(); p.src_specular = specular_[src].descriptor();
        p.dst_diffuse = diffuse_[dst].descriptor(); p.dst_specular = specular_[dst].descriptor();
    };
    CommandBatch commands;
    // Every lane in a shared-memory workgroup must reach its preload barrier.
    const uint2 neighborhood_size = make_uint2((size_.x + 15u) / 16u * 16u,
                                               (size_.y + 7u) / 8u * 8u);
    bind(1u, 0u);
    commands << pack_(p).dispatch(uint(tiles_.size()) * 32u);
    uint src = 0u;
    if (settings.reconstruction != HitDistanceReconstructionMode::OFF) {
        bind(src, src ^ 1u); commands << reconstruct_(p).dispatch(size_); src ^= 1u;
    }
    bind(src, src ^ 1u); commands << prepass_(p).dispatch(size_); src ^= 1u;
    bind(src, src ^ 1u); commands << temporal_(p).dispatch(neighborhood_size); src ^= 1u;
    bind(src, src ^ 1u); commands << fix_(p).dispatch(neighborhood_size); src ^= 1u;
    bind(src, src ^ 1u); commands << blur_(p).dispatch(size_); src ^= 1u;
    bind(src, src ^ 1u); commands << post_blur_(p).dispatch(size_); src ^= 1u;
    bind(src, src ^ 1u); commands << stabilize_(p).dispatch(neighborhood_size);
    state.frame = p.frame;
    return commands;
}
}
