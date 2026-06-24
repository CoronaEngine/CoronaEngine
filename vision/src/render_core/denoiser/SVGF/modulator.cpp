#include "modulator.h"
#include "svgf.h"
#include "svgf_config.h"

namespace vision::svgf {

using Cfg = SVGFConfig;

void Modulator::prepare() noexcept {}

void Modulator::compile() noexcept {
Pipeline *pipeline_ref = pipeline();
auto safe_value = [](Float x, Float epsilon) -> Float {
    return sqrt(x * x + epsilon * epsilon);
};

auto safe_albedo = [&](Float3 albedo, Float epsilon) -> Float3 {
    return make_float3(
        safe_value(albedo.x, epsilon),
        safe_value(albedo.y, epsilon),
        safe_value(albedo.z, epsilon));
};

auto linear_demodulate = [&](RadType3Var radiance, Float3 albedo,
                             Float epsilon) -> RadType3Var {
    Float3 safe_a = safe_albedo(albedo, epsilon);
    return make_RadType3(make_float3(
        radiance.x / safe_a.x,
        radiance.y / safe_a.y,
        radiance.z / safe_a.z));
};

auto linear_modulate = [&](RadType3Var value, Float3 albedo,
                           Float epsilon) -> RadType3Var {
    Float3 safe_a = safe_albedo(albedo, epsilon);
    return make_RadType3(value * safe_a);
};

    Kernel demodulate_kernel = [&, pipeline_ref](Var<ModulatorParam> param) noexcept {
        Uint idx = dispatch_id();

        TriangleHitVar cur_hit = param.visibility_buffer.read(idx);

        $if(!PixelStateUtils::is_sky(cur_hit) &&
            !PixelStateUtils::is_emissive(pipeline_ref, cur_hit)) {
            RadType4Var radiance_direct = param.radiance_direct.read(idx);
            RadType4Var radiance_indirect = param.radiance_indirect.read(idx);
            // channel_kind == DiffuseSpecular: diffuse channel demodulated by DIFFUSE
            // albedo; specular channel left in radiance space (unless kDemodulateSpecular).
            // channel_kind == DirectIndirect (ReSTIR): both channels are lighting signals,
            // demodulate BOTH by the full surface albedo (canonical SVGF).
            $if(param.channel_kind == 0u) {
                Float3 diff_albedo = PixelStateUtils::query_diffuse_albedo(pipeline_ref, cur_hit, param.camera_pos.as_vec3());
                param.radiance_direct.write(idx, make_RadType4(
                                                     linear_demodulate(radiance_direct.xyz(), diff_albedo,
                                                                       Cfg::Modulator::kSoftEpsilon),
                                                     0.f));
                if constexpr (Cfg::Modulator::kDemodulateSpecular) {
                    Float3 spec_albedo = PixelStateUtils::query_specular_albedo(pipeline_ref, cur_hit, param.camera_pos.as_vec3());
                    param.radiance_indirect.write(idx, make_RadType4(
                                                           linear_demodulate(radiance_indirect.xyz(), spec_albedo,
                                                                           Cfg::Modulator::kSoftEpsilon),
                                                           0.f));
                }
            } $else {
                Float3 albedo = PixelStateUtils::query_albedo(pipeline_ref, cur_hit, param.camera_pos.as_vec3());
                param.radiance_direct.write(idx, make_RadType4(
                                                     linear_demodulate(radiance_direct.xyz(), albedo,
                                                                     Cfg::Modulator::kSoftEpsilon),
                                                     0.f));
                param.radiance_indirect.write(idx, make_RadType4(
                                                       linear_demodulate(radiance_indirect.xyz(), albedo,
                                                                       Cfg::Modulator::kSoftEpsilon),
                                                       0.f));
            };
        };
    };
    demodulate_ = device().compile(demodulate_kernel, "SVGF-Demodulate");

    Kernel modulate_kernel = [&, pipeline_ref](Var<ModulatorParam> param) noexcept {
        Uint idx = dispatch_id();

        TriangleHitVar cur_hit = param.visibility_buffer.read(idx);

        $if(!PixelStateUtils::is_sky(cur_hit) &&
            !PixelStateUtils::is_emissive(pipeline_ref, cur_hit)) {
            RadType4Var direct_filtered = param.radiance_direct.read(idx);
            RadType4Var indirect_filtered = param.radiance_indirect.read(idx);
            $if(param.channel_kind == 0u) {
                Float3 diff_albedo = PixelStateUtils::query_diffuse_albedo(pipeline_ref, cur_hit, param.camera_pos.as_vec3());
                param.radiance_direct.write(idx, make_RadType4(
                                                     linear_modulate(direct_filtered.xyz(), diff_albedo,
                                                                   Cfg::Modulator::kSoftEpsilon),
                                                     direct_filtered.w));
                if constexpr (Cfg::Modulator::kDemodulateSpecular) {
                    Float3 spec_albedo = PixelStateUtils::query_specular_albedo(pipeline_ref, cur_hit, param.camera_pos.as_vec3());
                    param.radiance_indirect.write(idx, make_RadType4(
                                                           linear_modulate(indirect_filtered.xyz(), spec_albedo,
                                                                         Cfg::Modulator::kSoftEpsilon),
                                                           indirect_filtered.w));
                }
            } $else {
                Float3 albedo = PixelStateUtils::query_albedo(pipeline_ref, cur_hit, param.camera_pos.as_vec3());
                param.radiance_direct.write(idx, make_RadType4(
                                                     linear_modulate(direct_filtered.xyz(), albedo,
                                                                   Cfg::Modulator::kSoftEpsilon),
                                                     direct_filtered.w));
                param.radiance_indirect.write(idx, make_RadType4(
                                                       linear_modulate(indirect_filtered.xyz(), albedo,
                                                                     Cfg::Modulator::kSoftEpsilon),
                                                       indirect_filtered.w));
            };
        };
    };
    modulate_ = device().compile(modulate_kernel, "SVGF-Modulate");
}

CommandBatch Modulator::demodulate(vision::RealTimeDenoiseInput &input) noexcept {
    ModulatorParam param;
    param.radiance_direct = input.direct.descriptor();
    param.radiance_indirect = input.indirect.descriptor();
    param.visibility_buffer = input.visibility.descriptor();
    param.motion_vectors = input.motion_vec.descriptor();
    param.camera_pos = input.camera_pos;
    param.channel_kind = static_cast<uint>(input.channel_kind);
    CommandBatch ret;
    ret << demodulate_(param).dispatch(input.resolution);
    return ret;
}

CommandBatch Modulator::modulate(vision::RealTimeDenoiseInput &input) noexcept {
    ModulatorParam param;
    param.radiance_direct = input.direct.descriptor();
    param.radiance_indirect = input.indirect.descriptor();
    param.visibility_buffer = input.visibility.descriptor();
    param.motion_vectors = input.motion_vec.descriptor();
    param.camera_pos = input.camera_pos;
    param.channel_kind = static_cast<uint>(input.channel_kind);
    CommandBatch ret;
    ret << modulate_(param).dispatch(input.resolution);
    return ret;
}

}// namespace vision::svgf
