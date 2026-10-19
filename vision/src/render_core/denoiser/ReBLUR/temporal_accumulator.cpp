#include "utils.h"

namespace vision::reblur {
namespace {
struct Reprojected {
    Var<History> h{};
    Float quality{0.f};
    Float3 normal{make_float3(0.f)};
    Float roughness{0.f};
};

Float catrom(Float x) {
    x = abs(x);
    return select(x < 1.f, 1.5f * x * x * x - 2.5f * x * x + 1.f,
                  select(x < 2.f, -.5f * x * x * x + 2.5f * x * x - 4.f * x + 2.f, 0.f));
}

Reprojected reproject(const Var<FilterParam> &p, const Var<Guide> &g, Float2 raster) {
    auto previous_geometry = p.prev_guide.soa_view_var<GeometryHistory>();
    auto previous_history = p.prev_history.soa_view_var<PersistentHistory>();
    Reprojected result;
    result.h.diffuse = make_float4(0.f); result.h.specular = make_float4(0.f);
    result.h.fast_count = make_float4(0.f); result.h.stabilized = make_float4(0.f);
    result.h.tracking = make_float4(0.f);
    Float2 coord = raster - .5f;
    Int2 base = make_int2(floor(coord)); Float2 f = coord - floor(coord);
    Float4 weights = make_float4((1.f - f.x) * (1.f - f.y), f.x * (1.f - f.y),
                                (1.f - f.x) * f.y, f.x * f.y);
    $for(k, 4u) {
        Int2 q = base + make_int2(k % 2u, k / 2u);
        $if(in_screen(q, p.size) && weights[k] > 0.f) {
            Uint j = pixel_index(q, p.size); auto old_g = previous_geometry.read(j);
            Float delta = abs(dot(g.normal_roughness.xyz(), old_g.position.xyz() - g.position.xyz()));
            // Plane distance is measured in world units, scaled by the current frustum.
            Bool accepts = same_surface(g, old_g) && delta < .01f * frustum_size(p, g) &&
                           dot(g.normal_roughness.xyz(), old_g.normal_roughness.xyz()) > .5f;
            // Invalid tiles only invalidate geometry; their history need not be read or cleared.
            $if(accepts) {
                Float w = weights[k];
                auto h = previous_history.read(j);
                result.h.diffuse += make_float4(h.diffuse) * w; result.h.specular += make_float4(h.specular) * w;
                result.h.fast_count += make_float4(h.fast_count) * w;
                result.h.stabilized += make_float4(make_float2(h.stabilized), 0.f, 0.f) * w;
                result.h.tracking.w += h.hit_distance * w;
                result.normal += old_g.normal_roughness.xyz() * w;
                result.roughness += old_g.normal_roughness.w * w;
                result.quality += w;
            };
        };
    };
    Float inv = 1.f / max(result.quality, 1e-6f);
    result.h.diffuse *= inv; result.h.specular *= inv; result.h.fast_count *= inv;
    result.h.stabilized *= inv; result.h.tracking *= inv;
    result.normal *= inv; result.roughness *= inv;

    // Catmull-Rom only on a fully valid footprint; otherwise keep the custom bilinear result.
    $if(result.quality > .999f) {
        Float4 d = make_float4(0.f), s = make_float4(0.f);
        Bool all_valid = true;
        $for(y, -1, 3) {
            $for(x, -1, 3) {
                Int2 q = base + make_int2(x, y);
                Uint j = pixel_index(q, p.size); auto ng = previous_geometry.read(j);
                Float plane = abs(dot(g.normal_roughness.xyz(), ng.position.xyz() - g.position.xyz()));
                Bool accepts = in_screen(q, p.size) && same_surface(g, ng) &&
                               plane < .01f * frustum_size(p, g) && dot(g.normal_roughness.xyz(), ng.normal_roughness.xyz()) > .9f;
                all_valid &= accepts;
                Float w = catrom(x.cast<float>() - f.x) * catrom(y.cast<float>() - f.y);
                $if(accepts && w != 0.f) {
                    auto h = previous_history.read(j);
                    d += make_float4(h.diffuse) * w; s += make_float4(h.specular) * w;
                };
            };
        };
        $if(all_valid) {
            result.h.diffuse = make_float4(to_ycocg(to_rgb(d.xyz())), saturate(d.w));
            result.h.specular = make_float4(to_ycocg(to_rgb(s.xyz())), saturate(s.w));
        };
    };
    return result;
}
}

void Filter::compile_temporal(Device &device) {
    Kernel temporal = [](Var<FilterParam> p) {
        ocarina::configure_block(32u, 4u);
        auto guides = p.guide.soa_view_var<Guide>();
        auto current_history = p.history.soa_view_var<TemporalData>();
        Uint i = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        $if(!active_tile(p)) {
            Var<TemporalData> empty;
            empty.fast_count = make_half4(0.f); empty.stabilized = make_half4(0.f);
            empty.tracking = make_float4(0.f);
            current_history.write(i, empty);
            // Pack and PrePass already zeroed both ping-pong signals here.
            $return();
        };
        auto g = guides.read(i);
        Float4 d = make_float4(p.src_diffuse.read(i)), s = make_float4(p.src_specular.read(i));
        Var<History> out{};
        out.diffuse = d; out.specular = s;
        out.fast_count = make_float4(d.x, s.x, 0.f, 0.f);
        out.stabilized = make_float4(d.x, s.x, 0.f, 0.f);
        out.tracking = make_float4(make_float2(pixel) + .5f, 0.f, p.tracking.read(i));
        $if(valid_guide(g) && p.history_valid != 0u) {
            Float2 motion = make_float2(g.diffuse_factor.w, g.specular_factor.w);
            Float2 current = make_float2(pixel) + .5f;
            Float2 surface_raster = current - motion;
            auto surface = reproject(p, g, surface_raster);
            Float diff_count = min(surface.h.fast_count.z * surface.quality, p.max_history);
            Float3 X = g.position.xyz(), N = g.normal_roughness.xyz();
            Float3 V = normalize(p.camera.as_vec3() - X);
            Float3 Vprev = normalize(p.prev_camera.as_vec3() - X);
            Float NoV = abs(dot(N, V)), r = g.normal_roughness.w;
            Float Dfactor = dominant_factor(NoV, r);
            Float hit = p.tracking.read(i);

            // Curvature along the direction of camera motion (zero for a static view).
            Float curvature = 0.f;
            $if(length(motion) > .01f) {
                Int2 q = pixel + make_int2(round(motion / max(length(motion), 1e-6f) * 2.f));
                auto ng = guides.read(pixel_index(q, p.size));
                Float3 edge = ng.position.xyz() - X;
                $if(in_screen(q, p.size) && same_surface(g, ng) && dot(edge, edge) > 1e-10f) {
                    curvature = dot(ng.normal_roughness.xyz() - N, edge) / dot(edge, edge);
                    $if(length(X - p.camera.as_vec3()) < -1.f / min(curvature, -1e-6f)) { curvature *= NoV; };
                };
            };
            Float denominator = 1.f + 2.f * curvature * hit;
            denominator = select(abs(denominator) < .01f, select(denominator < 0.f, -.01f, .01f), denominator);
            Float focused = hit / denominator;
            // Express virtual motion relative to the surface displacement.
            // Absolute projection contains the current stochastic film jitter;
            // adding it every frame diffuses static reflections across pixels.
            Float2 reprojection_offset = surface_raster - project_raster(p.prev_world_to_raster, X);
            Float2 virtual_raster = project_raster(p.prev_world_to_raster, X - V * focused * Dfactor) + reprojection_offset;
            // Reject unstable concave-curvature acceleration before sampling history.
            Float2 flat_raster = project_raster(p.prev_world_to_raster, X - V * hit * Dfactor) + reprojection_offset;
            $if(length(virtual_raster - flat_raster) > 4.f * length(motion) + 1.f) {
                curvature = 0.f; virtual_raster = flat_raster;
            };
            auto virtual_h = reproject(p, g, virtual_raster);
            Float traveled = length(virtual_raster - current);
            Float normal_conf = 1.f / (1.f + .5f * Dfactor * max(length(N - virtual_h.normal) - .5f / 255.f, 0.f) * traveled);
            Float rough_conf = 1.f - smooth01(abs(r - virtual_h.roughness) / max(.003f + r * .15f, .003f));
            Float previous_hit = virtual_h.h.tracking.w;
            Float2 previous_virtual = project_raster(p.prev_world_to_raster, X - V * previous_hit * Dfactor) + reprojection_offset;
            Float lobe_pixels = lobe_tan(r, .75f) * min(hit, previous_hit) /
                                max(p.unproject * (g.position.w + hit * Dfactor), 1e-6f);
            Float parallax_conf = parallax_confidence(length(previous_virtual - virtual_raster), lobe_pixels);
            Float virtual_conf = virtual_h.quality * normal_conf * rough_conf * parallax_conf;

            // Surface history becomes less reliable than virtual history for narrow lobes under translation.
            Float view_angle = direction_angle(V, Vprev);
            Float surface_count = min(surface.h.fast_count.w, p.max_history) * surface.quality;
            Float tana = lobe_tan(r, .75f) * lerp(r, NoV, 1.f) / (1.f + surface_count);
            tana /= saturate(hit / frustum_size(p, g)) + 1e-6f;
            Float a0 = max(atan(tana), .5f / 255.f);
            Float surface_conf = pow(saturate(1.f - view_angle / a0), 4.f);
            surface_count = min(surface_count, p.max_history * surface_conf);
            Float virtual_count = min(virtual_h.h.fast_count.w, p.max_history) * virtual_conf;
            Float amount = smooth01(linear_step(.05f, .95f, Dfactor)) * normal_conf * rough_conf;
            Float ratio = saturate(virtual_count / (surface_count + 1e-6f));
            amount *= ratio / (1.f + max(surface_count - virtual_count, 0.f) * (1.f - ratio));
            Float magic = select(virtual_count > surface_count, 8.f, .5f);
            amount *= 1.f + (virtual_count - surface_count) / (magic * max(virtual_count, surface_count) + 1.f);
            amount = saturate(amount) * (hit > 0.f).cast<float>();

            Float4 sd = mix_history(surface.h.diffuse, d, 1.f / (1.f + diff_count), 1.f, p.max_history);
            Float4 ss = mix_history(surface.h.specular, s, 1.f / (1.f + surface_count), r, p.max_history);
            Float4 vs = mix_history(virtual_h.h.specular, s, 1.f / (1.f + virtual_count), r, p.max_history);
            Float df = lerp(1.f / (1.f + min(diff_count, p.fast_history)), surface.h.fast_count.x, d.x);
            Float sf = lerp(1.f / (1.f + min(surface_count, p.fast_history)), surface.h.fast_count.y, s.x);
            Float vf = lerp(1.f / (1.f + min(virtual_count, p.fast_history)), virtual_h.h.fast_count.y, s.x);
            d = sd; s = lerp(amount, ss, vs);
            out.fast_count = make_float4(df, lerp(amount, sf, vf), diff_count, lerp(amount, surface_count, virtual_count));
            out.stabilized = make_float4(surface.h.stabilized.x,
                lerp(amount, surface.h.stabilized.y, virtual_h.h.stabilized.y), surface.quality,
                lerp(amount, surface.quality * surface_conf, virtual_conf));
            out.tracking = make_float4(virtual_raster, curvature, hit);
        };
        $if(!valid_guide(g)) {
            out.diffuse = make_float4(0.f); out.specular = make_float4(0.f);
            out.fast_count = make_float4(0.f); out.stabilized = make_float4(0.f); out.tracking = make_float4(0.f);
            d = make_float4(0.f); s = make_float4(0.f);
        };
        Var<TemporalData> current;
        current.fast_count = store_half(out.fast_count); current.stabilized = store_half(out.stabilized);
        current.tracking = out.tracking;
        current_history.write(i, current);
        p.dst_diffuse.write(i, store_half(d)); p.dst_specular.write(i, store_half(s));
    };
    temporal_ = device.compile(temporal, "REBLUR-TemporalAccumulation");
}
}
