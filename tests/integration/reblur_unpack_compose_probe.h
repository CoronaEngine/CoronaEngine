#pragma once

#include "render_core/denoiser/ReBLUR/edsl_nrd.h"
#include "render_core/denoiser/ReBLUR/edsl_nrd_common.h"
#include "render_core/denoiser/ReBLUR/edsl_nrd_resources.h"
#include "rhi/resources/dynamic_buffer.h"
#include <array>
#include <bit>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace unpack_compose_probe {
using namespace vision::reblur;
using namespace vision::reblur::edsl;
using namespace ocarina;

inline void require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error("Unpack composition probe: " + message);
}
inline std::string lane(float value) {
    std::ostringstream out;
    out << std::setprecision(9) << value << " [0x" << std::hex << std::bit_cast<uint>(value) << ']';
    return out.str();
}
inline void same(const std::vector<float4> &expected, const std::vector<float4> &actual,
                 const std::string &label) {
    require(expected.size() == actual.size(), label + " size differs");
    for (size_t i = 0; i != expected.size(); ++i) for (uint c = 0; c != 4u; ++c)
        require(std::bit_cast<uint>(expected[i][c]) == std::bit_cast<uint>(actual[i][c]),
                label + " float lane differs at " + std::to_string(i) + ":" + std::to_string(c) +
                ", expected=" + lane(expected[i][c]) + ", actual=" + lane(actual[i][c]));
}

