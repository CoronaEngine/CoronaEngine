#include "utils.h"

namespace vision::reblur {
void Filter::compile_stabilization(Device &device) {
    Kernel stabilization = [](Var<FilterParam> p) {
        ocarina::configure_block(16u, 8u);
        auto guides = guide_view(p.guide);
        auto current_history = p.history.soa_view_var<TemporalData>();
        auto previous_geometry = geometry_history_view(p.prev_guide);
        auto previous_history = p.prev_history.soa_view_var<PersistentHistory>();
        Uint2 local = ocarina::thread_idx().xy();
        Int2 pixel = make_int2(dispatch_idx().xy());
        Uint i = pixel.y.cast<uint>() * p.size.x + pixel.x.cast<uint>();
        // This branch is uniform: each 16x8 block belongs to one activity tile.
        $if(!active_tile(p)) {
            $if(in_screen(pixel, p.size)) {
                // Reprojection checks this validity before consuming old history.
                previous_geometry.identity.write(i, make_uint4(0u));
                p.output_diffuse.write(i, p.raw_diffuse.read(i));
                p.output_specular.write(i, p.raw_specular.read(i));
            };
            $return();
        };
        Int2 origin = pixel - make_int2(local);
        ocarina::SharedArray<float4, 180> cached_luma;
        ocarina::SharedArray<uint4, 180> cached_identity;
        // Halo pixels use the same screen clamping as the original 3x3 reads.
        $for(entry, local.y * 16u + local.x, 180u, 128u) {
            Int2 q = origin + make_int2(entry % 18u, entry / 18u) - 1;
            Uint j = pixel_index(q, p.size);
            Uint4 identity = guides.identity.read(j);
            identity.w = select(guides.position.read(j).w > 0.f, identity.w, 0u);
            Float2 luma = make_float2(0.f);
            $if(identity.w != 0u) {
                luma = make_float2(make_float4(p.src_diffuse.read(j)).x, make_float4(p.src_specular.read(j)).x);
            };
            cached_luma.write(entry, make_float4(luma, 0.f, 0.f));
            cached_identity.write(entry, identity);
        };
        ocarina::synchronize_block();
        $if(!in_screen(pixel, p.size)) { $return(); };
        Uint cache_center = (local.y + 1u) * 18u + local.x + 1u;
        auto g = guides.read(i);
        $if(!valid_guide(g)) {
            previous_geometry.identity.write(i, make_uint4(0u));
            p.output_diffuse.write(i, p.raw_diffuse.read(i));
            p.output_specular.write(i, p.raw_specular.read(i));
            $return();
        };
        auto h = current_history.read(i);
        Float4 d = make_float4(p.src_diffuse.read(i)), s = make_float4(p.src_specular.read(i));
        Var<PersistentHistory> saved;
        // Reprojection consumes the post-blur signal, before stabilization.
        saved.diffuse = store_half(d); saved.specular = store_half(s);
        Float4 fast_count = make_float4(make_float2(p.fast.read(i)), make_float2(h.fast_count.zw()));
        Float4 stabilized_history = make_float4(h.stabilized);
        $if(valid_guide(g)) {
            Float2 means = make_float2(0.f), moments = make_float2(0.f);
            Float sum = 0.f;
            $for(y, -1, 2) {
                $for(x, -1, 2) {
                    Uint j = (cache_center.cast<int>() + y * 18 + x).cast<uint>();
                    Uint4 identity = cached_identity.read(j);
                    Float2 v = cached_luma.read(j).xy();
                    Float w = (identity.w != 0u && all(g.identity.xyz() == identity.xyz())).cast<float>();
                    means += v * w; moments += v * v * w; sum += w;
                };
            };
            means /= max(sum, 1.f); moments /= max(sum, 1.f);
            for (bool spec : {false, true}) {
                Float4 value = spec ? s : d;
                Float mean = spec ? means.y : means.x, m2 = spec ? moments.y : moments.x;
                Float sigma = sqrt(max(m2 - mean * mean, 0.f));
                Float count = spec ? fast_count.w : fast_count.z;
                Float previous = spec ? stabilized_history.y : stabilized_history.x;
                Float quality = spec ? stabilized_history.w : stabilized_history.z;
                Float lag = antilag(previous, mean, sigma, count, p);
                Float weight = quality * count / (1.f + count);
                Float clipped = clamp(previous, mean - sigma * (1.f + 3.f * p.frame_rate_scale * weight), mean + sigma * (1.f + 3.f * p.frame_rate_scale * weight));
                Float stabilized = lerp(min(weight * lag, p.stabilization), value.x, clipped);
                value = change_luma(value, stabilized);
                Float next_count = min(lerp(lag, min(count + 1.f, p.fix_frames), count + 1.f), p.max_history);
                if (spec) { s = value; stabilized_history.y = stabilized; fast_count.w = next_count; }
                else { d = value; stabilized_history.x = stabilized; fast_count.z = next_count; }
            }
        };
        // Spatial passes read separate scratch buffers. No neighbour reads this write target.
        h.fast_count = store_half(fast_count); h.stabilized = store_half(stabilized_history);
        current_history.fast_count.write(i, h.fast_count);
        current_history.stabilized.write(i, h.stabilized);
        // All previous-frame neighborhood reads finished in TemporalAccumulation.
        saved.fast_count = h.fast_count; saved.stabilized = h.stabilized.xy(); saved.hit_distance = h.tracking.w;
        previous_history.write(i, saved);
        Float4 output_d = p.raw_diffuse.read(i), output_s = p.raw_specular.read(i);
        $if(valid_guide(g)) {
            // Preserve the old Stabilization -> FP16 scratch -> Unpack rounding.
            d = make_float4(store_half(d)); s = make_float4(store_half(s));
            output_d = make_float4(to_rgb(d.xyz()) * max(g.diffuse_factor.xyz(), make_float3(1e-3f)), d.w * hit_scale(g.position.w, 1.f));
            output_s = make_float4(to_rgb(s.xyz()) * max(g.specular_factor.xyz(), make_float3(1e-3f)), s.w * hit_scale(g.position.w, g.normal_roughness.w));
        };
        p.output_diffuse.write(i, output_d); p.output_specular.write(i, output_s);
        Var<GeometryHistory> geometry;
        geometry.position = g.position; geometry.normal_roughness = g.normal_roughness; geometry.identity = g.identity;
        previous_geometry.write(i, geometry);
    };
    stabilize_ = device.compile(stabilization, "REBLUR-TemporalStabilization");
}
}
