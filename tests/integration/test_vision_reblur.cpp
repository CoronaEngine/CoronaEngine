#include "render_core/denoiser/ReBLUR/reblur.h"
#include "render_core/denoiser/ReBLUR/utils.h"
#include "rhi/context.h"
#include "rhi/stats.h"
#include "base/import/importer.h"
#include "base/integral/integrator.h"
#include "base/mgr/pipeline.h"
#include "base/sampler.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <chrono>

namespace {
using namespace vision;
void expect(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

ocarina::HostByteBuffer pack_guides(const reblur::Guide *guides, size_t count) {
    ocarina::HostByteBuffer bytes;
    ocarina::DynamicBufferLayoutCodec<reblur::Guide>::encode(guides, count, bytes,
        {}, ocarina::DynamicBufferLayout::SOA);
    return bytes;
}

void unpack_history(const ocarina::HostByteBuffer &bytes, reblur::TemporalData *history, size_t count) {
    ocarina::DynamicBufferLayoutCodec<reblur::TemporalData>::decode(bytes, count, history,
        {}, ocarina::DynamicBufferLayout::SOA);
}

void check_storage_budget(Device &device) {
    const size_t before = ocarina::MemoryStats::instance().buffer_size();
    reblur::Filter filter;
    const uint2 size = make_uint2(640u, 360u);
    filter.prepare(device, size, 3u);
    const size_t bytes = ocarina::MemoryStats::instance().buffer_size() - before;
    std::cout << "REBLUR three-layer allocation: " << bytes << " bytes\n";
    // Budget includes full-precision geometry/tracking, three independent
    // histories and all scratch buffers. The FP32 layout uses 556 B/pixel.
    expect(bytes <= size_t(size.x) * size.y * 394u,
           "three-layer REBLUR storage must stay within 394 bytes per pixel");
}

void check_helpers(Device &device, Stream &stream) {
    auto result = device.create_buffer<float4>(11, "reblur helper results");
    Kernel kernel = [&](BufferVar<float4> output, Var<reblur::FilterParam> p) {
        Float3 rgb = make_float3(.2f, 4.f, 1.f);
        output.write(0u, make_float4(reblur::to_rgb(reblur::to_ycocg(rgb)), 1.f));
        output.write(1u, make_float4(reblur::hit_scale(10.f, 1.f), reblur::hit_scale(10.f, 0.f),
                                    reblur::hit_scale(-10.f, .3f), 0.f));
        output.write(2u, reblur::pack_signal(make_float4(0.f, 0.f, 0.f, 2.f), make_float3(0.f), 10.f, 1.f));
        Float2 motion = make_float2(2.f, -3.f);
        Float2 previous = make_float2(8.5f, 4.5f) - motion;
        output.write(3u, make_float4(previous, sqrt(.4f * .4f), sqrt(.2f * .8f)));
        output.write(4u, make_float4(reblur::spec_magic(0.f), reblur::spec_magic(1.f),
                                    reblur::exponential_weight(0.f), 0.f));
        output.write(5u, reblur::mix_history(make_float4(2.f, 0.f, 0.f, 1.f),
                                            make_float4(4.f, 0.f, 0.f, 0.f), .25f, 0.f, 30.f));
        output.write(6u, make_float4(select(reblur::guide_instance(1u, 0u) != reblur::guide_instance(2u, 0u), 1.f, 0.f),
            select(reblur::guide_instance(1u, 0u) != reblur::guide_instance(1u, 7u), 1.f, 0.f), 0.f, 0.f));
        output.write(7u, make_float4(reblur::store_half(make_float4(70000.f, -70000.f, .125f, -.125f))));
        // Read a rounded unit normal from the GPU buffer to avoid folding the
        // normalization on the host. A surface must give itself unit weight,
        // even when its FP32 squared length rounds below one.
        Float3 normal = output.read(8u).xyz();
        Var<reblur::Guide> g;
        g.position = make_float4(0.f, 0.f, 2.f, 2.f);
        g.identity = make_uint4(1u);
        g.normal_roughness = make_float4(normal, .001f);
        p.size = make_uint2(32u); p.unproject = .05f;
        p.plane_sensitivity = .02f; p.lobe_fraction = .15f; p.roughness_fraction = .15f;
        Float self_weight = reblur::spatial_weight(p, g, g, 1.f, 1.f, true);
        Var<reblur::Guide> opposite = g;
        opposite.normal_roughness = make_float4(-normal, .001f);
        Float opposite_weight = reblur::spatial_weight(p, g, opposite, 1.f, 1.f, true);
        output.write(8u, make_float4(self_weight, opposite_weight, 0.f, 0.f));
        Float3 tangent = normalize(make_float3(-normal.y, normal.x, 0.f));
        Float3 rotated = cos(.0001f) * normal + sin(.0001f) * tangent;
        output.write(9u, make_float4(reblur::direction_angle(normal, normal),
            reblur::direction_angle(normal, rotated), reblur::direction_angle(normal, -normal), 0.f));
        output.write(10u, make_float4(reblur::parallax_confidence(0.f, 0.f),
            reblur::parallax_confidence(.5f, 0.f), reblur::parallax_confidence(16.f, 0.f),
            reblur::parallax_confidence(.5f, 8.f)));
    };
    auto shader = device.compile(kernel, "reblur_reference_helpers");
    std::array<float4, 11> actual{};
    actual[8] = make_float4(.5773502588f, .5773502588f, .5773502588f, 0.f);
    stream << result.upload(actual.data()) << shader(result, reblur::FilterParam{}).dispatch(1u)
           << result.download(actual.data()) << synchronize() << commit();
    auto expect_near = [&](float got, float ref, const char *name) {
        expect(std::isfinite(got) && std::abs(got - ref) <= 1e-5f + 1e-4f * std::abs(ref), name);
    };
    expect_near(actual[0].x, .2f, "YCoCg red roundtrip");
    expect_near(actual[0].y, 4.f, "YCoCg green roundtrip");
    expect_near(actual[0].z, 1.f, "YCoCg blue roundtrip");
    expect_near(actual[1].x, 4.f * (1.f + 19.f * std::exp2(-25.f)), "diffuse world distance normalization");
    expect_near(actual[1].y, 80.f, "mirror world distance normalization");
    expect_near(actual[1].z, 4.f * (1.f + 19.f * std::exp2(-2.25f)), "roughness distance normalization");
    expect_near(actual[2].x, 0.f, "black sample remains black");
    expect(actual[2].w > 0.f, "valid black sample must retain hit distance");
    expect_near(actual[3].x, 6.5f, "motion x sign"); expect_near(actual[3].y, 7.5f, "motion y sign");
    expect_near(actual[3].z, .4f, "isotropic roughness"); expect_near(actual[3].w, .4f, "anisotropic roughness");
    expect_near(actual[4].x, 0.f, "mirror specular curve"); expect_near(actual[4].y, 1.f, "diffuse specular curve");
    expect_near(actual[4].z, 1.f, "center spatial weight");
    expect_near(actual[5].x, 2.5f, "radiance accumulation weight");
    expect_near(actual[5].w, 0.f, "mirror tracking must react immediately");
    expect(actual[6].x == 1.f && actual[6].y == 1.f,
           "layered identity must reject a changed receiver or stable prefix independently");
    expect(actual[7].x == 65504.f && actual[7].y == -65504.f &&
           actual[7].z == .125f && actual[7].w == -.125f,
           "FP16 storage must saturate reprojection overshoot without discarding signed chroma");
    expect_near(actual[8].x, 1.f, "identical glossy normals must have unit spatial weight");
    expect_near(actual[8].y, 0.f, "opposite glossy normals must reject spatial history");
    expect(actual[9].x == 0.f, "static directions must have exactly zero angular change");
    expect(std::abs(actual[9].y - .0001f) < 1e-7f,
           "real subpixel angular motion must not be rounded away");
    expect_near(actual[9].z, 3.14159265359f, "opposite directions must retain a pi angle");
    expect(actual[10].x == 1.f, "zero parallax must retain full confidence");
    expect(actual[10].y >= .5f, "half-pixel tracking variation must retain the pixel footprint");
    expect(actual[10].z < .04f, "large mirror parallax must strongly reduce confidence");
    expect(actual[10].w > actual[10].y, "a broad reflection lobe must tolerate more parallax");
    std::cout << "PASS: reference packing, distance, motion and accumulation helpers\n";
}

void check_storage_precision(Device &device, Stream &stream) {
    reblur::Filter filter;
    filter.prepare(device, make_uint2(1u));
    filter.compile(device);
    auto input = device.create_buffer<float4>(1u);
    auto output_d = device.create_buffer<float4>(1u), output_s = device.create_buffer<float4>(1u);
    reblur::Guide guide;
    guide.position = make_float4(0.f, 0.f, 1000000.f, 1000000.f);
    guide.normal_roughness = make_float4(0.f, 0.f, -1.f, .4f);
    guide.identity = make_uint4(1u);
    reblur::FilterParam p;
    p.raw_diffuse = p.raw_specular = input.descriptor();
    p.output_diffuse = output_d.descriptor(); p.output_specular = output_s.descriptor();
    p.unproject = .05f; p.history_valid = 1u;
    for (auto signal : {make_float4(.2f, .6f, 1.f, 100000.f),
                        make_float4(50000.f, 10000.f, 30000.f, 100000.f),
                        make_float4(0.f, 1000.f, 40000.f, 100000.f),
                        make_float4(65504.f, 65504.f, 65504.f, 100000.f)}) {
        filter.reset();
        for (uint frame = 0; frame < 16u; ++frame) {
            p.frame = frame;
            float4 d, s;
            reblur::TemporalData history;
            ocarina::HostByteBuffer history_bytes(sizeof(history));
            const auto packed = pack_guides(&guide, 1u);
            stream << filter.guides().upload(packed.data()) << input.upload(&signal)
                   << filter.dispatch(p) << output_d.download(&d) << output_s.download(&s)
                   << filter.history().download(history_bytes.data()) << synchronize() << commit();
            unpack_history(history_bytes, &history, 1u);
            // Bound RGB error relative to the signal peak: FP16 YCoCg may
            // round a dim channel in the presence of a much brighter channel.
            const float tolerance = .003f * std::max({signal.x, signal.y, signal.z});
            for (auto value : {d, s}) {
                for (uint c = 0; c < 3u; ++c) expect(std::isfinite(value[c]) &&
                    std::abs(value[c] - signal[c]) <= tolerance,
                    "FP16 histories must preserve HDR and signed chroma within the quantization budget");
                expect(std::isfinite(value.w) && std::abs(value.w - signal.w) < 100.f,
                    "world-space hit distance must survive FP16 signal normalization");
            }
            expect(std::isfinite(history.tracking.w) && history.tracking.w > 99900.f,
                "world-space tracking must retain distances above the FP16 range");
        }
    }
    std::cout << "PASS: HDR, signed chroma and large-world tracking through all REBLUR stages\n";
}

void check_filter(Device &device, Stream &stream) {
    reblur::Filter filter;
    const uint2 size = make_uint2(32u, 32u);
    const uint n = size.x * size.y;
    filter.prepare(device, size, 3u);
    filter.compile(device);
    auto diffuse = device.create_buffer<float4>(n, "reblur test diffuse");
    auto specular = device.create_buffer<float4>(n, "reblur test specular");
    auto output_d = device.create_buffer<float4>(n, "reblur test output diffuse");
    auto output_s = device.create_buffer<float4>(n, "reblur test output specular");
    std::vector<reblur::Guide> guides(n);
    std::vector<float4> d(n, make_float4(.2f, .6f, 1.f, 4.f));
    std::vector<float4> s(n, make_float4(1.f, .3f, .1f, 3.f)), got_d(n), got_s(n);
    std::vector<reblur::TemporalData> history(n);
    ocarina::HostByteBuffer history_bytes(n * sizeof(reblur::TemporalData));
    for (uint y = 0; y < size.y; ++y) for (uint x = 0; x < size.x; ++x) {
        auto &g = guides[y * size.x + x];
        g.position = make_float4((float(x) + .5f - 16.f) * .1f, (float(y) + .5f - 16.f) * .1f, 2.f, 2.f);
        g.normal_roughness = make_float4(0.f, 0.f, -1.f, .4f);
        g.identity = make_uint4(1u, 2u, 3u, 1u);
        g.diffuse_factor = make_float4(.2f, .3f, .7f, 0.f);
        g.specular_factor = make_float4(.8f, .1f, .01f, 0.f);
    }
    reblur::FilterParam p;
    p.raw_diffuse = diffuse.descriptor(); p.raw_specular = specular.descriptor();
    p.output_diffuse = output_d.descriptor(); p.output_specular = output_s.descriptor();
    p.unproject = .05f; p.history_valid = 1u;
    p.world_to_raster = make_float4x4(make_float4(10.f, 0.f, 0.f, 0.f), make_float4(0.f, 10.f, 0.f, 0.f),
        make_float4(0.f, 0.f, 1.f, 0.f), make_float4(16.f, 16.f, 0.f, 1.f));
    p.prev_world_to_raster = p.world_to_raster;
    auto run = [&](uint frame, uint layer = 0u) {
        p.frame = frame;
        const auto packed = pack_guides(guides.data(), n);
        stream << filter.guides().upload(packed.data()) << diffuse.upload(d.data()) << specular.upload(s.data());
        stream << filter.dispatch(p, layer) << output_d.download(got_d.data()) << output_s.download(got_s.data())
               << filter.history().download(history_bytes.data()) << synchronize() << commit();
        unpack_history(history_bytes, history.data(), n);
    };
    for (uint frame = 0; frame < 64u; ++frame) {
        run(frame);
        for (uint i = 0; i < n; ++i) {
            expect(std::abs(got_d[i].x - .2f) < .001f && std::abs(got_d[i].y - .6f) < .001f &&
                   std::abs(got_s[i].z - .1f) < .001f, "constant radiance must survive every REBLUR stage");
            expect(std::isfinite(got_s[i].x), "all filtered values must remain finite");
        }
    }
    expect(history[n / 2].fast_count.z > 25.f, "static diffuse history must grow to the configured limit");
    const auto saved_motion = make_float2(guides[n / 2].diffuse_factor.w, guides[n / 2].specular_factor.w);
    for (auto &g : guides) g.identity.z = 9u;
    std::fill(d.begin(), d.end(), make_float4(4.f, 0.f, 0.f, 4.f));
    run(64u);
    expect(history[n / 2].fast_count.z <= 1.01f && got_d[n / 2].x > 3.99f,
           "branch changes must reject stale lighting history");
    std::vector<reblur::Guide> downloaded(n);
    ocarina::HostByteBuffer downloaded_bytes(n * sizeof(reblur::Guide));
    stream << filter.guides().download(downloaded_bytes.data()) << synchronize() << commit();
    ocarina::DynamicBufferLayoutCodec<reblur::Guide>::decode(downloaded_bytes, n, downloaded.data(),
        {}, ocarina::DynamicBufferLayout::SOA);
    expect(downloaded[n / 2].diffuse_factor.w == saved_motion.x && downloaded[n / 2].specular_factor.w == saved_motion.y,
           "denoising must not mutate producer motion");
    run(64u, 1u);
    expect(history[n / 2].fast_count.z <= 1.01f, "new layers must not inherit another layer's history");
    for (auto &g : guides) { g.identity.w = 0u; g.position.w = 0.f; }
    std::fill(d.begin(), d.end(), make_float4(0.f)); std::fill(s.begin(), s.end(), make_float4(0.f));
    run(65u);
    for (uint i = 0; i < n; ++i) expect(history[i].fast_count.z == 0.f && got_d[i].x == 0.f,
        "empty tiles must clear history and output");
    for (auto &g : guides) { g.identity.w = 1u; g.position.w = 2.f; }
    std::fill(d.begin(), d.end(), make_float4(.2f, .6f, 1.f, 4.f));
    run(66u);
    expect(history[n / 2].fast_count.z <= 1.01f, "reappearing layers must start fresh");
    filter.reset(); run(67u);
    expect(history[n / 2].fast_count.z <= 1.01f, "explicit reset must discard history");
    run(70u);
    expect(history[n / 2].fast_count.z <= 1.01f, "a frame gap must discard history");
    filter.reset();
    for (uint i = 0; i < n; ++i) {
        const bool right = i % size.x >= size.x / 2;
        guides[i].position.z = right ? 8.f : 2.f; guides[i].position.w = guides[i].position.z;
        d[i] = right ? make_float4(0.f, 0.f, 8.f, 4.f) : make_float4(2.f, 0.f, 0.f, 4.f);
    }
    run(71u);
    expect(got_d[16u * 32u + 15u].z < 1e-4f && got_d[16u * 32u + 16u].x < 1e-4f,
           "spatial passes must reject a depth discontinuity even within one instance");
    filter.settings.max_radius = 0.f; filter.settings.diffuse_prepass = 0.f;
    filter.settings.specular_prepass = 0.f; filter.settings.history_fix_stride = 0.f;
    filter.settings.anti_firefly = 0u; filter.settings.stabilization = 0.f;
    filter.reset();
    for (uint i = 0; i < n; ++i) {
        guides[i].position.z = 2.f; guides[i].position.w = 2.f;
        guides[i].normal_roughness.w = 0.f;
        d[i] = make_float4(.5f, .5f, .5f, 2.f);
        s[i] = make_float4(float(i % size.x) / 32.f, 0.f, 0.f, 2.f);
    }
    run(72u);
    p.prev_camera = {0.f, 0.f, 0.f}; p.camera = {.4f, 0.f, 0.f};
    run(73u);
    const uint center = 16u * 32u + 16u;
    expect(std::abs(history[center].tracking.x - 16.5f) > 2.f,
           "mirror history must use reflected virtual motion instead of surface motion");
    expect(history[center].fast_count.z > 1.9f,
           "surface diffuse history must remain independent of mirror virtual motion");
    // Pixel jitter changes the sampled world position, not camera motion. It
    // must not cause a static mirror's virtual history to walk between pixels.
    p.camera = p.prev_camera = {0.f, 0.f, 0.f};
    p.world_to_raster = make_float4x4(make_float4(20.f, 0.f, 0.f, 0.f), make_float4(0.f, 20.f, 0.f, 0.f),
        make_float4(16.f, 16.f, 1.f, 1.f), make_float4(0.f));
    p.prev_world_to_raster = p.world_to_raster;
    filter.reset();
    for (auto &g : guides) g.position.x += .075f;
    run(74u); run(75u);
    expect(std::abs(history[center].tracking.x - 16.5f) < 1e-4f,
           "static mirror reprojection must remove the current film-sample jitter");
    std::cout << "PASS: depth discontinuity and specular virtual reprojection\n";
    // Interleave all three layers over consecutive frames. Identical geometry IDs
    // intentionally make storage isolation, not identity rejection, decisive.
    filter.reset();
    for (uint frame = 76u; frame < 96u; ++frame) for (uint layer = 0u; layer < 3u; ++layer) {
        std::fill(d.begin(), d.end(), make_float4(float(layer + 1u), .5f, .25f, 2.f));
        run(frame, layer);
        expect(std::abs(got_d[center].x - float(layer + 1u)) < .003f * float(layer + 1u),
            "shared current buffers must not mix stable-layer illumination");
        if (frame == 95u) expect(history[center].fast_count.z > 15.f,
            "each layer must retain its own history across sequential dispatches");
    }
    // Resize discards old histories, including partial tiles and a one-pixel image.
    for (auto extent : {make_uint2(1u), make_uint2(127u, 73u)}) {
        const uint count = extent.x * extent.y;
        filter.prepare(device, extent);
        auto small_d = device.create_buffer<float4>(count);
        auto small_s = device.create_buffer<float4>(count);
        auto small_o = device.create_buffer<float4>(count);
        auto small_q = device.create_buffer<float4>(count);
        std::vector<reblur::Guide> small_g(count, guides.front());
        std::vector<float4> constant(count, make_float4(.5f, .5f, .5f, 2.f)), actual(count);
        p.raw_diffuse = small_d.descriptor(); p.raw_specular = small_s.descriptor();
        p.output_diffuse = small_o.descriptor(); p.output_specular = small_q.descriptor(); p.frame = 74u;
        const auto packed = pack_guides(small_g.data(), count);
        stream << filter.guides().upload(packed.data()) << small_d.upload(constant.data())
               << small_s.upload(constant.data()) << filter.dispatch(p) << small_o.download(actual.data())
               << synchronize() << commit();
        for (const auto &pixel : actual) expect(std::isfinite(pixel.x) && std::abs(pixel.x - .5f) < .0015f,
            "resize and partial tiles must preserve a constant signal");
    }
    std::cout << "PASS: actual REBLUR passes, 64-frame history, layers, empty tiles, reset and gaps\n";
}

void check_curved_glossy_footprint(Device &device, Stream &stream) {
    reblur::Filter filter;
    const uint2 size = make_uint2(32u);
    const uint n = size.x * size.y;
    filter.prepare(device, size); filter.compile(device);
    filter.settings.max_radius = 10.f;
    filter.settings.specular_prepass = 15.f;
    filter.settings.anti_firefly = 0u;
    auto input = device.create_buffer<float4>(n), output = device.create_buffer<float4>(n);
    std::vector<reblur::Guide> guides(n);
    std::vector<float4> signal(n), actual(n);
    for (uint i = 0u; i < n; ++i) {
        float x = float(i % size.x) - 15.5f, y = float(i / size.x) - 15.5f;
        auto &g = guides[i];
        g.position = make_float4(x * .01f, y * .01f, 2.f + .00005f * (x*x + y*y), 2.f);
        auto normal = normalize(make_float3(.01f * x, .01f * y, -1.f));
        g.normal_roughness = make_float4(normal, .01f);
        g.identity = make_uint4(1u);
        float value = ((i % size.x + i / size.x) & 1u) ? .75f : 1.25f;
        signal[i] = make_float4(value, value, value, 2.f);
    }
    reblur::FilterParam p;
    p.raw_diffuse = p.raw_specular = input.descriptor();
    p.output_diffuse = p.output_specular = output.descriptor();
    p.unproject = .005f;
    const auto packed = pack_guides(guides.data(), n);
    stream << filter.guides().upload(packed.data()) << input.upload(signal.data())
           << filter.dispatch(p) << output.download(actual.data()) << synchronize() << commit();
    double mse = 0.;
    for (uint y = 4u; y < 28u; ++y) for (uint x = 4u; x < 28u; ++x) {
        float error = actual[y * size.x + x].x - 1.f;
        mse += error * error / (24. * 24.);
    }
    std::cout << "Curved glossy first-frame MSE: " << mse << " (input 0.0625)\n";
    expect(mse < .045, "curved low-roughness surfaces need a nonzero pixel reconstruction footprint");
    // A narrow, noise-free reflected highlight must survive repeated spatial
    // feedback. A material/branch change must discard the mature history.
    for (uint variant = 0u; variant < 4u; ++variant) {
        const float roughness = variant == 1u ? .03f : variant == 2u ? .04f : .01f;
        for (uint i = 0u; i < n; ++i) {
            guides[i].normal_roughness.w = roughness;
            guides[i].identity = make_uint4(1u);
            if (variant == 3u) {
                guides[i].normal_roughness = make_float4(0.f, 0.f, -1.f, roughness);
                guides[i].position.z = 2.f;
            }
        }
        const auto detail_guides = pack_guides(guides.data(), n);
        stream << filter.guides().upload(detail_guides.data()) << synchronize() << commit();
        filter.settings.stabilization = 0.f;
        filter.reset();
        for (uint i = 0u; i < n; ++i) {
            float value = i % size.x == 16u ? 4.f : 1.f;
            signal[i] = make_float4(value, value, value, 2.f);
        }
        p.history_valid = 1u;
        p.world_to_raster = make_float4x4(make_float4(200.f, 0.f, 0.f, 0.f), make_float4(0.f, 200.f, 0.f, 0.f),
            make_float4(16.f, 16.f, 1.f, 1.f), make_float4(0.f));
        p.prev_world_to_raster = p.world_to_raster;
        for (uint frame = 0u; frame < 64u; ++frame) {
            p.frame = frame;
            stream << input.upload(signal.data()) << filter.dispatch(p) << synchronize() << commit();
        }
        stream << output.download(actual.data()) << synchronize() << commit();
        float peak = actual[16u * size.x + 16u].x, side = actual[16u * size.x + 15u].x;
        std::cout << "Glossy narrow highlight variant " << variant << ": " << peak << ", neighbor " << side << "\n";
        expect(peak > 3.7f && side < 1.1f,
            "mature glossy history must preserve a one-pixel reflected highlight without bleeding");
        if (variant == 3u) {
            // Camera translation moves a planar reflected stripe by one pixel
            // while the surface motion stays zero. Virtual history must follow
            // the reflection instead of leaving the mature bright stripe behind.
            for (uint i = 0u; i < n; ++i) {
                float value = i % size.x == 17u ? 4.f : 1.f;
                signal[i] = make_float4(value, value, value, 2.f);
            }
            p.camera = {.02f, 0.f, 0.f}; p.frame = 64u;
            stream << input.upload(signal.data()) << filter.dispatch(p)
                   << output.download(actual.data()) << synchronize() << commit();
            std::cout << "Moving reflection: peak " << actual[16u * size.x + 17u].x
                      << ", old location " << actual[16u * size.x + 16u].x << "\n";
            expect(actual[16u * size.x + 17u].x > 3.7f && actual[16u * size.x + 16u].x < 1.1f,
                "same-surface reflected highlights must follow virtual motion without a bright trail");
        }
        for (auto &g : guides) g.identity.z = 2u;
        const auto changed = pack_guides(guides.data(), n);
        std::fill(signal.begin(), signal.end(), make_float4(.25f, .25f, .25f, 2.f));
        p.frame = variant == 3u ? 65u : 64u;
        stream << filter.guides().upload(changed.data()) << input.upload(signal.data())
               << filter.dispatch(p) << output.download(actual.data()) << synchronize() << commit();
        for (const auto &v : actual) expect(std::abs(v.x - .25f) < .001f,
            "disoccluded glossy surfaces must not retain the old bright highlight");
    }

}

void check_sparse_tiles(Device &device, Stream &stream) {
    reblur::Filter filter;
    const uint2 size = make_uint2(49u, 33u);
    const uint n = size.x * size.y;
    filter.prepare(device, size);
    filter.compile(device);
    auto input_d = device.create_buffer<float4>(n), input_s = device.create_buffer<float4>(n);
    auto output_d = device.create_buffer<float4>(n), output_s = device.create_buffer<float4>(n);
    std::vector<reblur::Guide> guides(n);
    std::vector<float4> signal_d(n, make_float4(.5f, .25f, 1.f, 2.f));
    std::vector<float4> signal_s(n, make_float4(1.5f, .75f, .125f, 4.f));
    std::array<std::vector<float4>, 2> actual{std::vector<float4>(n), std::vector<float4>(n)};
    std::vector<reblur::TemporalData> history(n);
    ocarina::HostByteBuffer history_bytes(n * sizeof(reblur::TemporalData));
    reblur::FilterParam p;
    p.raw_diffuse = input_d.descriptor(); p.raw_specular = input_s.descriptor();
    p.output_diffuse = output_d.descriptor(); p.output_specular = output_s.descriptor();
    p.unproject = .05f; p.history_valid = 1u;
    for (auto mode : {HitDistanceReconstructionMode::OFF, HitDistanceReconstructionMode::AREA_3X3,
                      HitDistanceReconstructionMode::AREA_5X5}) {
        filter.settings.reconstruction = mode;
        filter.reset();
        for (uint frame = 0; frame < 3u; ++frame) {
            for (uint i = 0; i < n; ++i) {
                auto &g = guides[i];
                const uint x = i % size.x, y = i / size.x;
                // Last pixel of each checkerboard tile: lane 31 / iteration 7
                // for a full tile, with different lanes in the partial tiles.
                const bool last_x = x % 16u == 15u || x + 1u == size.x;
                const bool last_y = y % 16u == 15u || y + 1u == size.y;
                const bool sparse = ((x / 16u + y / 16u) % 2u == 0u) && last_x && last_y;
                g.position = make_float4(float(x) * .1f, float(y) * .1f, 2.f, frame != 1u || sparse ? 2.f : 0.f);
                g.normal_roughness = make_float4(0.f, 0.f, -1.f, .4f);
                // Isolate receivers by tile: virtual specular reprojection must not
                // legitimately borrow a live neighbor's history after reappearance.
                const uint receiver = 3u + (y / 16u) * ((size.x + 15u) / 16u) + x / 16u;
                g.identity = make_uint4(1u, 2u, receiver, frame != 1u || sparse ? 1u : 0u);
            }
            p.frame = frame;
            const auto packed = pack_guides(guides.data(), n);
            stream << filter.guides().upload(packed.data()) << input_d.upload(signal_d.data()) << input_s.upload(signal_s.data())
                   << filter.dispatch(p) << output_d.download(actual[0].data()) << output_s.download(actual[1].data())
                   << filter.history().download(history_bytes.data()) << synchronize() << commit();
            unpack_history(history_bytes, history.data(), n);
            for (uint i = 0; i < n; ++i) {
                for (uint lobe = 0; lobe < 2u; ++lobe) {
                    const auto &color = actual[lobe][i];
                    const auto &raw = lobe == 0u ? signal_d[i] : signal_s[i];
                    const float count = lobe == 0u ? float(history[i].fast_count.z) : float(history[i].fast_count.w);
                    expect(std::isfinite(color.x) && std::isfinite(color.y) && std::isfinite(color.z) && std::isfinite(color.w),
                           "sparse and partial tiles must produce finite colors");
                    if (guides[i].identity.w != 0u)
                        expect(count >= 1.f,
                               "a lone live pixel must activate its tile and accumulate history");
                    if (frame == 1u && guides[i].identity.w == 0u) {
                        expect(color.x == raw.x && color.y == raw.y && color.z == raw.z && color.w == raw.w,
                               "invalid pixels must pass through their raw signal");
                        expect(count == 0.f,
                               "invalid pixels must clear temporal state even inside a partially active tile");
                    }
                    if (frame == 2u && (i % size.x / 16u + i / size.x / 16u) % 2u != 0u)
                        expect(count <= 1.01f,
                               "a reappearing empty tile must not inherit its earlier lighting history");
                }
            }
        }
    }
    std::cout << "PASS: sparse partial tiles, both lobes and all reconstruction modes\n";
}

void check_packed_motion(Device &device, Stream &stream) {
    reblur::Filter filter;
    const uint2 size = make_uint2(17u, 19u);
    const uint n = size.x * size.y, center = 9u * size.x + 8u;
    filter.prepare(device, size);
    filter.compile(device);
    auto input = device.create_buffer<float4>(n);
    auto output_d = device.create_buffer<float4>(n), output_s = device.create_buffer<float4>(n);
    std::vector<reblur::Guide> guides(n);
    for (uint i = 0; i < n; ++i) {
        auto &g = guides[i];
        g.position = make_float4(float(i % size.x) * .1f, float(i / size.x) * .1f, 2.f, 2.f);
        g.normal_roughness = make_float4(0.f, 0.f, -1.f, .4f);
        g.identity = make_uint4(1u);
        g.diffuse_factor.w = 2.f; g.specular_factor.w = -3.f;
    }
    // Zero hit distance makes virtual motion equal to surface motion.
    std::vector<float4> signal(n, make_float4(.5f, .25f, 1.f, 0.f));
    const auto packed = pack_guides(guides.data(), n);
    ocarina::HostByteBuffer history_bytes(n * sizeof(reblur::TemporalData)), guide_bytes(packed.size());
    std::vector<reblur::TemporalData> history(n);
    reblur::FilterParam p;
    p.raw_diffuse = p.raw_specular = input.descriptor();
    p.output_diffuse = output_d.descriptor(); p.output_specular = output_s.descriptor();
    p.unproject = .05f; p.history_valid = 1u;
    stream << filter.guides().upload(packed.data()) << input.upload(signal.data());
    for (uint frame = 0; frame < 2u; ++frame) { p.frame = frame; stream << filter.dispatch(p); }
    stream << filter.history().download(history_bytes.data()) << filter.guides().download(guide_bytes.data())
           << synchronize() << commit();
    unpack_history(history_bytes, history.data(), n);
    expect(std::abs(history[center].tracking.x - 6.5f) < 1e-4f &&
           std::abs(history[center].tracking.y - 12.5f) < 1e-4f,
           "temporal reprojection must preserve packed motion component order and sign");
    expect(std::equal(packed.data(), packed.data() + packed.size(), guide_bytes.data()),
           "filtering must preserve every guide field, including signed packed motion");
    std::cout << "PASS: signed packed motion and temporal reprojection\n";
}

void check_material(Pipeline &pipeline) {
    pipeline.activate_global_context();
    auto &sampler = pipeline.renderer().sampler();
    for (bool individual : {false, true}) {
        MaterialRegistry::instance().individual_ns() = individual;
        Kernel kernel = [&](BufferVar<float4> output) {
            Env::instance().clear_global_vars(); sampler->load_data();
            sampler->set_seed(dispatch_idx().xy(), Uint{17u}, Dimension::PathTracing);
            SampledWavelengths swl{3u};
            Interaction it{false};
            it.shading.x = make_float3(1.f, 0.f, 0.f); it.shading.y = make_float3(0.f, 1.f, 0.f);
            it.shading.z = make_float3(0.f, 0.f, 1.f); it.ng = it.shading.z;
            auto diffuse = make_shared<DiffuseLobe>(SampledSpectrum{make_float3(.7f, .2f, .1f)}, swl);
            auto reflection = make_shared<PureReflectionLobe>(make_shared<FresnelConstant>(swl),
                make_unique<MicrofacetReflection>(SampledSpectrum{make_float3(.1f, .4f, .8f)}, swl,
                    make_shared<GGXMicrofacet>(make_float2(.3f), true)), SurfaceData::Glossy);
            diffuse->set_shading_frame(it.shading); reflection->set_shading_frame(it.shading);
            MaterialEvaluator evaluator{it, swl};
            evaluator.link(LobeSet::create_mix(Float{.6f}, reflection, diffuse));
            Float3 wo = make_float3(0.f, 0.f, 1.f), wi = normalize(make_float3(.2f, .3f, 1.f));
            auto expected_d = diffuse->evaluate(wo, wi, MaterialEvalMode::All, BxDFFlag::All, TransportMode::Radiance).f.vec3() * .6f;
            auto expected_s = reflection->evaluate(wo, wi, MaterialEvalMode::All, BxDFFlag::All, TransportMode::Radiance).f.vec3() * .4f;
            auto total = evaluator.evaluate(wo, wi).f.vec3();
            auto fraction = evaluator.specular_fraction_rgb(wo, wi);
            Bool ok = all(abs(total * fraction - expected_s) < 1e-5f) && all(abs(total * (1.f - fraction) - expected_d) < 1e-5f);
            auto d = evaluator.sample_lobe(wo, sampler, false);
            auto s = evaluator.sample_lobe(wo, sampler, true);
            ok &= d.valid() && !BxDFFlag::is_glossy(d.eval.flags) && !BxDFFlag::is_specular(d.eval.flags);
            ok &= !s.valid() || BxDFFlag::is_glossy(s.eval.flags) || BxDFFlag::is_specular(s.eval.flags);
            MaterialEvaluator mirror{it, swl};
            auto mirror_lobe = make_unique<PureReflectionLobe>(make_shared<FresnelConstant>(swl),
                make_unique<MicrofacetReflection>(SampledSpectrum{swl, .7f}, swl,
                    make_shared<GGXMicrofacet>(make_float2(.001f), true)), SurfaceData::NearSpec);
            mirror_lobe->set_shading_frame(it.shading); mirror.link(std::move(mirror_lobe));
            auto mirror_fraction = mirror.specular_fraction_rgb(wo, wo);
            auto mirror_sample = mirror.sample_lobe(wo, sampler, true);
            ok &= all(abs(mirror_fraction - make_float3(1.f)) < 1e-5f) && mirror_sample.valid();
            SampledSpectrum mirror_d{swl.dimension()}, mirror_s{swl.dimension()};
            mirror.denoiser_factors(wo, mirror_d, mirror_s);
            ok &= mirror_d.max() == 0.f && mirror_s.max() > .1f;
            auto tilted_frame = it.shading;
            tilted_frame.update(normalize(make_float3(1.f, 0.f, .2f)));
            auto tilted_lobe = make_unique<PureReflectionLobe>(make_shared<FresnelDielectric>(SampledSpectrum{swl, 1.5f}, swl),
                make_unique<MicrofacetReflection>(SampledSpectrum{swl, 1.f}, swl,
                    make_shared<GGXMicrofacet>(make_float2(.1f), true)), SurfaceData::Glossy);
            tilted_lobe->set_shading_frame(tilted_frame);
            auto expected_factor = tilted_lobe->albedo(individual ? abs(dot(tilted_frame.normal(), wo)) : Float{1.f});
            MaterialEvaluator tilted{it, swl}; tilted.link(std::move(tilted_lobe));
            SampledSpectrum tilted_d{swl.dimension()}, tilted_s{swl.dimension()};
            tilted.denoiser_factors(wo, tilted_d, tilted_s);
            ok &= all(abs(tilted_s.vec3() - expected_factor.vec3()) < 1e-5f);
            output.write(dispatch_id(), make_float4(fraction, select(ok, 1.f, 0.f)));
        };
        auto shader = pipeline.device().compile(kernel, "reblur_material_contract");
        auto buffer = pipeline.device().create_buffer<float4>(128);
        std::array<float4, 128> result;
        pipeline.stream() << shader(buffer).dispatch(128u) << buffer.download(result.data()) << synchronize() << commit();
        for (auto value : result) expect(value.w == 1.f && std::abs(value.x - value.z) > .1f,
            "colored BSDF lobes must split per channel and guide sampling must select the requested class");
    }
    std::cout << "PASS: colored BSDF energy and independent lobe guide sampling in both normal modes\n";
}

void check_compound_material(Pipeline &pipeline) {
    pipeline.activate_global_context();
    Global::SceneGpuContextScope scope{pipeline.geometry().bindless_array(), pipeline.device()};
    auto &sampler = pipeline.renderer().sampler();
    Kernel kernel = [&](BufferVar<float4> output) {
        Env::instance().clear_global_vars(); sampler->load_data();
        sampler->set_seed(dispatch_idx().xy(), Uint{73u}, Dimension::PathTracing);
        SampledWavelengths swl{3u};
        Interaction it{false};
        it.shading.x = make_float3(1.f, 0.f, 0.f); it.shading.y = make_float3(0.f, 1.f, 0.f);
        it.shading.z = make_float3(0.f, 0.f, 1.f); it.ng = it.shading.z; it.wo = it.ng;
        pipeline.scene().materials().dispatch(Uint{0u}, [&](const Material *material) {
            auto evaluator = material->create_evaluator(it, swl);
            auto guide = evaluator.sample_lobe(it.wo, sampler, false);
            SampledSpectrum dn{swl.dimension()}, sn{swl.dimension()}, dg{swl.dimension()}, sg{swl.dimension()};
            evaluator.denoiser_factors(it.wo, dn, sn);
            evaluator.denoiser_factors(normalize(make_float3(1.f, 0.f, .1f)), dg, sg);
            output.write(dispatch_id(), make_float4(select(guide.valid(), 1.f, 0.f), sn.average(), sg.average(), dg.average() / dn.average()));
        });
    };
    auto shader = pipeline.device().compile(kernel, "reblur_compound_material_contract");
    auto buffer = pipeline.device().create_buffer<float4>(128);
    std::array<float4, 128> result;
    pipeline.stream() << shader(buffer).dispatch(128u) << buffer.download(result.data()) << synchronize() << commit();
    for (auto value : result) {
        expect(value.x == 1.f, "smooth compound materials must retain diffuse guide sampling at exact normal incidence");
        expect(value.z > value.y + .1f && value.w > 0.f && value.w < .8f,
            "compound demodulation factors must include directional Fresnel and diffuse attenuation");
    }
    std::cout << "PASS: smooth compound guide support and directional material factors\n";
}

void check_scene_signals() {
    namespace fs = std::filesystem;
    const auto fixture = fs::temp_directory_path() / ("corona_reblur_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(fixture);
    std::ofstream(fixture / "receiver.obj") << "v -4 -4 -1\nv 4 -4 -1\nv 0 4 -1\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    auto scene = DataWrap::parse(R"({
      "scene": {
        "camera":{"type":"thin_lens","param":{"fov_y":45,"lens_radius":0,"filter":{"type":"box"},"transform":{"type":"look_at","param":{"position":[0,0,0],"target_pos":[0,0,-1],"up":[0,1,0]}}}},
        "materials":[{"type":"substrate","name":"receiver","param":{"color":[0.7,0.2,0.1],"spec":[0.1,0.4,0.8],"roughness":0.3}}],
        "shapes":[{"type":"model","param":{"fn":"receiver.obj","material":"receiver","normalize_to_unit_bounds":false}}],
        "lights":[{"type":"spherical","param":{"color":{"channels":"xyz","node":{"type":"number","param":{"value":[1,1,1]}}},"scale":1}}]
      },
      "render":{"sampler":{"type":"independent","param":{"spp":1}},"light_sampler":{"type":"uniform"},
        "integrator":{"type":"pt","param":{"max_depth":3,"min_depth":3,"denoiser":{"type":"ReBLUR"},
          "direct":{"M_light":8,"M_bsdf":1,"temporal":{"open":false},"spatial":{"open":false}},
          "indirect":{"temporal":{"open":false},"spatial":{"open":false}}}}},
      "pipeline":{"type":"fixed","param":{"frame_buffer":{"type":"normal","param":{"resolution":[16,16]}}}},
      "output":{"spp":0,"save_exit":false,"denoise":false,"fn":"unused.exr"}
    })");
    std::vector<SP<Pipeline>> pipelines;
    const auto receiver_materials = scene["scene"]["materials"];
    const auto receiver_shapes = scene["scene"]["shapes"];
    std::ofstream(fixture / "back.obj") << "v -4 -4 -3\nv 4 -4 -3\nv 0 4 -3\nvn 0 0 1\nf 1//1 2//1 3//1\n";
    std::ofstream(fixture / "front.obj") << "v -4 -4 1\nv 0 4 1\nv 4 -4 1\nvn 0 0 -1\nf 1//1 2//1 3//1\n";
    std::ofstream(fixture / "rear.obj") << "v -4 -4 -1.2\nv 0 4 -1.2\nv 4 -4 -1.2\nvn 0 0 -1\nf 1//1 2//1 3//1\n";
    for (uint scenario = 0; scenario < 12u; ++scenario) {
        const bool pt = scenario == 0u || scenario >= 7u, stable = scenario >= 3u && !pt;
        scene["scene"]["materials"] = receiver_materials;
        scene["scene"]["shapes"] = receiver_shapes;
        if (scenario >= 10u) {
            auto &material = scene["scene"]["materials"][0];
            if (scenario == 11u) {
                material = DataWrap::parse(R"({"type":"plastic","name":"receiver","param":{"color":[0.7,0.2,0.1],"ior":1.5}})");
            }
            material["param"]["roughness"] = .0001f;
            material["param"]["remapping_roughness"] = false;
        }
        if (scenario == 5u || scenario == 6u || scenario == 9u) {
            scene["scene"]["materials"] = DataWrap::parse(R"([
                {"name":"glass","type":"glass","param":{"material_name":"BK7","roughness":0.001,"remapping_roughness":false}},
                {"name":"red","type":"diffuse","param":{"color":[0.8,0.1,0.1]}},
                {"name":"blue","type":"diffuse","param":{"color":[0.1,0.1,0.8]}}])");
            scene["scene"]["shapes"] = DataWrap::parse(R"([
                {"type":"model","param":{"fn":"receiver.obj","material":"glass","normalize_to_unit_bounds":false}},
                {"type":"model","param":{"fn":"back.obj","material":"red","normalize_to_unit_bounds":false}},
                {"type":"model","param":{"fn":"front.obj","material":"blue","normalize_to_unit_bounds":false}}])");
            if (scenario == 9u) scene["scene"]["shapes"].push_back(DataWrap::parse(
                R"({"type":"model","param":{"fn":"rear.obj","material":"glass","normalize_to_unit_bounds":false}})"));
        }
        auto &integrator = scene["render"]["integrator"];
        integrator["type"] = pt ? "pt" : "rt";
        integrator["param"]["direct"]["stable_planes"] = stable;
        const bool direct_on = scenario != 2u && scenario != 6u;
        integrator["param"]["max_depth"] = scenario == 7u ? 1u : 4u;
        integrator["param"]["mis_mode"] = scenario >= 8u ? 2u : 0u;
        integrator["param"]["direct"]["open"] = direct_on;
        integrator["param"]["indirect"]["open"] = scenario != 4u;
        { std::ofstream file(fixture / "scene.json"); file << scene.dump(2); }
        Global::instance().set_scene_path(fixture);
        auto pipeline = Importer::import_scene(fixture / "scene.json"); pipelines.push_back(pipeline);
        auto *illumination = dynamic_cast<IlluminationIntegrator *>(pipeline->renderer().integrator().get());
        expect(illumination && illumination->denoiser()->requires_lobe_signals(), "explicit REBLUR must load the real plugin");
        illumination->set_denoise_enabled(false);
        pipeline->frame_buffer()->set_enable_accumulation(false);
        pipeline->prepare(); pipeline->frame_buffer()->prepare_view_texture();
        pipeline->upload_data();
        if (pt) check_material(*pipeline);
        if (scenario >= 10u) check_compound_material(*pipeline);
        auto &fb = *pipeline->frame_buffer();
        const uint count = pipeline->pixel_num(), layers = stable ? 3u : 1u;
        std::vector<float4> pixels(count), d(count * 3u), s(d.size()), b(d.size()), emission(count);
        for (uint frame = 0; frame < 4u; ++frame) {
            pipeline->upload_data(); pipeline->display(1.0 / 60.0);
            pipeline->stream() << fb.rt_buffer().device_buffer().download(pixels.data())
                << fb.denoiser_diffuse().view().download(d.data()) << fb.denoiser_specular().view().download(s.data())
                << fb.denoiser_bypass().view().download(b.data());
            if (stable) pipeline->stream() << fb.stable_radiance().view().download(emission.data());
            pipeline->stream() << synchronize() << commit();
            if (scenario == 8u) {
                float diffuse_sum = 0.f, specular_sum = 0.f;
                for (uint y = 4u; y < 12u; ++y) for (uint x = 4u; x < 12u; ++x) {
                    diffuse_sum += d[y * 16u + x].x; specular_sum += s[y * 16u + x].z;
                }
                expect(diffuse_sum > .01f && specular_sum > .01f,
                    "BSDF-sampled environment from a mixed material must retain both primary lobe contributions");
            }
            if (scenario == 9u) {
                uint continued = 0u;
                for (uint y = 4u; y < 12u; ++y) for (uint x = 4u; x < 12u; ++x)
                    continued += s[y * 16u + x].w > 1.5f;
                expect(continued > 45u, "glass hit distance must continue through the rear interface to the distant target");
            }
            for (uint i = 0; i < count; ++i) for (uint c = 0; c < 3u; ++c) {
                float sum = stable && direct_on ? emission[i][c] : 0.f;
                for (uint layer = 0; layer < layers; ++layer) {
                    const uint j = layer * count + i;
                    sum += d[j][c] + s[j][c] + b[j][c];
                }
                expect(std::isfinite(sum) && std::abs(sum - pixels[i][c]) < 1e-4f * std::max(1.f, std::abs(pixels[i][c])),
                    "raw per-lobe energy plus bypass must equal the original estimator, including DI/GI switches and stable layers");
            }
        }
        illumination->set_denoise_enabled(true);
        for (uint frame = 0; frame < 8u; ++frame) {
            pipeline->upload_data(); pipeline->display(1.0 / 60.0);
        }
        pipeline->stream() << fb.rt_buffer().device_buffer().download(pixels.data()) << synchronize() << commit();
        for (auto pixel : pixels) expect(std::isfinite(pixel.x) && std::isfinite(pixel.y) && std::isfinite(pixel.z),
            "enabling the actual REBLUR plugin after raw frames must produce finite colors");
        std::cout << "PASS: producer energy, runtime enable, scenario=" << scenario << '\n';
    }
    fs::remove_all(fixture);
}

template<typename T>
void check_input_contract() {
    if constexpr (requires(const T &filter) { filter.requires_lobe_signals(); }) {
        expect(!std::is_abstract_v<T>, "REBLUR must be a concrete denoiser plugin");
    } else {
        expect(false, "REBLUR does not expose its required lobe input contract");
    }
}
}

int main() {
    try {
        check_input_contract<vision::reblur::ReBLUR>();
        Env::set_valid_check(false);
        DynamicModule::clear_search_path();
        RHIContext::instance().init(std::filesystem::current_path());
        static auto device = RHIContext::instance().create_device("cuda");
        device.init_rtx();
        Global::instance().set_device(&device);
        auto stream = device.create_stream();
        check_storage_budget(device);
        check_helpers(device, stream);
        check_storage_precision(device, stream);
        check_filter(device, stream);
        check_curved_glossy_footprint(device, stream);
        check_sparse_tiles(device, stream);
        check_packed_motion(device, stream);
        check_scene_signals();
        std::cout << "PASS: REBLUR input contract\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
