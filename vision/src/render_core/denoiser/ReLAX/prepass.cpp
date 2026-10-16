#include "filter.h"
#include "utils.h"

namespace vision::relax {
void Filter::compile_prepass(Device &device) {
    Kernel classify = [](Var<FilterParam> p) {
        Uint tile = dispatch_id(), width = (p.size.x + 15u) / 16u;
        Uint2 origin = make_uint2(tile % width, tile / width) * 16u;
        Bool active = false;
        $for(y, 0u, 16u) {
            $for(x, 0u, 16u) {
                Uint2 pixel = origin + make_uint2(x, y);
                $if(all(pixel < p.size)) { active |= valid_guide(p.guide.read(pixel.y * p.size.x + pixel.x)); };
            };
        };
        p.tiles.write(tile, cast<uint>(active));
    };
    classify_ = device.compile(classify, "RELAX-ClassifyTiles");

    Kernel reconstruct = [](Var<FilterParam> p) {
        Uint idx = dispatch_id(); Int2 pos = make_int2(dispatch_idx().xy());
        auto center = p.guide.read(idx);
        Float4 d = p.src_diffuse.read(idx), s = p.src_specular.read(idx);
        $if(valid_guide(center)) {
            Float wd = select(d.w != 0.f, 1000.f, 0.f), ws = select(s.w != 0.f, 1000.f, 0.f);
            Float hd = d.w * wd, hs = s.w * ws;
            Float npd = 1.f / atan(lobe_tan(1.f, .75f));
            Float nps = 1.f / max(atan(lobe_tan(center.normal_roughness.w, .75f)), .5f / 255.f);
            Int radius = select(p.settings.reconstruction == 2u, 2, 1);
            $for(y, -2, 3) {
                $for(x, -2, 3) {
                    $if((x != 0 || y != 0) && abs(x) <= radius && abs(y) <= radius && inside(pos + make_int2(x, y), p.size)) {
                        Uint tap = pixel_index(pos + make_int2(x, y), p.size);
                        auto g = p.guide.read(tap);
                        $if(valid_guide(g)) {
                            Float angle = acos_approx(dot(center.normal_roughness.xyz(), g.normal_roughness.xyz()));
                            Float depth = abs(g.position.w - center.position.w) / max(g.position.w, center.position.w);
                            Float w = exp(-.66f * .25f * cast<float>(x * x + y * y)) * linear_step(.03f, 0.f, depth);
                            Float4 td = p.src_diffuse.read(tap), ts = p.src_specular.read(tap);
                            Float dw = w * exponential_weight(angle * npd) * cast<float>(td.w != 0.f);
                            Float sw = w * exponential_weight(angle * nps) * cast<float>(ts.w != 0.f);
                            hd += td.w * dw; wd += dw; hs += ts.w * sw; ws += sw;
                        };
                    };
                };
            };
            d.w = hd / max(wd, 1e-6f); s.w = hs / max(ws, 1e-6f);
        };
        p.dst_diffuse.write(idx, d); p.dst_specular.write(idx, s);
    };
    reconstruct_ = device.compile(reconstruct, "RELAX-HitDistanceReconstruction");

    Kernel prepass = [](Var<FilterParam> p) {
        Uint idx = dispatch_id(); Int2 pos = make_int2(dispatch_idx().xy());
        auto center = p.guide.read(idx);
        $if(!valid_guide(center)) {
            p.prepass_diffuse.write(idx, make_float4(0.f)); p.prepass_specular.write(idx, make_float4(0.f));
            $return();
        };
        Float3 n = center.normal_roughness.xyz();
        Float3 view = safe_normalize(p.camera.as_vec3() - center.position.xyz());
        Float roughness = saturate(center.normal_roughness.w);
        Float4 dominant = dominant_direction(n, view, roughness);
        Float frustum = p.unproject * cast<float>(min(p.size.x, p.size.y)) * center.position.w;
        constexpr float poisson[8][3] = {
            {-.4706069f, -.4427112f, .6461146f}, {-.9057375f, .3003471f, .9542373f},
            {-.3487388f, .4037880f, .5335386f}, {.1023042f, .6439373f, .6520134f},
            {.5699277f, .3513750f, .6695386f}, {.2939128f, -.1131226f, .3149309f},
            {.7836658f, -.4208784f, .8895339f}, {.1564120f, -.8198990f, .8346850f}};
        for (uint lobe = 0u; lobe < 2u; ++lobe) {
            const bool spec = lobe != 0u;
            const auto &source = spec ? p.src_specular : p.src_diffuse;
            Float4 color = source.read(idx);
            Float requested = spec ? p.settings.specular_prepass : p.settings.diffuse_prepass;
            Float r = spec ? roughness : Float(1.f);
            Float no_d = spec ? abs(dot(n, dominant.xyz())) : Float(1.f);
            Float hit = select(color.w == 0.f, 1.f, color.w);
            Float radius = requested * saturate(hit * no_d / max(frustum, 1e-6f));
            if (spec) {
                radius *= spec_magic(r);
                Float lobe_radius = hit * no_d * lobe_tan(r, .75f);
                radius = min(radius, lobe_radius / max(p.unproject * (center.position.w + hit * dominant.w), 1e-6f));
            }
            radius = select(color.w == 0.f, max(radius, 1.f), radius);
            Float normal_param = 1.f / max(atan(legacy_lobe_tan(r, p.settings.lobe_fraction * (spec ? .5f : .25f))), 1.5f / 255.f);
            Float hit_norm = lerp(min(1.f / 9.f, spec_magic(r)), .0005f, 1.f);
            Float rough_norm = lerp(saturate(r * p.settings.roughness_fraction), .01f, 1.f);
            Float min_hit_weight = p.settings.min_hit_weight * 2.f;
            if (spec) min_hit_weight = select(color.w == 0.f, 1.f, min_hit_weight * spec_magic(r));
            Float weight_sum = 1.f, hit_min = select(color.w == 0.f, 1e10f, color.w);
            Float4 sum = color;
            $if(requested > 0.f) {
                for (uint k = 0u; k < 8u; ++k) {
                    Float2 offset = make_float2(poisson[k][0], poisson[k][1]);
                    Float2 rotated = make_float2(p.rotator.x * offset.x + p.rotator.y * offset.y,
                                                 p.rotator.z * offset.x + p.rotator.w * offset.y);
                    Int2 sample_pos = make_int2(floor(make_float2(pos) + .5f + rotated * radius));
                    $if(inside(sample_pos, p.size)) {
                        Uint tap = pixel_index(sample_pos, p.size); auto g = p.guide.read(tap);
                        $if(spatial_compatible(center, g)) {
                            Float angle = acos_approx(dot(n, g.normal_roughness.xyz()));
                            Float w = plane_weight(center, g, p.settings.depth_threshold) * nonexp_weight(angle * normal_param);
                            if (spec) w *= nonexp_weight((g.normal_roughness.w - r) / rough_norm);
                            Float4 sample = source.read(tap);
                            w *= lerp(exponential_weight((sample.w - color.w) / hit_norm), min_hit_weight, 1.f);
                            w *= exp(-.66f * poisson[k][2] * poisson[k][2]);
                            if (spec) {
                                Float distance = length(g.position.xyz() - center.position.xyz());
                                Float t = sample.w / max(color.w + distance, 1e-6f);
                                w *= lerp(linear_step(.5f, 1.f, r), saturate(t), 1.f);
                                $if(w > 0.f && sample.w > 0.f) { hit_min = min(hit_min, sample.w); };
                                sum.xyz() += sample.xyz() * w;
                            } else { sum += sample * w; }
                            weight_sum += w;
                        };
                    };
                }
                if (spec) {
                    sum.xyz() /= weight_sum; sum.w = select(hit_min == 1e10f, 0.f, hit_min);
                } else { sum /= weight_sum; }
            };
            auto &output = spec ? p.prepass_specular : p.prepass_diffuse;
            output.write(idx, clamp(sum, 0.f, 65504.f));
        }
    };
    prepass_ = device.compile(prepass, "RELAX-PrePass");
}
}
