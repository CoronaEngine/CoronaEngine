#include "relax_test_fixture.h"
#include <iostream>
#include <limits>

void check_relax_atrous(ocarina::Device &device, ocarina::Stream &stream) {
    using namespace relax_test;
    for (auto size : {make_uint2(1u), make_uint2(127u, 73u)}) {
        Fixture constant(device, stream, size);
        constant.filter.settings.diffuse_prepass = constant.filter.settings.specular_prepass = 0.f;
        for (uint iterations : {2u, 5u, 8u}) {
            constant.filter.settings.atrous_iterations = iterations;
            constant.filter.reset(); constant.run(iterations);
            for (uint i = 0; i < constant.n; ++i) {
                expect(std::abs(constant.got_d[i].x - .2f) < 1e-4f && std::abs(constant.got_s[i].z - .1f) < 1e-4f,
                       "every Atrous iteration count must preserve a constant signal and partial tiles");
                expect(constant.got_d[i].w >= 0.f && constant.got_d[i].w < 1e-4f,
                       "Atrous must convert the constant signal's second moment to zero variance");
            }
        }
    }
    Fixture impulse(device, stream, make_uint2(32u));
    auto &settings = impulse.filter.settings;
    settings.diffuse_prepass = settings.specular_prepass = 0.f;
    settings.history_fix_frames = 0.f; settings.diffuse_fast_history = settings.diffuse_history;
    settings.antilag_acceleration = settings.antilag_reset = 0.f;
    for (auto &d : impulse.d) d = make_float4(0.f, 0.f, 0.f, 7.f);
    impulse.d[528] = make_float4(.2f, .2f, .2f, 7.f);
    std::vector<vision::relax::PersistentHistory> stored(impulse.n);
    float two_pass = 0.f;
    for (uint iterations : {2u, 5u, 8u}) {
        settings.atrous_iterations = iterations;
        impulse.filter.reset(); impulse.run(iterations);
        stream << impulse.filter.persistent_history().download(stored.data()) << synchronize() << commit();
        std::cout << "Atrous " << iterations << ": history=" << stored[528].diffuse.x << ',' << stored[528].diffuse.w
                  << ", output=" << impulse.got_d[528].x << ',' << impulse.got_d[528].w << '\n';
        expect(std::abs(stored[528].diffuse.x - .2f) < 1e-5f && std::abs(stored[528].diffuse.w - .04f) < 1e-5f,
               "spatially filtered output must not feed back into slow illumination or moment history");
        for (auto c : impulse.got_d) expect(std::isfinite(c.x) && std::isfinite(c.w) && c.w >= 0.f,
                                         "Atrous RGB and propagated variance must remain finite");
        if (iterations == 2u) {
            two_pass = impulse.got_d[528].x;
            expect(std::abs(two_pass - .008f) < 1e-5f,
                   "short-history spatial estimation must average the 5x5 impulse before step-two filtering");
            expect(std::abs(impulse.got_d[528].w - .000378558618439f) < 1e-7f,
                   "Atrous variance must propagate with squared normalized filter weights");
        } else expect(impulse.got_d[528].x < two_pass - 1e-4f, "larger Atrous steps must execute additional spatial reuse");
    }
    std::cout << "PASS: RELAX 2/5/8 Atrous iterations, variance propagation and independent display output\n";
}

