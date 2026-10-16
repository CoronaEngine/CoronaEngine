#include "filter.h"
#include "utils.h"

namespace vision::relax {
struct Reprojected {
    float4 diffuse{}, specular{}, diffuse_fast{}, specular_fast{}, normal_roughness{};
    float length{}, hit{}, footprint{}, bilinear_valid{}, bicubic{};
};
}
OC_STRUCT(vision::relax, Reprojected, diffuse, specular, diffuse_fast, specular_fast,
          normal_roughness, length, hit, footprint, bilinear_valid, bicubic) {};

namespace vision::relax {
namespace {
Float4 catrom_weights(const Float &f) {
    return make_float4(f * (f * (-.5f * f + 1.f) - .5f),
                       f * f * (1.5f * f - 2.5f) + 1.f,
                       f * (f * (-1.5f * f + 2.f) + .5f),
                       f * f * (.5f * f - .5f));
}

Var<Reprojected> reproject(Var<FilterParam> &p, const Var<Guide> &center, const Float2 &position,
                          const Float &expected_z, const Float &threshold, bool virtual_motion) {
    Var<Reprojected> result;
    $if(p.history_valid != 0u) {
        Int2 base = make_int2(floor(position - .5f));
        Float2 f = position - .5f - make_float2(base);
        Float4 wx = catrom_weights(f.x), wy = catrom_weights(f.y);
        Var<Reprojected> linear, cubic;
        Float linear_sum = 0.f, cubic_sum = 0.f;
        Bool all_valid = true;
        for (int y = -1; y <= 2; ++y) for (int x = -1; x <= 2; ++x) {
            if ((x == -1 || x == 2) && (y == -1 || y == 2)) continue;
            Int2 pixel = base + make_int2(x, y);
            Bool valid = false;
            $if(inside(pixel, p.size)) {
                Uint tap = pixel_index(pixel, p.size);
                auto geometry = p.prev_guide.read(tap);
                Bool identity = geometry.identity.w != 0u && geometry.identity.x == center.identity.x &&
                                geometry.identity.y == center.identity.y && geometry.identity.z == center.identity.z;
                Float distance = abs(geometry.position.w - expected_z);
                if (virtual_motion)
                    distance = abs(dot(geometry.position.xyz() - center.position.xyz(), center.normal_roughness.xyz()));
                valid = identity && distance <= threshold && dot(geometry.normal_roughness.xyz(), center.normal_roughness.xyz()) >= 0.f;
                $if(valid) {
                    auto history = p.prev_history.read(tap);
                    Float w = wx[x + 1] * wy[y + 1];
                    cubic.diffuse += history.diffuse * w; cubic.specular += history.specular * w;
                    cubic.diffuse_fast += history.diffuse_fast * w; cubic.specular_fast += history.specular_fast * w;
                    cubic_sum += w;
                    if (x >= 0 && x <= 1 && y >= 0 && y <= 1) {
                        Float b = (x == 0 ? 1.f - f.x : f.x) * (y == 0 ? 1.f - f.y : f.y);
                        linear.diffuse += history.diffuse * b; linear.specular += history.specular * b;
                        linear.diffuse_fast += history.diffuse_fast * b; linear.specular_fast += history.specular_fast * b;
                        linear.normal_roughness += geometry.normal_roughness * b;
                        linear.length += history.length_hit.x * b; linear.hit += history.length_hit.y * b;
                        linear_sum += b; result.bilinear_valid += 1.f;
                    }
                };
            };
            all_valid &= valid;
        }
        $if(linear_sum > .0001f) {
            result.bicubic = cast<float>(all_valid);
            result.footprint = select(all_valid, 1.f, linear_sum);
            Float denominator = max(select(all_valid, cubic_sum, linear_sum), .0001f);
            result.diffuse = max(select(all_valid, cubic.diffuse, linear.diffuse) / denominator, make_float4(0.f));
            result.specular = max(select(all_valid, cubic.specular, linear.specular) / denominator, make_float4(0.f));
            result.diffuse_fast = max(select(all_valid, cubic.diffuse_fast, linear.diffuse_fast) / denominator, make_float4(0.f));
            result.specular_fast = max(select(all_valid, cubic.specular_fast, linear.specular_fast) / denominator, make_float4(0.f));
            result.normal_roughness = linear.normal_roughness / linear_sum;
            result.normal_roughness.xyz() = safe_normalize(result.normal_roughness.xyz());
            result.length = linear.length / linear_sum;
            result.hit = max(.001f, linear.hit / linear_sum);
        };
    };
    return result;
}

Float4 previous_normal(Var<FilterParam> &p, const Float2 &position) {
    Int2 base = make_int2(floor(position - .5f));
    Float2 f = position - .5f - make_float2(base);
    Float4 result = make_float4(0.f);
    $if(p.history_valid != 0u) {
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
            Float w = (x == 0 ? 1.f - f.x : f.x) * (y == 0 ? 1.f - f.y : f.y);
            result += p.prev_guide.read(pixel_index(base + make_int2(x, y), p.size)).normal_roughness * w;
        }
    };
    result.xyz() = safe_normalize(result.xyz());
    return result;
}

Float3 virtual_position_v2(const Var<Guide> &g, const Float &hit, const Float &curvature, const Float3 &view, const Float3 &camera) {
    Float4 d = dominant_direction(g.normal_roughness.xyz(), view, g.normal_roughness.w);
    Float z = -dot(d.xyz(), g.normal_roughness.xyz()) * hit;
    Float denominator = 2.f * curvature * z - 1.f;
    denominator = select(abs(denominator) < 1e-6f, -1e-6f, denominator);
    Float magnification = abs(1.f / denominator);
    Float silhouette = length(g.position.xyz() - camera) * (1.f - abs(dot(g.normal_roughness.xyz(), view))) * max(curvature, 0.f);
    return g.position.xyz() - view * (hit * magnification / (1.f + silhouette) * d.w);
}
}

