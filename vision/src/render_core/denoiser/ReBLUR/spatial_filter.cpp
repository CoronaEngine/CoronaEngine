#include "utils.h"

namespace vision::reblur {
Shader<void(FilterParam)> Filter::compile_spatial_pass(Device &device, uint stage) {
    Kernel kernel = [stage](Var<FilterParam> p) {
        ocarina::configure_block(32u, 8u);
        auto guides = p.guide.soa_view_var<Guide>();
        auto current_history = p.history.soa_view_var<TemporalData>();
        Uint i = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        Uint2 tile = dispatch_idx().xy() / 16u;
        $if(p.tiles.read(tile.y * ((p.size.x + 15u) / 16u) + tile.x) == 0u) {
            if (stage == 0u) {
                // Pack and PrePass initialize both signals; later passes keep them zero.
                p.dst_diffuse.write(i, make_half4(0.f)); p.dst_specular.write(i, make_half4(0.f));
                p.tracking.write(i, 0.f);
            }
            $return();
        };
        auto g = guides.read(i);
        Float4 d = make_float4(p.src_diffuse.read(i)), s = make_float4(p.src_specular.read(i));
        Float tracking = s.w * hit_scale(g.position.w, g.normal_roughness.w);
        $if(valid_guide(g)) {
            Float2 counts = make_float2(0.f);
            if (stage != 0u) counts = make_float2(current_history.fast_count.read(i).zw());
            Float3 N = g.normal_roughness.xyz();
            Float3 V = normalize(p.camera.as_vec3() - g.position.xyz());
            Float NoV = abs(dot(N, V));
            Float r = g.normal_roughness.w;
            Float pixel_support = 0.f, normal_footprint = 0.f;
            if (stage == 2u) {
                // A narrow lobe still covers a pixel. Estimate its normal
                // footprint from continuous curvature, taking the smaller
                // one-sided slope so a crease cannot widen the filter.
                pixel_support = 1.f - smooth01(linear_step(.02f, .1f, r));
                $if(pixel_support > 0.f) {
                    Float2 slope = make_float2(0.f);
                    for (uint axis = 0u; axis < 2u; ++axis) {
                        Int2 step = axis == 0u ? make_int2(1, 0) : make_int2(0, 1);
                        auto a = guides.read(pixel_index(pixel - step, p.size));
                        auto b = guides.read(pixel_index(pixel + step, p.size));
                        slope[axis] = select(same_surface(g, a) && same_surface(g, b),
                            min(direction_angle(N, a.normal_roughness.xyz()), direction_angle(N, b.normal_roughness.xyz())), 0.f);
                    }
                    normal_footprint = 2.f * min(length(slope), .1f) * pixel_support;
                };
            }
            Float Dfactor = dominant_factor(NoV, r);
            Float3 D = normalize(lerp(Dfactor, N, 2.f * dot(N, V) * N - V));
            Float NoD = abs(dot(N, D));
            Float frustum = frustum_size(p, g);
            Float angle = (p.frame % 32u).cast<float>() * 2.39996323f + static_cast<float>(stage) * 1.04719755f;
            Float cs = cos(angle), sn = sin(angle);
            constexpr std::array<float2, 8> taps{{{-1.f, 0.f}, {0.f, 1.f}, {1.f, 0.f}, {0.f, -1.f},
                {-.35355339f, .35355339f}, {.35355339f, .35355339f}, {.35355339f, -.35355339f}, {-.35355339f, -.35355339f}}};
            for (bool spec : {false, true}) {
                Float4 center = spec ? s : d;
                Float rough = spec ? r : Float{1.f};
                Float count = spec ? counts.y : counts.x;
                Float smc = spec_magic(rough);
                Float h = center.w * hit_scale(g.position.w, rough);
                Float hfactor = saturate(h / frustum);
                Float alpha = 1.f / 11.f;
                Float radius = spec ? p.specular_prepass : p.diffuse_prepass;
                Float fraction_scale = 2.f;
                if (stage != 0u) {
                    Float fade = linear_step(p.fix_frames * 2.f / 3.f + 1e-6f, p.fix_frames * 4.f / 3.f + 2e-6f, count);
                    Float boost = (1.f - fade) * (1.f - pow(NoV, 5.f)) * smc;
                    alpha = 1.f / (1.f + (1.f - boost) * count);
                    radius = p.max_radius;
                    fraction_scale = stage == 1u ? 1.f : .5f;
                }
                Float area = rough * hfactor * (stage == 0u ? Float{1.f} : alpha);
                radius *= sqrt(area);
                if (stage == 0u && spec) {
                    Float lobe_radius = h * NoD * lobe_tan(r, .3f);
                    radius = min(radius, lobe_radius / max(p.unproject * (g.position.w + h * Dfactor), 1e-6f));
                }
                if (stage == 2u) radius *= 2.f;
                radius = max(radius, p.min_radius * smc);
                Float base_radius = radius;
                if (stage == 2u && spec) radius = max(radius, min(p.min_radius, 1.f) * pixel_support);
                // A zero radius setting is an exact spatial bypass for evaluation.
                if (stage == 0u) radius *= (spec ? p.specular_prepass : p.diffuse_prepass) != 0.f;
                else { radius *= p.max_radius != 0.f; base_radius *= p.max_radius != 0.f; }
                Float4 sum = center;
                Float weight_sum = 1.f;
                Float min_hit_weight = p.min_hit_weight * fraction_scale * smc;
                if (stage != 0u) min_hit_weight *= sqrt(alpha);
                Float hit_norm = lerp(min(alpha, smc), .0005f, 1.f);
                for (uint k = 0; k < taps.size(); ++k) {
                    Float2 tap = make_float2(taps[k]);
                    Float2 direction = make_float2(cs * tap.x - sn * tap.y, sn * tap.x + cs * tap.y);
                    Float2 offset = direction * radius;
                    Int2 q = make_int2(floor(make_float2(pixel) + .5f + offset));
                    $if(in_screen(q, p.size)) {
                        Uint j = pixel_index(q, p.size); auto ng = guides.read(j);
                        Float4 value = make_float4(spec ? p.src_specular.read(j) : p.src_diffuse.read(j));
                        Float w = spatial_weight(p, g, ng, alpha, fraction_scale, spec);
                        if (stage == 0u && spec) {
                            Float hs = value.w * hit_scale(ng.position.w, ng.normal_roughness.w);
                            Float dist = length(ng.position.xyz() - g.position.xyz()) + 1e-6f;
                            // Stable integer hash; min tracking distance accepts geometry-consistent samples.
                            Uint hash = (i * 747796405u + p.frame * 2891336453u + k * 277803737u);
                            hash = ((hash >> ((hash >> 28u) + 4u)) ^ hash) * 277803737u;
                            Float random = (hash & 0xffffu).cast<float>() / 65536.f;
                            $if(hs > 0.f && random < w * saturate(hs / dist)) {
                                tracking = select(tracking > 0.f, min(tracking, hs), hs);
                            };
                        }
                        Float hw = exponential_weight((value.w - center.w) / hit_norm);
                        w *= lerp(hw, min_hit_weight, 1.f);
                        if (stage == 2u && spec) {
                            $if(pixel_support > 0.f) {
                                Float expanded = spatial_weight(p, g, ng, alpha, fraction_scale, true, normal_footprint);
                                // Secondary-hit variation cannot remove pixel
                                // support; wider taps keep the original test.
                                Float local_hit = max(hw, pixel_support * (dot(offset, offset) <= 1.f).cast<float>());
                                expanded *= lerp(local_hit, min_hit_weight, 1.f);
                                Int2 original_q = make_int2(floor(make_float2(pixel) + .5f + direction * base_radius));
                                // A tap moved by the radius floor is a new
                                // contribution, not an old weight to preserve.
                                Float base = select(all(q == original_q), w, 0.f);
                                $if(expanded > base) {
                                    // Raw-to-fast innovation distinguishes noise
                                    // from a stable, narrow luminance edge.
                                    Float a = make_float2(p.fast.read(i)).y, b = make_float2(p.fast.read(j)).y;
                                    Float raw_a = pack_signal(p.raw_specular.read(i), g.specular_factor.xyz(), g.position.w, r).x;
                                    Float raw_b = pack_signal(p.raw_specular.read(j), ng.specular_factor.xyz(), ng.position.w, ng.normal_roughness.w).x;
                                    Float noise = abs(raw_a - a) + abs(raw_b - b);
                                    Float difference = (a - b) / (.2f * (a + b) + noise + 1e-6f);
                                    Float edge = exponential_weight(difference * difference);
                                    // Guard all added support, including the
                                    // roughness taper, without weakening the
                                    // original normal/depth/hit rejection.
                                    w = base + (expanded - base) * edge;
                                };
                            };
                        }
                        w *= exp(-.66f * dot(tap, tap));
                        sum += value * w; weight_sum += w;
                    };
                }
                if (spec) s = sum / weight_sum;
                else d = sum / weight_sum;
            }
        };
        p.dst_diffuse.write(i, store_half(d)); p.dst_specular.write(i, store_half(s));
        if (stage == 0u) p.tracking.write(i, tracking);
    };
    return device.compile(kernel, stage == 0u ? "REBLUR-PrePass" : stage == 1u ? "REBLUR-Blur" : "REBLUR-PostBlur");
}
void Filter::compile_spatial(Device &device) {
    blur_ = compile_spatial_pass(device, 1u);
    post_blur_ = compile_spatial_pass(device, 2u);
}
}