void check_relax_history(ocarina::Device &device, ocarina::Stream &stream) {
    using namespace relax_test;
    Fixture fix(device, stream, make_uint2(32u));
    fix.filter.settings.diffuse_prepass = fix.filter.settings.specular_prepass = 0.f;
    fix.filter.settings.antilag_reset = 0.f;
    for (auto &d : fix.d) d = make_float4(0.f, 0.f, 0.f, 7.f);
    fix.d[528] = make_float4(100.f, 0.f, 0.f, 7.f);
    fix.run(0u);
    std::vector<vision::relax::PersistentHistory> stored(fix.n);
    stream << fix.filter.persistent_history().download(stored.data()) << synchronize() << commit();
    expect(std::abs(stored[528].diffuse.x - 4.f) < 1e-4f,
           "short-history repair must use 25 compatible taps at the reference stride");

    Fixture light(device, stream, make_uint2(32u));
    light.filter.settings.diffuse_prepass = light.filter.settings.specular_prepass = 0.f;
    for (auto &d : light.d) d = make_float4(1.f, 1.f, 1.f, 7.f);
    for (uint frame = 0; frame < 32; ++frame) light.run(frame);
    for (auto &d : light.d) d = make_float4(5.f, 5.f, 5.f, 7.f);
    light.run(32u);
    stream << light.filter.persistent_history().download(stored.data()) << synchronize() << commit();
    // Slow=35/31, fast=11/7, acceleration=288/217, reset=12/31.
    // The independently reduced reference result is 24971/6727.
    expect(std::abs(stored[528].diffuse.x - 3.71205589416f) < 1e-4f,
           "clamping and antilag must accelerate the reference uniform lighting transition");
    for (uint frame = 33; frame < 65; ++frame) light.run(frame);
    for (auto &d : light.d) d = make_float4(0.f, 0.f, 0.f, 7.f);
    light.run(65u);
    stream << light.filter.persistent_history().download(stored.data()) << synchronize() << commit();
    expect(stored[528].diffuse.x < 3.f && std::isfinite(stored[528].diffuse.w),
           "turning a light off must rapidly remove the old bright history");

    Fixture firefly(device, stream, make_uint2(16u));
    auto &settings = firefly.filter.settings;
    settings.diffuse_prepass = settings.specular_prepass = 0.f;
    settings.history_fix_frames = 0.f;
    settings.diffuse_fast_history = settings.diffuse_history;
    settings.specular_fast_history = settings.specular_history;
    settings.antilag_acceleration = settings.antilag_reset = 0.f;
    for (auto &d : firefly.d) d = make_float4(1.f, 1.f, 1.f, 7.f);
    firefly.d[136] = make_float4(100.f, 100.f, 100.f, 7.f);
    std::vector<vision::relax::PersistentHistory> bright(firefly.n);
    firefly.run(0u);
    stream << firefly.filter.persistent_history().download(bright.data()) << synchronize() << commit();
    expect(bright[136].diffuse.x == 100.f, "AntiFirefly disabled must preserve the illumination history");
    settings.anti_firefly = 1u; firefly.filter.reset(); firefly.run(1u);
    stream << firefly.filter.persistent_history().download(bright.data()) << synchronize() << commit();
    expect(bright[136].diffuse.x == 1.f, "AntiFirefly must replace an isolated high outlier from a read-only neighborhood");
    for (uint y = 7; y <= 8; ++y) for (uint x = 7; x <= 8; ++x)
        firefly.d[y * 16 + x] = make_float4(5.f, 5.f, 5.f, 7.f);
    firefly.filter.reset(); firefly.run(2u);
    stream << firefly.filter.persistent_history().download(bright.data()) << synchronize() << commit();
    expect(bright[136].diffuse.x == 5.f, "AntiFirefly must retain a supported small bright feature");
    std::cout << "PASS: RELAX short-history repair, color clamping, antilag and safe firefly suppression\n";
}

