#pragma once

#include "rtxpt_gi_standard_bsdf.h"
#include "rtxpt_gi_boiling.h"
#include "rtxpt_gi_rays.h"
#include "base/mgr/global.h"
#include <cstdlib>
#include <fstream>

namespace vision {
struct RtxptGIValidationInput {
    array<float4, 9> data{};
    uint4 header{};
};
struct RtxptGIValidationOutput {
    array<float4, 4> data{};
    uint4 flags{};
    float4 misc{};
};
struct RtxptGIConditioningOutput {
    array<float4, 4> rows{};
};
struct RtxptGIDSLPrimitiveOutput {
    uint4 flags{};
    float4 values{};
};
static_assert(sizeof(RtxptGIValidationInput) == 160u);
static_assert(sizeof(RtxptGIValidationOutput) == 96u);
static_assert(sizeof(RtxptGIConditioningOutput) == 64u);
}// namespace vision
OC_STRUCT(vision, RtxptGIValidationInput, data, header) {};
OC_STRUCT(vision, RtxptGIValidationOutput, data, flags, misc) {};
OC_STRUCT(vision, RtxptGIConditioningOutput, rows) {};
OC_STRUCT(vision, RtxptGIDSLPrimitiveOutput, flags, values) {};

namespace vision::rtxpt_gi {
// An explicit validation run also exercises the subgroup and exact ray-distance
// DSL primitives. The ordinary PT/DI tracing entry points retain their behavior.
inline void validate_dsl_primitives_gpu(const char *output_path) {
    constexpr uint count = 256u;
    auto output = Toolkit::device().create_buffer<RtxptGIDSLPrimitiveOutput>(count);
    Kernel probe = [&] {
        ocarina::Function::current()->require_ray_query_compute_workgroups();
        Uint index = dispatch_id(), local = thread_idx().x, group = index / 64u;
        Float weight = 0.f;
        $if(group == 1u) { weight = 1.f; };
        $if(group == 2u) { weight = select(local == 0u, 1000000.f, 1.f); };
        $if(group == 3u && local < 32u) { weight = cast<float>(local + 1u); };
        Bool boiling = native_boiling_filter(make_uint2(local % 8u, local / 8u), 0.35f, weight);
        Float x = (cast<float>(index % 16u) - 7.5f) * 0.04f;
        Float y = (cast<float>(index / 16u) - 7.5f) * 0.04f;
        RayVar ray = make_ray(make_float3(1.21100497f, 1.80475104f, 3.85239005f),
                              normalize(make_float3(-0.43810073f + x, -0.04167017f + y, -0.89795953f)), 100.f);
        Float distance = 0.f;
        const auto actual = Toolkit::geometry().accel().trace_closest(ray, distance);
        const auto expected = Toolkit::geometry().accel().trace_closest(ray);
        Bool same_hit = actual.inst_id == expected.inst_id && actual.prim_id == expected.prim_id &&
                        all(actual.bary == expected.bary);
        Bool hit = actual->is_hit();
        Float residual = 0.f;
        Bool valid_distance = distance == 0.f;
        $if(hit) {
            Interaction surface = Toolkit::geometry().compute_surface_interaction(actual, false);
            residual = length(ray->at(distance) - surface.pos);
            valid_distance = !ocarina::isnan(distance) && !ocarina::isinf(distance) &&
                             distance >= ray.org_min.w && distance <= ray.dir_max.w &&
                             residual <= max(1e-4f, length(surface.pos) * 1e-5f);
        };
        RtxptGIDSLPrimitiveOutputVar row;
        row.flags = make_uint4(cast<uint>(boiling), cast<uint>(same_hit), cast<uint>(valid_distance), cast<uint>(hit));
        row.values = make_float4(distance, residual, cast<float>(subgroup_lane_count()),
                                 cast<float>(subgroup_active_count_bits(weight > 0.f)));
        output.write(index, row);
    };
    probe.function()->set_block_dim(64u, 1u, 1u);
    auto shader = Toolkit::device().compile(probe, "RTXPT GI DSL primitive validation");
    vector<RtxptGIDSLPrimitiveOutput> results(count);
    Toolkit::stream() << shader().dispatch(count) << output.download(results.data()) << synchronize() << commit();
    std::ofstream saved(fs::path(string{output_path} + ".dsl-primitives.bin"), std::ios::binary);
    saved.write(reinterpret_cast<const char *>(results.data()), results.size() * sizeof(RtxptGIDSLPrimitiveOutput));
    saved.close();
    OC_ERROR_IF(!saved, "Cannot write RTXPT GI DSL primitive validation output");
}

// Explicit numerical validation only. Normal rendering has no extra dispatch.
inline void validate_standard_bsdf_gpu() {
    const char *input_path = std::getenv("VISION_RTXPT_GI_VALIDATE_INPUT");
    if (!input_path || !*input_path) return;
    const char *output_path = std::getenv("VISION_RTXPT_GI_VALIDATE_OUTPUT");
    OC_ERROR_IF(!output_path || !*output_path, "RTXPT GI validation requires an output path");
    validate_dsl_primitives_gpu(output_path);
    std::ifstream file(fs::path(input_path), std::ios::binary | std::ios::ate);
    OC_ERROR_IF(!file, "Cannot open RTXPT GI validation inputs");
    const auto bytes = file.tellg();
    OC_ERROR_IF(bytes <= 0 || uint64_t(bytes) % sizeof(RtxptGIValidationInput) != 0u,
                "RTXPT GI validation input size is invalid");
    vector<RtxptGIValidationInput> cases(uint64_t(bytes) / sizeof(RtxptGIValidationInput));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(cases.data()), bytes);
    OC_ERROR_IF(!file, "Cannot read RTXPT GI validation inputs");
    auto input = Toolkit::device().create_buffer<RtxptGIValidationInput>(cases.size());
    auto output = Toolkit::device().create_buffer<RtxptGIValidationOutput>(cases.size());
    Kernel probe = [&] {
        Uint index = dispatch_id();
        $if(index >= uint(cases.size())) { $return(); };
        const auto value = input.read(index);
        StandardMaterial material{value.data[0u], value.data[1u], value.data[2u], value.data[3u], value.header.xy()};
        Float3 n = value.data[4u].xyz(), v = value.data[5u].xyz();
        Float3 wi = value.data[6u].xyz(), wo = value.data[7u].xyz(), u = value.data[8u].xyz();
        auto evaluated = standard_eval(material, n, v, wi, wo);
        auto sampled = standard_sample(material, n, v, wi, u);
        RtxptGIValidationOutputVar result;
        result.data[0u] = make_float4(evaluated.diffuse, 0.f);
        result.data[1u] = make_float4(evaluated.specular, 0.f);
        result.data[2u] = make_float4(sampled.direction, sampled.pdf);
        result.data[3u] = make_float4(sampled.weight, sampled.lobe_probability);
        result.flags = make_uint4(sampled.lobe, cast<uint>(sampled.valid), standard_lobes(material), 0u);
        result.misc = make_float4(standard_pdf(material, n, v, wi, wo), 0.f, 0.f, 0.f);
        output.write(index, result);
    };
    probe.function()->set_block_dim(64u, 1u, 1u);
    auto shader = Toolkit::device().compile(probe, "RTXPT GI StandardBSDF numerical validation");
    vector<RtxptGIValidationOutput> results(cases.size());
    Toolkit::stream() << input.upload(cases.data()) << shader().dispatch(uint(cases.size()))
                      << output.download(results.data()) << synchronize() << commit();
    std::ofstream saved(fs::path(output_path), std::ios::binary);
    saved.write(reinterpret_cast<const char *>(results.data()), results.size() * sizeof(RtxptGIValidationOutput));
    saved.close();
    OC_ERROR_IF(!saved, "Cannot write RTXPT GI validation output");
    OC_INFO_FORMAT("RTXPT GI StandardBSDF GPU numerical validation wrote {} cases", cases.size());

