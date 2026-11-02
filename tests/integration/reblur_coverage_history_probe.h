#pragma once

#include "base/scattering/interaction.h"
#include "base/mgr/pipeline.h"
#include "base/integral/integrator.h"
#include <bit>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace coverage_history_probe {
using namespace vision;
using namespace ocarina;
inline void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

inline void check(Device &device, Stream &stream) {
    constexpr uint width = 5u, height = 3u, pixels = width * height;
    std::vector<StablePlaneData> current(pixels * StablePlaneCount);
    std::vector<StablePlaneHistory> full(current.size());
    for (uint i = 0u; i < pixels; ++i) {
        auto &base = current[i];
        base.set_valid(1u);
        base.surface.hit = {i, i * 7u, make_float2(.125f, .375f)};
        base.surface.virtual_position = {12345.f + float(i) * .25f, -.03125f, 1e-6f};
        base.surface.virtual_geometric_normal = {.25f, -.5f, .75f};
        base.depth_position = {float(i) + .5f, -30.f, 50.f};
        base.surface.stable_branch = 100u + i;
        base.set_motion(make_float2(float(i) * .25f, -1.75f));
        if (i % 5u == 0u) current[pixels + i].set_valid(1u);
        if (i % 5u == 1u) current[2u * pixels + i].set_valid(1u);
        if (i % 5u == 2u) base.surface.approximate = 1u;
        if (i % 5u == 3u) base.surface.hit.inst_id = InvalidUI32;
        if (i == 4u) base.set_valid(0u);
        if (i == 9u) base.surface.stable_branch = InvalidUI32;
        // Invalid secondary fields must not set aggregate ambiguity.
        if (i == 14u) current[pixels + i].surface.approximate = 1u;
    }
    for (size_t i = 0u; i < current.size(); ++i) {
        auto &old = full[i]; const auto &plane = current[i];
        old.surface.hit = plane.surface.hit;
        old.surface.virtual_position = plane.surface.virtual_position;
        old.surface.virtual_geometric_normal = plane.surface.virtual_geometric_normal;
        old.surface.approximate = plane.surface.approximate;
        old.surface.stable_branch = plane.surface.stable_branch;
        old.depth_position = plane.depth_position; old.motion = plane.motion(); old.valid = plane.valid();
    }
    auto input = device.create_buffer<StablePlaneData>(current.size(), "coverage history current fixture");
    auto output = device.create_buffer<CoveragePlaneHistory>(pixels, "coverage history aggregation probe");
    input.upload_immediately(current.data());
    Kernel aggregate = [](BufferVar<StablePlaneData> planes, BufferVar<CoveragePlaneHistory> history) {
        Uint i = dispatch_id(), count = dispatch_dim().x * dispatch_dim().y;
        Uint secondary_valid = 0u;
        for (uint layer = 1u; layer < StablePlaneCount; ++layer)
            secondary_valid |= planes.read(layer * count + i)->valid();
        history.write(i, planes.read(i)->coverage_history(secondary_valid));
    };
    auto shader = device.compile(aggregate, "compact coverage history exact aggregation probe");
    std::vector<CoveragePlaneHistory> actual(pixels);
    stream << shader(input, output).dispatch(make_uint2(width, height))
           << output.download(actual.data()) << synchronize() << commit();
    auto exact = [](float a, float b) { return std::bit_cast<uint>(a) == std::bit_cast<uint>(b); };
    for (uint i = 0u; i < pixels; ++i) {
        const auto &base = full[i]; const auto &value = actual[i];
        bool ambiguous = false;
        for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
            const auto &p = full[layer * pixels + i];
            ambiguous |= p.valid != 0u && (layer > 0u || p.surface.approximate != 0u ||
                         p.surface.hit.is_miss() || p.surface.hit.inst_id == InvalidUI32);
        }
        require(value.valid == base.valid && value.ambiguous == uint(ambiguous), "compact coverage validity/ambiguity differs from full aggregation");
        require(value.stable_branch == base.surface.stable_branch && value.hit.inst_id == base.surface.hit.inst_id &&
                value.hit.prim_id == base.surface.hit.prim_id, "compact coverage hit/branch identity lost");
        for (uint c = 0u; c < 2u; ++c) {
            require(exact(value.hit.bary[c], base.surface.hit.bary[c]) && exact(value.motion[c], base.motion[c]),
                    "compact coverage barycentrics/motion lost precision");
        }
        for (uint c = 0u; c < 3u; ++c) {
            require(exact(value.virtual_position[c], base.surface.virtual_position[c]) &&
                    exact(value.virtual_geometric_normal[c], base.surface.virtual_geometric_normal[c]) &&
                    exact(value.depth_position[c], base.depth_position[c]), "compact coverage geometry lost FP32 precision");
        }
    }
    require(output.size_in_byte() == pixels * 80u && Type::of<CoveragePlaneHistory>()->size() == 80u,
            "compact coverage GPU storage must use aligned80-byte records");
    std::cout << "PASS: compact coverage GPU records match independent full-history aggregation\n";
}

