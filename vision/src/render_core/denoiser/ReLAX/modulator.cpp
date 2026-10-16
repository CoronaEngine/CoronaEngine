#include "filter.h"
#include "utils.h"

namespace vision::relax {
void Filter::compile_modulation(Device &device) {
    Kernel pack = [](Var<FilterParam> p) {
        Uint i = dispatch_id();
        auto g = p.guide.read(i);
        Float4 d = make_float4(0.f), s = make_float4(0.f);
        $if(g.identity.w != 0u && g.position.w > 0.f) {
            d = pack_signal(p.raw_diffuse.read(i), g.diffuse_factor.xyz());
            s = pack_signal(p.raw_specular.read(i), g.specular_factor.xyz());
        };
        p.dst_diffuse.write(i, d); p.dst_specular.write(i, s);
    };
    pack_ = device.compile(pack, "RELAX-Pack");
    Kernel unpack = [](Var<FilterParam> p) {
        Uint i = dispatch_id(); auto g = p.guide.read(i);
        Float4 d = make_float4(0.f), s = make_float4(0.f);
        $if(g.identity.w != 0u && g.position.w > 0.f) {
            d = p.src_diffuse.read(i); s = p.src_specular.read(i);
            d.xyz() *= max(g.diffuse_factor.xyz(), make_float3(.01f));
            s.xyz() *= max(g.specular_factor.xyz(), make_float3(.01f));
        };
        p.output_diffuse.write(i, d); p.output_specular.write(i, s);
    };
    unpack_ = device.compile(unpack, "RELAX-Unpack");
}
}
