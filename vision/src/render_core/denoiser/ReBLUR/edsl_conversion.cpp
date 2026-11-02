#include "edsl_nrd_common.h"
#include "edsl_nrd_resources.h"

namespace vision::reblur::edsl {
namespace {
inline Bool conversion_valid(Uint4 identity, Float z, Float range) {
    return (identity.w & 1u) != 0u && !ocarina::isnan(z) && !ocarina::isinf(z) && z > 0.f && z < range;
}
void pack_invalid_pixel(const Var<Dispatch> &p, const Int2 &position) {
    $if(p.inputs_precleared == 0u) {
        Image<float4, Format::RGBA16Float>{p.outputs.t0}.write_in_bounds(position, make_float4(0.f));
        Image<float4, Format::RGBA16Float>{p.outputs.t1}.write_in_bounds(position, make_float4(0.f));
        Image<float4, Format::RGB10A2Unorm>{p.outputs.t2}.write_in_bounds(position, make_float4(0.f));
        Image<float, Format::R32Float>{p.outputs.t3}.write_in_bounds(position, max(1e6f, p.constants.gDenoisingRange * 2.f));
        Image<float4, Format::RGBA16Float>{p.outputs.t4}.write_in_bounds(position, make_float4(0.f));
        Image<float, Format::R8Unorm>{p.outputs.t5}.write_in_bounds(position, Float{0.f});
    };
}
void pack_valid_signals(const Var<Dispatch> &p, const Var<Guide> &guide, const Int2 &position,
                        const Float2 &repaired_hits, Float4 diffuse, Float4 specular) {
    auto c = p.constants;
    Float4 geometry = guide.position;
    Uint4 identity = guide.identity;
    Float4 normal_roughness = guide.normal_roughness;
    Float4 diffuse_factor = guide.diffuse_factor;
    Float4 specular_factor = guide.specular_factor;
    // Guide fallback only changes w. Forward those two scalars rather than
    // relying on physical-storage aliases to order a same-invocation reload.
    diffuse.w = select(repaired_hits.x >= 0.f, repaired_hits.x, diffuse.w);
    specular.w = select(repaired_hits.y >= 0.f, repaired_hits.y, specular.w);
    diffuse = make_float4(diffuse.xyz() / max(diffuse_factor.xyz(), make_float3(1e-3f)), diffuse.w);
    specular = make_float4(specular.xyz() / max(specular_factor.xyz(), make_float3(1e-3f)), specular.w);
    // Validate before saturation can turn an infinite hit into a valid one.
    Bool valid_diffuse_hit = abs(diffuse.w) < 3.402823466e38f;
    Bool valid_specular_hit = abs(specular.w) < 3.402823466e38f;
    Float diffuse_hit = select(valid_diffuse_hit,
        saturate(diffuse.w / HitDistanceNormalization(geometry.w, c.gHitDistParams, 1.f)), 0.f);
    Float specular_hit = select(valid_specular_hit,
        saturate(specular.w / HitDistanceNormalization(geometry.w, c.gHitDistParams, normal_roughness.w)), 0.f);
    Image<float4, Format::RGBA16Float>{p.outputs.t0}.write_in_bounds(position, PackRadiance(diffuse.xyz(), diffuse_hit));
    Image<float4, Format::RGBA16Float>{p.outputs.t1}.write_in_bounds(position, PackRadiance(specular.xyz(), specular_hit));

    Float disocclusion_mix = ((identity.w >> 8u) & 255u).cast<float>() / 255.f;
    Image<float, Format::R8Unorm>{p.outputs.t5}.write_in_bounds(position, disocclusion_mix);
    // Filtering roughness receives the mix after hit-distance normalization.
    Image<float4, Format::RGB10A2Unorm>{p.outputs.t2}.write_in_bounds(position, PackNormalRoughness(normal_roughness.xyz(), saturate(normal_roughness.w + disocclusion_mix), 0.f));
    Image<float, Format::R32Float>{p.outputs.t3}.write_in_bounds(position, geometry.w);
    Float previous_depth = dot(p.previous_view_depth_row, make_float4(geometry.xyz(), 1.f));
    Image<float4, Format::RGBA16Float>{p.outputs.t4}.write_in_bounds(position, make_float4(-make_float2(diffuse_factor.w, specular_factor.w), previous_depth - geometry.w, 0.f));
}
void pack_valid_pixel(const Var<Dispatch> &p, const Var<Guide> &guide, const Uint &index, const Int2 &position,
                      const Float2 &repaired_hits) {
    Var<Buffer<float4>> raw_diffuse{p.external.raw_diffuse.expression()};
    Var<Buffer<float4>> raw_specular{p.external.raw_specular.expression()};
    Float4 diffuse, specular;
    $if(p.defer_unpack != 0u) {
        // Read and retire through the same typed descriptors. Pass the original
        // values explicitly to conversion before replacing their storage.
        diffuse = raw_diffuse.read(index);
        specular = raw_specular.read(index);
    } $else {
        diffuse = call<float4>("oc_nrd_buffer_load_float4_aligned16", p.external.raw_diffuse, index.cast<ulong>());
        specular = call<float4>("oc_nrd_buffer_load_float4_aligned16", p.external.raw_specular, index.cast<ulong>());
    };
    pack_valid_signals(p, guide, position, repaired_hits, diffuse, specular);
    $if(p.defer_unpack != 0u) {
        raw_diffuse.write(index, make_float4(guide.diffuse_factor.xyz(), guide.position.w));
        raw_specular.write(index, make_float4(guide.specular_factor.xyz(), guide.normal_roughness.w));
    };
}
void retire_invalid_pixel(const Var<Dispatch> &p, const Uint &index) {
    $if(p.defer_unpack != 0u) {
        // Invalid guides compose the original RGB. Only the retired diffuse w
        // becomes the zero validity marker; specular RGB already stays in place.
        Var<Buffer<float4>> raw_diffuse{p.external.raw_diffuse.expression()};
        Float4 diffuse = raw_diffuse.read(index);
        raw_diffuse.write(index, make_float4(diffuse.xyz(), 0.f));
    };
}
} // namespace

void emit_pack_pixel(const Var<Dispatch> &p, const Var<Guide> &guide, const Float2 &repaired_hits) {
    Uint2 pixel = dispatch_idx().xy();
    Uint index = pixel.y * p.external.size.x + pixel.x;
    Int2 position = make_int2(pixel);
    $if(conversion_valid(guide.identity, guide.position.w, p.constants.gDenoisingRange)) {
        pack_valid_pixel(p, guide, index, position, repaired_hits);
    } $else {
        pack_invalid_pixel(p, position);
        retire_invalid_pixel(p, index);
    };
}

PassShader compile_pack(Device &device) {
    Kernel kernel = [](Var<Dispatch> p) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Uint index = pixel.y * p.external.size.x + pixel.x;
        Int2 position = make_int2(pixel);
        // Preserve the standalone path's aligned loads and early invalid exit.
        Ulong field_bytes = p.external.size.x.cast<ulong>() * p.external.size.y.cast<ulong>() * 16ull;
        Ulong element_offset = index.cast<ulong>() * 16ull;
        Var<Guide> guide;
        guide.position = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, element_offset);
        guide.identity = call<uint4>("oc_nrd_byte_load_uint4_aligned16", p.external.guide, field_bytes + element_offset);
        $if(!conversion_valid(guide.identity, guide.position.w, p.constants.gDenoisingRange)) {
            pack_invalid_pixel(p, position);
            retire_invalid_pixel(p, index);
            $return();
        };
        guide.normal_roughness = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, field_bytes * 2ull + element_offset);
        guide.diffuse_factor = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, field_bytes * 3ull + element_offset);
        guide.specular_factor = call<float4>("oc_nrd_byte_load_float4_aligned16", p.external.guide, field_bytes * 4ull + element_offset);
        pack_valid_pixel(p, guide, index, position, Float2{make_float2(-1.f)});
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-Pack");
}

