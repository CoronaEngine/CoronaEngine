#include "render_core/integrator/ReSTIR/gi_reconnection.h"

#include <array>
#include <iostream>

namespace {
bool expect(bool condition, const char *message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
}

int main() {
    using vision::gi_reconnection_supported;
    bool ok = true;
    ok &= expect(gi_reconnection_supported(1.f, 1.f),
                 "front-side secondary reconnection must remain supported");
    ok &= expect(gi_reconnection_supported(-1.f, -1.f),
                 "back-side secondary reconnection must remain supported for two-sided materials");
    // A secondary at the origin with normal +Z and receivers at Z=+1/-1
    // has an absolute-cosine Jacobian of exactly one, despite changing sides.
    ok &= expect(!gi_reconnection_supported(1.f, -1.f),
                 "unit-Jacobian front-to-back reconnection must reject cached opposite-side radiance");
    ok &= expect(!gi_reconnection_supported(-1.f, 1.f),
                 "unit-Jacobian back-to-front reconnection must reject cached opposite-side radiance");
    ok &= expect(!gi_reconnection_supported(0.f, 1.f) &&
                 !gi_reconnection_supported(1.f, 0.f) &&
                 !gi_reconnection_supported(0.f, 0.f),
                 "grazing or degenerate cosine support must remain invalid");
    for (float target : std::array{-.9f, -.1f, 0.f, .1f, .9f}) {
        for (float source : std::array{-.9f, -.1f, 0.f, .1f, .9f}) {
            ok &= expect(gi_reconnection_supported(target, source) ==
                         gi_reconnection_supported(-target, -source),
                         "reversing the geometric normal must preserve reconnection support");
        }
    }
    ok &= expect(gi_reconnection_supported(.0001f, .9f) &&
                 gi_reconnection_supported(.9f, .0001f),
                 "same-side support must not introduce a Jacobian threshold or clamp");
    if (ok) std::cout << "GI secondary reconnection support checks passed\n";
    return ok ? 0 : 1;
}
