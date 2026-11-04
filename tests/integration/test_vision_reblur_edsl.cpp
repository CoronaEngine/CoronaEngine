#include "render_core/denoiser/ReBLUR/edsl_nrd_resources.h"
#include "render_core/denoiser/ReBLUR/edsl_nrd.h"
#include "test_reblur_matrix_probe.h"
#include "test_reblur_parameter_probe.h"
#include "rhi/context.h"
#include <algorithm>
#include <cmath>
#include <bit>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace vision::reblur::edsl;
void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void check_resources(Device &device, Stream &stream) {
    constexpr uint n = 8u;
    std::vector<Texture2D> storage;
    Dispatch parameters;
    const PixelStorage formats[]{PixelStorage::RGB10A2, PixelStorage::HALF4,
                                 PixelStorage::UNORM1, PixelStorage::UNORM2,
                                 PixelStorage::USHORT1, PixelStorage::HALF1,
                                 PixelStorage::UINT1, PixelStorage::FLOAT1};
    for (uint slot = 0; slot != 8u; ++slot) {
        storage.push_back(device.create_texture2d(make_uint2(n, 1u), formats[slot]));
        parameters.inputs[slot] = {storage.back(), make_uint2(n, 1u)};
    }
    auto output = device.create_buffer<float4>(n * 9u);
    parameters.external.output_diffuse = output.descriptor();
    Kernel probe = [](Var<Dispatch> p) {
        configure_block(8u);
        Uint lane = thread_idx().x;
        Int2 q = make_int2(lane, 0u);
        vision::reblur::edsl::Image<float4, Format::RGB10A2Unorm> normal(p.inputs.t0);
        vision::reblur::edsl::Image<float4, Format::RGBA16Float> signal(p.inputs.t1);
        vision::reblur::edsl::Image<float, Format::R8Unorm> r8(p.inputs.t2);
        vision::reblur::edsl::Image<float2, Format::RG8Unorm> rg8(p.inputs.t3);
        vision::reblur::edsl::Image<uint, Format::R16Uint> r16u(p.inputs.t4);
        vision::reblur::edsl::Image<float, Format::R16Float> r16f(p.inputs.t5);
        vision::reblur::edsl::Image<uint, Format::R32Uint> r32u(p.inputs.t6);
        vision::reblur::edsl::Image<float, Format::R32Float> r32f(p.inputs.t7);
        Float f = lane.cast<float>() / 7.f;
        normal.write(q, make_float4(f, .2f, .4f, 0.f));
        signal.write(q, make_float4(f, -.25f, 2.f, .5f));
        r8.write(q, f); rg8.write(q, make_float2(f, .25f));
        r16u.write(q, lane * 1000u); r16f.write(q, f);
        r32u.write(q, 0xff000000u + lane); r32f.write(q, f);
        SharedArray<float, 8> scalar;
        SharedArray<float2, 8> pair;
        scalar.write(lane, lane.cast<float>());
        pair.write(lane, make_float2(lane.cast<float>(), f));
        synchronize_block();
        auto &out = p.external.output_diffuse;
        out.write(lane, normal.read(q));
        out.write(8u + lane, signal.read(q));
        out.write(16u + lane, make_float4(r8.read(q), rg8.read(q), 0.f));
        out.write(24u + lane, make_float4(r16u.read(q).cast<float>(), r16f.read(q), 0.f, 0.f));
        out.write(32u + lane, as<float4>(make_uint4(r32u.read(q), 0u, 0u, 0u)));
        out.write(40u + lane, make_float4(r32f.read(q), scalar.read(7u-lane), pair.read(7u-lane)));
        out.write(48u + lane, make_float4(signal.read(make_int2(-1, 0)).x, r8.nearest(make_int2(-1, 0)), 0.f, 0.f));
        out.write(56u + lane, r32f.gather(make_float2(.5f, .5f)));
        out.write(64u + lane, normal.sample(make_float2(.5f, .5f)));
    };
    auto shader = device.compile(probe, "REBLUR-EDSL-resource-contract");
    std::vector<float4> actual(n * 9u);
    stream << shader(parameters).dispatch(n) << output.download(actual.data()) << synchronize() << commit();
    for (uint i = 0; i != n; ++i) {
        const float f = float(i) / 7.f;
        expect(std::abs(actual[i].x - f) <= 1.f/1023.f, "RGB10 input quantization");
        expect(std::abs(actual[8u+i].x - f) < .001f && actual[8u+i].y == -.25f, "FP16 signed signal storage");
        expect(actual[16u+i].x >= 0.f && actual[16u+i].x <= 1.f, "R8 unorm storage range");
        expect(actual[24u+i].x == float(i*1000u), "R16 uint storage");
        expect(std::abs(actual[24u+i].y - f) < .001f, "R16 float storage");
        expect(std::bit_cast<uint>(actual[32u+i].x) == 0xff000000u+i, "R32 uint full-width storage");
        expect(actual[40u+i].y == float(7u-i) && actual[40u+i].z == float(7u-i), "scalar/vector shared memory");
        expect(actual[48u+i].x == 0.f && actual[48u+i].y == 0.f, "zero load versus clamp sampling");
        expect(actual[56u+i].x == actual[43].x && actual[56u+i].y == actual[44].x &&
               actual[56u+i].z == actual[44].x && actual[56u+i].w == actual[43].x, "HLSL gather order");
        expect(std::abs(actual[64u+i].x - .5f) < .001f, "linear clamp sampling");
    }
    // Exercise the public Texture2D API too: NRD's private Image helpers do
    // not pass through the generic storage-format addressing or bounds paths.
    auto generic_output = device.create_buffer<uint4>(n);
    Kernel generic_write = [](Texture2DVar texture) {
        Uint i = dispatch_id();
        texture.write(Uint(65535u - i * 1234u), i, 0u);
        $if(i == 0u) {
            texture.write(Uint(123u), 8u, 0u);
            texture.write(Uint(456u), 0u, 1u);
        };
    };
    Kernel generic_read = [](Texture2DVar texture, BufferVar<uint4> out) {
        Uint i = dispatch_id();
        Uint bounds = call<bool>("oc_tex2d_in_bounds", texture, i, 0u).cast<uint>();
        bounds |= call<bool>("oc_tex2d_in_bounds", texture, 8u, 0u).cast<uint>() << 1u;
        bounds |= call<bool>("oc_tex2d_in_bounds", texture, 0u, 1u).cast<uint>() << 2u;
        out.write(i, make_uint4(texture.read<uint>(i, 0u), texture.read<uint>(8u, 0u),
                               texture.read<uint>(0u, 1u), bounds));
    };
    auto write_generic = device.compile(generic_write, "REBLUR-EDSL-USHORT1-generic-write-contract");
    auto read_generic = device.compile(generic_read, "REBLUR-EDSL-USHORT1-generic-read-contract");
    std::array<uint4, n> generic_actual{};
    std::array<uint16_t, n> generic_pixels{};
    stream << write_generic(storage[4]).dispatch(n) << read_generic(storage[4], generic_output).dispatch(n)
           << generic_output.download(generic_actual.data()) << storage[4].download(generic_pixels.data())
           << synchronize() << commit();
    for (uint i = 0u; i != n; ++i) {
        const uint expected = 65535u - i * 1234u;
        expect(generic_actual[i].x == expected && generic_pixels[i] == expected,
               "USHORT1 generic uint access must address one complete texel per x coordinate");
        expect(generic_actual[i].y == 0u && generic_actual[i].z == 0u && generic_actual[i].w == 1u,
               "USHORT1 generic uint bounds must reject x/y outside the image");
    }
    // Zero-border selection must discard even a NaN texel without 0 * NaN.
    const std::array<int2, 8> zero_border_positions{
        make_int2(0, 0), make_int2(-1, 0), make_int2(1, 0), make_int2(0, -1),
        make_int2(0, 1), make_int2(-17, 23),
        make_int2(std::numeric_limits<int>::min(), std::numeric_limits<int>::max()),
        make_int2(std::numeric_limits<int>::max(), std::numeric_limits<int>::min())};
    const float zero_border_nan = std::bit_cast<float>(0x7fc12345u);
    const uint zero_border_uint = 0xfedcba98u;
    auto zero_border_float_image = device.create_texture2d(make_uint2(1u), PixelStorage::FLOAT1);
    auto zero_border_uint_image = device.create_texture2d(make_uint2(1u), PixelStorage::UINT1);
    auto zero_border_coordinates = device.create_buffer<int2>(zero_border_positions.size());
    auto zero_border_output = device.create_buffer<uint4>(zero_border_positions.size());
    Dispatch zero_border_parameters{};
    zero_border_parameters.inputs[0] = {zero_border_float_image, make_uint2(1u)};
    zero_border_parameters.inputs[1] = {zero_border_uint_image, make_uint2(1u)};
    Kernel zero_border_probe = [](Var<Dispatch> p, BufferVar<int2> coordinates, BufferVar<uint4> out) {
        configure_block(8u);
        Uint i = dispatch_id();
        Int2 q = coordinates.read(i);
        vision::reblur::edsl::Image<float, Format::R32Float> floats(p.inputs.t0);
        vision::reblur::edsl::Image<uint, Format::R32Uint> integers(p.inputs.t1);
        out.write(i, make_uint4(as<uint>(floats.read(q)), integers.read(q),
                               as<uint>(floats.nearest(q)), integers.nearest(q)));
    };
    auto zero_border_shader = device.compile(zero_border_probe, "REBLUR-EDSL-NaN-zero-border-contract");
    std::array<uint4, 8> zero_border_actual{};
    stream << zero_border_float_image.upload(&zero_border_nan) << zero_border_uint_image.upload(&zero_border_uint)
           << zero_border_coordinates.upload(zero_border_positions.data())
           << zero_border_shader(zero_border_parameters, zero_border_coordinates, zero_border_output).dispatch(8u)
           << zero_border_output.download(zero_border_actual.data()) << synchronize() << commit();
    for (uint i = 0u; i != zero_border_positions.size(); ++i) {
        const bool inside = zero_border_positions[i].x == 0 && zero_border_positions[i].y == 0;
        if (inside) expect(std::isnan(std::bit_cast<float>(zero_border_actual[i].x)), "in-bounds R32Float NaN must survive");
        else expect(zero_border_actual[i].x == 0u, "OOB R32Float must select positive zero even when edge texel is NaN");
        expect(zero_border_actual[i].y == (inside ? zero_border_uint : 0u), "checked R32Uint must preserve all bits and zero border");
        expect(std::isnan(std::bit_cast<float>(zero_border_actual[i].z)) && zero_border_actual[i].w == zero_border_uint,
               "nearest must retain clamped NaN and full-width uint values");
    }
    std::cout << "PASS: NRD EDSL formats, sampling, scalar shared storage and generic USHORT1 access\n";
}
void check_uint_gather(Device &device, Stream &stream) {
    // Use dyadic coordinates away from footprint boundaries so the expected
    // HLSL gather texels are exact, including every edge and corner clamp.
    constexpr uint count = 64u;
    const uint2 size = make_uint2(4u);
    std::array<uint16_t, 16> values16{};
    std::array<uint, 16> values32{};
    std::array<float, 16> values_float{};
    for (uint y = 0u; y != 4u; ++y) for (uint x = 0u; x != 4u; ++x) {
        const uint index = y * 4u + x;
        values16[index] = static_cast<uint16_t>(0x8000u + y * 1024u + x * 17u);
        values32[index] = 0xff000000u + y * 0x00100001u + x * 0x00001003u;
        values_float[index] = .125f + float(index) * .25f;
    }
    auto image16 = device.create_texture2d(size, PixelStorage::USHORT1);
    auto image32 = device.create_texture2d(size, PixelStorage::UINT1);
    auto image_float = device.create_texture2d(size, PixelStorage::FLOAT1);
    auto output = device.create_buffer<uint4>(count * 6u);
    Dispatch p{};
    p.inputs[0] = {image16, size}; p.inputs[1] = {image32, size}; p.inputs[2] = {image_float, size};
    Kernel probe = [](Var<Dispatch> p, BufferVar<uint4> out) {
        configure_block(64u);
        Uint i = dispatch_id();
        Int2 q = make_int2((i % 8u).cast<int>() - 2, (i / 8u).cast<int>() - 2);
        Float2 uv = (make_float2(q) + .75f) / 4.f;
        vision::reblur::edsl::Image<uint, Format::R16Uint> input16{p.inputs.t0};
        vision::reblur::edsl::Image<uint, Format::R32Uint> input32{p.inputs.t1};
        vision::reblur::edsl::Image<float, Format::R32Float> input_float{p.inputs.t2};
        out.write(i, make_uint4(input16.read(q), input32.read(q), 0u, 0u));
        out.write(64u + i, call<uint4>("oc_nrd_tex2d_gather_uint", p.inputs.t0.texture, uv));
        out.write(128u + i, call<uint4>("oc_nrd_tex2d_gather_uint", p.inputs.t1.texture, uv));
        out.write(192u + i, make_uint4(input16.gather(uv)));
        // Statically use float and uint sampled arrays in this same shader.
        out.write(256u + i, make_uint4(as<uint>(input_float.read(clamp(q, make_int2(0), make_int2(3)))), 0u, 0u, 0u));
        out.write(320u + i, as<uint4>(input32.gather(uv)));
    };
    auto shader = device.compile(probe, "REBLUR-EDSL-uint-SRV-gather-contract");
    std::vector<uint4> actual(count * 6u);
    stream << image16.upload(values16.data()) << image32.upload(values32.data())
           << image_float.upload(values_float.data()) << shader(p, output).dispatch(count)
           << output.download(actual.data()) << synchronize() << commit();
    const int2 offsets[]{make_int2(0, 1), make_int2(1, 1), make_int2(1, 0), make_int2(0, 0)};
    for (uint i = 0u; i != count; ++i) {
        const int x = int(i % 8u) - 2, y = int(i / 8u) - 2;
        const bool inside = x >= 0 && x < 4 && y >= 0 && y < 4;
        const uint index = inside ? uint(y * 4 + x) : 0u;
        expect(actual[i].x == (inside ? values16[index] : 0u) &&
                   actual[i].y == (inside ? values32[index] : 0u), "uint SRV Load must preserve full integer bits and zero border");
        const uint clamped = uint(std::clamp(y, 0, 3) * 4 + std::clamp(x, 0, 3));
        expect(actual[256u + i].x == std::bit_cast<uint>(values_float[clamped]),
               "float and uint sampled descriptor aliases must coexist");
        for (uint component = 0u; component != 4u; ++component) {
            const int sx = std::clamp(x + offsets[component].x, 0, 3);
            const int sy = std::clamp(y + offsets[component].y, 0, 3);
            const uint source = uint(sy * 4 + sx);
            expect(actual[64u + i][component] == values16[source] &&
                       actual[128u + i][component] == values32[source], "native uint GatherRed order and clamp");
            expect(actual[192u + i][component] == values16[source], "R16_UINT Image gather must use the native SRV path");
            expect(actual[320u + i][component] == std::bit_cast<uint>(float(values32[source])),
                   "R32_UINT Image gather retains its float-result API conversion");
        }
    }
    std::cout << "PASS: uint SRV Load/GatherRed, full-width values, clamp and float/uint descriptor aliases\n";
}
void check_queued_resize(Device &device, Stream &stream) {
    using namespace vision::reblur;
    struct Replay {
        uint2 size{};
        bool in_place{};
        ByteBuffer guide;
        Buffer<float4> input_d, input_s, output_d, output_s;
        HostByteBuffer packed;
        std::vector<float4> diffuse, specular, actual_d, actual_s;
    };
    EdslReblur implementation(device);
    std::array<Replay, 2> replays;
    const uint2 sizes[]{make_uint2(3u, 2u), make_uint2(5u, 3u)};
    for (uint index = 0u; index != replays.size(); ++index) {
        auto &replay = replays[index];
        replay.size = sizes[index]; replay.in_place = index != 0u;
        const uint count = replay.size.x * replay.size.y;
        replay.guide = device.create_byte_buffer(size_t(count) * sizeof(Guide));
        replay.input_d = device.create_buffer<float4>(count);
        replay.input_s = device.create_buffer<float4>(count);
        if (!replay.in_place) {
            replay.output_d = device.create_buffer<float4>(count);
            replay.output_s = device.create_buffer<float4>(count);
        }
        std::vector<Guide> guides(count); // Invalid pixels must preserve every raw component.
        replay.diffuse.resize(count); replay.specular.resize(count);
        replay.actual_d.resize(count); replay.actual_s.resize(count);
        for (uint pixel = 0u; pixel != count; ++pixel) {
            const float value = float(pixel + index * 16u);
            replay.diffuse[pixel] = make_float4(value + .125f, .25f, .5f, -1.25f - value);
            replay.specular[pixel] = make_float4(.75f + value, .875f, 1.25f, -3.5f - value);
        }
        DynamicBufferLayoutCodec<Guide>::encode(guides.data(), count, replay.packed, {}, DynamicBufferLayout::SOA);
        NativeCameraParams camera;
        camera.world_to_view = camera.world_to_view_prev =
            {1.f,0.f,0.f,0.f, 0.f,1.f,0.f,0.f, 0.f,0.f,1.f,0.f, 0.f,0.f,0.f,1.f};
        const float aspect = float(replay.size.x) / float(replay.size.y), depth = 1000.f / 999.9f;
        camera.view_to_clip = camera.view_to_clip_prev =
            {1.f/aspect,0.f,0.f,0.f, 0.f,1.f,0.f,0.f, 0.f,0.f,depth,1.f, 0.f,0.f,-.1f*depth,0.f};
        FilterParam parameters{};
        parameters.size = replay.size; parameters.guide = replay.guide.descriptor();
        parameters.raw_diffuse = replay.input_d.descriptor(); parameters.raw_specular = replay.input_s.descriptor();
        parameters.output_diffuse = (replay.in_place ? replay.input_d : replay.output_d).descriptor();
        parameters.output_specular = (replay.in_place ? replay.input_s : replay.output_s).descriptor();
        stream << replay.guide.upload(replay.packed.data()) << replay.input_d.upload(replay.diffuse.data())
               << replay.input_s.upload(replay.specular.data())
               << implementation.dispatch(parameters, camera, 0u, Settings{}, true);
        // Deliberately construct the next resolution before either batch is
        // committed. Both internal resource generations must remain alive.
    }
    for (auto &replay : replays) {
        stream << (replay.in_place ? replay.input_d : replay.output_d).download(replay.actual_d.data())
               << (replay.in_place ? replay.input_s : replay.output_s).download(replay.actual_s.data());
    }
    stream << synchronize() << commit();
    for (const auto &replay : replays) for (uint pixel = 0u; pixel != replay.diffuse.size(); ++pixel) {
        for (uint component = 0u; component != 4u; ++component) {
            expect(std::isfinite(replay.actual_d[pixel][component]) && std::isfinite(replay.actual_s[pixel][component]),
                   "queued resize output finite");
            expect(replay.actual_d[pixel][component] == replay.diffuse[pixel][component] &&
                       replay.actual_s[pixel][component] == replay.specular[pixel][component],
                   "queued resize invalid-pixel passthrough, separate and in-place outputs");
        }
    }
    std::cout << "PASS: queued EDSL resize retains both resource generations and preserves separate/in-place invalid outputs\n";
}
}

int main() {
    try {
        using namespace ocarina;
        Env::set_valid_check(false);
        DynamicModule::clear_search_path();
        RHIContext::instance().init(std::filesystem::current_path());
        auto device = RHIContext::instance().create_device();
        auto stream = device.create_stream();
        check_resources(device, stream);
        check_uint_gather(device, stream);
        vision::reblur::edsl::check_reblur_matrix_contract(device, stream);
        vision::reblur::edsl::check_reblur_parameter_contract(device, stream);
        check_queued_resize(device, stream);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
