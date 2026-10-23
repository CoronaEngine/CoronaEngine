#include "render_core/denoiser/ReBLUR/edsl_nrd_resources.h"
#include "render_core/denoiser/ReBLUR/edsl_nrd.h"
#include "test_reblur_matrix_probe.h"
#include "test_reblur_parameter_probe.h"
#include "test_reblur_format_probe.h"
#include "rhi/context.h"
#include <algorithm>
#include <cmath>
#include <bit>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdlib>
#include <fstream>

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
void check_sampling(Device &device, Stream &stream, const std::filesystem::path &probe_spirv) {
    using namespace vision::reblur;
    constexpr uint count = 256u;
    NativeReblur reference(device.context()->runtime_directory() / "reblur-native");
    auto native = device.create_buffer<float4>(count), software = device.create_buffer<float4>(count);
    auto storage = device.create_texture2d(make_uint2(4u), PixelStorage::HALF4);
    std::vector<half4> pixels(16u);
    for(uint y=0u;y!=4u;++y) for(uint x=0u;x!=4u;++x) pixels[y*4u+x]=make_half4(float(x),float(y),float(x*y),1.f);
    Dispatch p{}; p.inputs[0u]={storage,make_uint2(4u)}; p.external.output_diffuse=software.descriptor();
    Kernel sample=[](Var<Dispatch> p) {
        configure_block(64u);
        Uint i=dispatch_id();
        Float2 location=make_float2((i%16u).cast<float>()*.137f+.12345f,(i/16u).cast<float>()*.141f+.23456f);
        vision::reblur::edsl::Image<float4,Format::RGBA16Float> input(p.inputs.t0);
        p.external.output_diffuse.write(i,input.sample((location+.5f)/4.f));
    };
    auto shader=device.compile(sample,"REBLUR-EDSL-sampler-contract");
    std::vector<float4> a(count),b(count);
    stream << storage.upload(pixels.data()) << reference.sampling_probe(native.descriptor(),probe_spirv)
           << shader(p).dispatch(count) << native.download(a.data()) << software.download(b.data()) << synchronize() << commit();
    float max_error=0.f;
    for(uint i=0u;i!=count;++i) {
        for(uint c=0u;c!=4u;++c) max_error=std::max(max_error,std::abs(a[i][c]-b[i][c]));
        if(i<16u) std::cout << "sampler i=" << i << " native=" << a[i].x << ',' << a[i].y << ',' << a[i].z
                           << " edsl=" << b[i].x << ',' << b[i].y << ',' << b[i].z << '\n';
    }
    std::cout << "sampler max_abs=" << max_error << '\n';
    expect(max_error<1e-6f,"native linear-clamp sampler equivalence");
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
void check_native_equivalence(Device &device, Stream &stream) {
    using namespace vision::reblur;
    EdslReblur implementation(device);
    NativeReblur reference(device.context()->runtime_directory() / "reblur-native");
    static_assert(sizeof(float4) == 4u * sizeof(float));
    std::ofstream comparison_csv;
    if (const char *path = std::getenv("REBLUR_EDSL_COMPARISON_CSV"); path && *path) {
        const std::filesystem::path destination(path);
        if (!destination.parent_path().empty()) std::filesystem::create_directories(destination.parent_path());
        comparison_csv.open(destination);
        expect(bool(comparison_csv), "cannot open EDSL comparison CSV");
        comparison_csv << "width,height,frame,layer,components,rmse,max_abs,max_relative,failed_components,first_pixel,camera_yaw,camera_tx,camera_ty,camera_tz,anti_firefly\n"
                       << std::setprecision(std::numeric_limits<double>::max_digits10);
    }
    std::filesystem::path output_directory;
    if (const char *path = std::getenv("REBLUR_EDSL_OUTPUT_DIR"); path && *path) {
        output_directory = path;
        std::filesystem::create_directories(output_directory);
    }
    auto save_bytes = [&](const std::string &name, const void *data, size_t bytes) {
        if (output_directory.empty()) return;
        std::ofstream file(output_directory / name, std::ios::binary);
        file.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
        expect(bool(file), "cannot save EDSL comparison binary");
    };
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    uint failures = 0;
    for (uint anti_firefly : {1u, 0u}) for (uint2 size : {make_uint2(1u), make_uint2(31u, 19u), make_uint2(127u, 73u)}) {
        uint n = size.x * size.y;
        auto guide = device.create_byte_buffer(size_t(n) * sizeof(Guide));
        auto input_d = device.create_buffer<float4>(n), input_s = device.create_buffer<float4>(n);
        auto result_d = device.create_buffer<float4>(n), result_s = device.create_buffer<float4>(n);
        auto native_d = device.create_buffer<float4>(n), native_s = device.create_buffer<float4>(n);
        std::vector<Guide> guides(n);
        std::vector<float4> diffuse(n), specular(n), actual_d(n), actual_s(n), expected_d(n), expected_s(n);
        const float aspect = float(size.x) / float(size.y), z = 10.f;
        const bool rotated_camera = size.x > 1u || size.y > 1u;
        const float yaw = rotated_camera ? .37f : 0.f;
        const float cosine = std::cos(yaw), sine = std::sin(yaw);
        const float tx = rotated_camera ? .3f : 0.f, ty = rotated_camera ? -.2f : 0.f, tz = rotated_camera ? .4f : 0.f;
        NativeCameraParams camera;
        // Column-major world-to-view: view = R_y(yaw) * world + t.
        // Apply R_y^T to (view-t) below so every guide remains on viewZ=10.
        camera.world_to_view = camera.world_to_view_prev =
            {cosine,0.f,rotated_camera ? -sine : 0.f,0.f, 0.f,1.f,0.f,0.f,
             sine,0.f,cosine,0.f, tx,ty,tz,1.f};
        const float depth = 1000.f / 999.9f;
        camera.view_to_clip = camera.view_to_clip_prev =
            {1.f/aspect,0.f,0.f,0.f,0.f,1.f,0.f,0.f,0.f,0.f,depth,1.f,0.f,0.f,-.1f*depth,0.f};
        camera.frame_delta_ms = 1000.f / 60.f;
        std::cout << "EDSL oracle camera " << size.x << 'x' << size.y << " yaw=" << yaw
                  << " view_translation=" << tx << ',' << ty << ',' << tz << '\n';
        Settings settings;
        settings.anti_firefly = anti_firefly;
        FilterParam p; p.size = size; p.guide = guide.descriptor();
        p.raw_diffuse = input_d.descriptor(); p.raw_specular = input_s.descriptor();
        for (uint frame = 0u; frame != 8u; ++frame) for (uint layer = 0u; layer != 3u; ++layer) {
            for (uint y = 0; y != size.y; ++y) for (uint x = 0; x != size.x; ++x) {
                const uint i = y * size.x + x;
                auto &g = guides[i]; g = Guide{};
                const float vx = (2.f*(float(x)+.5f)/float(size.x)-1.f)*aspect*z;
                const float vy = (1.f-2.f*(float(y)+.5f)/float(size.y))*z;
                g.position = make_float4(cosine*(vx-tx)-sine*(z-tz), vy-ty,
                                        sine*(vx-tx)+cosine*(z-tz), z);
                g.identity = make_uint4(1u);
                g.normal_roughness = make_float4(sine,0.f,-cosine, .15f + .7f*float(x)/float(size.x));
                const float k = float(layer + 1u), pattern = float((x*13u+y*7u+frame)%17u)/17.f;
                diffuse[i] = make_float4((.2f+pattern)*k,.6f*k,1.f*k, (i%11u==0u)?0.f:2.f);
                specular[i] = make_float4(1.f*k,(.1f+pattern)*k,.1f*k, (i%13u==0u)?0.f:3.f);
                if (frame == 5u && (i%7u==0u)) g.identity.w = 0u;
                if (frame >= 3u) { g.diffuse_factor.w = .375f; g.specular_factor.w = -.125f; }
            }
            HostByteBuffer packed;
            DynamicBufferLayoutCodec<Guide>::encode(guides.data(), n, packed, {}, DynamicBufferLayout::SOA);
            const std::string prefix = "af" + std::to_string(anti_firefly) + "_s" + std::to_string(size.x) + "x" + std::to_string(size.y) +
                                       "_f" + std::to_string(frame) + "_l" + std::to_string(layer);
            if (frame == 0u) {
                save_bytes(prefix + "_input_GuideSOA.bin", packed.data(), packed.size());
                save_bytes(prefix + "_input_D_RGBA32F.bin", diffuse.data(), diffuse.size() * sizeof(float4));
                save_bytes(prefix + "_input_S_RGBA32F.bin", specular.data(), specular.size() * sizeof(float4));
            }
            p.frame = frame; p.history_valid = frame != 0u;
            p.output_diffuse = native_d.descriptor(); p.output_specular = native_s.descriptor();
            stream << guide.upload(packed.data()) << input_d.upload(diffuse.data()) << input_s.upload(specular.data())
                   << reference.dispatch(p,camera,layer,settings,frame==0u)
                   << native_d.download(expected_d.data()) << native_s.download(expected_s.data()) << synchronize() << commit();
            p.output_diffuse = result_d.descriptor(); p.output_specular = result_s.descriptor();
            stream << implementation.dispatch(p,camera,layer,settings,frame==0u)
                   << result_d.download(actual_d.data()) << result_s.download(actual_s.data()) << synchronize() << commit();
            save_bytes(prefix + "_native_D_RGBA32F.bin", expected_d.data(), expected_d.size() * sizeof(float4));
            save_bytes(prefix + "_native_S_RGBA32F.bin", expected_s.data(), expected_s.size() * sizeof(float4));
            save_bytes(prefix + "_edsl_D_RGBA32F.bin", actual_d.data(), actual_d.size() * sizeof(float4));
            save_bytes(prefix + "_edsl_S_RGBA32F.bin", actual_s.data(), actual_s.size() * sizeof(float4));
            double squared_error = 0.0, max_error = 0.0, max_relative = 0.0;
            uint mismatch=0u, first=0u;
            for (uint i=0u; i!=n; ++i) for(uint c=0u;c!=4u;++c) {
                const float a[]{actual_d[i][c],actual_s[i][c]}, b[]{expected_d[i][c],expected_s[i][c]};
                for (uint lobe=0u;lobe!=2u;++lobe) {
                    const float error=std::abs(a[lobe]-b[lobe]);
                    const double difference = double(a[lobe]) - double(b[lobe]);
                    if (std::isfinite(difference)) {
                        const double absolute = std::abs(difference);
                        squared_error += difference * difference;
                        max_error = std::max(max_error, absolute);
                        max_relative = std::max(max_relative, absolute / std::max(std::abs(double(b[lobe])), 1e-6));
                    } else {
                        squared_error = max_error = max_relative = std::numeric_limits<double>::infinity();
                    }
                    if(!std::isfinite(a[lobe]) || error> .001f+.002f*std::abs(b[lobe])) { if(mismatch==0u) first=i; ++mismatch; }
                }
            }
            const double rmse = std::sqrt(squared_error / (double(n) * 8.0));
            std::cout << "EDSL oracle " << size.x << 'x' << size.y << " frame=" << frame << " layer=" << layer
                      << " anti_firefly=" << anti_firefly
                      << " rmse=" << rmse << " max_abs=" << max_error << " max_relative=" << max_relative
                      << " failed_components=" << mismatch << " first_pixel=" << first << '\n';
            if (comparison_csv.is_open()) {
                comparison_csv << size.x << ',' << size.y << ',' << frame << ',' << layer << ',' << size_t(n) * 8u
                               << ',' << rmse << ',' << max_error << ',' << max_relative << ',' << mismatch << ',' << first
                               << ',' << yaw << ',' << tx << ',' << ty << ',' << tz << ',' << anti_firefly << '\n';
                comparison_csv.flush();
                expect(bool(comparison_csv), "cannot write EDSL comparison CSV");
            }
            if (mismatch) {
                ++failures;
                std::cout << "native_d=" << expected_d[first].x << ',' << expected_d[first].y << ',' << expected_d[first].z << ',' << expected_d[first].w
                          << " edsl_d=" << actual_d[first].x << ',' << actual_d[first].y << ',' << actual_d[first].z << ',' << actual_d[first].w << '\n';
                std::cout << "native_s=" << expected_s[first].x << ',' << expected_s[first].y << ',' << expected_s[first].z << ',' << expected_s[first].w
                          << " edsl_s=" << actual_s[first].x << ',' << actual_s[first].y << ',' << actual_s[first].z << ',' << actual_s[first].w << '\n';
                if (std::getenv("REBLUR_EDSL_STOP_FIRST_MISMATCH")) throw std::runtime_error("EDSL native numerical equivalence");
            }
        }
    }
    expect(failures==0u,"EDSL native numerical equivalence");
    std::cout << "PASS: EDSL/native same-input three-layer numerical equivalence\n";
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
        if (const char *probe=std::getenv("REBLUR_FORMAT_PROBE_SPIRV")) {
            const char *prefix=std::getenv("REBLUR_FORMAT_PROBE_OUTPUT");
            expect(prefix && *prefix,"format probe needs REBLUR_FORMAT_PROBE_OUTPUT");
            vision::reblur::edsl::check_reblur_format_contract(device,stream,probe,prefix);
        }
        if (const char *probe=std::getenv("REBLUR_SAMPLE_PROBE_SPIRV")) {
            check_sampling(device,stream,probe);
            if(std::getenv("REBLUR_EDSL_SAMPLING_ONLY")) return 0;
        }
        if (!std::getenv("REBLUR_EDSL_RESOURCES_ONLY")) check_native_equivalence(device, stream);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
