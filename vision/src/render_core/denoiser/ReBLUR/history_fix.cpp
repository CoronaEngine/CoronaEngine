#include "utils.h"

namespace vision::reblur {
void Filter::compile_history_fix(Device &device) {
    Kernel fix = [](Var<FilterParam> p) {
        Uint i = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        auto g = p.guide.read(i); auto h = p.history.read(i);
        Float4 d = p.src_diffuse.read(i), s = p.src_specular.read(i);
        Float2 new_fast = h.fast_count.xy();
        $if(valid_guide(g)) {
            for (bool spec : {false, true}) {
                Float count = spec ? h.fast_count.w : h.fast_count.z;
                Float4 value = spec ? s : d;
                Float rough = spec ? g.normal_roughness.w : Float{1.f};
                Float count_avg = count, norm = 1.f;
                Float mean = 0.f, moment = 0.f;
                $for(y, -1, 2) {
                    $for(x, -1, 2) {
                        Uint j = pixel_index(pixel + make_int2(x, y), p.size);
                        auto nh = p.history.read(j);
                        Float ncount = spec ? nh.fast_count.w : nh.fast_count.z;
                        Float fast = spec ? nh.fast_count.y : nh.fast_count.x;
                        mean += fast; moment += fast * fast;
                        $if((x != 0 || y != 0) && ncount >= count) {
                            count_avg += saturate(ncount / (p.fix_frames + 1e-6f)); norm += 1.f;
                        };
                    };
                };
                Int stride = floor(p.fix_stride / (2.f + count_avg / norm * p.fix_frames)).cast<int>();
                $if(count < p.fix_frames && stride > 0) {
                    Float sum_w = 1.f + count;
                    Float4 sum = value * sum_w;
                    $for(y, -2, 3) {
                        $for(x, -2, 3) {
                            Int2 q = pixel + make_int2(x, y) * stride;
                            $if((x != 0 || y != 0) && abs(x) + abs(y) != 4 && in_screen(q, p.size)) {
                                Uint j = pixel_index(q, p.size); auto ng = p.guide.read(j);
                                auto nh = p.history.read(j);
                                Float4 nv = spec ? p.src_specular.read(j) : p.src_diffuse.read(j);
                                Float w = spatial_weight(p, g, ng, 1.f / (1.f + count), 1.f, spec);
                                w *= 1.f + (spec ? nh.fast_count.w : nh.fast_count.z);
                                Float norm_h = lerp(min(1.f / (1.f + count), spec_magic(rough)), .0005f, 1.f);
                                w *= exponential_weight((nv.w - value.w) / norm_h);
                                sum += nv * w; sum_w += w;
                            };
                        };
                    };
                    value = sum / sum_w;
                };
                Float fast_center = spec ? h.fast_count.y : h.fast_count.x;
                Float corrected_fast = lerp(saturate(count / (p.fix_frames + 1e-6f)), value.x, fast_center);
                if (spec) new_fast.y = corrected_fast;
                else new_fast.x = corrected_fast;
                Float luma = value.x;
                $if(p.anti_firefly != 0u && p.max_radius > 0.f) {
                    Float m1 = 0.f, m2 = 0.f, weight = 0.f;
                    $for(y, -4, 5) {
                        $for(x, -4, 5) {
                            $if(abs(x) > 1 || abs(y) > 1) {
                                Uint j = pixel_index(pixel + make_int2(x, y), p.size);
                                auto ng = p.guide.read(j); auto nh = p.history.read(j);
                                Float f = spec ? nh.fast_count.y : nh.fast_count.x;
                                Float w = same_surface(g, ng).cast<float>();
                                m1 += f * w; m2 += f * f * w; weight += w;
                            };
                        };
                    };
                    $if(weight > 0.f) {
                        m1 /= weight; m2 /= weight;
                        Float sigma = 2.f * sqrt(max(m2 - m1 * m1, 0.f));
                        luma = clamp(luma, max(m1 - sigma, 0.f), m1 + sigma);
                    };
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
        p.fast.write(i, new_fast);
        p.dst_diffuse.write(i, d); p.dst_specular.write(i, s);
    };
    fix_ = device.compile(fix, "REBLUR-HistoryFix");
}
}