PassShader compile_unpack(Device &device, bool compact_guide) {
    auto compile = [&]<bool compact>() {
    Kernel kernel = [](Var<Dispatch> p) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Uint index = pixel.y * p.external.size.x + pixel.x;
        Int2 position = make_int2(pixel);
        Float view_z;
        Uint4 identity;
        Float4 diffuse_depth;
        if constexpr (compact) {
            auto guides = p.external.guide.soa_view_var<CompactGuide>();
            diffuse_depth = guides.diffuse_depth.read(index);
            view_z = diffuse_depth.w;
            identity = make_uint4(0u, 0u, 0u, 1u);
        } else {
            auto guides = p.external.guide.soa_view_var<Guide>();
            view_z = guides.position.read(index).w;
            identity = guides.identity.read(index);
        }
        auto c = p.constants;
        $if(conversion_valid(identity, view_z, c.gDenoisingRange)) {
            Float roughness;
            Float3 diffuse_factor, specular_factor;
            if constexpr (compact) {
                auto guides = p.external.guide.soa_view_var<CompactGuide>();
                Float4 specular_roughness = guides.specular_roughness.read(index);
                diffuse_factor = diffuse_depth.xyz();
                specular_factor = specular_roughness.xyz();
                roughness = specular_roughness.w;
            } else {
                auto guides = p.external.guide.soa_view_var<Guide>();
                roughness = guides.normal_roughness.read(index).w;
                diffuse_factor = guides.diffuse_factor.read(index).xyz();
                specular_factor = guides.specular_factor.read(index).xyz();
            }
            Float4 diffuse = Image<float4, Format::RGBA16Float>{p.inputs.t0}.read_in_bounds(position);
            Float4 specular = Image<float4, Format::RGBA16Float>{p.inputs.t1}.read_in_bounds(position);
            Float3 diffuse_rgb = UnpackRadiance(diffuse) * max(diffuse_factor, make_float3(1e-3f));
            Float3 specular_rgb = UnpackRadiance(specular) * max(specular_factor, make_float3(1e-3f));
            Float diffuse_hit = diffuse.w * HitDistanceNormalization(view_z, c.gHitDistParams, 1.f);
            Float specular_hit = specular.w * HitDistanceNormalization(view_z, c.gHitDistParams, roughness);
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
    };
    return compact_guide ? compile.operator()<true>() : compile.operator()<false>();
}

namespace {
template<bool compact>
void emit_composing_unpack(Var<Dispatch> &p, Var<ComposeParam> &composition) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Uint index = pixel.y * p.external.size.x + pixel.x;
        Int2 position = make_int2(pixel);
        Float3 current_diffuse, current_specular;
        Float view_z;
        Uint4 identity;
        Float4 diffuse_depth;
        if constexpr (compact) {
            auto guides = p.external.guide.soa_view_var<CompactGuide>();
            diffuse_depth = guides.diffuse_depth.read(index);
            view_z = diffuse_depth.w;
            identity = make_uint4(0u, 0u, 0u, 1u);
        } else {
            auto guides = p.external.guide.soa_view_var<Guide>();
            view_z = guides.position.read(index).w;
            identity = guides.identity.read(index);
        }
        auto c = p.constants;
        $if(conversion_valid(identity, view_z, c.gDenoisingRange)) {
            Float roughness;
            Float3 diffuse_factor, specular_factor;
            if constexpr (compact) {
                auto guides = p.external.guide.soa_view_var<CompactGuide>();
                Float4 specular_roughness = guides.specular_roughness.read(index);
                diffuse_factor = diffuse_depth.xyz();
                specular_factor = specular_roughness.xyz();
                roughness = specular_roughness.w;
            } else {
                auto guides = p.external.guide.soa_view_var<Guide>();
                roughness = guides.normal_roughness.read(index).w;
                diffuse_factor = guides.diffuse_factor.read(index).xyz();
                specular_factor = guides.specular_factor.read(index).xyz();
            }
            Float4 diffuse = Image<float4, Format::RGBA16Float>{p.inputs.t0}.read_in_bounds(position);
            Float4 specular = Image<float4, Format::RGBA16Float>{p.inputs.t1}.read_in_bounds(position);
            Float3 diffuse_rgb = UnpackRadiance(diffuse), specular_rgb = UnpackRadiance(specular);
            diffuse_factor = max(diffuse_factor, make_float3(1e-3f));
            specular_factor = max(specular_factor, make_float3(1e-3f));
            // These values previously crossed an FP32 buffer store before
            // composition. Keep each product rounded before the later sums.
            for (uint component = 0u; component != 3u; ++component) {
                diffuse_rgb[component] = call<float>("oc_nrd_round_mul", diffuse_rgb[component], diffuse_factor[component]);
                specular_rgb[component] = call<float>("oc_nrd_round_mul", specular_rgb[component], specular_factor[component]);
            }
            current_diffuse = diffuse_rgb; current_specular = specular_rgb;
            Float diffuse_hit = diffuse.w * HitDistanceNormalization(view_z, c.gHitDistParams, 1.f);
            Float specular_hit = specular.w * HitDistanceNormalization(view_z, c.gHitDistParams, roughness);
            p.external.output_diffuse.write(index, make_float4(diffuse_rgb, diffuse_hit));
            p.external.output_specular.write(index, make_float4(specular_rgb, specular_hit));
        } $else {
            // Invalid guides keep the raw lobe, including in-place aliases.
            Float4 raw_diffuse = p.external.raw_diffuse.read(index);
            Float4 raw_specular = p.external.raw_specular.read(index);
            current_diffuse = raw_diffuse.xyz(); current_specular = raw_specular.xyz();
            $if((p.output_alias_mask & 1u) == 0u) {
                p.external.output_diffuse.write(index, raw_diffuse);
            };
            $if((p.output_alias_mask & 2u) == 0u) {
                p.external.output_specular.write(index, raw_specular);
            };
        };
        // Only the final layer uses this variant. Earlier layer outputs are
        // visible through the existing dispatch barrier; all reads are pixel-local.
        Float3 total = make_float3(0.f);
        $if(composition.layered != 0u) {
            total = composition.stable_radiance.read(index).xyz() * composition.direct_factor;
        };
        Uint pixels = p.external.size.x * p.external.size.y;
        $for(layer, composition.layer_count) {
            Uint address = layer * pixels + index;
            Float3 diffuse_rgb, specular_rgb;
            $if(layer == composition.layer_index) {
                diffuse_rgb = current_diffuse; specular_rgb = current_specular;
            } $else {
                diffuse_rgb = composition.diffuse.read(address).xyz();
                specular_rgb = composition.specular.read(address).xyz();
            };
            total += diffuse_rgb + specular_rgb + composition.bypass.read(address).xyz();
        };
        composition.output.write(index, make_float4(total, 1.f));
}
} // namespace

