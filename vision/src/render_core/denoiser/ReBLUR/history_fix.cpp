#include "utils.h"

namespace vision::reblur {
void Filter::compile_history_fix(Device &device) {
    Kernel fix = [](Var<FilterParam> p) {
        ocarina::configure_block(16u, 8u);
        auto guides = guide_view(p.guide);
        auto current_history = p.history.soa_view_var<TemporalData>();
        // A 16x8 block lies wholly within one 16x16 activity tile.
        $if(!active_tile(p)) { $return(); };
        Uint2 local = ocarina::thread_idx().xy();
        Int2 pixel = make_int2(dispatch_idx().xy());
        Int2 origin = pixel - make_int2(local);
        ocarina::SharedArray<float4, 384> cached_fast;
        ocarina::SharedArray<uint4, 384> cached_identity;
        ocarina::SharedArray<uint4, 128> cached_identity_mismatch;
        ocarina::SharedArray<uint4, 4> cached_identity_mismatch_group;
        Uint local_index = local.y * 16u + local.x;
        Uint4 reference_identity = guides.identity.read(pixel_index(origin, p.size));
        Uint identity_mismatch = 0u;
        // All lanes prepare the clamped 24x16 footprint, including partial blocks.
        $for(entry, local_index, 384u, 128u) {
            Int2 q = origin + make_int2(entry % 24u, entry / 24u) - 4;
            Uint j = pixel_index(q, p.size);
            Uint4 identity = guides.identity.read(j);
            identity.w = select(guides.position.read(j).w > 0.f, identity.w, 0u);
            identity_mismatch |= (identity.w == 0u || any(identity.xyz() != reference_identity.xyz())).cast<uint>();
            Float4 fast_count = make_float4(0.f);
            $if(identity.w != 0u) { fast_count = make_float4(current_history.fast_count.read(j)); };
            cached_fast.write(entry, fast_count);
            cached_identity.write(entry, identity);
        };
        cached_identity_mismatch.write(local_index, make_uint4(identity_mismatch));
        ocarina::synchronize_block();
        // Reduce by local invocation index, independent of the backend's
        // native subgroup-to-invocation mapping. Source and result storage are
        // separate so no reducer can overwrite another reducer's inputs.
        $if(local_index < 4u) {
            Uint group_mismatch = 0u;
            $for(entry, 32u) {
                group_mismatch |= cached_identity_mismatch.read(local_index * 32u + entry).x;
            };
            cached_identity_mismatch_group.write(local_index, make_uint4(group_mismatch));
        };
        ocarina::synchronize_block();
        // A uniform footprint makes every anti-firefly identity weight exactly
        // one. The mixed/invalid footprint retains the original per-tap check.
        Bool uniform_identity = cached_identity_mismatch_group.read(0u).x == 0u &&
                                cached_identity_mismatch_group.read(1u).x == 0u &&
                                cached_identity_mismatch_group.read(2u).x == 0u &&
                                cached_identity_mismatch_group.read(3u).x == 0u;
        $if(!in_screen(pixel, p.size)) { $return(); };
        Uint i = pixel.y.cast<uint>() * p.size.x + pixel.x.cast<uint>();
        Uint cache_center = (local.y + 4u) * 24u + local.x + 4u;
        auto g = read_guide_geometry(guides, i);
        $if(!valid_guide(g)) { $return(); };
        Float4 d = make_float4(p.src_diffuse.read(i)), s = make_float4(p.src_specular.read(i));
        Float4 fast_count = cached_fast.read(cache_center);
        Float2 new_fast = fast_count.xy();
        $if(valid_guide(g)) {
            // Both lobes use the same neighborhood, identity and history fetches.
            Float2 count_sum = fast_count.zw(), count_norm = make_float2(1.f);
            Float2 means = make_float2(0.f), moments = make_float2(0.f);
            $for(y, -1, 2) {
                $for(x, -1, 2) {
                    Uint j = (cache_center.cast<int>() + y * 24 + x).cast<uint>();
                    Float4 nfast = cached_fast.read(j);
                    means += nfast.xy(); moments += nfast.xy() * nfast.xy();
                    $if(x != 0 || y != 0) {
                        Float2 accept = make_float2(nfast.zw() >= fast_count.zw());
                        count_sum += saturate(nfast.zw() / (p.fix_frames + 1e-6f)) * accept;
                        count_norm += accept;
                    };
                };
            };
            Float2 firefly_mean = make_float2(0.f), firefly_moment = make_float2(0.f);
            Float firefly_weight = 0.f;
            $if(p.anti_firefly != 0u && p.max_radius > 0.f) {
                $for(y, -4, 5) {
                    $for(x, -4, 5) {
                        $if(abs(x) > 1 || abs(y) > 1) {
                            Uint j = (cache_center.cast<int>() + y * 24 + x).cast<uint>();
                            Float2 fast = cached_fast.read(j).xy();
                            Float w = 1.f;
                            $if(!uniform_identity) {
                                Uint4 identity = cached_identity.read(j);
                                w = (identity.w != 0u && all(g.identity.xyz() == identity.xyz())).cast<float>();
                            };
                            firefly_mean += fast * w; firefly_moment += fast * fast * w;
                            firefly_weight += w;
                        };
                    };
                };
            };
            for (bool spec : {false, true}) {
                Float count = spec ? fast_count.w : fast_count.z;
                Float4 value = spec ? s : d;
                Float rough = spec ? g.normal_roughness.w : Float{1.f};
                Float count_avg = spec ? count_sum.y : count_sum.x;
                Float norm = spec ? count_norm.y : count_norm.x;
                Float mean = spec ? means.y : means.x, moment = spec ? moments.y : moments.x;
                Int stride = floor(p.fix_stride / (2.f + count_avg / norm * p.fix_frames)).cast<int>();
                $if(count < p.fix_frames && stride > 0) {
                    Float repair_support = 0.f, raw_center = 0.f;
                    if (spec) {
                        repair_support = 1.f - smooth01(linear_step(.02f, .1f, rough));
                        $if(repair_support > 0.f) {
                            Float jxx = 0.f, jxy = 0.f, jyy = 0.f;
                            // Local directional coherence rejects isotropic noise and
                            // isolated outliers while retaining narrow reflected lines.
                            $for(cy, -1, 1) {
                                $for(cx, -1, 1) {
                                    Uint cell = (cache_center.cast<int>() + cy * 24 + cx).cast<uint>();
                                    Bool accepts = true;
                                    $if(!uniform_identity) {
                                        for (uint offset : {0u, 1u, 24u, 25u}) {
                                            Uint4 identity = cached_identity.read(cell + offset);
                                            accepts &= identity.w != 0u && all(g.identity.xyz() == identity.xyz());
                                        }
                                    };
                                    $if(accepts) {
                                        Float a = cached_fast.read(cell).y, b = cached_fast.read(cell + 1u).y;
                                        Float c = cached_fast.read(cell + 24u).y, e = cached_fast.read(cell + 25u).y;
                                        Float gx = .5f * (b + e - a - c), gy = .5f * (c + e - a - b);
                                        jxx += gx * gx; jxy += gx * gy; jyy += gy * gy;
                                    };
                                };
                            };
                            Float coherence = saturate(sqrt((jxx - jyy) * (jxx - jyy) + 4.f * jxy * jxy) /
                                                       (jxx + jyy + 1e-6f));
                            repair_support *= coherence;
                            $if(repair_support > 0.f) {
                                raw_center = pack_signal(p.raw_specular.read(i), guides.specular_factor.read(i).xyz(),
                                                         g.position.w, rough).x;
                            };
                        };
                    }
                    Float sum_w = 1.f + count;
                    Float4 sum = value * sum_w;
                    $for(y, -2, 3) {
                        $for(x, -2, 3) {
                            Int2 q = pixel + make_int2(x, y) * stride;
                            $if((x != 0 || y != 0) && abs(x) + abs(y) != 4 && in_screen(q, p.size)) {
                                Uint j = pixel_index(q, p.size); auto ng = read_guide_geometry(guides, j);
                                Float4 nv = make_float4(0.f), nfast = make_float4(0.f);
                                $if(valid_guide(ng)) {
                                    nv = make_float4(spec ? p.src_specular.read(j) : p.src_diffuse.read(j));
                                    nfast = make_float4(current_history.fast_count.read(j));
                                };
                                Float w = spatial_weight(p, g, ng, 1.f / (1.f + count), 1.f, spec);
                                w *= 1.f + (spec ? nfast.w : nfast.z);
                                Float norm_h = lerp(min(1.f / (1.f + count), spec_magic(rough)), .0005f, 1.f);
                                w *= exponential_weight((nv.w - value.w) / norm_h);
                                if (spec) {
                                    $if(repair_support > 0.f && valid_guide(ng)) {
                                        Float a = fast_count.y, b = nfast.y;
                                        Float raw_neighbor = pack_signal(p.raw_specular.read(j), guides.specular_factor.read(j).xyz(),
                                                                         ng.position.w, ng.normal_roughness.w).x;
                                        Float noise = abs(raw_center - a) + abs(raw_neighbor - b);
                                        Float difference = (a - b) / (.2f * (a + b) + noise + 1e-6f);
                                        Float edge = exponential_weight(difference * difference);
                                        w *= lerp(repair_support, 1.f, edge);
                                    };
                                }
                                sum += nv * w; sum_w += w;
                            };
                        };
                    };
                    value = sum / sum_w;
                };
                Float fast_center = spec ? fast_count.y : fast_count.x;
                Float corrected_fast = lerp(saturate(count / (p.fix_frames + 1e-6f)), value.x, fast_center);
                if (spec) new_fast.y = corrected_fast;
                else new_fast.x = corrected_fast;
                Float luma = value.x;
                $if(firefly_weight > 0.f) {
                    Float m1 = (spec ? firefly_mean.y : firefly_mean.x) / firefly_weight;
                    Float m2 = (spec ? firefly_moment.y : firefly_moment.x) / firefly_weight;
                    Float sigma = 2.f * sqrt(max(m2 - m1 * m1, 0.f));
                    luma = clamp(luma, max(m1 - sigma, 0.f), m1 + sigma);
                };
                mean /= 9.f; moment /= 9.f;
                Float sigma = 2.f * sqrt(max(moment - mean * mean, 0.f));
                Float clamped = clamp(luma, max(mean - sigma, 0.f), mean + sigma);
                luma = lerp(1.f / (1.f + (p.fast_history < p.max_history).cast<float>() * count * 2.f), clamped, luma);
                value = change_luma(value, luma);
                if (spec) s = value;
                else d = value;
            }
        };
        p.fast.write(i, store_half(new_fast));
        p.dst_diffuse.write(i, store_half(d)); p.dst_specular.write(i, store_half(s));
    };
    fix_ = device.compile(fix, "REBLUR-HistoryFix");
}
}
