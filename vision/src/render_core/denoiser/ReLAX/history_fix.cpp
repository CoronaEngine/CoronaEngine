#include "filter.h"
#include "utils.h"

namespace vision::relax {
void Filter::compile_history_fix(Device &device) {
    Kernel fix = [](Var<FilterParam> p) {
        Uint idx = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        auto center = load_guide(p, idx); auto data = p.history.read(idx);
        Float4 d = data.diffuse_fast, s = data.specular_fast;
        $if(valid_guide(center) && p.settings.history_fix_frames > 0.f && data.meta.x <= p.settings.history_fix_frames + 1.f) {
            d = p.src_diffuse.read(idx); s = p.src_specular.read(idx);
            Float wd = 1.f, ws = 1.f;
            Float3 view = safe_normalize(p.camera.as_vec3() - center.position.xyz());
            Float2 normal_params = atrous_normal_params(center.normal_roughness.w, 5.f, 1.f, 0.f,
                                                       p.settings.lobe_fraction, p.settings.specular_lobe_slack);
            Int stride = cast<int>(floor(p.settings.history_fix_stride / (1.f + data.meta.x) + .5f));
            $for(y, -2, 3) {
                $for(x, -2, 3) {
                    Int2 pixel_tap = pixel + make_int2(x, y) * stride;
                    $if((x != 0 || y != 0) && inside(pixel_tap, p.size)) {
                        Uint tap = pixel_index(pixel_tap, p.size); auto g = load_guide(p, tap);
                        $if(spatial_compatible(center, g)) {
                            Float geometry = plane_weight(center, g, p.settings.depth_threshold);
                            Float w = geometry * pow(max(.01f, dot(center.normal_roughness.xyz(), g.normal_roughness.xyz())),
                                                     max(p.settings.history_fix_normal_power, .01f));
                            Float3 sample_view = safe_normalize(p.camera.as_vec3() - g.position.xyz() +
                                                               p.settings.roughness_relaxation * (p.camera.as_vec3() - center.position.xyz()));
                            Float v = geometry * atrous_specular_normal(normal_params, center.normal_roughness.xyz(),
                                                                        g.normal_roughness.xyz(), view, sample_view);
                            $if(w > 1e-4f) { d += p.src_diffuse.read(tap) * w; wd += w; };
                            $if(v > 1e-4f) { s += p.src_specular.read(tap) * v; ws += v; };
                        };
                    };
                };
            };
            d /= wd; s /= ws;
        };
        p.dst_diffuse.write(idx, d); p.dst_specular.write(idx, s);
    };
    fix_ = device.compile(fix, "RELAX-HistoryFix");
}
}