// Run on a fresh fixture before its first rendered frame. Exercise physical
// native buffer extents and restore its original enabled/retained/mode state.
inline void check_allocation(Pipeline &pipeline) {
    auto &fb = *pipeline.frame_buffer();
    auto &stream = pipeline.stream();
    stream << synchronize() << commit();
    const bool enabled = pipeline.renderer().integrator()->stable_planes_enabled();
    const bool retained = fb.stable_plane_history_retained(), compact = fb.compact_coverage_history();
    const bool raw_retained = fb.stable_raw_outputs_retained();
    const size_t pixels = pipeline.pixel_num();
    fb.manage_stable_plane_buffers(true, true, false);
    require(fb.stable_plane_history().size() == pixels * 3u && fb.coverage_history().size() == 1u,
            "ordinary denoisers must retain complete three-plane history");
    const size_t full_bytes = fb.stable_plane_history().view().size_in_byte() + fb.coverage_history().view().size_in_byte();
    fb.manage_stable_plane_buffers(true, true, true);
    require(fb.stable_plane_history().size() == 3u && fb.coverage_history().size() == pixels,
            "coverage-only history must allocate one80-byte record per pixel plus full-history stubs");
    const size_t compact_bytes = fb.stable_plane_history().view().size_in_byte() + fb.coverage_history().view().size_in_byte();
    require(full_bytes - compact_bytes == (pixels - 1u) * (3u * sizeof(StablePlaneHistory) - sizeof(CoveragePlaneHistory)),
            "compact history allocation saving differs from physical buffer extents");
    const uint direct_base = fb.stable_direct_base(), indirect_base = fb.stable_indirect_base();
    const size_t raw_full_bytes = fb.stable_direct().view().size_in_byte() + fb.stable_indirect().view().size_in_byte();
    for (bool keep_raw : {false, true, false}) {
        fb.manage_stable_plane_buffers(true, true, true, keep_raw);
        const size_t expected = keep_raw ? pixels * StablePlaneCount : StablePlaneCount;
        require(fb.stable_raw_outputs_retained() == keep_raw && fb.stable_direct().size() == expected &&
                fb.stable_indirect().size() == expected, "raw stable channel mode must match physical allocations");
        require(fb.stable_direct_base() == direct_base && fb.stable_indirect_base() == indirect_base,
                "raw channel mode changes must preserve registered layer slots");
        for (uint layer = 0u; layer < StablePlaneCount; ++layer) {
            // The base slot exposes the complete array; subsequent slots are
            // bounded single-layer views, matching register_self/register_view.
            const size_t bytes = (layer == 0u ? StablePlaneCount : 1u) * (keep_raw ? pixels : 1u) * sizeof(float4);
            require(pipeline.bindless_array()->buffer_view(direct_base + layer).size == bytes &&
                    pipeline.bindless_array()->buffer_view(indirect_base + layer).size == bytes,
                    "raw channel mode must rebind every layer to its current physical extent");
        }
        require(fb.stable_planes().size() == pixels * StablePlaneCount && fb.coverage_history().size() == pixels,
                "raw output suppression must preserve current planes and coverage history");
    }
    const size_t raw_stub_bytes = fb.stable_direct().view().size_in_byte() + fb.stable_indirect().view().size_in_byte();
    require(raw_stub_bytes == StablePlaneCount * 2u * sizeof(float4) &&
            raw_full_bytes - raw_stub_bytes == (pixels - 1u) * StablePlaneCount * 2u * sizeof(float4),
            "raw stable channel saving must be measured from physical buffer extents");
    fb.manage_stable_plane_buffers(false, false, true);
    require(fb.stable_planes().size() == 3u && fb.stable_plane_history().size() == 3u && fb.coverage_history().size() == 1u,
            "disabled/data-only stable history must retain only bounded stubs");
    fb.manage_stable_plane_buffers(enabled, retained, compact, raw_retained);
    pipeline.upload_bindless_array();
    std::cout << "PASS: compact coverage physical allocation saves " << full_bytes - compact_bytes << " bytes\n";
    std::cout << "PASS: raw stable output physical allocation saves " << raw_full_bytes - raw_stub_bytes << " bytes\n";
}
}
