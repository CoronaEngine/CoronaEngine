#include "render_core/denoiser/ReLAX/utils.h"
#include "render_core/integrator/ReSTIR/denoiser_signals.h"
#include "rhi/context.h"
#include <array>
#include <bit>
#include <cmath>
#include <filesystem>
#include <iostream>
void check_relax_layers(ocarina::Device &, ocarina::Stream &);
#include <limits>
#include <stdexcept>
#include <string_view>

void check_relax_prepass(ocarina::Device &, ocarina::Stream &);
void check_relax_temporal(ocarina::Device &, ocarina::Stream &);
void check_relax_history(ocarina::Device &, ocarina::Stream &);
void check_relax_atrous(ocarina::Device &, ocarina::Stream &);

namespace {
using namespace ocarina;
void expect_near(float actual, float expected, const char *message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-5f * (1.f + std::abs(expected))) {
        std::cerr << message << ": got " << actual << ", expected " << expected << '\n';
        throw std::runtime_error(message);
    }
}

void check_guide_columns(Device &device, Stream &stream) {
    using namespace vision::relax;
    constexpr uint n = 2u;
    std::array<Guide, n> expected{};
    std::array<float4, n * GuideColumnCount> columns{}, copied{};
    for (uint i = 0u; i < n; ++i) {
        auto &g = expected[i];
        g.position = make_float4(float(i), -0.f, 1e-30f, 100000.f);
        g.depth_position = make_float4(3.f, -7.f, float(i), 2.f);
        g.normal_roughness = make_float4(.25f, -.5f, .75f, .125f);
        g.diffuse_factor = make_float4(1e-20f, 2.f, 3.f, float(i));
        g.specular_factor = make_float4(4.f, 5.f, -0.f, float(i));
        g.motion = make_float2(-.375f, .625f);
        g.identity = make_uint4(0xffffffffu, 0x7fc12345u, i, 1u);
        columns[i] = g.position; columns[n + i] = g.depth_position;
        columns[2u * n + i] = g.normal_roughness; columns[3u * n + i] = g.diffuse_factor;
        columns[4u * n + i] = g.specular_factor; columns[5u * n + i] = make_float4(g.motion, 0.f, 0.f);
        columns[6u * n + i] = std::bit_cast<float4>(g.identity);
    }
    auto input = device.create_buffer<float4>(columns.size());
    auto output = device.create_buffer<float4>(columns.size());
    auto typed = device.create_buffer<Guide>(n);
    Kernel transfer = [](Var<FilterParam> p, BufferVar<Guide> values, BufferVar<float4> copy) {
        auto g = load_guide(p, dispatch_id());
        values.write(dispatch_id(), g);
        store_guide(copy, p.size.x * p.size.y, dispatch_id(), g);
    };
    auto shader = device.compile(transfer, "RELAX-guide-columns-FP32-bits");
    FilterParam p; p.guide = input.descriptor(); p.size = make_uint2(n, 1u);
    std::array<Guide, n> actual{};
    stream << input.upload(columns.data()) << shader(p, typed, output).dispatch(n)
           << typed.download(actual.data()) << output.download(copied.data()) << synchronize() << commit();
    auto bits_equal = [](const auto &a, const auto &b) {
        return std::bit_cast<std::array<uint, sizeof(a) / sizeof(uint)>>(a) ==
               std::bit_cast<std::array<uint, sizeof(b) / sizeof(uint)>>(b);
    };
    for (uint i = 0; i < n; ++i) {
        const auto &a = actual[i], &b = expected[i];
        if (!bits_equal(a.position, b.position) || !bits_equal(a.depth_position, b.depth_position) ||
            !bits_equal(a.normal_roughness, b.normal_roughness) || !bits_equal(a.diffuse_factor, b.diffuse_factor) ||
            !bits_equal(a.specular_factor, b.specular_factor) || !bits_equal(a.motion, b.motion) || !bits_equal(a.identity, b.identity))
            throw std::runtime_error("dense guide columns must preserve all FP32/identity bits and column offsets");
    }
    for (uint i = 0; i < columns.size(); ++i) if (!bits_equal(copied[i], columns[i]))
        throw std::runtime_error("guide writes must preserve column storage, including identity NaN bit patterns");
}

