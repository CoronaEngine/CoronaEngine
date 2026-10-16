#include "filter.h"
#include "utils.h"

namespace vision::relax {
namespace {
Uint hash32(Uint x) {
    x ^= x >> 16u; x *= 0x7FEB352Du; x ^= x >> 15u; x *= 0x846CA68Bu; x ^= x >> 16u;
    return x;
}
Uint explode(Uint x) {
    x = (x | (x << 8u)) & 0x00FF00FFu; x = (x | (x << 4u)) & 0x0F0F0F0Fu;
    x = (x | (x << 2u)) & 0x33333333u; return (x | (x << 1u)) & 0x55555555u;
}
Float2 offset_random(const Uint2 &pixel, const Uint &frame) {
    Uint zorder = explode(pixel.x) | (explode(pixel.y) << 1u);
    Uint seed = hash32(frame + 0x035F9F29u);
    seed ^= hash32(zorder) + 0x9E3779B9u + (seed << 6u) + (seed >> 2u);
    Uint x = hash32(seed), y = hash32(x);
    return make_float2(2.f - as<float>((x >> 9u) | 0x3F800000u), 2.f - as<float>((y >> 9u) | 0x3F800000u));
}
}

void Filter::compile_atrous(Device &device) {
    auto compile = [&](bool first) {
        Kernel kernel = [first](Var<FilterParam> p) {
            Uint idx = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
            auto center = p.guide.read(idx); auto data = p.history.read(idx);
            $if(!valid_guide(center)) {
                p.dst_diffuse.write(idx, make_float4(0.f)); p.dst_specular.write(idx, make_float4(0.f));
                $return();
            };
            Float3 normal = center.normal_roughness.xyz();
            Float3 center_view = safe_normalize(p.camera.as_vec3() - center.position.xyz());
            Float roughness = center.normal_roughness.w;
            Float fraction = p.settings.lobe_fraction;
            if (!first) {
                fraction /= sqrt(cast<float>(p.step));
                fraction = lerp(saturate(data.meta.x / 5.f), .99f, fraction);
            }
            Float diffuse_normal = 1.f / max(atan(legacy_lobe_tan(1.f, fraction)), 1.5f / 255.f);
            Float2 specular_normal = atrous_normal_params(roughness, data.meta.x, data.meta.y, p.settings.normal_relaxation,
                                                         p.settings.lobe_fraction, p.settings.specular_lobe_slack);
            Float rough_norm = lerp(saturate(roughness * p.settings.roughness_fraction), .01f, 1.f);
            Int2 offset = make_int2(0);
            if (!first) {
                $if(p.step > 4u) { offset = make_int2(cast<float>(p.step) * .5f * (offset_random(make_uint2(pixel), p.frame) - .5f)); };
            }
            for (uint lobe = 0; lobe < 2u; ++lobe) {
                const bool spec = lobe != 0u;
                const auto &input = spec ? p.src_specular : p.src_diffuse;
                auto &output = spec ? p.dst_specular : p.dst_diffuse;
                Float4 color = input.read(idx), filtered = make_float4(0.f);
                if (first) {
                    $if(data.meta.x < p.settings.variance_history_threshold) {
                        Float4 sum = make_float4(0.f); Float weights = 0.f;
                        $for(y, -2, 3) {
                            $for(x, -2, 3) {
                                Uint tap = pixel_index(pixel + make_int2(x, y), p.size); auto g = p.guide.read(tap);
                                $if(spatial_compatible(center, g)) {
                                    Float angle = acos_approx(dot(normal, g.normal_roughness.xyz()));
                                    // NRD leaves depthW=1 here. Keep the engine's
                                    // geometric ownership at newly exposed depth edges.
                                    Float w = nonexp_weight(angle * diffuse_normal) * plane_weight(center, g, p.settings.depth_threshold);
                                    sum += input.read(tap) * w; weights += w;
                                };
                            };
                        };
                        sum /= max(weights, 1e-6f);
                        Float l = luminance(sum.xyz());
                        Float variance = max(0.f, sum.w - l * l) * max(1.f, 4.f / (data.meta.x + 1.f));
                        filtered = make_float4(sum.xyz(), variance);
                    } $else {
                        Float4 gaussian = make_float4(0.f);
                        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
                            float w = (x == 0 ? .5f : .25f) * (y == 0 ? .5f : .25f);
                            gaussian += input.read(pixel_index(pixel + make_int2(x, y), p.size)) * w;
                        }
                        Float l = luminance(gaussian.xyz()); color.w = max(0.f, gaussian.w - l * l);
                    };
                }
                // The first mature-history pass consumes second moments. All
                // following passes consume and propagate variance instead.
                Bool regular = first ? data.meta.x >= p.settings.variance_history_threshold : Bool(true);
                $if(regular) {
                    Float phi = spec ? p.settings.specular_phi : p.settings.diffuse_phi;
                    Float phi_inv = 1.f / max(1e-4f, phi * sqrt(max(color.w, 0.f)));
                    Float luminance_relaxation = 1.f;
                    if (spec) {
                        // Match the pinned Source/Relax.cpp constant mapping:
                        // gLuminanceEdgeStoppingRelaxation <- roughness relaxation.
                        $if(p.step <= 4u) { luminance_relaxation = lerp(p.settings.roughness_relaxation, 1.f, data.meta.y); };
                    }
                    Float sum_w = first ? Float(0.f) : Float(.44198f * .44198f);
                    Float4 sum = make_float4(0.f);
                    if (!first) sum = color * make_float4(sum_w, sum_w, sum_w, sum_w * sum_w);
                    Float center_l = luminance(color.xyz());
                    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
                        const bool is_center = x == 0 && y == 0;
                        if (!first && is_center) continue;
                        Int2 pos = pixel + offset + make_int2(x, y) * cast<int>(p.step);
                        const float kernel_weight = (x == 0 ? .44198f : .27901f) * (y == 0 ? .44198f : .27901f);
                        $if(inside(pos, p.size)) {
                            Uint tap = pixel_index(pos, p.size); auto g = p.guide.read(tap);
                            $if(spatial_compatible(center, g)) {
                                Float4 sample = input.read(tap);
                                Float angle = acos_approx(dot(normal, g.normal_roughness.xyz()));
                                Float nw = nonexp_weight(angle * diffuse_normal);
                                if (spec) {
                                    Float3 sample_view = safe_normalize(p.camera.as_vec3() - g.position.xyz() +
                                                                       p.settings.roughness_relaxation * (p.camera.as_vec3() - center.position.xyz()));
                                    Float detailed = atrous_specular_normal(specular_normal, normal, g.normal_roughness.xyz(), center_view, sample_view);
                                    Float rw = nonexp_weight((g.normal_roughness.w - roughness) / rough_norm);
                                    nw = select(p.settings.roughness_stopping != 0u, detailed * rw, nw);
                                }
                                Float w = kernel_weight * plane_weight(center, g, p.settings.depth_threshold) * nw;
                                w *= exp(-abs(center_l - luminance(sample.xyz())) * phi_inv * luminance_relaxation);
                                if (first && is_center) w = kernel_weight;
                                sum_w += w;
                                if (first) sum += sample * w;
                                else sum += sample * make_float4(w, w, w, w * w);
                            };
                        };
                    }
                    Float denominator = max(sum_w, 1e-6f);
                    if (first) {
                        sum /= denominator; Float l = luminance(sum.xyz());
                        filtered = make_float4(sum.xyz(), max(0.f, sum.w - l * l));
                    } else filtered = sum / make_float4(denominator, denominator, denominator, denominator * denominator);
                };
                output.write(idx, filtered);
            }
        };
        return device.compile(kernel, first ? "RELAX-AtrousFirst" : "RELAX-Atrous");
    };
    atrous_first_ = compile(true); atrous_ = compile(false);
}
}
