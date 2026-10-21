#include "edsl_nrd_common.h"
#include "edsl_nrd_resources.h"

namespace vision::reblur::edsl {
namespace {
inline Bool conversion_valid(Uint4 identity, Float z, Float range) {
    return (identity.w & 1u) != 0u && !ocarina::isnan(z) && !ocarina::isinf(z) && z > 0.f && z < range;
}
} // namespace

PassShader compile_pack(Device &device) {
    Kernel kernel = [](Var<Dispatch> p) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Uint index = pixel.y * p.external.size.x + pixel.x;
        Int2 position = make_int2(pixel);
        auto guides = p.external.guide.soa_view_var<Guide>();
        Float4 geometry = guides.position.read(index);
        Uint4 identity = guides.identity.read(index);
        auto c = p.constants;

        Image<float4, Format::RGBA16Float> diffuse_output{p.outputs.t0};
        Image<float4, Format::RGBA16Float> specular_output{p.outputs.t1};
        Image<float4, Format::RGB10A2Unorm> normal_output{p.outputs.t2};
        Image<float, Format::R32Float> depth_output{p.outputs.t3};
        Image<float4, Format::RGBA16Float> motion_output{p.outputs.t4};
        Image<float, Format::R8Unorm> disocclusion_output{p.outputs.t5};
        $if(!conversion_valid(identity, geometry.w, c.gDenoisingRange)) {
            diffuse_output.write_in_bounds(position, make_float4(0.f));
            specular_output.write_in_bounds(position, make_float4(0.f));
            normal_output.write_in_bounds(position, make_float4(0.f));
            depth_output.write_in_bounds(position, max(1e6f, c.gDenoisingRange * 2.f));
            motion_output.write_in_bounds(position, make_float4(0.f));
            disocclusion_output.write_in_bounds(position, Float{0.f});
            $return();
        };

        Float4 normal_roughness = guides.normal_roughness.read(index);
        Float4 diffuse_factor = guides.diffuse_factor.read(index);
        Float4 specular_factor = guides.specular_factor.read(index);
        Float4 diffuse = p.external.raw_diffuse.read(index);
        Float4 specular = p.external.raw_specular.read(index);
        diffuse = make_float4(diffuse.xyz() / max(diffuse_factor.xyz(), make_float3(1e-3f)), diffuse.w);
        specular = make_float4(specular.xyz() / max(specular_factor.xyz(), make_float3(1e-3f)), specular.w);
        // Validate before saturation can turn an infinite hit into a valid one.
        Bool valid_diffuse_hit = abs(diffuse.w) < 3.402823466e38f;
        Bool valid_specular_hit = abs(specular.w) < 3.402823466e38f;
        Float diffuse_hit = select(valid_diffuse_hit,
            saturate(diffuse.w / HitDistanceNormalization(geometry.w, c.gHitDistParams, 1.f)), 0.f);
        Float specular_hit = select(valid_specular_hit,
            saturate(specular.w / HitDistanceNormalization(geometry.w, c.gHitDistParams, normal_roughness.w)), 0.f);
        diffuse_output.write_in_bounds(position, PackRadiance(diffuse.xyz(), diffuse_hit));
        specular_output.write_in_bounds(position, PackRadiance(specular.xyz(), specular_hit));

        Float disocclusion_mix = ((identity.w >> 8u) & 255u).cast<float>() / 255.f;
        disocclusion_output.write_in_bounds(position, disocclusion_mix);
        // Filtering roughness receives the mix after hit-distance normalization.
        normal_output.write_in_bounds(position, PackNormalRoughness(normal_roughness.xyz(), saturate(normal_roughness.w + disocclusion_mix), 0.f));
        depth_output.write_in_bounds(position, geometry.w);
        Float previous_depth = dot(p.previous_view_depth_row, make_float4(geometry.xyz(), 1.f));
        motion_output.write_in_bounds(position, make_float4(-make_float2(diffuse_factor.w, specular_factor.w), previous_depth - geometry.w, 0.f));
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-Pack");
}

PassShader compile_unpack(Device &device) {
    Kernel kernel = [](Var<Dispatch> p) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Uint index = pixel.y * p.external.size.x + pixel.x;
        Int2 position = make_int2(pixel);
        auto guides = p.external.guide.soa_view_var<Guide>();
        Float4 geometry = guides.position.read(index);
        Uint4 identity = guides.identity.read(index);
        auto c = p.constants;
        $if(conversion_valid(identity, geometry.w, c.gDenoisingRange)) {
            Float4 normal_roughness = guides.normal_roughness.read(index);
            Float3 diffuse_factor = guides.diffuse_factor.read(index).xyz();
            Float3 specular_factor = guides.specular_factor.read(index).xyz();
            Float4 diffuse = Image<float4, Format::RGBA16Float>{p.inputs.t0}.read_in_bounds(position);
            Float4 specular = Image<float4, Format::RGBA16Float>{p.inputs.t1}.read_in_bounds(position);
            Float3 diffuse_rgb = UnpackRadiance(diffuse) * max(diffuse_factor, make_float3(1e-3f));
            Float3 specular_rgb = UnpackRadiance(specular) * max(specular_factor, make_float3(1e-3f));
            Float diffuse_hit = diffuse.w * HitDistanceNormalization(geometry.w, c.gHitDistParams, 1.f);
            Float specular_hit = specular.w * HitDistanceNormalization(geometry.w, c.gHitDistParams, normal_roughness.w);
            p.external.output_diffuse.write(index, make_float4(diffuse_rgb, diffuse_hit));
            p.external.output_specular.write(index, make_float4(specular_rgb, specular_hit));
        } $else {
            // Match the native adapter's sky contract, including raw w and
            // avoiding all accesses when output and raw refer to the same span.
            $if((p.output_alias_mask & 1u) == 0u) {
                p.external.output_diffuse.write(index, p.external.raw_diffuse.read(index));
            };
            $if((p.output_alias_mask & 2u) == 0u) {
                p.external.output_specular.write(index, p.external.raw_specular.read(index));
            };
        };
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-Unpack");
}
} // namespace vision::reblur::edsl
