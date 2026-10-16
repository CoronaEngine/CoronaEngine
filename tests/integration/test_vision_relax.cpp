#include "render_core/denoiser/ReLAX/utils.h"
#include "rhi/context.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
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

void check_helpers(Device &device, Stream &stream) {
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
        else throw std::runtime_error("Unknown RELAX test group");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
