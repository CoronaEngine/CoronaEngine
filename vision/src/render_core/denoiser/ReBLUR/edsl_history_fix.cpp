#include "edsl_nrd_common.h"
#include "edsl_nrd_resources.h"
#include <cstdlib>
#include <string_view>

namespace vision::reblur::edsl {
PassShader compile_history_fix(Device &device) {
    const char *probe = std::getenv("REBLUR_EDSL_HF_PROBE");
    const bool hf_probe = probe && std::string_view(probe) == "1";
    Kernel kernel = [hf_probe](Var<Dispatch> p) {
        configure_block(8u, 16u);
        auto &c = p.constants;
        Image<float, Format::R8Unorm> tiles(p.inputs.t0);
        Image<float4, Format::RGB10A2Unorm> normals(p.inputs.t1);
        Image<float2, Format::RG8Unorm> data1(p.inputs.t2);
        Image<float, Format::R32Float> depth(p.inputs.t3);
        Image<float4, Format::RGBA16Float> in_diffuse(p.inputs.t4), in_specular(p.inputs.t5);
        Image<float, Format::R16Float> in_diffuse_fast(p.inputs.t6), in_specular_fast(p.inputs.t7);
        Image<float4, Format::RGBA16Float> out_diffuse(p.outputs.t0), out_specular(p.outputs.t1);
        Image<float, Format::R16Float> out_diffuse_fast(p.outputs.t2), out_specular_fast(p.outputs.t3);
        Uint2 groups = (make_uint2(c.gRectSize) + make_uint2(7u, 15u)) / make_uint2(8u, 16u);
        Int2 local = make_int2(thread_idx().xy());
        Int2 pixel = make_int2((groups - 1u - block_idx().xy()) * make_uint2(8u, 16u)) + local;
        // The padded 8x16 dispatch still fits the ceil(rect / 16) tile grid.
        Float is_sky = tiles.read_in_bounds(pixel >> 4);

        // BORDER=2, 12x20 footprint, matching the three upstream shared arrays.
        SharedArray<float, 240> cached_diff_luma;
        SharedArray<float, 240> cached_spec_luma;
        SharedArray<float2, 240> cached_frames;
        Uint lane = thread_idx().y * 8u + thread_idx().x;
        $if(is_sky == 0.f) {
            for (uint iteration = 0u; iteration != 2u; ++iteration) {
                Uint entry = lane + iteration * 128u;
                $if(entry < 240u) {
                    Int2 source = clamp(pixel - local - 2 + make_int2(entry % 12u, entry / 12u), make_int2(0), c.gRectSizeMinusOne);
                    Float2 frames = UnpackData1(data1.read_in_bounds(source));
                    Float4 entry_value = make_float4(in_diffuse_fast.read_in_bounds(source), in_specular_fast.read_in_bounds(source), frames);
                    cached_diff_luma.write(entry, entry_value.x);
                    cached_spec_luma.write(entry, entry_value.y);
                    cached_frames.write(entry, entry_value.zw());
                };
            }
        };
        call("GroupMemoryBarrierWithGroupSync");
        $if(is_sky != 0.f || any(pixel > c.gRectSizeMinusOne)) { $return(); };
        Float z = UnpackViewZ(c, depth.read_in_bounds(pixel));
        $if(z > c.gDenoisingRange) { $return(); };

        Float material;
        Float4 nr = NRD_FrontEnd_UnpackNormalAndRoughness(normals.read_in_bounds(pixel), material);
        Float3 n = nr.xyz();
        Float roughness = nr.w;
        Float frustum = GetFrustumSize(c.gMinRectDimMulUnproject, c.gOrthoMode, z);
        Float2 uv = (make_float2(pixel) + .5f) * c.gRectSizeInv;
        Float3 xv = ReconstructViewPosition(uv, c.gFrustum, z, c.gOrthoMode);
        Float3 nv = RotateVectorInverse(c.gViewToWorld, n);
        Int2 shared_pos = local + 2;
        auto shared_index = [](Int2 position) { return (position.y * 12 + position.x).cast<uint>(); };
        Float2 frames = cached_frames.read(shared_index(shared_pos));
        Float2 frame_average = frames;
        Float2 frame_weight = make_float2(1.f);
        Float inverse_fix = 1.f / (c.gHistoryFixFrameNum + NRD_EPS);
        $for(x, -1, 2) {
            $for(y, -1, 2) {
                $if(x != 0 || y != 0) {
                    Float2 f = cached_frames.read(shared_index(shared_pos + make_int2(x, y)));
                    Float2 w = select(f >= frames, Float2{make_float2(1.f)}, Float2{make_float2(0.f)});
                    frame_average += saturate(f * inverse_fix) * w;
                    frame_weight += w;
                };
            };
        };
        frame_average /= frame_weight;
        Float2 stride = c.gHistoryFixBasePixelStride / (2.f + frame_average * c.gHistoryFixFrameNum);
        stride *= select(frames < c.gHistoryFixFrameNum, Float2{make_float2(1.f)}, Float2{make_float2(0.f)});

        auto filter_lobe = [&](bool specular) {
            auto &input = specular ? in_specular : in_diffuse;
            auto &output = specular ? out_specular : out_diffuse;
            auto &input_fast = specular ? in_specular_fast : in_diffuse_fast;
            auto &output_fast = specular ? out_specular_fast : out_diffuse_fast;
            Float accum = specular ? frames.y : frames.x;
            Float lobe_roughness = specular ? roughness : Float{1.f};
            Float magic = specular ? GetSpecMagicCurve(roughness) : Float{1.f};
            Float lobe_stride = specular ? stride.y * Lerp(.5f, 1.f, magic) : Float{stride.x};
            lobe_stride = floor(lobe_stride);
            Float4 signal = input.read_in_bounds(pixel);
            $if(lobe_stride != 0.f) {
                Int stride_i = (lobe_stride + .5f).cast<int>();
                Float non_linear = 1.f / (1.f + accum);
                Float normal_param = GetNormalWeightParam(non_linear, c.gLobeAngleFraction, lobe_roughness);
                Float2 geometry_param = GetGeometryWeightParams(c.gPlaneDistSensitivity, frustum, xv, nv, non_linear);
                Float2 roughness_param = GetRelaxedRoughnessWeightParams(roughness * roughness, sqrt(c.gRoughnessFraction));
                Float hit_scale = _REBLUR_GetHitDistanceNormalization(z, c.gHitDistParams, lobe_roughness);
                Float hit = ExtractHitDist(signal) * hit_scale;
                Float2 hit_param = GetHitDistanceWeightParams(GetHitDistFactor(hit, frustum), non_linear, lobe_roughness);
                Float sum = 1.f + accum;
                signal *= sum;
                for (int y = -2; y != 3; ++y) {
                    for (int x = -2; x != 3; ++x) {
                        if ((x != 0 || y != 0) && std::abs(x) + std::abs(y) != 4) {
                            Float2 tap_uv = ocarina::fma(make_float2(pixel) + .5f, c.gRectSizeInv, make_float2(x, y) * lobe_stride * c.gRectSizeInv);
                            Int2 tap = clamp(pixel + make_int2(x, y) * stride_i, make_int2(0), c.gRectSizeMinusOne);
                            Float tap_z = UnpackViewZ(c, depth.read_in_bounds(tap));
                            Float tap_material;
                            Float4 tap_nr = NRD_FrontEnd_UnpackNormalAndRoughness(normals.read_in_bounds(tap), tap_material);
                            Float angle = AcosApprox(dot(tap_nr.xyz(), n));
                            Float3 tap_x = ReconstructViewPosition(tap_uv, c.gFrustum, tap_z, c.gOrthoMode);
                            Float w = IsInScreenNearest(tap_uv);
                            w *= ComputeWeight(dot(nv, tap_x), geometry_param.x, geometry_param.y);
                            w *= CompareMaterials(material, tap_material, specular ? c.gSpecMinMaterial : c.gDiffMinMaterial);
                            w *= ComputeExponentialWeight(angle, normal_param, 0.f);
                            if (specular) w *= ComputeExponentialWeight(tap_nr.w * tap_nr.w, roughness_param.x, roughness_param.y);
                            Float2 tap_frames = UnpackData1(data1.read_in_bounds(tap));
                            w *= 1.f + (specular ? tap_frames.y : tap_frames.x);
                            Float4 sample = Denanify(w, input.read_in_bounds(tap));
                            Float tap_hit = ExtractHitDist(sample) * hit_scale;
                            Float last_weight = ComputeExponentialWeight(GetHitDistFactor(tap_hit, frustum), hit_param.x, hit_param.y);
                            if (specular) {
                                w *= last_weight;
                                Float d = abs(hit - tap_hit) / (max(hit, tap_hit) + .001f);
                                Float b = LinearStep(.03f, .05f, roughness);
                                last_weight = SmoothStep(.2f + b, .05f + b, d);
                            }
                            sum = ocarina::fma(w, last_weight, sum);
                            w *= last_weight;
                            signal += sample * w;
                            if (hf_probe && specular) {
                                $if(all(pixel == make_int2(75, 25))) {
                                    Image<float4, Format::RGBA16Float> diagnostic(p.outputs.t4);
                                    Int probe_index = ((y + 2) * 5 + x + 2) * 3;
                                    auto store_probe = [&](Int slot, Float value) {
                                        Uint bits = as<uint>(value);
                                        Uint4 bytes = (make_uint4(bits) >> make_uint4(0u, 8u, 16u, 24u)) & 255u;
                                        diagnostic.write_in_bounds(make_int2(slot, 0), make_float4(bytes));
                                    };
                                    store_probe(probe_index, w);
                                    // Diagnostic expression; it may CSE with the
                                    // algorithm's rounded vector multiply.
                                    store_probe(probe_index + 1, sample.x * w);
                                    store_probe(probe_index + 2, signal.x);
                                };
                            }
                        };
                    };
                };
                if (hf_probe && specular) {
                    Float predivide_y = signal.x;
                    Float denominator = sum;
                    Float reciprocal = PositiveRcp(sum);
                    signal *= reciprocal;
                    $if(all(pixel == make_int2(75, 25))) {
                        Image<float4, Format::RGBA16Float> diagnostic(p.outputs.t4);
                        const std::array<Float, 5> values{
                            signal.x, denominator, predivide_y, reciprocal, signal.w};
                        for (uint index = 0u; index != values.size(); ++index) {
                            Uint bits = as<uint>(values[index]);
                            Uint4 bytes = (make_uint4(bits) >> make_uint4(0u, 8u, 16u, 24u)) & 255u;
                            diagnostic.write_in_bounds(make_int2(index + 75u, 0u), make_float4(bytes));
                        }
                    };
                } else {
                    signal *= PositiveRcp(sum);
                }
            };

            auto &cached_luma = specular ? cached_spec_luma : cached_diff_luma;
            Float center = cached_luma.read(shared_index(shared_pos));
            Float moment1 = center, moment2 = center * center;
            Float f = saturate(accum / (c.gHistoryFixFrameNum + NRD_EPS));
            if (specular) f = Lerp(1.f, f, magic);
            center = Lerp(GetLuma(signal), center, f);
            output_fast.write_in_bounds(pixel, center);
            $for(y, 0, 5) {
                $for(x, 0, 5) {
                    $if(x != 2 || y != 2) {
                        Float sample = cached_luma.read(shared_index(local + make_int2(x, y)));
                        moment1 += sample;
                        moment2 = ocarina::fma(sample, sample, moment2);
                    };
                };
            };
            // Square the rounded normalized mean, matching the original shader.
            // Reassociating its scale into rawSum * rawSum changes near-constant
            // variances and can move the output into a different FP16 bin.
            Float luma = GetLuma(signal);
            $if(c.gAntiFirefly != 0.f) {
                Float m1 = 0.f, m2 = 0.f;
                $for(y, -4, 5) {
                    $for(x, -4, 5) {
                        $if(abs(x) > 1 || abs(y) > 1) {
                            Int2 tap = clamp(pixel + make_int2(x, y), make_int2(0), c.gRectSizeMinusOne);
                            Float sample = input_fast.read_in_bounds(tap);
                            m1 += sample;
                            m2 = ocarina::fma(sample, sample, m2);
                        };
                    };
                };
                m1 = call<float>("oc_nrd_round_mul", m1, 1.f / 72.f);
                Float sigma = sqrt(abs(ocarina::fma(m2, 1.f / 72.f, -(m1 * m1)))) * 2.f;
                luma = clamp(luma, m1 - sigma, m1 + sigma);
            };
            moment1 = call<float>("oc_nrd_round_mul", moment1, 1.f / 25.f);
            Float sigma = sqrt(abs(ocarina::fma(moment2, 1.f / 25.f, -(moment1 * moment1)))) * 2.f;
            Float clamped_luma = clamp(luma, moment1 - sigma, moment1 + sigma);
            Float has_fast_history = (c.gMaxFastAccumulatedFrameNum < c.gMaxAccumulatedFrameNum).cast<float>();
            luma = Lerp(clamped_luma, luma, 1.f / (1.f + has_fast_history * accum * 2.f));
            output.write_in_bounds(pixel, ChangeLuma(signal, luma));
        };
        filter_lobe(false);
        filter_lobe(true);
    };
    return device.compile(kernel, "REBLUR-EDSL-HistoryFix");
}
}
