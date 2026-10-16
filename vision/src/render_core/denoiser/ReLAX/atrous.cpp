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
    auto compile = [&](bool first, bool last) {
        Kernel kernel = [first, last](Var<FilterParam> p) {
            Uint idx = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
            auto center = load_guide(p, idx); auto data = p.history.read(idx);
            if (first) {
                // Reprojection has finished; Atrous reads only current guides.
                // NRD's first Atrous pass also publishes previous geometry.
                Var<GeometryHistory> geometry;
                geometry.position = center.position; geometry.normal_roughness = center.normal_roughness; geometry.identity = center.identity;
                p.prev_guide.write(idx, geometry);
            }
            $if(!valid_guide(center)) {
                if (last) {
                    p.output_diffuse.write(idx, make_float4(0.f)); p.output_specular.write(idx, make_float4(0.f));
                } else {
                    p.dst_diffuse.write(idx, make_float4(0.f)); p.dst_specular.write(idx, make_float4(0.f));
                }
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
            auto write_result = [&](bool spec, const Float4 &filtered) {
                if (last) {
                    Float4 output = filtered;
                    Float3 factor = spec ? center.specular_factor.xyz() : center.diffuse_factor.xyz();
                    output.xyz() *= max(factor, make_float3(.01f));
                    if (spec) p.output_specular.write(idx, output);
                    else p.output_diffuse.write(idx, output);
                } else {
                    if (spec) p.dst_specular.write(idx, filtered);
                    else p.dst_diffuse.write(idx, filtered);
                }
            };
            if (!first) {
                // NRD traverses the neighborhood once for diffuse and specular.
                // Share positions, normals, ownership and plane weights without
                // changing either lobe's tap order or squared-weight variance.
                Float4 diffuse = p.src_diffuse.read(idx), specular = p.src_specular.read(idx);
                Float2 center_l = make_float2(luminance(diffuse.xyz()), luminance(specular.xyz()));
                Float2 phi_inv = make_float2(1.f / max(1e-4f, p.settings.diffuse_phi * sqrt(max(diffuse.w, 0.f))),
                                            1.f / max(1e-4f, p.settings.specular_phi * sqrt(max(specular.w, 0.f))));
                Float relaxation = 1.f;
                $if(p.step <= 4u) { relaxation = lerp(p.settings.roughness_relaxation, 1.f, data.meta.y); };
                constexpr float center_weight = .44198f * .44198f;
                Float2 sum_w = make_float2(center_weight);
                Float4 sum_d = diffuse * make_float4(center_weight, center_weight, center_weight, center_weight * center_weight);
                Float4 sum_s = specular * make_float4(center_weight, center_weight, center_weight, center_weight * center_weight);
                for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
                    if (x == 0 && y == 0) continue;
                    Int2 pos = pixel + offset + make_int2(x, y) * cast<int>(p.step);
                    const float kernel_weight = (x == 0 ? .44198f : .27901f) * (y == 0 ? .44198f : .27901f);
                    $if(inside(pos, p.size)) {
                        Uint tap = pixel_index(pos, p.size); auto g = load_guide(p, tap);
                        $if(spatial_compatible(center, g)) {
                            Float nw = nonexp_weight(acos_approx(dot(normal, g.normal_roughness.xyz())) * diffuse_normal);
                            Float geometry = kernel_weight * plane_weight(center, g, p.settings.depth_threshold);
                            Float3 sample_view = safe_normalize(p.camera.as_vec3() - g.position.xyz() +
                                                               p.settings.roughness_relaxation * (p.camera.as_vec3() - center.position.xyz()));
                            Float detailed = atrous_specular_normal(specular_normal, normal, g.normal_roughness.xyz(), center_view, sample_view);
                            Float rw = nonexp_weight((g.normal_roughness.w - roughness) / rough_norm);
                            Float dw = geometry * nw;
                            Float sw = geometry * select(p.settings.roughness_stopping != 0u, detailed * rw, nw);
                            Float4 sample_d = p.src_diffuse.read(tap), sample_s = p.src_specular.read(tap);
                            dw *= exp(-abs(center_l.x - luminance(sample_d.xyz())) * phi_inv.x);
                            sw *= exp(-abs(center_l.y - luminance(sample_s.xyz())) * phi_inv.y * relaxation);
                            sum_w.x += dw; sum_w.y += sw;
                            sum_d += sample_d * make_float4(dw, dw, dw, dw * dw);
                            sum_s += sample_s * make_float4(sw, sw, sw, sw * sw);
                        };
                    };
                }
                Float2 denominator = max(sum_w, make_float2(1e-6f));
                write_result(false, sum_d / make_float4(denominator.x, denominator.x, denominator.x, denominator.x * denominator.x));
                write_result(true, sum_s / make_float4(denominator.y, denominator.y, denominator.y, denominator.y * denominator.y));
            } else {
                for (uint lobe = 0; lobe < 2u; ++lobe) {
                    const bool spec = lobe != 0u;
                    const auto &input = spec ? p.src_specular : p.src_diffuse;
                    Float4 color = input.read(idx), filtered = make_float4(0.f);
                    $if(data.meta.x < p.settings.variance_history_threshold) {
                        Float4 sum = make_float4(0.f); Float weights = 0.f;
                        $for(y, -2, 3) {
                            $for(x, -2, 3) {
                                Uint tap = pixel_index(pixel + make_int2(x, y), p.size); auto g = load_guide(p, tap);
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
                    // The first mature-history pass consumes second moments. All
                    // following passes consume and propagate variance instead.
                    Bool regular = data.meta.x >= p.settings.variance_history_threshold;
                    $if(regular) {
                        Float phi = spec ? p.settings.specular_phi : p.settings.diffuse_phi;
                        Float phi_inv = 1.f / max(1e-4f, phi * sqrt(max(color.w, 0.f)));
                        Float luminance_relaxation = 1.f;
                        if (spec) {
                            // Match the pinned Source/Relax.cpp constant mapping:
                            // gLuminanceEdgeStoppingRelaxation <- roughness relaxation.
                            $if(p.step <= 4u) { luminance_relaxation = lerp(p.settings.roughness_relaxation, 1.f, data.meta.y); };
                        }
                        Float sum_w = 0.f;
                        Float4 sum = make_float4(0.f);
                        Float center_l = luminance(color.xyz());
                        for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
                            const bool is_center = x == 0 && y == 0;
                            Int2 pos = pixel + offset + make_int2(x, y) * cast<int>(p.step);
                            const float kernel_weight = (x == 0 ? .44198f : .27901f) * (y == 0 ? .44198f : .27901f);
                            $if(inside(pos, p.size)) {
                                Uint tap = pixel_index(pos, p.size); auto g = load_guide(p, tap);
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
                                    if (is_center) w = kernel_weight;
                                    sum_w += w;
                                    sum += sample * w;
                                };
                            };
                        }
                        Float denominator = max(sum_w, 1e-6f);
                        sum /= denominator; Float l = luminance(sum.xyz());
                        filtered = make_float4(sum.xyz(), max(0.f, sum.w - l * l));
                    };
                    write_result(spec, filtered);
                }
            }
        };
        return device.compile(kernel, first ? "RELAX-AtrousFirst" : last ? "RELAX-AtrousLast" : "RELAX-Atrous");
    };
    atrous_first_ = compile(true, false); atrous_ = compile(false, false); atrous_last_ = compile(false, true);
}
}