void check_relax_temporal(ocarina::Device &device, ocarina::Stream &stream) {
    using namespace relax_test;
    Fixture f(device, stream, make_uint2(32u), 3u);
    f.filter.settings.diffuse_prepass = f.filter.settings.specular_prepass = 0.f;
    std::vector<vision::relax::TemporalData> history(f.n);
    stream << f.filter.history().upload(history.data()) << synchronize() << commit();
    auto download = [&] { stream << f.filter.history().download(history.data()) << synchronize() << commit(); };
    f.run(0u); download();
    expect(history[528].meta.x == 1.f, "first frame must publish one frame of RELAX history");
    for (uint frame = 1; frame < 64u; ++frame) {
        for (auto &g : f.guides) g.motion = make_float2((frame & 1u) ? .25f : -.25f, 0.f);
        f.run(frame);
    }
    download();
    expect(history[528].meta.x > 29.f && history[528].meta.x <= 31.f,
           "stationary history must grow to the NRD maximum plus current frame");
    expect(std::abs(f.got_d[528].x - .2f) < 1e-4f && std::abs(f.got_s[528].z - .1f) < 1e-4f,
           "temporal moments and both histories must preserve constant RGB");
    for (auto &g : f.guides) g.identity.z = 91u;
    for (auto &d : f.d) d = make_float4(4.f, 0.f, 0.f, 7.f);
    f.run(64u); download();
    expect(history[528].meta.x == 1.f && std::abs(f.got_d[528].x - 4.f) < 1e-4f,
           "changed branch must discard incompatible previous illumination");
    f.run(65u, 1u); download();
    expect(history[528].meta.x == 1.f, "a new layer must not consume another layer's history");
    f.run(68u); download();
    expect(history[528].meta.x == 1.f, "frame gaps must invalidate history");
    f.filter.reset(); f.run(69u); download();
    expect(history[528].meta.x == 1.f, "explicit camera or scene reset must discard history");
    for (auto &g : f.guides) { g.identity.w = 0u; g.position.w = 0.f; }
    f.run(70u); download();
    expect(history[528].meta.x == 0.f && f.got_d[528].x == 0.f, "empty layers must clear temporal history");

    Fixture mirror(device, stream, make_uint2(32u));
    mirror.filter.settings.diffuse_prepass = mirror.filter.settings.specular_prepass = 0.f;
    mirror.filter.settings.history_fix_frames = 0.f;
    mirror.filter.settings.antilag_acceleration = mirror.filter.settings.antilag_reset = 0.f;
    for (uint i = 0; i < mirror.n; ++i) {
        mirror.guides[i].normal_roughness.w = 0.f;
        mirror.s[i] = make_float4(float(i % 32u), 0.f, 0.f, 5.f);
    }
    mirror.run(0u);
    for (auto &s : mirror.s) s = make_float4(0.f, 0.f, 0.f, 5.f);
    mirror.run(1u);
    expect(mirror.got_s[528].x > 10.9f && std::isfinite(mirror.got_s[528].x),
           "mirror history must use virtual reprojection instead of only the surface pixel");

    Fixture motion(device, stream, make_uint2(32u));
    motion.filter.settings.diffuse_prepass = motion.filter.settings.specular_prepass = 0.f;
    motion.filter.settings.diffuse_fast_history = 30.f;
    motion.filter.settings.history_fix_frames = 0.f;
    motion.filter.settings.antilag_acceleration = motion.filter.settings.antilag_reset = 0.f;
    for (uint i = 0; i < motion.n; ++i) motion.d[i] = make_float4(float(i % 32u), 0.f, 0.f, 7.f);
    motion.run(0u);
    for (auto &g : motion.guides) g.motion = make_float2(1.f, 0.f);
    for (auto &d : motion.d) d = make_float4(0.f, 0.f, 0.f, 7.f);
    motion.run(1u);
    std::vector<vision::relax::PersistentHistory> persistent(motion.n);
    stream << motion.filter.persistent_history().download(persistent.data()) << synchronize() << commit();
    expect(std::abs(persistent[264].diffuse.x - 3.5f) < 1e-4f,
           "positive one-pixel motion must reproject x=8 to previous x=7 with alpha one half");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (auto &h : persistent) { h.diffuse = h.specular = h.diffuse_fast = h.specular_fast = make_float4(nan); h.length_hit = make_float2(nan); }
    motion.filter.reset();
    stream << motion.filter.persistent_history().upload(persistent.data()) << synchronize() << commit();
    motion.run(2u);
    for (auto c : motion.got_s) expect(std::isfinite(c.x), "reset must not read poisoned previous illumination");
    std::cout << "PASS: RELAX slow/fast histories, virtual motion, layer identity and invalidation\n";
}

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