    const char *conditioning = std::getenv("VISION_RTXPT_GI_VALIDATE_CONDITIONING");
    if (!conditioning || string_view{conditioning} != "1") return;
    auto intermediate = Toolkit::device().create_buffer<RtxptGIConditioningOutput>(cases.size());
    Kernel conditioning_probe = [&] {
        Uint index = dispatch_id();
        $if(index >= uint(cases.size())) { $return(); };
        const auto value = input.read(index);
        StandardMaterial material{value.data[0u], value.data[1u], value.data[2u],
                                  value.data[3u], value.header.xy()};
        const Float3 n = value.data[4u].xyz(), v = value.data[5u].xyz();
        const Float3 wi = value.data[6u].xyz(), u = value.data[8u].xyz();
        const auto sampled = standard_sample(material, n, v, wi, u);
        Float3 h = normalize(wi + sampled.direction);
        Float alpha = material.p0.w * material.p0.w;
        Float alpha2 = alpha * alpha;
        Float denominator = (h.z * alpha2 - h.z) * h.z + 1.f;
        Float ndf = alpha2 / (denominator * denominator * 3.14159265358979323846f);
        Float4x4 packed = make_float4x4(
            make_float4(sampled.direction, sampled.pdf), make_float4(h, alpha),
            make_float4(denominator, ndf, standard_pdf(material, n, v, wi, sampled.direction),
                        sampled.lobe_probability),
            make_float4(ocarina::as<float>(sampled.lobe), select(sampled.valid, 1.f, 0.f), 0.f, 0.f));
        RtxptGIConditioningOutputVar result;
        for (uint row = 0u; row < 4u; ++row) result.rows[row] = packed[row];
        intermediate.write(index, result);
    };
    conditioning_probe.function()->set_block_dim(64u, 1u, 1u);
    auto conditioning_shader = Toolkit::device().compile(conditioning_probe, "RTXPT GI StandardBSDF conditioning validation");
    vector<RtxptGIConditioningOutput> conditioning_results(cases.size());
    Toolkit::stream() << conditioning_shader().dispatch(uint(cases.size()))
                      << intermediate.download(conditioning_results.data()) << synchronize() << commit();
    std::ofstream conditioning_saved(fs::path(string{output_path} + ".conditioning.bin"), std::ios::binary);
    conditioning_saved.write(reinterpret_cast<const char *>(conditioning_results.data()),
                             conditioning_results.size() * sizeof(RtxptGIConditioningOutput));
    conditioning_saved.close();
    OC_ERROR_IF(!conditioning_saved, "Cannot write RTXPT GI conditioning validation output");
    OC_INFO_FORMAT("RTXPT GI StandardBSDF conditioning validation wrote {} cases", cases.size());
}
}// namespace vision::rtxpt_gi
