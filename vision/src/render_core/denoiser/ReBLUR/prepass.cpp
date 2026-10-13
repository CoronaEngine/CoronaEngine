#include "utils.h"

namespace vision::reblur {
void Filter::compile_prepass(Device &device) {
    Kernel reconstruction = [](Var<FilterParam> p) {
        auto guides = p.guide.soa_view_var<Guide>();
        Uint i = dispatch_id(); Int2 pixel = make_int2(dispatch_idx().xy());
        auto g = guides.read(i);
        Float4 d = make_float4(p.src_diffuse.read(i)), s = make_float4(p.src_specular.read(i));
        $if(valid_guide(g) && (d.w == 0.f || s.w == 0.f)) {
            Float2 sum = make_float2(0.f), weights = make_float2(0.f);
            Int radius = select(p.reconstruction == 1u, 1, 2);
            $for(y, -radius, radius + 1) {
                $for(x, -radius, radius + 1) {
                    Int2 q = pixel + make_int2(x, y);
                    $if(in_screen(q, p.size)) {
                        Uint j = pixel_index(q, p.size);
                        auto ng = guides.read(j);
                        Float4 nd = make_float4(p.src_diffuse.read(j)), ns = make_float4(p.src_specular.read(j));
                        Float gaussian = exp(-.66f * (x * x + y * y).cast<float>());
                        Float wd = spatial_weight(p, g, ng, 1.f, 1.f, false) * gaussian * (nd.w > 0.f).cast<float>();
                        Float ws = spatial_weight(p, g, ng, 1.f, 1.f, true) * gaussian * (ns.w > 0.f).cast<float>();
                        sum += make_float2(nd.w * wd, ns.w * ws); weights += make_float2(wd, ws);
                    };
                };
            };
            d.w = select(d.w == 0.f && weights.x > 0.f, sum.x / max(weights.x, 1e-6f), d.w);
            s.w = select(s.w == 0.f && weights.y > 0.f, sum.y / max(weights.y, 1e-6f), s.w);
        };
        p.dst_diffuse.write(i, store_half(d)); p.dst_specular.write(i, store_half(s));
    };
    reconstruct_ = device.compile(reconstruction, "REBLUR-HitDistReconstruction");
    prepass_ = compile_spatial_pass(device, 0u);
}
}
