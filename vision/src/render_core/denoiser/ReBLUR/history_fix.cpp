#include "utils.h"

namespace vision::reblur {
void Filter::compile_history_fix(Device &device) {
    Kernel fix = [](Var<FilterParam> p) {
        ocarina::configure_block(32u, 4u);
        auto guides = p.guide.soa_view_var<Guide>();
        auto current_history = p.history.soa_view_var<TemporalData>();
        $if(!active_tile(p)) { $return(); };
        Uint i = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        auto g = guides.read(i); auto h = current_history.read(i);
        Float4 d = make_float4(p.src_diffuse.read(i)), s = make_float4(p.src_specular.read(i));
        Float4 fast_count = make_float4(h.fast_count);
        Float2 new_fast = fast_count.xy();
        $if(valid_guide(g)) {
            // Both lobes use the same neighborhood, identity and history fetches.
            Float2 count_sum = fast_count.zw(), count_norm = make_float2(1.f);
            Float2 means = make_float2(0.f), moments = make_float2(0.f);
            $for(y, -1, 2) {
                $for(x, -1, 2) {
                    Uint j = pixel_index(pixel + make_int2(x, y), p.size);
                    Float4 nfast = make_float4(current_history.fast_count.read(j));
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
                            Uint j = pixel_index(pixel + make_int2(x, y), p.size);
                            auto ng = guides.read(j);
                            Float2 fast = make_float2(current_history.fast_count.read(j).xy());
                            Float w = same_surface(g, ng).cast<float>();
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
                    Float sum_w = 1.f + count;
                    Float4 sum = value * sum_w;
                    $for(y, -2, 3) {
                        $for(x, -2, 3) {
                            Int2 q = pixel + make_int2(x, y) * stride;
                            $if((x != 0 || y != 0) && abs(x) + abs(y) != 4 && in_screen(q, p.size)) {
                                Uint j = pixel_index(q, p.size); auto ng = guides.read(j);
                                auto nh = current_history.read(j);
                                Float4 nv = make_float4(spec ? p.src_specular.read(j) : p.src_diffuse.read(j));
                                Float w = spatial_weight(p, g, ng, 1.f / (1.f + count), 1.f, spec);
                                Float4 nfast = make_float4(nh.fast_count);
                                w *= 1.f + (spec ? nfast.w : nfast.z);
                                Float norm_h = lerp(min(1.f / (1.f + count), spec_magic(rough)), .0005f, 1.f);
                                w *= exponential_weight((nv.w - value.w) / norm_h);
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
