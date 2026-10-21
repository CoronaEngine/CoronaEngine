#include "edsl_nrd_common.h"
#include "edsl_nrd_resources.h"

namespace vision::reblur::edsl {
namespace {
using SignalImage = Image<float4, Format::RGBA16Float>;
using NormalImage = Image<float4, Format::RGB10A2Unorm>;
using ViewZImage = Image<float, Format::R32Float>;
using TileImage = Image<float, Format::R8Unorm>;

inline Float spatial_angle(Float cosine) {
    // Math::AcosApprox from the MathLib version used by NRD 4.14.3.
    return 1.4142135623730950488f * sqrt(saturate(1.f - cosine));
}

inline Float in_screen_weight(Float2 uv) {
    return (all(uv > 0.f) && all(uv < 1.f)).cast<float>();
}

inline Float2 viewport_uv(const Var<SharedConstants> &c, Float2 uv) {
    // NRD_USE_DENANIFICATION=1, NRD_USE_VIEWPORT_OFFSET=0.
    return min(uv * c.gResolutionScale, c.gResolutionScale - .5f * c.gResourceSizeInv);
}

inline Float4 denanify(Float weight, Float4 value) {
    return select(weight == 0.f, make_float4(0.f), value);
}

inline Float2 rotate_offset(Float4 rotation, Float2 v) {
    return v.x * rotation.xz() + v.y * rotation.yw();
}

inline Float4 scale_rotation(Float4 rotation, Float2 scale) {
    return make_float4(scale.x * rotation.xz(), scale.y * rotation.yw());
}

inline Int2 spatial_pixel(const Var<SharedConstants> &c, bool reversed) {
    Uint2 position = dispatch_idx().xy();
    if (reversed) {
        Uint2 block = make_uint2(8u, 16u);
        Uint2 groups = (make_uint2(c.gRectSize) + block - 1u) / block;
        position = (groups - 1u - position / block) * block + thread_idx().xy();
    }
    return make_int2(position);
}

inline Uint integer_explode(Uint x) {
    x = (x | (x << 8u)) & 0x00ff00ffu;
    x = (x | (x << 4u)) & 0x0f0f0f0fu;
    x = (x | (x << 2u)) & 0x33333333u;
    x = (x | (x << 1u)) & 0x55555555u;
    return x;
}

inline Uint spatial_hash(Uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

inline Uint tracking_rng_seed(Uint2 pixel, Uint frame) {
    Uint linear = integer_explode(pixel.x) | (integer_explode(pixel.y) << 1u);
    Uint seed = spatial_hash(frame + 0x035f9f29u);
    return seed ^ (spatial_hash(linear) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u));
}

inline Float tracking_random(Uint &state) {
    state = spatial_hash(state);
    // MathLib ML_RNG_MANTISSA_BITS: 2-asfloat((state>>9)|0x3f800000).
    // The arithmetic form is exact for these 23-bit integer mantissas.
    return 1.f - (state >> 9u).cast<float>() * (1.f / 8388608.f);
}

inline void kernel_basis(Float3 D, Float3 N, Float3 &T, Float3 &B) {
    Float sz = select(N.z >= 0.f, 1.f, -1.f);
    Float a = 1.f / (sz + N.z);
    Float ya = N.y * a;
    Float b = N.x * ya;
    Float c = N.x * sz;
    T = make_float3(ocarina::fma(c * N.x, a, -1.f), sz * b, c);
    B = make_float3(b, ocarina::fma(N.y, ya, -sz), N.y);
    $if(abs(dot(D, N)) < .999f) {
        Float3 R = call<float3>("reflect", -D, N);
        T = normalize(cross(N, R));
        B = cross(R, T);
    };
}

inline Float accumulated_fade(const Var<SharedConstants> &c, Float count) {
    Float a = ocarina::fma(c.gHistoryFixFrameNum, 2.f / 3.f, 1e-6f);
    Float b = ocarina::fma(c.gHistoryFixFrameNum, 4.f / 3.f, 2e-6f);
    return saturate((count - a) / (b - a));
}

// g_Special8 from NRD Common.hlsli. The third component is the Gaussian radius.
constexpr std::array<float3, 8> spatial_taps{{
    {-1.f, 0.f, 1.f}, {0.f, 1.f, 1.f}, {1.f, 0.f, 1.f}, {0.f, -1.f, 1.f},
    {-.3535533905932738f, .3535533905932738f, .5f},
    {.3535533905932738f, .3535533905932738f, .5f},
    {.3535533905932738f, -.3535533905932738f, .5f},
    {-.3535533905932738f, -.3535533905932738f, .5f}
}};

void spatial_filter(Var<Dispatch> &p, uint stage, bool temporal_stabilization) {
    auto &c = p.constants;
    Int2 pixel = spatial_pixel(c, stage != 1u);
    TileImage tiles{p.inputs.t0};
    $if(tiles.read_in_bounds(pixel >> 4) != 0.f || any(pixel > c.gRectSizeMinusOne)) { $return(); };

    NormalImage normals{p.inputs.t1};
    ViewZImage view_z{stage == 0u ? p.inputs.t2 : p.inputs.t5};
    Float view_z_packed = view_z.read_in_bounds(pixel);
    if (stage == 1u) ViewZImage{p.outputs.t2}.write_in_bounds(pixel, view_z_packed);
    Float z = UnpackViewZ(c, view_z_packed);
    $if(z > c.gDenoisingRange) { $return(); };

    Float material = 0.f;
    Float4 normal_packed = normals.read_in_bounds(pixel);
    Float4 normal = NRD_FrontEnd_UnpackNormalAndRoughness(normal_packed, material);
    Float3 N = normal.xyz();
    Float3 Nv = RotateVectorInverse(c.gViewToWorld, N);
    Float roughness = normal.w;
    Float2 pixel_uv = (make_float2(pixel) + .5f) * c.gRectSizeInv;
    Float3 Xv = ReconstructViewPosition(pixel_uv, c.gFrustum, z, c.gOrthoMode);
    Float3 Vv = GetViewVector(c, Xv, true);
    Float NoV = abs(dot(Nv, Vv));
    Float frustum_size = GetFrustumSize(c.gMinRectDimMulUnproject, c.gOrthoMode, z);
    Float4 rotator = stage == 0u ? Float4{c.gRotatorPre} :
                     stage == 1u ? Float4{c.gRotator} : Float4{c.gRotatorPost};
    Float2 data1 = make_float2(0.f);
    if (stage != 0u) data1 = UnpackData1(Image<float2, Format::RG8Unorm>{p.inputs.t2}.read_in_bounds(pixel));
    if (stage == 2u) {
        NormalImage{p.outputs.t0}.write_in_bounds(pixel, normal_packed);
        if (!temporal_stabilization)
            Image<uint, Format::R16Uint>{p.outputs.t3}.write_in_bounds(pixel, PackInternalData(data1.x + 1.f, data1.y + 1.f, material));
    }

    for (bool specular : {false, true}) {
        SignalImage signal{specular ? p.inputs.t4 : p.inputs.t3};
        Float4 value = signal.read_in_bounds(pixel);
        auto filter = [&] {
            Float smc = specular ? GetSpecMagicCurve(roughness) : Float{1.f};
            Float signal_roughness = specular ? roughness : Float{1.f};
            Float4 Dv = make_float4(0.f);
            Float NoD = 0.f;
            if (specular) {
                Dv = GetSpecularDominantDirection(Nv, Vv, roughness);
                NoD = abs(dot(Nv, Dv.xyz()));
            }
            Float hit_scale = _REBLUR_GetHitDistanceNormalization(z, c.gHitDistParams, signal_roughness);
            Float hit_distance = value.w * hit_scale;
            Float hit_factor = GetHitDistFactor(hit_distance, frustum_size);
            Float tracking_distance = select(hit_distance == 0.f, 1e6f, hit_distance);

            Float alpha = 1.f / 11.f;
            Float radius = specular ? Float{c.gSpecPrepassBlurRadius} : Float{c.gDiffPrepassBlurRadius};
            Float fraction_scale = 2.f;
            Float radius_scale = 1.f;
            if (stage != 0u) {
                Float count = specular ? Float{data1.y} : Float{data1.x};
                Float fade = accumulated_fade(c, count);
                Float fresnel_complement = 1.f - pow(saturate(1.f - NoV), 5.f);
                Float one_minus_boost;
                if (specular) {
                    Float base_boost = (1.f - fade) * fresnel_complement;
                    one_minus_boost = ocarina::fma(-base_boost, smc, 1.f);
                } else {
                    one_minus_boost = ocarina::fma(fade - 1.f, fresnel_complement, 1.f);
                }
                alpha = 1.f / ocarina::fma(one_minus_boost, count, 1.f);
                radius = c.gMaxBlurRadius;
                fraction_scale = stage == 1u ? 1.f : .5f;
                radius_scale = stage == 1u ? 1.f : 2.f;
            }
            Float area = signal_roughness * hit_factor;
            if (stage != 0u) area *= alpha;
            radius *= sqrt(saturate(area));
            if (stage == 0u && specular) {
                Float lobe_tan = GetSpecularLobeTanHalfAngle(roughness, .3f);
                Float lobe_radius = hit_distance * NoD * lobe_tan;
                Float projected_radius = lobe_radius / PixelRadiusToWorld(c.gUnproject, c.gOrthoMode, 1.f, ocarina::fma(hit_distance, Dv.w, z));
                radius = min(radius, projected_radius);
            }
            radius *= radius_scale;
            radius = max(radius, c.gMinBlurRadius * smc);

            Float2 geometry_parameters = GetGeometryWeightParams(c.gPlaneDistSensitivity, frustum_size, Xv, Nv, alpha);
            Float normal_parameter = GetNormalWeightParam(alpha, c.gLobeAngleFraction, signal_roughness) / fraction_scale;
            Float2 roughness_parameters = make_float2(0.f);
            if (specular) roughness_parameters = GetRoughnessWeightParams(roughness, saturate(c.gRoughnessFraction * fraction_scale));
            Float2 hit_parameters = GetHitDistanceWeightParams(value.w, alpha, signal_roughness);
            Float min_hit_weight = c.gMinHitDistanceWeight * fraction_scale * smc;
            if (stage != 0u) min_hit_weight *= sqrt(alpha);

            Float4 scaled_rotator = make_float4(0.f);
            Float3 T = make_float3(0.f), B = make_float3(0.f);
            if (stage == 0u || !specular) {
                Float2 skew = make_float2(1.f);
                if (stage != 0u) {
                    skew = Lerp(make_float2(1.f) - abs(Nv.xy()), make_float2(1.f), NoV);
                    skew /= max(skew.x, skew.y);
                }
                skew *= c.gRectSizeInv * radius;
                scaled_rotator = scale_rotation(rotator, skew);
            } else {
                Float bent_factor = sqrt(hit_factor);
                Float skew = Lerp(ocarina::fma(.75f, roughness, .25f), 1.f, NoD);
                skew = Lerp(skew, 1.f, alpha);
                skew = Lerp(1.f, skew, bent_factor);
                Float3 bent_Dv = normalize(Lerp(Nv, Dv.xyz(), bent_factor));
                kernel_basis(bent_Dv, Nv, T, B);
                Float world_radius = PixelRadiusToWorld(c.gUnproject, c.gOrthoMode, radius, z);
                T *= world_radius * skew;
                B *= world_radius / skew;
            }

            Float4 sum = value;
            Float weight_sum = 1.f;
            Uint rng = tracking_rng_seed(make_uint2(pixel), c.gFrameIndex);
            for (uint sample = 0u; sample < spatial_taps.size(); ++sample) {
                Float3 offset = make_float3(spatial_taps[sample]);
                Float2 uv = make_float2(0.f);
                if (stage == 0u) uv = ocarina::fma(make_float2(pixel) + .5f, c.gRectSizeInv, rotate_offset(scaled_rotator, offset.xy()));
                else if (!specular) uv = ocarina::fma(make_float2(pixel) + .5f, c.gRectSizeInv, rotate_offset(scaled_rotator, offset.xy()));
                else {
                    Float2 rotated = rotate_offset(rotator, offset.xy());
                    Float3 point = Xv + T * rotated.x + B * rotated.y;
                    Float4 clip = c.gViewToClip * make_float4(point, 1.f);
                    uv = make_float2(clip.x, -clip.y) / clip.w * .5f + .5f;
                }
                uv = (floor(uv * c.gRectSize) + .5f) * c.gRectSizeInv;
                Float2 uv_scaled = viewport_uv(c, uv);
                Float sample_z = UnpackViewZ(c, view_z.sample(uv_scaled, false));
                Float sample_material = 0.f;
                Float4 sample_normal = NRD_FrontEnd_UnpackNormalAndRoughness(normals.sample(uv_scaled, false), sample_material);
                Float angle = spatial_angle(dot(N, sample_normal.xyz()));
                Float3 sample_Xv = ReconstructViewPosition(uv, c.gFrustum, sample_z, c.gOrthoMode);
                Float w = in_screen_weight(uv);
                w *= ComputeWeight(dot(Nv, sample_Xv), geometry_parameters.x, geometry_parameters.y);
                Float min_material = specular ? Float{c.gSpecMinMaterial} : Float{c.gDiffMinMaterial};
                w *= (max(material, min_material) == max(sample_material, min_material)).cast<float>();
                w *= ComputeWeight(angle, normal_parameter, 0.f);
                if (specular) w *= ComputeWeight(sample_normal.w, roughness_parameters.x, roughness_parameters.y);
                Float4 neighbor = denanify(w, signal.sample(uv_scaled, false));

                if (stage == 0u && specular) {
                    Float hs = neighbor.w * _REBLUR_GetHitDistanceNormalization(sample_z, c.gHitDistParams, sample_normal.w);
                    Float distance = length(sample_Xv - Xv) + 1e-6f;
                    Float geometry_weight = w * saturate(hs / distance);
                    $if(tracking_random(rng) < geometry_weight) { tracking_distance = min(tracking_distance, hs); };
                    w *= c.gUsePrepassNotOnlyForSpecularMotionEstimation;
                    Float t = hs / ocarina::fma(value.w, hit_scale, distance);
                    w *= Lerp(saturate(t), 1.f, saturate((roughness - .5f) * 2.f));
                }
                w *= Lerp(min_hit_weight, 1.f, ComputeExponentialWeight(neighbor.w, hit_parameters.x, hit_parameters.y));
                Float gaussian = exp(-.66f * offset.z * offset.z);
                // DXC contracts the Gaussian contribution into the scalar
                // normalization sum, while the signal uses the rounded weight.
                weight_sum = ocarina::fma(w, gaussian, weight_sum);
                w *= gaussian;
                sum += neighbor * w;
            }
            // Math::PositiveRcp.
            value = sum * PositiveRcp(weight_sum);
            if (stage == 0u && specular)
                Image<float, Format::R16Float>{p.outputs.t2}.write_in_bounds(pixel, select(tracking_distance == 1e6f, 0.f, tracking_distance));
        };

        if (stage == 0u) {
            Float setting = specular ? Float{c.gSpecPrepassBlurRadius} : Float{c.gDiffPrepassBlurRadius};
            $if(setting != 0.f) { filter(); };
        } else filter();
        uint output = (stage == 2u ? 1u : 0u) + (specular ? 1u : 0u);
        SignalImage{output == 0u ? p.outputs.t0 : output == 1u ? p.outputs.t1 : output == 2u ? p.outputs.t2 : output == 3u ? p.outputs.t3 : output == 4u ? p.outputs.t4 : p.outputs.t5}.write_in_bounds(pixel, value);
        if (stage == 2u && !temporal_stabilization)
            SignalImage{specular ? p.outputs.t5 : p.outputs.t4}.write_in_bounds(pixel, value);
    }
}
} // namespace

PassShader compile_classify(Device &device) {
    Kernel kernel = [](Var<Dispatch> p) {
        configure_block(8u, 4u);
        auto &c = p.constants;
        Uint2 local = thread_idx().xy();
        Uint local_index = local.y * 8u + local.x;
        Uint2 tile = dispatch_idx().xy() / make_uint2(8u, 4u);
        Uint2 pixel = tile * 16u + local * make_uint2(2u, 4u);
        ViewZImage view_z{p.inputs.t0};
        Uint sum = 0u;
        for (uint x = 0u; x < 2u; ++x) {
            for (uint y = 0u; y < 4u; ++y) {
                Float z = UnpackViewZ(c, view_z.read(make_int2(pixel + make_uint2(x, y))));
                sum += (z > c.gDenoisingRange).cast<uint>();
            }
        }
        SharedArray<uint, 32> sums;
        sums.write(local_index, sum);
        synchronize_block();
        $if(local_index == 0u) {
            Uint total = 0u;
            $for(lane, 32u) { total += sums.read(lane); };
            TileImage{p.outputs.t0}.write_in_bounds(make_int2(tile), (total == 256u).cast<float>());
        };
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-ClassifyTiles");
}

PassShader compile_reconstruction(Device &device, bool is_5x5) {
    Kernel kernel = [is_5x5](Var<Dispatch> p) {
        configure_block(8u, 16u);
        auto &c = p.constants;
        Int2 pixel = make_int2(dispatch_idx().xy());
        TileImage tiles{p.inputs.t0};
        Float is_sky = tiles.read_in_bounds(pixel >> 4);
        NormalImage normals{p.inputs.t1};
        ViewZImage view_z{p.inputs.t2};
        SignalImage diffuse{p.inputs.t3}, specular{p.inputs.t4};
        const int radius = is_5x5 ? 2 : 1;
        const uint shared_width = 8u + 2u * radius;
        const uint shared_height = 16u + 2u * radius;
        Uint2 local = thread_idx().xy();
        Uint local_index = local.y * 8u + local.x;
        Int2 origin = pixel - make_int2(local);
        SharedArray<float4, 240> cached_normals;
        SharedArray<float3, 240> cached_hit_view_z;
        // Each 8x16 CTA lies inside a single 16x16 classification tile.
        // Every lane participates, including lanes outside a partial rect.
        $if(is_sky == 0.f) {
            $for(entry, local_index, shared_width * shared_height, 128u) {
                Int2 q = origin + make_int2(entry % shared_width, entry / shared_width) - radius;
                q = clamp(q, make_int2(0), c.gRectSizeMinusOne);
                Float sample_z = UnpackViewZ(c, view_z.read_in_bounds(q));
                Float4 sample_normal = NRD_FrontEnd_UnpackNormalAndRoughness(normals.read_in_bounds(q));
                Float2 hit = make_float2(diffuse.read_in_bounds(q).w, specular.read_in_bounds(q).w);
                cached_normals.write(entry, sample_normal);
                cached_hit_view_z.write(entry, make_float3(hit, sample_z));
            };
        };
        synchronize_block();
        $if(is_sky != 0.f || any(pixel > c.gRectSizeMinusOne)) { $return(); };
        Uint2 shared_center = local + static_cast<uint>(radius);
        Uint center_index = shared_center.y * shared_width + shared_center.x;
        Float3 center_hit_view_z = cached_hit_view_z.read(center_index);
        Float z = center_hit_view_z.z;
        $if(z > c.gDenoisingRange) { $return(); };
        Float4 normal = cached_normals.read(center_index);
        Float3 N = normal.xyz();
        Float roughness = normal.w;
        Float2 uv = (make_float2(pixel) + .5f) * c.gRectSizeInv;
        Float3 Xv = ReconstructViewPosition(uv, c.gFrustum, z, c.gOrthoMode);
        Float3 Nv = RotateVectorInverse(c.gViewToWorld, N);
        Float frustum_size = GetFrustumSize(c.gMinRectDimMulUnproject, c.gOrthoMode, z);
        Float2 geometry_parameters = GetGeometryWeightParams(c.gPlaneDistSensitivity, frustum_size, Xv, Nv, 1.f);
        Float2 relaxed_roughness_parameters = GetRelaxedRoughnessWeightParams(roughness * roughness);
        Float diffuse_normal_parameter = GetNormalWeightParam(1.f, 1.f);
        Float specular_normal_parameter = GetNormalWeightParam(1.f, 1.f, roughness);

        Float4 d = diffuse.read_in_bounds(pixel), s = specular.read_in_bounds(pixel);
        Float2 center = center_hit_view_z.xy();
        Float2 weights = make_float2((center.x != 0.f).cast<float>(), (center.y != 0.f).cast<float>()) * 1000.f;
        center *= weights;
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                if (x == 0 && y == 0) continue;
                Int2 shared_sample = make_int2(shared_center) + make_int2(x, y);
                Uint shared_index = shared_sample.y.cast<uint>() * shared_width + shared_sample.x.cast<uint>();
                Float3 sample_data = cached_hit_view_z.read(shared_index);
                Float sample_z = sample_data.z;
                Float2 hit = sample_data.xy();
                Float2 offset = make_float2(static_cast<float>(x), static_cast<float>(y));
                Float2 sample_uv = uv + offset * c.gRectSizeInv;
                Float w = in_screen_weight(sample_uv);
                w *= GetGaussianWeight(length(offset) * .5f);
                Float3 sample_Xv = ReconstructViewPosition(sample_uv, c.gFrustum, sample_z, c.gOrthoMode);
                w *= ComputeWeight(dot(Nv, sample_Xv), geometry_parameters.x, geometry_parameters.y);
                Float4 sample_normal = cached_normals.read(shared_index);
                Float angle = spatial_angle(dot(N, sample_normal.xyz()));
                Float2 ww = make_float2(w);
                ww.x *= ComputeExponentialWeight(angle, diffuse_normal_parameter, 0.f);
                ww.y *= ComputeExponentialWeight(angle, specular_normal_parameter, 0.f);
                ww.y *= ComputeExponentialWeight(sample_normal.w * sample_normal.w, relaxed_roughness_parameters.x, relaxed_roughness_parameters.y);
                hit.x = select(ww.x == 0.f, 0.f, hit.x);
                hit.y = select(ww.y == 0.f, 0.f, hit.y);
                ww *= make_float2((hit.x != 0.f).cast<float>(), (hit.y != 0.f).cast<float>());
                center += hit * ww;
                weights += ww;
            }
        }
        center /= max(weights, make_float2(1e-6f));
        d.w = center.x;
        s.w = center.y;
        SignalImage{p.outputs.t0}.write_in_bounds(pixel, d);
        SignalImage{p.outputs.t1}.write_in_bounds(pixel, s);
    };
    return device.compile(kernel, is_5x5 ? "NRD-EDSL-REBLUR-HitDistReconstruction5x5" : "NRD-EDSL-REBLUR-HitDistReconstruction3x3");
}

PassShader compile_prepass(Device &device) {
    Kernel kernel = [](Var<Dispatch> p) {
        configure_block(8u, 16u);
        spatial_filter(p, 0u, true);
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-PrePass");
}

PassShader compile_blur(Device &device, bool post, bool temporal_stabilization) {
    Kernel kernel = [post, temporal_stabilization](Var<Dispatch> p) {
        configure_block(8u, 16u);
        spatial_filter(p, post ? 2u : 1u, temporal_stabilization);
    };
    return device.compile(kernel, post ? (temporal_stabilization ? "NRD-EDSL-REBLUR-PostBlur" : "NRD-EDSL-REBLUR-PostBlurNoTS") : "NRD-EDSL-REBLUR-Blur");
}
} // namespace vision::reblur::edsl