ComposingUnpackShader compile_unpack_and_compose(Device &device, bool compact_guide) {
    // Keep the large EDSL body outside generic lambdas: MSVC 19.44 ICEs when
    // the nested generic compiler lambda instantiates both parameter structs.
    if (compact_guide) {
        Kernel kernel = [](Var<Dispatch> p, Var<ComposeParam> composition) {
            emit_composing_unpack<true>(p, composition);
        };
        return device.compile(kernel, "NRD-EDSL-REBLUR-Unpack");
    }
    Kernel kernel = [](Var<Dispatch> p, Var<ComposeParam> composition) {
        emit_composing_unpack<false>(p, composition);
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-Unpack");
}

BatchedUnpackShader compile_unpack_batch(Device &device) {
    Kernel kernel = [](Var<Dispatch> p, Var<BatchUnpackParam> batch) {
        configure_block(16u, 8u);
        Uint2 pixel = dispatch_idx().xy();
        $if(any(pixel >= p.external.size)) { $return(); };
        Uint index = pixel.y * p.external.size.x + pixel.x;
        Uint pixels = p.external.size.x * p.external.size.y;
        Int2 position = make_int2(pixel);
        auto &composition = batch.composition;
        Float3 total = make_float3(0.f);
        $if(composition.layered != 0u) {
            Float3 stable = composition.stable_radiance.read(index).xyz();
            for (uint component = 0u; component != 3u; ++component)
                total[component] = call<float>("oc_nrd_round_mul", stable[component], composition.direct_factor);
        };
        for (uint layer = 0u; layer != 3u; ++layer) {
            $if(layer < composition.layer_count) {
                Uint address = layer * pixels + index;
                Float4 diffuse_cache = composition.diffuse.read(address);
                Float4 specular_cache = composition.specular.read(address);
                Float3 diffuse_rgb = diffuse_cache.xyz(), specular_rgb = specular_cache.xyz();
                $if(conversion_valid(Uint4{make_uint4(0u, 0u, 0u, 1u)}, diffuse_cache.w, p.constants.gDenoisingRange)) {
                    Float4 diffuse, specular;
                    if (layer == 0u) {
                        diffuse = Image<float4, Format::RGBA16Float>{batch.diffuse0}.read_in_bounds(position);
                        specular = Image<float4, Format::RGBA16Float>{batch.specular0}.read_in_bounds(position);
                    } else if (layer == 1u) {
                        diffuse = Image<float4, Format::RGBA16Float>{batch.diffuse1}.read_in_bounds(position);
                        specular = Image<float4, Format::RGBA16Float>{batch.specular1}.read_in_bounds(position);
                    } else {
                        diffuse = Image<float4, Format::RGBA16Float>{batch.diffuse2}.read_in_bounds(position);
                        specular = Image<float4, Format::RGBA16Float>{batch.specular2}.read_in_bounds(position);
                    }
                    diffuse_rgb = UnpackRadiance(diffuse); specular_rgb = UnpackRadiance(specular);
                    Float3 diffuse_factor = max(diffuse_cache.xyz(), make_float3(1e-3f));
                    Float3 specular_factor = max(specular_cache.xyz(), make_float3(1e-3f));
                    for (uint component = 0u; component != 3u; ++component) {
                        diffuse_rgb[component] = call<float>("oc_nrd_round_mul", diffuse_rgb[component], diffuse_factor[component]);
                        specular_rgb[component] = call<float>("oc_nrd_round_mul", specular_rgb[component], specular_factor[component]);
                    }
                };
                total += diffuse_rgb + specular_rgb + composition.bypass.read(address).xyz();
            };
        }
        composition.output.write(index, make_float4(total, 1.f));
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-Unpack-BatchCompose");
}
} // namespace vision::reblur::edsl