void Filter::compile_temporal(Device &device) {
    Kernel temporal = [](Var<FilterParam> p) {
        Uint idx = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        auto g = p.guide.read(idx);
        Var<TemporalData> data;
        Float4 out_d = make_float4(0.f), out_s = make_float4(0.f);
        $if(valid_guide(g)) {
            Float3 camera = p.camera.as_vec3(), previous_camera = p.prev_camera.as_vec3();
            Float3 view = safe_normalize(camera - g.position.xyz());
            Float3 previous_view = safe_normalize(previous_camera - g.position.xyz());
            Float3 normal = g.normal_roughness.xyz();
            Float nov = abs(dot(normal, view));
            Float2 current_pixel = make_float2(pixel) + .5f, previous_pixel = current_pixel - g.motion;
            Float3 delta = camera - previous_camera;
            Float2 translation_motion = previous_pixel - project_pixel(p.prev_world_to_raster, g.position.xyz() - delta);
            Float parallax1 = length(translation_motion);
            Float parallax2 = length(project_pixel(p.world_to_raster, g.position.xyz() + delta) - current_pixel);
            Float parallax = max(parallax1, parallax2);
            Float min_parallax = min(parallax1, parallax2);
            Float pixel_size = p.unproject * g.position.w;
            Float slope = 1.f / lerp(saturate(parallax / 30.f), lerp(nov, .05f, 1.f), 1.f);
            Float threshold = saturate(p.disocclusion_threshold * slope) * pixel_size * cast<float>(min(p.size.x, p.size.y));
            Float expected_z = abs((p.prev_world_to_view * make_float4(g.position.xyz(), 1.f)).z);
            auto surface = reproject(p, g, previous_pixel, expected_z, max(threshold - 1e-6f, 0.f), false);

            Float3 averaged_normal = make_float3(0.f);
            Float min_hit = 1e10f;
            for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
                Uint tap = pixel_index(pixel + make_int2(x, y), p.size);
                averaged_normal += p.guide.read(tap).normal_roughness.xyz() / 9.f;
                Float h = p.prepass_specular.read(tap).w;
                min_hit = min(min_hit, select(h > 0.f, h, 1e10f));
            }
            Float hit = select(min_hit == 1e10f, 0.f, min_hit);
            Float normal_length = length(averaged_normal);
            Float kappa = saturate(1.f - normal_length * normal_length) / max(normal_length * (3.f - normal_length * normal_length), 1e-6f);
            Float roughness = saturate(g.normal_roughness.w);
            Float modified_roughness = sqrt(saturate(roughness * roughness + kappa));

            Float footprint = surface.footprint;
            Float size_quality = (abs(dot(normal, previous_view)) + .001f) / (nov + .001f);
            size_quality *= size_quality; size_quality *= size_quality;
            footprint *= lerp(saturate(size_quality), .1f, 1.f);
            Float history_length = min(surface.length + 1.f, 255.f);
            history_length = max(1.f, history_length * sqrt(footprint));
            history_length = min(history_length, 1.f + max(p.settings.diffuse_history, p.settings.specular_history));
            history_length = select(p.history_valid == 0u, 1.f, history_length);

            Float4 noisy_d = p.prepass_diffuse.read(idx), noisy_s = p.prepass_specular.read(idx);
            Float ld = luminance(noisy_d.xyz()), ls = luminance(noisy_s.xyz());
            Float alpha_d = max(1.f / (1.f + p.settings.diffuse_history), 1.f / history_length);
            Float alpha_df = max(1.f / (1.f + p.settings.diffuse_fast_history), 1.f / history_length);
            alpha_d = select(surface.footprint > 0.f, alpha_d, 1.f);
            alpha_df = select(surface.footprint > 0.f, alpha_df, 1.f);
            out_d = lerp(alpha_d, surface.diffuse, make_float4(noisy_d.xyz(), ld * ld));
            data.diffuse_fast = make_float4(lerp(alpha_df, surface.diffuse_fast.xyz(), noisy_d.xyz()), 0.f);

            // Curvature along camera-translation parallax; tangent-plane intersections
            // avoid turning depth discontinuities into curvature.
            Float2 motion_direction = translation_motion / max(parallax1, 1.f / 256.f);
            Float2 axis_weights = abs(motion_direction) + 1.f / 256.f;
            axis_weights /= axis_weights.x + axis_weights.y;
            Float3 x_edge = make_float3(0.f), n_edge = make_float3(0.f);
            for (int axis = 0; axis < 2; ++axis) {
                auto neighbor = p.guide.read(pixel_index(pixel + (axis == 0 ? make_int2(1, 0) : make_int2(0, 1)), p.size));
                Float3 ray = safe_normalize(neighbor.position.xyz() - camera);
                Float denominator = dot(normal, ray);
                Float3 tangent = g.position.xyz();
                $if(abs(denominator) > 1e-6f) {
                    tangent = camera + ray * (dot(g.position.xyz() - camera, normal) / denominator);
                };
                Float w = axis == 0 ? axis_weights.x : axis_weights.y;
                x_edge += tangent * w; n_edge += neighbor.normal_roughness.xyz() * w;
            }
            // Same high-parallax flattening rule, using current cached world guides.
            Uint2 wrapped = make_uint2(pixel) & 3u;
            Uint a = 2068378560u * (1u - (wrapped.x >> 1u)) + 1500172770u * (wrapped.x >> 1u);
            Uint b = (wrapped.y + ((wrapped.x & 1u) << 2u)) << 2u;
            Uint offset = ((p.frame & 5u) << 1u) | ((p.frame & 10u) >> 1u);
            offset = ((offset & 3u) << 2u) | ((offset & 12u) >> 2u);
            Uint bayer = ((a >> b) + offset) & 15u;
            Float high_length = min_parallax * (1.f + p.framerate_scale * cast<float>(bayer) / 16.f);
            Int2 high_pixel = make_int2(floor(current_pixel + high_length * motion_direction));
            $if(high_length > 1.f && inside(high_pixel, p.size)) {
                auto high = p.guide.read(pixel_index(high_pixel, p.size));
                $if(spatial_compatible(g, high) && abs(high.position.w - g.position.w) / max(high.position.w, g.position.w) < .1f) {
                    n_edge = high.normal_roughness.xyz(); x_edge = high.position.xyz();
                };
            };
            Float3 edge = x_edge - g.position.xyz();
            Float curvature = dot(safe_normalize(n_edge) - normal, edge) / max(dot(edge, edge), 1e-10f);
            Float focused = thin_lens(hit, curvature);
            Float2 virtual_pixel = project_pixel(p.prev_world_to_raster, g.position.xyz() - view * focused);
            auto virtual_history = reproject(p, g, virtual_pixel, expected_z,
                                              max(p.disocclusion_threshold * g.position.w - 1e-6f, 0.f), true);
            Float4 dominant = dominant_direction(normal, view, modified_roughness);
            Float amount = select(virtual_history.bilinear_valid == 4.f, dominant.w, 0.f);
            amount *= cast<float>(dot(virtual_history.normal_roughness.xyz(), averaged_normal) > 0.f);
            Float2 uv_delta = virtual_pixel - previous_pixel;
            Float uv_length = length(uv_delta);
            Float curvature_angle = atan(abs(curvature * pixel_size) * max(uv_length / max(nov, .01f), 1.f));
            Float lobe_angle = max(atan(legacy_lobe_tan(modified_roughness, .75f)), 1.5f / 255.f);
            Float normal_weight = encoding_normal_weight(normal, virtual_history.normal_roughness.xyz(), lobe_angle,
                                                          curvature_angle, 1.5f / 255.f, true);
            amount *= lerp(normal_weight, 1.f - saturate(uv_length), 1.f);
            Float m = roughness * roughness;
            Float rough_norm = lerp(lerp(p.settings.roughness_fraction, m * m, m), .01f, 1.f);
            Float rough_weight = nonexp_weight((virtual_history.normal_roughness.w * virtual_history.normal_roughness.w - m) / rough_norm);
            rough_weight = lerp(rough_weight, 1.f - saturate(uv_length), 1.f);
            amount *= rough_weight;
            Float confidence_v = .9f * rough_weight + .1f;
            Float2 back_step = uv_delta / max(uv_length, 1e-6f) * (saturate(uv_length / .1f) + uv_length * .5f);
            Float back_normals = 1.f, back_roughness = 1.f;
            for (int back = 1; back <= 2; ++back) {
                Float2 back_pixel = virtual_pixel + back_step * float(back);
                Float4 bn = previous_normal(p, back_pixel);
                Float w = encoding_normal_weight(virtual_history.normal_roughness.xyz(), bn.xyz(), lobe_angle,
                                                 curvature_angle * float(back + 1), 1.5f / 255.f, true);
                back_normals *= select(inside(make_int2(floor(back_pixel)), p.size), w, 1.f);
                back_roughness *= nonexp_weight((bn.w * bn.w - m) / rough_norm);
            }
            amount *= .33f + .67f * back_normals;
            confidence_v *= .33f + .67f * back_normals;
            amount *= .9f * back_roughness + .1f;

            Float smc = spec_magic(modified_roughness);
            Float reference_hit = lerp(smc, noisy_s.w, surface.hit);
            Float hit1 = thin_lens(reference_hit, curvature), hit2 = thin_lens(virtual_history.hit, curvature);
            Float hit_confidence = 1.f - saturate(lerp(smc, 20.f, 0.f) * abs(hit1 - hit2) /
                                                 max(g.position.w + max(hit1, hit2), 1e-6f));
            hit_confidence = lerp(smc, hit_confidence, 1.f);
            Float3 virtual_world = virtual_position_v2(g, hit, curvature, view, camera);
            Float previous_tracking = virtual_history.specular_fast.w;
            Float3 test_world = virtual_position_v2(g, previous_tracking, curvature, view, camera);
            Float lobe_radius = max(legacy_lobe_tan(roughness, .6f), .5f / cast<float>(p.size.x));
            lobe_radius *= min(hit, previous_tracking) /
                           max(p.unproject * max(length(virtual_world - camera), length(test_world - camera)), 1e-6f);
            Float delta_parallax = length(project_pixel(p.prev_world_to_raster, test_world) - virtual_pixel);
            hit_confidence *= smooth01(1.f - delta_parallax / (lobe_radius + .25f));
            Float confidence_s = cast<float>(surface.footprint > 0.f) *
                                 encoding_normal_weight(view, previous_view, lobe_angle * nov / p.framerate_scale, 0.f, 0.f, false);
            Float frames = min(p.settings.specular_history, history_length);
            Float fast_frames = min(p.settings.specular_fast_history, history_length);
            Float alpha_s = max(1.f - confidence_s, 1.f / (1.f + frames));
            Float alpha_sf = max(alpha_s, 1.f / (1.f + fast_frames));
            Float alpha_v = max(1.f - confidence_v, 1.f / (1.f + frames));
            Float alpha_vf = max(1.f - confidence_v * hit_confidence, 1.f / (1.f + fast_frames));
            Float alpha_vh = max(1.f - confidence_v * hit_confidence, 1.f / (1.f + frames));
            amount *= saturate(confidence_v / (confidence_s + 1e-6f));
            Float4 noisy_moment = make_float4(noisy_s.xyz(), ls * ls);
            Float4 accumulated_s = lerp(alpha_s, surface.specular, noisy_moment);
            Float4 accumulated_v = lerp(alpha_v, virtual_history.specular, noisy_moment);
            out_s = lerp(amount, accumulated_s, accumulated_v);
            Float3 fast_s = lerp(alpha_sf, surface.specular_fast.xyz(), noisy_s.xyz());
            Float3 fast_v = lerp(alpha_vf, virtual_history.specular_fast.xyz(), noisy_s.xyz());
            data.specular_fast = make_float4(lerp(amount, fast_s, fast_v), hit);
            Float tracking_s = lerp(max(alpha_s, .1f), surface.hit, noisy_s.w);
            Float tracking_v = lerp(max(alpha_vh, .1f), virtual_history.hit, noisy_s.w);
            Float confidence = lerp(amount, confidence_s, confidence_v);
            $if(out_s.w == 0.f) { out_s.w = p.settings.specular_variance_boost * (1.f - confidence); };
            data.meta = make_float4(history_length, confidence, lerp(amount, tracking_s, tracking_v), curvature);
        };
        p.dst_diffuse.write(idx, out_d); p.dst_specular.write(idx, out_s);
        p.history.write(idx, data);
    };
    temporal_ = device.compile(temporal, "RELAX-TemporalAccumulation");

    // Publishing is a separate dispatch: no thread can overwrite a neighbor's
    // previous geometry or illumination while reprojection is still reading it.
    Kernel publish = [](Var<FilterParam> p) {
        Uint idx = dispatch_id(); auto g = p.guide.read(idx); auto data = p.history.read(idx);
        Var<PersistentHistory> h;
        h.diffuse = p.src_diffuse.read(idx); h.specular = p.src_specular.read(idx);
        h.diffuse_fast = data.diffuse_fast; h.specular_fast = data.specular_fast;
        h.length_hit = make_float2(data.meta.x, data.meta.z);
        p.prev_history.write(idx, h);
        Var<GeometryHistory> geometry;
        geometry.position = g.position; geometry.normal_roughness = g.normal_roughness; geometry.identity = g.identity;
        p.prev_guide.write(idx, geometry);
    };
    publish_ = device.compile(publish, "RELAX-PublishHistory");
}
}