void check_helpers(Device &device, Stream &stream) {
    check_guide_columns(device, stream);
    Kernel distances = [](BufferVar<float4> out) {
        const std::array<float, 5> scales{1.f, 1e-4f, 1e-6f, 1e-8f, 0.f};
        for (uint i = 0; i < scales.size(); ++i) {
            // Both lobe distances are actual receiver endpoints, including black samples.
            Float2 merged = vision::denoiser_signals::merge_hit_distances(make_float2(scales[i], 3.f * scales[i]), Float{10.f},
                make_float2(0.f, scales[i]), Float{30.f});
            Float4 accumulated = vision::denoiser_signals::accumulate_lobe(make_float4(scales[i], scales[i], scales[i], 10.f),
                make_float3(3.f * scales[i]), Float{30.f});
            out.write(i, make_float4(merged, accumulated.w, 0.f));
        }
        out.write(5u, make_float4(
            vision::denoiser_signals::merge_hit_distances(make_float2(1.f), Float{0.f}, make_float2(1e-8f), Float{30.f}),
            vision::denoiser_signals::merge_hit_distances(make_float2(0.f), Float{10.f}, make_float2(0.f), Float{0.f})));
    };
    auto distance_shader = device.compile(distances, "RELAX-ReSTIR-distance-validity");
    auto distance_output = device.create_buffer<float4>(6u);
    std::array<float4, 6> actual_distances{};
    stream << distance_shader(distance_output).dispatch(1u) << distance_output.download(actual_distances.data()) << synchronize() << commit();
    for (uint i = 0; i < actual_distances.size(); ++i) {
        std::cout << "distance scale " << i << ": " << actual_distances[i].x << ',' << actual_distances[i].y << ',' << actual_distances[i].z << '\n';
    }
    for (uint i = 0; i < 4u; ++i) {
        expect_near(actual_distances[i].x, 10.f, "dim/fractionally weighted single endpoint must retain distance");
        expect_near(actual_distances[i].y, 15.f, "distance mixing must be independent of the common brightness/factor scale");
        expect_near(actual_distances[i].z, 25.f, "layer accumulation must normalize any positive brightness sum");
    }
    expect_near(actual_distances[4].x, 30.f, "black producer samples must retain a valid endpoint");
    expect_near(actual_distances[4].y, 30.f, "black specular samples must retain a valid endpoint");
    expect_near(actual_distances[4].z, 30.f, "black layer accumulation must retain a valid endpoint");
    expect_near(actual_distances[5].x, 30.f, "an invalid/disabled endpoint must not dilute a valid endpoint");
    expect_near(actual_distances[5].z, 10.f, "a black valid endpoint must survive an invalid/disabled peer");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::array<float4, 8> signals{
        make_float4(1.f, 2.f, 3.f, 7.f), make_float4(0.f, 0.f, 0.f, 7.f),
        make_float4(-1.f, 2.f, 3.f, -7.f), make_float4(nan, 2.f, 3.f, 7.f),
        make_float4(1.f, 2.f, 3.f, inf), make_float4(70000.f),
        make_float4(inf, 2.f, 3.f, 7.f), make_float4(2.f, 6.f, 12.f, 7.f)};
    std::array<float4, 64> ramp{};
    for (uint y = 0; y < 8; ++y) for (uint x = 0; x < 8; ++x)
        ramp[y * 8 + x] = make_float4(float(x + y * 10));
    auto input = device.create_buffer<float4>(signals.size(), "RELAX helper input");
    auto pixels = device.create_buffer<float4>(ramp.size(), "RELAX sampling fixture");
    auto output = device.create_buffer<float4>(12, "RELAX helper output");
    Kernel kernel = [](BufferVar<float4> in, BufferVar<float4> image, BufferVar<float4> out) {
        for (uint i = 0; i < 8u; ++i)
            out.write(i, vision::relax::pack_signal(in.read(i), make_float3(1.f)));
        out.write(8u, vision::relax::pack_signal(in.read(7u), make_float3(.5f, 2.f, 3.f)));
        out.write(9u, make_float4(vision::relax::motion_to_uv(make_float2(2.f, -1.f), make_uint2(100u, 50u)), 0.f, 0.f));
        out.write(10u, vision::relax::sample_linear(image, make_float2(3.25f, 3.75f), make_uint2(8u)));
        out.write(11u, vision::relax::sample_bicubic(image, make_float2(3.25f, 3.75f), make_uint2(8u)));
    };
    auto shader = device.compile(kernel, "RELAX-ReferenceHelpers");
    std::array<float4, 12> actual{};
    stream << input.upload(signals.data()) << pixels.upload(ramp.data())
           << shader(input, pixels, output).dispatch(1u) << output.download(actual.data())
           << synchronize() << commit();
    const std::array<float4, 8> expected{
        make_float4(1.f, 2.f, 3.f, 7.f), make_float4(0.f, 0.f, 0.f, 7.f),
        make_float4(0.f, 2.f, 3.f, 0.f), make_float4(0.f, 0.f, 0.f, 7.f),
        make_float4(1.f, 2.f, 3.f, 0.f), make_float4(65504.f),
        make_float4(0.f, 0.f, 0.f, 7.f), make_float4(2.f, 6.f, 12.f, 7.f)};
    for (uint i = 0; i < 8; ++i) for (uint c = 0; c < 4; ++c)
        expect_near(actual[i][c], expected[i][c], "RELAX RGB/world-distance sanitization");
    expect_near(actual[8].x, 4.f, "diffuse demodulation red");
    expect_near(actual[8].y, 3.f, "diffuse demodulation green");
    expect_near(actual[8].z, 4.f, "diffuse demodulation blue");
    expect_near(actual[9].x, -.02f, "motion x sign and pixel units");
    expect_near(actual[9].y, .02f, "motion y sign and pixel units");
    expect_near(actual[10].x, 35.25f, "bilinear pixel centers on a linear ramp");
    // Independently sum the 12 Catmull-Rom taps: 34.979736328125 / .9912109375.
    // Omitting four corners gives the small bias specified by NRD's kernel.
    expect_near(actual[11].x, 35.2899014778f, "NRD 12-tap bicubic footprint");
    std::cout << "PASS: RELAX RGB, world hit distance, demodulation, motion and sampling\n";
}
}

int main(int argc, char **argv) {
    try {
        using namespace ocarina;
        Env::set_valid_check(false);
        DynamicModule::clear_search_path();
        RHIContext::instance().init(std::filesystem::current_path());
        auto device = RHIContext::instance().create_device("cuda");
        auto stream = device.create_stream();
        const std::string_view group = argc > 1 ? argv[1] : "helpers";
        if (group == "helpers") check_helpers(device, stream);
        else if (group == "prepass") check_relax_prepass(device, stream);
        else if (group == "temporal") check_relax_temporal(device, stream);
        else if (group == "history") check_relax_history(device, stream);
        else if (group == "atrous") check_relax_atrous(device, stream);
        else if (group == "layers") check_relax_layers(device, stream);
        else throw std::runtime_error("Unknown RELAX test group");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
