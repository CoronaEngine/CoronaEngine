#include "utils.h"

namespace vision::reblur {
void Filter::compile_modulation(Device &device) {
    Kernel pack = [](Var<FilterParam> p) {
        Uint i = dispatch_id();
        auto g = p.guide.read(i);
        Float4 d = make_float4(0.f), s = make_float4(0.f);
        $if(valid_guide(g)) {
            d = pack_signal(p.raw_diffuse.read(i), g.diffuse_factor.xyz(), g.position.w, 1.f);
            s = pack_signal(p.raw_specular.read(i), g.specular_factor.xyz(), g.position.w, g.normal_roughness.w);
        };
        p.dst_diffuse.write(i, d); p.dst_specular.write(i, s);
    };
    pack_ = device.compile(pack, "REBLUR-Pack");
    Kernel unpack = [](Var<FilterParam> p) {
        Uint i = dispatch_id();
        auto g = p.guide.read(i);
        Float4 d = p.raw_diffuse.read(i), s = p.raw_specular.read(i);
        $if(valid_guide(g)) {
            Float4 dd = p.src_diffuse.read(i), ss = p.src_specular.read(i);
            d = make_float4(to_rgb(dd.xyz()) * max(g.diffuse_factor.xyz(), make_float3(1e-3f)), dd.w * hit_scale(g.position.w, 1.f));
            s = make_float4(to_rgb(ss.xyz()) * max(g.specular_factor.xyz(), make_float3(1e-3f)), ss.w * hit_scale(g.position.w, g.normal_roughness.w));
        };
        p.output_diffuse.write(i, d); p.output_specular.write(i, s);
        Var<GeometryHistory> saved;
        saved.position = g.position; saved.normal_roughness = g.normal_roughness; saved.identity = g.identity;
        p.prev_guide.write(i, saved);
    };
    unpack_ = device.compile(unpack, "REBLUR-Unpack");
}
}