inline void check(Device &device, Stream &stream) {
    const uint2 size = make_uint2(17u, 9u); // Padded 16x8 conversion dispatch.
    const uint n = size.x * size.y;
    auto image_d = device.create_texture2d(size, PixelStorage::HALF4, "compose probe NRD diffuse");
    auto image_s = device.create_texture2d(size, PixelStorage::HALF4, "compose probe NRD specular");
    auto raw_d = device.create_buffer<float4>(n, "compose probe distinct raw D");
    auto raw_s = device.create_buffer<float4>(n, "compose probe distinct raw S");
    auto stable = device.create_buffer<float4>(n, "compose probe stable radiance");
    auto bypass = device.create_buffer<float4>(n * 3u, "compose probe bypass");
    auto reference_d = device.create_buffer<float4>(n * 3u, "compose probe reference D");
    auto reference_s = device.create_buffer<float4>(n * 3u, "compose probe reference S");
    auto actual_d = device.create_buffer<float4>(n * 3u, "compose probe fused D");
    auto actual_s = device.create_buffer<float4>(n * 3u, "compose probe fused S");
    auto reference_total = device.create_buffer<float4>(n, "compose probe reference total");
    auto actual_total = device.create_buffer<float4>(n, "compose probe fused total");
    auto batch_d = device.create_buffer<float4>(n * 3u, "compose probe retired raw D guide cache");
    auto batch_s = device.create_buffer<float4>(n * 3u, "compose probe retired raw S guide cache");
    auto batch_raw_d = device.create_buffer<float4>(n * 3u, "compose probe batch reference raw D");
    auto batch_raw_s = device.create_buffer<float4>(n * 3u, "compose probe batch reference raw S");
    auto batch_total = device.create_buffer<float4>(n, "compose probe batched total");
    std::array<Texture2D, 3> batch_images_d, batch_images_s;
    for (uint layer = 0u; layer != 3u; ++layer) {
        batch_images_d[layer] = device.create_texture2d(size, PixelStorage::HALF4, "compose probe layer NRD diffuse");
        batch_images_s[layer] = device.create_texture2d(size, PixelStorage::HALF4, "compose probe layer NRD specular");
    }
    auto pack_normal = device.create_texture2d(size, PixelStorage::RGB10A2, "compose probe pack normal");
    auto pack_depth = device.create_texture2d(size, PixelStorage::FLOAT1, "compose probe pack depth");
    auto pack_motion = device.create_texture2d(size, PixelStorage::HALF4, "compose probe pack motion");
    auto pack_disocclusion = device.create_texture2d(size, PixelStorage::UNORM1, "compose probe pack disocclusion");
    Kernel seed = [](Var<Dispatch> p) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Int2 position = make_int2(pixel);
        Uint i = pixel.y * p.external.size.x + pixel.x;
        Float v = (i % 7u).cast<float>() + p.constants.gFrameIndex.cast<float>() * 13.f;
        vision::reblur::edsl::Image<float4, Format::RGBA16Float>{p.outputs.t0}.write_in_bounds(position,
            PackRadiance(make_float3(64.f + v, .125f + v * .03125f, 16384.f + v), .125f));
        vision::reblur::edsl::Image<float4, Format::RGBA16Float>{p.outputs.t1}.write_in_bounds(position,
            PackRadiance(make_float3(.0625f + v, 2048.f + v, 3.f + v * .125f), .75f));
    };
    auto seed_shader = device.compile(seed, "Unpack compose probe typed FP16 NRD outputs");
    // Independent exact copy of the old post-Unpack kernel. No fused helper.
    Kernel old_compose = [](Var<Dispatch> p, Var<ComposeParam> cp) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Uint i = pixel.y * p.external.size.x + pixel.x;
        Float3 total = make_float3(0.f);
        $if(cp.layered != 0u) { total = cp.stable_radiance.read(i).xyz() * cp.direct_factor; };
        $for(layer, cp.layer_count) {
            Uint j = layer * (p.external.size.x * p.external.size.y) + i;
            total += cp.diffuse.read(j).xyz() + cp.specular.read(j).xyz() + cp.bypass.read(j).xyz();
        };
        cp.output.write(i, make_float4(total, 1.f));
    };
    auto compose_shader = device.compile(old_compose, "Original Unpack then FP32 Compose oracle");
    Dispatch base;
    base.external.size = size;
    base.constants.gDenoisingRange = 100000.f;
    base.constants.gHitDistParams = make_float4(3.f, .1f, 20.f, -25.f);
    base.outputs.t0 = ImageDesc{image_d, size}; base.outputs.t1 = ImageDesc{image_s, size};
    base.inputs.t0 = base.outputs.t0; base.inputs.t1 = base.outputs.t1;
    stream << seed_shader(base).dispatch(size) << synchronize() << commit();

    std::vector<Guide> guides(n);
    std::vector<CompactGuide> compact(n);
    std::vector<float4> input_d(n), input_s(n), stable_values(n), bypass_values(n * 3u);
    for (uint i = 0u; i != n; ++i) {
        auto &g = guides[i];
        const bool valid = i % 5u != 0u;
        g.position = make_float4(0.f, 0.f, 0.f, valid ? 2.f + float(i % 3u) * 200.f : 0.f);
        g.identity = make_uint4(7u, 3u, 1u, valid ? 1u : 0u);
        g.normal_roughness = make_float4(0.f, 0.f, 1.f, float(i % 3u) * .5f);
        g.diffuse_factor = make_float4(.3f, .03125f, 5.f, 0.f);
        g.specular_factor = make_float4(2.f, .333333343f, .0001f, 0.f);
        compact[i].diffuse_depth = make_float4(g.diffuse_factor.xyz(), g.position.w);
        compact[i].specular_roughness = make_float4(g.specular_factor.xyz(), g.normal_roughness.w);
        // Invalid guides retain substantial raw radiance and distinct w lanes.
        input_d[i] = make_float4(1024.125f + float(i), -.25f, 17.5f, -4.f);
        input_s[i] = make_float4(.375f, 8192.5f + float(i), -.125f, 11.f);
        stable_values[i] = make_float4(123456.75f, -.25f, .0000001f, -91.f);
        for (uint layer = 0u; layer != 3u; ++layer)
            bypass_values[layer * n + i] = make_float4(.125f + float(layer), -.0625f, .03125f, 300.f + float(layer));
    }
    raw_d.upload_immediately(input_d.data()); raw_s.upload_immediately(input_s.data());
    stable.upload_immediately(stable_values.data()); bypass.upload_immediately(bypass_values.data());
    std::array<std::vector<Guide>, 3> layer_guides;
    std::array<ByteBuffer, 3> layer_full_guides, layer_compact_guides;
    std::vector<float4> batch_raw_values_d(n * 3u), batch_raw_values_s(n * 3u);
    for (uint layer = 0u; layer != 3u; ++layer) {
        layer_guides[layer] = guides;
        auto layer_compact = compact;
        for (uint i = 0u; i != n; ++i) {
            auto &g = layer_guides[layer][i];
            const uint category = (i + layer) % 7u;
            const bool valid = category >= 3u;
            g.position.w = category == 0u ? 0.f : (category == 1u ? -2.f :
                (category == 2u ? base.constants.gDenoisingRange : 2.f + float(i % 3u) * 200.f + float(layer)));
            g.identity.w = category == 0u ? 0u : 1u;
            g.diffuse_factor.x += float(layer) * .125f;
            g.specular_factor.y += float(layer) * .0625f;
            layer_compact[i].diffuse_depth = make_float4(g.diffuse_factor.xyz(), g.position.w);
            layer_compact[i].specular_roughness = make_float4(g.specular_factor.xyz(), g.normal_roughness.w);
            require(valid == (g.position.w > 0.f && g.position.w < base.constants.gDenoisingRange),
                "batch guide validity fixture is inconsistent");
            batch_raw_values_d[layer * n + i] = input_d[i] + make_float4(float(layer) * 17.f, 0.f, float(layer), 0.f);
            batch_raw_values_s[layer * n + i] = input_s[i] + make_float4(0.f, float(layer) * 31.f, -float(layer), 0.f);
        }
        HostByteBuffer full_bytes, compact_bytes;
        DynamicBufferLayoutCodec<Guide>::encode(layer_guides[layer].data(), n, full_bytes, {}, DynamicBufferLayout::SOA);
        DynamicBufferLayoutCodec<CompactGuide>::encode(layer_compact.data(), n, compact_bytes, {}, DynamicBufferLayout::SOA);
        layer_full_guides[layer] = device.create_byte_buffer(full_bytes.size(), "compose probe layer full guide");
        layer_compact_guides[layer] = device.create_byte_buffer(compact_bytes.size(), "compose probe layer compact guide");
        layer_full_guides[layer].upload_immediately(full_bytes.data());
        layer_compact_guides[layer].upload_immediately(compact_bytes.data());
    }
    batch_raw_d.upload_immediately(batch_raw_values_d.data()); batch_raw_s.upload_immediately(batch_raw_values_s.data());
    auto deferred_pack = compile_pack(device);
    auto batch_unpack = compile_unpack_batch(device);
    std::vector<float4> expected_d(n * 3u), expected_s(n * 3u), got_d(n * 3u), got_s(n * 3u);
    std::vector<float4> expected_total(n), got_total(n);
    uint variants = 0u;
    bool observed_distinct_cache_and_filtered = false;
    for (bool compact_guide : {false, true}) {
        HostByteBuffer guide_bytes;
        if (compact_guide) DynamicBufferLayoutCodec<CompactGuide>::encode(compact.data(), n, guide_bytes, {}, DynamicBufferLayout::SOA);
        else DynamicBufferLayoutCodec<Guide>::encode(guides.data(), n, guide_bytes, {}, DynamicBufferLayout::SOA);
        auto guide_buffer = device.create_byte_buffer(guide_bytes.size(), "compose probe guide SOA");
        guide_buffer.upload_immediately(guide_bytes.data());
        auto unpack = compile_unpack(device, compact_guide);
        auto fused = compile_unpack_and_compose(device, compact_guide);
        for (uint layers : {1u, 3u}) for (uint alias : {0u, 1u, 2u, 3u}) for (float factor : {0.f, .3f, 1.f}) {
            const uint last = layers - 1u;
            std::vector<float4> initial_d(n * 3u), initial_s(n * 3u);
            for (uint layer = 0u; layer != 3u; ++layer) for (uint i = 0u; i != n; ++i) {
                initial_d[layer * n + i] = make_float4(524288.25f + float(layer), 16.f, .0001f, 41.f + float(layer));
                initial_s[layer * n + i] = make_float4(-524288.f - float(layer), -15.75f, -.00009f, -43.f - float(layer));
            }
            for (uint i = 0u; i != n; ++i) {
                initial_d[last * n + i] = (alias & 1u) ? input_d[i] : make_float4(-777.f);
                initial_s[last * n + i] = (alias & 2u) ? input_s[i] : make_float4(-888.f);
            }
            stream << reference_d.upload(initial_d.data()) << actual_d.upload(initial_d.data())
                   << reference_s.upload(initial_s.data()) << actual_s.upload(initial_s.data());
            Dispatch ref = base, candidate = base;
            ref.external.guide = candidate.external.guide = guide_buffer.descriptor();
            ref.output_alias_mask = candidate.output_alias_mask = alias;
            ref.external.output_diffuse = reference_d.view(last * n, n).descriptor();
            ref.external.output_specular = reference_s.view(last * n, n).descriptor();
            candidate.external.output_diffuse = actual_d.view(last * n, n).descriptor();
            candidate.external.output_specular = actual_s.view(last * n, n).descriptor();
            ref.external.raw_diffuse = (alias & 1u) ? ref.external.output_diffuse : raw_d.descriptor();
            ref.external.raw_specular = (alias & 2u) ? ref.external.output_specular : raw_s.descriptor();
            candidate.external.raw_diffuse = (alias & 1u) ? candidate.external.output_diffuse : raw_d.descriptor();
            candidate.external.raw_specular = (alias & 2u) ? candidate.external.output_specular : raw_s.descriptor();
            ComposeParam cp;
            cp.layer_count = layers; cp.layer_index = last; cp.layered = uint(layers > 1u); cp.direct_factor = factor;
            cp.diffuse = reference_d.descriptor(); cp.specular = reference_s.descriptor();
            cp.bypass = bypass.descriptor(); cp.stable_radiance = stable.descriptor(); cp.output = reference_total.descriptor();
            auto fused_cp = cp;
            fused_cp.diffuse = actual_d.descriptor(); fused_cp.specular = actual_s.descriptor(); fused_cp.output = actual_total.descriptor();
            stream << seed_shader(base).dispatch(size)
                   << unpack(ref).dispatch(size) << compose_shader(ref, cp).dispatch(size)
                   << fused(candidate, fused_cp).dispatch(size)
                   << reference_d.download(expected_d.data()) << reference_s.download(expected_s.data())
                   << actual_d.download(got_d.data()) << actual_s.download(got_s.data())
                   << reference_total.download(expected_total.data()) << actual_total.download(got_total.data())
                   << synchronize() << commit();
            const auto label = std::string(compact_guide ? "compact32" : "full80") + ", layers=" + std::to_string(layers) +
                ", alias=" + std::to_string(alias) + ", factor=" + std::to_string(factor);
            same(expected_d, got_d, label + " diffuse+hit"); same(expected_s, got_s, label + " specular+hit");
            same(expected_total, got_total, label + " FP32 total");
            // n=153 is not a multiple of five; stop before the next invalid
            // sample index155 instead of walking beyond the CPU fixture arrays.
            for (uint i = 0u; i < n; i += 5u) for (uint c = 0u; c != 4u; ++c) {
                const std::string location = label + ", pixel=" + std::to_string(i) + ", lane=" + std::to_string(c);
                require(std::bit_cast<uint>(got_d[last * n + i][c]) == std::bit_cast<uint>(input_d[i][c]),
                    location + " invalid raw diffuse changed: input=" + lane(input_d[i][c]) +
                    ", old=" + lane(expected_d[last * n + i][c]) + ", fused=" + lane(got_d[last * n + i][c]));
                require(std::bit_cast<uint>(got_s[last * n + i][c]) == std::bit_cast<uint>(input_s[i][c]),
                    location + " invalid raw specular changed: input=" + lane(input_s[i][c]) +
                    ", old=" + lane(expected_s[last * n + i][c]) + ", fused=" + lane(got_s[last * n + i][c]));
            }
            // Independent batch oracle: old Unpack publishes every filtered
            // layer, then old Compose sums them once. The candidate's Pack
            // retires raw D/S into compact guides, and its final Unpack reads
            // all retained typed NRD outputs without publishing filtered D/S.
            std::vector<float4> total_sentinel(n, make_float4(-1234567.f, 987654.f, -321.f, 19.f));
            std::vector<float4> initial_cache_d(n * 3u), initial_cache_s(n * 3u);
            std::vector<float4> expected_cache_d(n * 3u), expected_cache_s(n * 3u);
            for (uint layer = 0u; layer < 3u; ++layer) for (uint i = 0u; i < n; ++i) {
                const uint address = layer * n + i;
                initial_cache_d[address] = (alias & 1u) ? initial_d[address] : batch_raw_values_d[address];
                initial_cache_s[address] = (alias & 2u) ? initial_s[address] : batch_raw_values_s[address];
                expected_cache_d[address] = initial_cache_d[address]; expected_cache_s[address] = initial_cache_s[address];
                if (layer >= layers) continue;
                const auto &g = layer_guides[layer][i];
                const bool valid = (g.identity.w & 1u) != 0u && g.position.w > 0.f &&
                    g.position.w < base.constants.gDenoisingRange;
                expected_cache_d[address] = valid ? make_float4(g.diffuse_factor.xyz(), g.position.w) :
                    make_float4(initial_cache_d[address].xyz(), 0.f);
                expected_cache_s[address] = valid ? make_float4(g.specular_factor.xyz(), g.normal_roughness.w) :
                    initial_cache_s[address];
            }
            stream << reference_d.upload(initial_d.data()) << reference_s.upload(initial_s.data())
                   << batch_d.upload(initial_cache_d.data()) << batch_s.upload(initial_cache_s.data())
                   << batch_total.upload(total_sentinel.data());
            ComposeParam batch_reference;
            batch_reference.layer_count = layers; batch_reference.layered = uint(layers > 1u);
            batch_reference.direct_factor = factor;
            batch_reference.diffuse = reference_d.descriptor(); batch_reference.specular = reference_s.descriptor();
            batch_reference.bypass = bypass.descriptor(); batch_reference.stable_radiance = stable.descriptor();
            batch_reference.output = reference_total.descriptor();
            BatchUnpackParam batch;
            batch.composition = batch_reference;
            batch.composition.diffuse = batch_d.descriptor(); batch.composition.specular = batch_s.descriptor();
            batch.composition.output = batch_total.descriptor();
            batch.diffuse0 = {batch_images_d[0], size}; batch.specular0 = {batch_images_s[0], size};
            batch.diffuse1 = {batch_images_d[1], size}; batch.specular1 = {batch_images_s[1], size};
            batch.diffuse2 = {batch_images_d[2], size}; batch.specular2 = {batch_images_s[2], size};
            for (uint layer = 0u; layer < layers; ++layer) {
                Dispatch old_layer = base, packing = base;
                old_layer.external.guide = compact_guide ? layer_compact_guides[layer].descriptor() : layer_full_guides[layer].descriptor();
                old_layer.output_alias_mask = alias;
                old_layer.external.output_diffuse = reference_d.view(layer * n, n).descriptor();
                old_layer.external.output_specular = reference_s.view(layer * n, n).descriptor();
                old_layer.external.raw_diffuse = (alias & 1u) ? old_layer.external.output_diffuse : batch_raw_d.view(layer * n, n).descriptor();
                old_layer.external.raw_specular = (alias & 2u) ? old_layer.external.output_specular : batch_raw_s.view(layer * n, n).descriptor();
                packing.defer_unpack = 1u;
                packing.external.guide = layer_full_guides[layer].descriptor();
                packing.external.raw_diffuse = batch_d.view(layer * n, n).descriptor();
                packing.external.raw_specular = batch_s.view(layer * n, n).descriptor();
                packing.external.output_diffuse = packing.external.raw_diffuse;
                packing.external.output_specular = packing.external.raw_specular;
                packing.outputs.t0 = {batch_images_d[layer], size}; packing.outputs.t1 = {batch_images_s[layer], size};
                packing.outputs.t2 = {pack_normal, size}; packing.outputs.t3 = {pack_depth, size};
                packing.outputs.t4 = {pack_motion, size}; packing.outputs.t5 = {pack_disocclusion, size};
                auto layer_seed = base;
                layer_seed.constants.gFrameIndex = layer;
                layer_seed.outputs.t0 = packing.outputs.t0; layer_seed.outputs.t1 = packing.outputs.t1;
                old_layer.inputs.t0 = packing.outputs.t0; old_layer.inputs.t1 = packing.outputs.t1;
                stream << deferred_pack(packing).dispatch(size)
                       << seed_shader(layer_seed).dispatch(size)
                       << unpack(old_layer).dispatch(size);
            }
            stream << compose_shader(base, batch_reference).dispatch(size)
                   << batch_d.download(got_d.data()) << batch_s.download(got_s.data())
                   << synchronize() << commit();
            same(expected_cache_d, got_d, label + " Pack retired diffuse cache including invalid raw fallback");
            same(expected_cache_s, got_s, label + " Pack retired specular cache including invalid raw fallback");
            stream << batch_unpack(base, batch).dispatch(size)
                   << reference_d.download(expected_d.data()) << reference_s.download(expected_s.data())
                   << batch_d.download(got_d.data()) << batch_s.download(got_s.data())
                   << reference_total.download(expected_total.data()) << batch_total.download(got_total.data())
                   << synchronize() << commit();
            same(expected_total, got_total, label + " batched final FP32 RT");
            // These arrays now contain guides/raw fallback, never filtered D/S.
            same(expected_cache_d, got_d, label + " final batch must not overwrite retired diffuse cache");
            same(expected_cache_s, got_s, label + " final batch must not overwrite retired specular cache");
            for (size_t i = 0u; i < size_t(n) * layers; ++i) for (uint c = 0u; c < 4u; ++c) {
                observed_distinct_cache_and_filtered |= std::bit_cast<uint>(expected_cache_d[i][c]) != std::bit_cast<uint>(expected_d[i][c]) ||
                    std::bit_cast<uint>(expected_cache_s[i][c]) != std::bit_cast<uint>(expected_s[i][c]);
            }
            ++variants;
        }
    }
    require(variants == 48u, "fixture matrix incomplete");
    require(observed_distinct_cache_and_filtered,
        "batch fixture must distinguish retired guide/raw cache from omitted filtered D/S outputs");
    // Standalone/default dispatch must still publish complete distinct output
    // buffers. Invalid guides make its raw passthrough contract byte-exact.
    std::vector<Guide> invalid_guides(n);
    HostByteBuffer invalid_bytes;
    DynamicBufferLayoutCodec<Guide>::encode(invalid_guides.data(), n, invalid_bytes, {}, DynamicBufferLayout::SOA);
    auto invalid_buffer = device.create_byte_buffer(invalid_bytes.size(), "compose probe default invalid guides");
    invalid_buffer.upload_immediately(invalid_bytes.data());
    EdslReblur default_adapter(device);
    NativeCameraParams camera;
    camera.world_to_view = camera.world_to_view_prev = camera.view_to_clip = camera.view_to_clip_prev =
        {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    FilterParam default_input;
    default_input.size = size; default_input.guide = invalid_buffer.descriptor();
    default_input.raw_diffuse = raw_d.descriptor(); default_input.raw_specular = raw_s.descriptor();
    default_input.output_diffuse = actual_d.view(0u, n).descriptor();
    default_input.output_specular = actual_s.view(0u, n).descriptor();
    default_input.unproject = 2.f / float(size.y);
    std::vector<float4> default_d(n), default_s(n), unchanged_raw_d(n), unchanged_raw_s(n);
    std::vector<float4> default_sentinel(n, make_float4(-97531.f));
    stream << actual_d.view(0u, n).upload(default_sentinel.data()) << actual_s.view(0u, n).upload(default_sentinel.data())
           << default_adapter.dispatch(default_input, camera, 0u, Settings{}, true)
           << actual_d.view(0u, n).download(default_d.data()) << actual_s.view(0u, n).download(default_s.data())
           << raw_d.download(unchanged_raw_d.data()) << raw_s.download(unchanged_raw_s.data())
           << synchronize() << commit();
    same(input_d, default_d, "default nonalias output diffuse raw fallback");
    same(input_s, default_s, "default nonalias output specular raw fallback");
    same(input_d, unchanged_raw_d, "default nonalias dispatch must retain raw diffuse");
    same(input_s, unchanged_raw_s, "default nonalias dispatch must retain raw specular");
    std::cout << "PASS: retained-output last-layer fusion and final-output-only batch match old Unpack+Compose bitwise in " << variants
              << " variants; production Pack preserves valid guides and invalid raw fallback; default nonalias outputs remain retained\n";
}
} // namespace unpack_compose_probe
