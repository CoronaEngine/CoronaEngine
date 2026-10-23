#pragma once
#include "filter.h"
namespace vision::reblur {
template<typename Buffer>
[[nodiscard]] auto guide_view(const Buffer &buffer) {
    return buffer.template soa_view_var<Guide>();
}

template<typename Guides, typename Index>
[[nodiscard]] Var<Guide> read_guide_geometry(const Guides &guides, const Index &index) {
    // Spatial geometry tests do not consume demodulation or motion. Avoid
    // emitting reads/address calculations for those two unused field planes.
    Var<Guide> guide;
    guide.position = guides.position.read(index);
    guide.identity = guides.identity.read(index);
    guide.normal_roughness = guides.normal_roughness.read(index);
    guide.diffuse_factor = make_float4(0.f);
    guide.specular_factor = make_float4(0.f);
    return guide;
}
template<typename Buffer>
[[nodiscard]] auto geometry_history_view(const Buffer &buffer) {
    return buffer.template soa_view_var<GeometryHistory>();
}
}// namespace vision::reblur
