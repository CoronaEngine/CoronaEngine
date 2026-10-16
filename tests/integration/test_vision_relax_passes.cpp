#include "relax_test_fixture.h"
#include <iostream>

void check_relax_prepass(ocarina::Device &device, ocarina::Stream &stream) {
    using namespace relax_test;
    for (auto size : {make_uint2(1u), make_uint2(8u), make_uint2(127u, 73u)}) {
        Fixture f(device, stream, size);
        f.run(0u);
        for (uint i = 0; i < f.n; ++i) {
            expect(std::abs(f.got_d[i].x - .2f) < 1e-4f && std::abs(f.got_s[i].z - .1f) < 1e-4f,
                   "prepass must preserve constant RGB, including partial tiles");
            expect(std::abs(f.prepass_d[i].w - 7.f) < 1e-4f && std::abs(f.prepass_s[i].w - 5.f) < 1e-4f,
                   "prepass must retain world-space hit distances");
        }
        f.filter.settings.diffuse_prepass = 0.f;
        f.filter.settings.specular_prepass = 0.f;
        for (uint mode = 1; mode <= 2; ++mode) {
            f.filter.settings.reconstruction = mode;
            f.d[f.n / 2] = make_float4(0.f);
            f.s[f.n / 2] = make_float4(0.f);
            f.run(mode);
            if (f.n > 1u) {
                expect(std::abs(f.prepass_d[f.n / 2].w - 7.f) < 1e-4f,
                       "missing diffuse hit distance must reconstruct from compatible neighbors");
                expect(std::abs(f.prepass_s[f.n / 2].w - 5.f) < 1e-4f,
                       "missing specular hit distance must reconstruct from compatible neighbors");
            }
            f.d[f.n / 2] = make_float4(0.f, 0.f, 0.f, 3.f);
            f.s[f.n / 2] = make_float4(0.f, 0.f, 0.f, 3.f);
            f.run(mode + 3u);
            expect(f.prepass_d[f.n / 2].w > 2.99f && f.prepass_d[f.n / 2].w < 3.1f,
                   "a valid black sample must dominate reconstruction of its own distance");
        }
        for (auto &g : f.guides) { g.identity.w = 0u; g.position.w = 0.f; }
        f.run(8u);
        for (uint i = 0; i < f.n; ++i)
            expect(f.got_d[i].x == 0.f && f.got_s[i].x == 0.f, "empty layer output must be zero");
    }
    Fixture edge(device, stream, make_uint2(32u));
    for (uint i = 0; i < edge.n; ++i) {
        bool right = i % 32u >= 16u;
        edge.guides[i].position.z = edge.guides[i].position.w = right ? 8.f : 2.f;
        edge.guides[i].normal_roughness.w = right ? 1.f : 0.f;
        edge.d[i] = right ? make_float4(0.f, 0.f, 8.f, 7.f) : make_float4(2.f, 0.f, 0.f, 7.f);
    }
    edge.run(0u);
    expect(edge.got_d[16u * 32u + 15u].z < 1e-5f && edge.got_d[16u * 32u + 16u].x < 1e-5f,
           "prepass must reject a depth discontinuity in the same material");
    Fixture oracle(device, stream, make_uint2(5u));
    oracle.filter.settings.diffuse_prepass = 1.f;
    for (auto &d : oracle.d) d = make_float4(2.f, 0.f, 0.f, 7.f);
    oracle.d[12] = make_float4(1.f, 0.f, 0.f, 7.f);
    oracle.run(0u);
    // Eight independently rounded Poisson taps: 0/2/5 hit the center;
    // 1/3/4/6/7 hit value 2. All geometry/hit-distance weights are one.
    constexpr double r[] = {.6461146, .9542373, .5335386, .6520134, .6695386, .3149309, .8895339, .8346850};
    double sum = 1., weights = 1.;
    for (uint k = 0; k < 8; ++k) {
        double w = std::exp(-.66 * r[k] * r[k]);
        weights += w; sum += w * ((k == 0 || k == 2 || k == 5) ? 1. : 2.);
    }
    expect(std::abs(oracle.prepass_d[12].x - float(sum / weights)) < 1e-4f,
           "prepass must apply the reference Poisson locations and Gaussian weights");
    std::cout << "PASS: RELAX prepass, reconstruction, black samples, empty and partial tiles\n";
}
