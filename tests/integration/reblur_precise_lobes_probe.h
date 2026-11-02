#pragma once

#include "render_core/integrator/ReSTIR/denoiser_signals.h"
#include "render_core/integrator/ReSTIR/di_radiance_storage.h"
#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace precise_lobes_probe {
using namespace vision;
using namespace ocarina;

inline void check(Device &device, Stream &stream) {
    // Nine FP32 records per fixture: DI/GI totals, DI/GI spec+distance,
    // bypass, old layer diffuse/spec/bypass, factors and routing flags.
    using Case = std::array<float4, 9>;
    const Case ordinary{make_float4(5.f, 2.f, 1.f, 1.f), make_float4(3.f, 4.f, 2.f, 1.f),
        make_float4(.5f, .25f, .125f, 2.f), make_float4(.25f, .125f, .0625f, 8.f),
        make_float4(.2f, .1f, .05f, 0.f), make_float4(1.f, .3f, .2f, 3.f),
        make_float4(.5f, .1f, .2f, 9.f), make_float4(.1f, .2f, .3f, 0.f), make_float4(1.f, 1.f, 0.f, 0.f)};
    std::vector<Case> cases(10, ordinary);
    cases[1][0] = make_float4(1048576.75f, 1048576.25f, -1048575.75f, 1.f);
    cases[1][4] = make_float4(1048576.5f, 1048576.f, -1048576.f, 0.f);
    cases[1][8] = make_float4(.375f, 1.25f, 0.f, 0.f);
    cases[2][8] = make_float4(0.f, 1.f, 0.f, 0.f);
    cases[3][1] = make_float4(0.f); cases[3][3] = make_float4(0.f);
    cases[3][8] = make_float4(1.f, 0.f, 0.f, 0.f); // Disabled/missing GI still finalizes DI.
    cases[4][0] = make_float4(0.f); cases[4][2] = make_float4(0.f); cases[4][4] = make_float4(0.f);
    cases[4][8] = make_float4(0.f, 1.f, 0.f, 0.f); // Disabled DI has no staging.
    cases[5][2].w = 0.f; cases[5][3].w = 0.f;
    cases[5][8] = make_float4(0.f, 0.f, 0.f, 0.f);
    cases[6][5] = make_float4(0.f); cases[6][6] = make_float4(0.f); cases[6][7] = make_float4(0.f); // SP off.
    cases[7][5] = make_float4(.125f, 20000.f, 1e-6f, 7.f); // Distinct prior secondary plane values.
    cases[8][6] = make_float4(30000.f, .25f, 1e-7f, 11.f);
    cases[9][8].z = 1.f; // No dominant: neither path may overwrite its old outputs.
    std::vector<float4> inputs;
    for (const auto &sample : cases) inputs.insert(inputs.end(), sample.begin(), sample.end());
    const uint count = uint(cases.size());
    auto source = device.create_buffer<float4>(inputs.size(), "precise lobe fixtures");
    auto staged = device.create_buffer<float4>(count * 3u, "precise DI staging");
    auto compact = device.create_buffer<uint2>(count, "precise raw compact output");
    auto reference = device.create_buffer<float4>(count * 3u, "old explicit lobe merge");
    auto actual = device.create_buffer<float4>(count * 3u, "staged FP32 lobe merge");
    auto rounded = device.create_buffer<float4>(count * 3u, "incorrect rounded lobe merge");
    source.upload_immediately(inputs.data());
    std::vector<float4> sentinel(count * 3u, make_float4(-123.f));
    reference.upload_immediately(sentinel.data()); actual.upload_immediately(sentinel.data());
    rounded.upload_immediately(sentinel.data());
    Kernel old_merge = [](BufferVar<float4> src, BufferVar<float4> dst) {
        Uint i = dispatch_id(), base = i * 9u;
        Float4 factors = src.read(base + 8u);
        $if(factors.z != 0.f) { $return(); };
        Float4 sd = src.read(base + 2u), sg = src.read(base + 3u);
        Float3 bypass = src.read(base + 4u).xyz();
        Float3 d = src.read(base).xyz() - bypass;
        Float3 g = src.read(base + 1u).xyz();
        Float di = factors.x, gi = factors.y;
        Float3 spec = sd.xyz() * di + sg.xyz() * gi;
        Float3 diff = (d - sd.xyz()) * di + (g - sg.xyz()) * gi;
        Float2 wd = make_float2(max(luminance(d - sd.xyz()), 0.f) * di, max(luminance(sd.xyz()), 0.f) * di);
        Float2 wg = make_float2(max(luminance(g - sg.xyz()), 0.f) * gi, max(luminance(sg.xyz()), 0.f) * gi);
        Float2 distance = denoiser_signals::merge_hit_distances(wd, select(di > 0.f, sd.w, 0.f),
                                                             wg, select(gi > 0.f, sg.w, 0.f));
        dst.write(i * 3u, denoiser_signals::accumulate_lobe(src.read(base + 5u), diff, distance.x));
        dst.write(i * 3u + 1u, denoiser_signals::accumulate_lobe(src.read(base + 6u), spec, distance.y));
        dst.write(i * 3u + 2u, src.read(base + 7u) + make_float4(bypass * di, 0.f));
    };
    Kernel stage_di = [](BufferVar<float4> src, BufferVar<float4> stage, BufferVar<uint2> raw) {
        Uint i = dispatch_id(), base = i * 9u;
        Float4 direct = src.read(base), sd = src.read(base + 2u);
        stage.write(i * 3u, make_float4(direct.xyz(), sd.w));
        stage.write(i * 3u + 1u, make_float4(sd.xyz(), 0.f));
        stage.write(i * 3u + 2u, src.read(base + 4u));
        raw.write(i, pack_di_radiance(direct));
    };
    Kernel merge_staged = [](BufferVar<float4> src, BufferVar<float4> stage, BufferVar<uint2> raw,
                             BufferVar<float4> dst, Uint use_rounded) {
        Uint i = dispatch_id(), base = i * 9u;
        Float4 factors = src.read(base + 8u);
        $if(factors.z != 0.f) { $return(); };
        Float4 direct = stage.read(i * 3u);
        Float4 sd = make_float4(stage.read(i * 3u + 1u).xyz(), direct.w);
        $if(use_rounded != 0u) { direct = unpack_di_radiance(raw.read(i)); };
        auto merged = denoiser_signals::merge_restir_lobes(direct.xyz(), src.read(base + 1u).xyz(),
            sd, src.read(base + 3u), stage.read(i * 3u + 2u).xyz(), factors.x, factors.y,
            src.read(base + 5u), src.read(base + 6u), src.read(base + 7u));
        dst.write(i * 3u, merged.diffuse); dst.write(i * 3u + 1u, merged.specular);
        dst.write(i * 3u + 2u, merged.bypass);
    };
    auto old_shader = device.compile(old_merge, "old explicit precision oracle");
    auto stage_shader = device.compile(stage_di, "DI FP32 before raw packing probe");
    auto merge_shader = device.compile(merge_staged, "GI local FP32 merge probe");
    std::vector<float4> expected(count * 3u), result(count * 3u), wrong(count * 3u);
    stream << old_shader(source, reference).dispatch(count) << stage_shader(source, staged, compact).dispatch(count)
           << merge_shader(source, staged, compact, actual, 0u).dispatch(count)
           << merge_shader(source, staged, compact, rounded, 1u).dispatch(count)
           << reference.download(expected.data()) << actual.download(result.data()) << rounded.download(wrong.data())
           << synchronize() << commit();
    bool catches_quantization = false;
    for (size_t i = 0; i < result.size(); ++i) for (uint c = 0; c != 4u; ++c) {
        if (std::bit_cast<uint>(result[i][c]) != std::bit_cast<uint>(expected[i][c]))
            throw std::runtime_error("FP32 staged producer lobe merge differs from old operation order");
        catches_quantization |= std::bit_cast<uint>(wrong[i][c]) != std::bit_cast<uint>(expected[i][c]);
    }
    if (!catches_quantization) throw std::runtime_error("Precision oracle failed to reject rounded producer totals");
    std::cout << "PASS: FP32 lobe staging matches old GPU merge bitwise; HDR quantization control rejected\n";
}
}
