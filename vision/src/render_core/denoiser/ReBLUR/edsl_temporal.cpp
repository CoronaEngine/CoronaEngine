#include "edsl_nrd_common.h"
#include "edsl_nrd_resources.h"
#include <array>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace vision::reblur::edsl {
namespace {
using ScalarImage = Image<float, Format::R16Float>;
using SignalImage = Image<float4, Format::RGBA16Float>;
using NormalImage = Image<float4, Format::RGB10A2Unorm>;
constexpr float temporal_normal_ulp = .75f / 255.f;

struct TemporalBilinear {
    Float2 origin;
    Float2 fraction;
    Float4 weights;
};

TemporalBilinear temporal_bilinear(Float2 uv, Float2 size) {
    TemporalBilinear f;
    // DXC contracts the upstream uv * size - 0.5. Its low product bits decide
    // which side of an exact pixel center supplies the history footprint.
    Float2 p = ocarina::fma(uv, size, make_float2(-.5f));
    f.origin = floor(p);
    f.fraction = saturate(p - f.origin);
    Float2 a = saturate(1.f - f.fraction);
    f.weights = make_float4(a.x * a.y, f.fraction.x * a.y,
                           a.x * f.fraction.y, f.fraction.x * f.fraction.y);
    return f;
}

Float4 temporal_screen_bilinear(Float2 origin, Float2 size) {
    Float4 p = make_float4(origin, origin) + make_float4(0.f, 0.f, 1.f, 1.f);
    Float4 w = make_float4(p >= 0.f) * make_float4(p < make_float4(size, size));
    return w.xzxz() * w.yyww();
}

Float temporal_in_screen(Float2 uv) {
    return (all(uv > 0.f) && all(uv < 1.f)).cast<float>();
}

// D3D Gather*.wzxy: upper-left, upper-right, lower-left, lower-right.
template<typename I>
Float4 temporal_gather(const I &image, Float2 uv) {
    return image.template gather<0u>(uv).wzxy();
}

Float temporal_apply(Float4 values, Float4 weights) {
    Float sum = dot(weights, make_float4(1.f));
    Float value = ocarina::fma(Float{values.x}, Float{weights.x}, values.y * weights.y);
    value = ocarina::fma(Float{values.z}, Float{weights.z}, value);
    value = ocarina::fma(Float{values.w}, Float{weights.w}, value);
    return value * select(sum < .0001f, 0.f, 1.f / sum);
}

Float temporal_bilinear_value(Float4 values, const TemporalBilinear &f) {
    return Lerp(Lerp(values.x, values.y, f.fraction.x), Lerp(values.z, values.w, f.fraction.x), f.fraction.y);
}

template<typename I>
auto temporal_sample_custom(const I &image, Float2 sample_uv, Float2 rect_size, Float2 inv_size,
                            Float4 weights, Bool bicubic, const Float2 *sample_origin = nullptr) {
    using T = typename decltype(image.read(make_int2(0)))::org_type;
    // Preserve the existing DXC-aligned fused footprint initialization. These
    // are Common.hlsli's centerPos and saturate(samplePos - centerPos).
    Float2 origin = sample_origin ? Float2{*sample_origin} : floor(ocarina::fma(sample_uv, rect_size, make_float2(-.5f)));
    Float2 center = origin + .5f;
    Float2 f = saturate(ocarina::fma(sample_uv, rect_size, make_float2(-.5f) - origin));
    // Original _Init: NRD_CATROM_SHARPNESS == 0.5, then select the custom
    // bilinear weights and coordinates without a divergent sampling branch.
    Float2 w0 = f * ocarina::fma(f, -.5f * f + 1.f, make_float2(-.5f));
    Float2 w1 = ocarina::fma(f, f * (1.5f * f - 2.5f), make_float2(1.f));
    Float2 w2_middle = ocarina::fma(f, -1.5f * f + 2.f, make_float2(.5f));
    Float2 w2 = f * w2_middle;
    Float2 w3 = f * (f * (.5f * f - .5f));
    Float2 w12 = ocarina::fma(f, w2_middle, w1);
    Float2 tc = w2 / w12;
    Float4 w = make_float4(w12.x * w0.y, w0.x * w12.y,
                          w12.x * w12.y, w3.x * w12.y);
    Float w4 = w12.x * w3.y;
    w = select(bicubic, w, weights);
    w4 = select(bicubic, w4, 0.f);
    Float sum = dot(w, make_float4(1.f)) + w4;
    Float4 uv01 = make_float4(center, center) + select(bicubic,
        make_float4(tc.x, -1.f, -1.f, tc.y), make_float4(0.f, 0.f, 1.f, 0.f));
    Float4 uv23 = make_float4(center, center) + select(bicubic,
        make_float4(tc.x, tc.y, 2.f, tc.y), make_float4(0.f, 1.f, 1.f, 1.f));
    Float2 uv4 = center + select(bicubic, make_float2(tc.x, 2.f), f);
    uv01 *= make_float4(inv_size, inv_size);
    uv23 *= make_float4(inv_size, inv_size);
    uv4 *= inv_size;
    // Original _Color always performs five linear-clamp samples. The fifth
    // bilinear-fallback contribution has weight zero, then the same sum test
    // selects an exact zero rather than multiplying a potentially invalid value.
    Var<T> color;
    if constexpr (std::is_same_v<T, float>) {
        Float first = image.sample(uv01.xy());
        Float second = image.sample(uv01.zw());
        color = ocarina::fma(first, w.x, second * w.y);
        color = ocarina::fma(image.sample(uv23.xy()), w.z, color);
        color = ocarina::fma(image.sample(uv23.zw()), w.w, color);
        color = ocarina::fma(image.sample(uv4), w4, color);
    } else {
        color = image.sample(uv01.xy()) * w.x;
        color += image.sample(uv01.zw()) * w.y;
        color += image.sample(uv23.xy()) * w.z;
        color += image.sample(uv23.zw()) * w.w;
        color += image.sample(uv4) * w4;
    }
    return select(sum < .0001f, Var<T>{T{0.f}}, color / sum);
}

Float temporal_sample_fast(const ScalarImage &image, Float2 sample_origin, Float4 weights) {
    // HLSL int(centerPos) truncates after +0.5, including negative corners.
    Int2 origin = make_int2(sample_origin + .5f);
    Float c00 = image.read(origin);
    Float c10 = image.read(origin + make_int2(1, 0));
    Float color = ocarina::fma(c00, Float{weights.x}, c10 * weights.y);
    color = ocarina::fma(image.read(origin + make_int2(0, 1)), Float{weights.z}, color);
    color = ocarina::fma(image.read(origin + make_int2(1, 1)), Float{weights.w}, color);
    Float sum = dot(weights, make_float4(1.f));
    return select(sum < .0001f, 0.f, color / sum);
}

Uint temporal_hash(Uint x) {
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

Uint temporal_explode(Uint x) {
    x = (x | (x << 8u)) & 0x00ff00ffu;
    x = (x | (x << 4u)) & 0x0f0f0f0fu;
    x = (x | (x << 2u)) & 0x33333333u;
    return (x | (x << 1u)) & 0x55555555u;
}

Uint temporal_rng_init(Uint2 pixel, Uint frame) {
    Uint seed = temporal_hash(frame + 0x035f9f29u);
    Uint z = temporal_explode(pixel.x) | (temporal_explode(pixel.y) << 1u);
    return seed ^ (temporal_hash(z) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u));
}

Float temporal_rng_float(Uint &state) {
    state = temporal_hash(state);
    return 2.f - as<float>((state >> 9u) | 0x3f800000u);
}

Float2 temporal_stochastic_bilinear(Float2 uv, Float2 size, Uint &state) {
    auto f = temporal_bilinear(uv, size);
    Float x = temporal_rng_float(state);
    Float y = temporal_rng_float(state);
    f.origin += make_float2(f.fraction >= make_float2(x, y));
    return (f.origin + .5f) / size;
}

Float temporal_bayer(Uint2 pixel, Uint frame) {
    Uint2 p = pixel & 3u;
    Uint a = 2068378560u * (1u - (p.x >> 1u)) + 1500172770u * (p.x >> 1u);
    Uint b = (p.y + ((p.x & 1u) << 2u)) << 2u;
    Uint reverse = ((frame & 1u) << 3u) | ((frame & 2u) << 1u) |
                   ((frame & 4u) >> 1u) | ((frame & 8u) >> 3u);
    return (((a >> b) + reverse) & 15u).cast<float>() / 16.f;
}

Float temporal_modified_roughness(Float roughness, Float3 navg) {
    Float l = length(navg);
    Float numerator = saturate(ocarina::fma(-l, l, 1.f));
    Float denominator = max(l * ocarina::fma(-l, l, 3.f), 1e-15f);
    Float kappa = numerator * (1.f / denominator);
    return sqrt(saturate(ocarina::fma(roughness, roughness, kappa)));
}

Float temporal_parallax(Float3 x, Float2 uv, Float4x4 matrix, Float2 size) {
    return length((GetScreenUv(matrix, x) - uv) * size);
}

Float4 temporal_nonexp_sigma(Float4 x, Float2 params, Float sigma) {
    Float4 residual = abs(x * params.x + params.y) - sigma * params.x;
    Float4 v = saturate((residual - 1.f) * -1.f);
    return v * v * (3.f - 2.f * v);
}

Float temporal_nonexp_sigma(Float x, Float2 params, Float sigma) {
    Float affine = ocarina::fma(x, params.x, params.y);
    Float residual = ocarina::fma(-sigma, params.x, abs(affine));
    Float v = saturate((residual - 1.f) * -1.f);
    return v * v * ocarina::fma(-v, 2.f, 3.f);
}

Float temporal_smooth(Float a, Float b, Float x) {
    Float v = saturate((x - a) / (b - a));
    return v * v * (3.f - 2.f * v);
}

// MathLib BRDF::EnvironmentTerm_Rtg, used by the optional native MV update.
Float3 temporal_environment(Float3 rf0, Float nov, Float roughness) {
    Float m = saturate(roughness * roughness);
    Float4 x = make_float4(1.f, nov, nov * nov, nov * nov * nov);
    Float4 y = make_float4(1.f, m, m * m, m * m * m);
    Float bias_num = (0.99044f * x.x - 1.28514f * x.y) * y.x +
                     (1.29678f * x.x - .755907f * x.y) * y.y;
    Float bias_den = (x.x + 2.92338f * x.y + 59.4188f * x.w) * y.x +
                     (20.3225f * x.x - 27.0302f * x.y + 222.592f * x.w) * y.y +
                     (121.563f * x.x + 626.13f * x.y + 316.627f * x.w) * y.w;
    Float scale_num = (.0365463f * x.x + 3.32707f * x.y) * y.x +
                      (9.0632f * x.x - 9.04756f * x.y) * y.y;
    Float scale_den = (x.x + 3.59685f * x.z - 1.36772f * x.w) * y.x +
                      (9.04401f * x.x - 16.3174f * x.z + 9.22949f * x.w) * y.y +
                      (5.56589f * x.x + 19.7886f * x.z - 20.2123f * x.w) * y.w;
    return saturate(rf0 * (scale_num / max(scale_den, 1e-15f)) + bias_num / max(bias_den, 1e-15f));
}
}

PassShader compile_temporal(Device &device) {
    const char *probe = std::getenv("REBLUR_EDSL_TA_PROBE");
    const char *blend = std::getenv("REBLUR_EDSL_TA_BLEND_PROBE");
    const bool ta_blend_probe = probe && std::string_view(probe) == "1" && blend && std::string_view(blend) == "1";
    const char *edge = std::getenv("REBLUR_EDSL_TA_EDGE_PROBE");
    const bool ta_edge_probe = probe && std::string_view(probe) == "1" && edge && std::string_view(edge) == "1" && !ta_blend_probe;
    const bool ta_probe = probe && std::string_view(probe) == "1" && !ta_edge_probe && !ta_blend_probe;
    const char *xvirtual = std::getenv("REBLUR_EDSL_XVIRTUAL_PROBE");
    const bool xvirtual_probe = ta_probe && xvirtual && std::string_view(xvirtual) == "1";
    uint probe_x = 23u, probe_y = 0u;
    if (const char *pixel = std::getenv("REBLUR_EDSL_XVIRTUAL_PROBE_PIXEL"); xvirtual_probe && pixel) {
        const std::string_view value{pixel};
        const auto separator = value.find(',');
        if (separator == std::string_view::npos) throw std::invalid_argument("REBLUR_EDSL_XVIRTUAL_PROBE_PIXEL must be x,y");
        const auto x = std::from_chars(value.data(), value.data() + separator, probe_x);
        const auto y = std::from_chars(value.data() + separator + 1u, value.data() + value.size(), probe_y);
        if (x.ec != std::errc{} || x.ptr != value.data() + separator ||
            y.ec != std::errc{} || y.ptr != value.data() + value.size())
            throw std::invalid_argument("REBLUR_EDSL_XVIRTUAL_PROBE_PIXEL must be x,y");
    }
    Kernel kernel = [ta_probe, ta_edge_probe, ta_blend_probe, xvirtual_probe, probe_x, probe_y](Var<Dispatch> p) {
        configure_block(8u, 16u);
        // The optional diagnostic never aliases an algorithm/history image.
        // Its scoped curvature operands must survive until the final u7 store.
        std::optional<std::array<Float, 8>> curvature_probe;
        if (ta_probe) curvature_probe.emplace();
        std::optional<std::array<Float, 63>> xvirtual_values;
        if (xvirtual_probe) xvirtual_values.emplace();
        auto &c = p.constants;
        Image<float, Format::R8Unorm> tiles{p.inputs.t0};
        NormalImage normals{p.inputs.t1};
        Image<float, Format::R32Float> view_z{p.inputs.t2};
        SignalImage motion{p.inputs.t3};
        Image<float, Format::R32Float> prev_z{p.inputs.t4};
        NormalImage prev_normals{p.inputs.t5};
        Image<uint, Format::R16Uint> prev_internal{p.inputs.t6};
        Image<float, Format::R8Unorm> threshold_mix{p.inputs.t7}, diff_confidence{p.inputs.t8}, spec_confidence{p.inputs.t9};
        SignalImage in_diff{p.inputs.t10}, in_spec{p.inputs.t11};
        SignalImage history_diff{p.inputs.t12}, history_spec{p.inputs.t13};
        ScalarImage history_diff_fast{p.inputs.t14}, history_spec_fast{p.inputs.t15};
        ScalarImage prev_hit{p.inputs.t16}, in_hit{p.inputs.t17};
        SignalImage out_diff{p.outputs.t0}, out_spec{p.outputs.t1};
        ScalarImage out_diff_fast{p.outputs.t2}, out_spec_fast{p.outputs.t3}, out_hit{p.outputs.t4};
        Image<float2, Format::RG8Unorm> out_data1{p.outputs.t5};
        Image<uint, Format::R32Uint> out_data2{p.outputs.t6};
        Int2 pixel = make_int2(dispatch_idx().xy());
        Uint2 local = thread_idx().xy();
        Int2 origin = pixel - make_int2(local);
        SharedArray<float4, 180> cached_normal;
        SharedArray<float, 180> cached_hit;
        Float is_sky = tiles.read(pixel >> 4);
        $if(is_sky == 0.f) {
            for (uint iteration = 0u; iteration < 2u; ++iteration) {
                Uint entry = local.y * 8u + local.x + 128u * iteration;
                $if(entry < 180u) {
                    Int2 q = clamp(origin + make_int2(entry % 10u, entry / 10u) - 1, make_int2(0), c.gRectSizeMinusOne);
                    cached_normal.write(entry, NRD_FrontEnd_UnpackNormalAndRoughness(normals.read_in_bounds(q)));
                    Float h;
                    $if(c.gSpecPrepassBlurRadius == 0.f) { h = in_spec.read_in_bounds(q).w; }
                    $else { h = in_hit.read_in_bounds(q); };
                    cached_hit.write(entry, select(h == 0.f, 1e6f, h));
                };
            }
        };
        call("GroupMemoryBarrierWithGroupSync");
        $if(is_sky != 0.f || any(pixel > c.gRectSizeMinusOne)) { $return(); };
        Float viewZ = UnpackViewZ(c, view_z.read_in_bounds(pixel));
        $if(viewZ > c.gDenoisingRange) { $return(); };
        Float2 pixelUv = (make_float2(pixel) + .5f) * c.gRectSizeInv;
        Float3 Xv = ReconstructViewPosition(pixelUv, c.gFrustum, viewZ, c.gOrthoMode);
        Float3 X = RotateVector(c.gViewToWorld, Xv);
        Float3 Navg = make_float3(0.f);
        Float hitDistForTracking = 1e6f, roughnessM1 = 0.f, roughnessM2 = 0.f;
        Float roughnessFirst = 0.f, roughnessFirstSq = 0.f;
        for (uint y = 0u; y <= 2u; ++y) {
            for (uint x = 0u; x <= 2u; ++x) {
                Uint j = (local.y + y) * 10u + local.x + x;
                Float4 nr = cached_normal.read(j);
                if (x < 2u && y < 2u) { Navg += nr.xyz(); }
                hitDistForTracking = min(hitDistForTracking, cached_hit.read(j));
                Float r2 = nr.w * nr.w;
                if (x == 0u && y == 0u) {
                    roughnessFirst = nr.w;
                    roughnessFirstSq = r2;
                } else if (x == 1u && y == 0u) {
                    roughnessM1 = ocarina::fma(roughnessFirst, roughnessFirst, r2);
                    roughnessM2 = ocarina::fma(roughnessFirstSq, roughnessFirstSq, r2 * r2);
                } else {
                    roughnessM1 = ocarina::fma(nr.w, nr.w, roughnessM1);
                    roughnessM2 = ocarina::fma(r2, r2, roughnessM2);
                }
            }
        }
        Navg /= 4.f;
        Float materialID;
        Float4 normalAndRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(normals.read_in_bounds(pixel), materialID);
        Float3 N = normalAndRoughness.xyz();
        Float roughness = normalAndRoughness.w;
        Float roughnessModified = temporal_modified_roughness(roughness, Navg);
        roughnessM1 = call<float>("oc_nrd_round_mul", roughnessM1, 1.f / 9.f);
        Float roughnessSigma = sqrt(abs(ocarina::fma(roughnessM2, 1.f / 9.f, -(roughnessM1 * roughnessM1))));
        Uint rng = temporal_rng_init(make_uint2(pixel), c.gFrameIndex);
        hitDistForTracking = select(hitDistForTracking == 1e6f, 0.f, hitDistForTracking);
        Float hitDistNormalization = _REBLUR_GetHitDistanceNormalization(viewZ, c.gHitDistParams, roughness);
        hitDistForTracking *= select(c.gSpecPrepassBlurRadius == 0.f, hitDistNormalization, 1.f);
        out_hit.write_in_bounds(pixel, hitDistForTracking);

        // Surface motion and previous position, in ReBLUR's camera-relative world.
        Float3 mv = motion.read_in_bounds(pixel).xyz() * c.gMvScale.xyz();
        Float3 Xprev = X;
        Float2 smbPixelUv = ocarina::fma(make_float2(pixel) + .5f, c.gRectSizeInv, mv.xy());
        $if(c.gMvScale.w == 0.f) {
            $if(c.gMvScale.z == 0.f) { mv.z = AffineTransform(c.gWorldToViewPrev, X).z - viewZ; };
            Float3 xvp = ReconstructViewPosition(smbPixelUv, c.gFrustumPrev, viewZ + mv.z, c.gOrthoMode);
            Xprev = RotateVectorInverse(c.gWorldToViewPrev, xvp) + c.gCameraDelta.xyz();
        } $else {
            Xprev += mv;
            smbPixelUv = GetScreenUv(c.gWorldToClipPrev, Xprev);
        };
        auto smb = temporal_bilinear(smbPixelUv, c.gRectSizePrev);
        Float2 cat_origin = smb.origin - 1.f;
        Float2 cat_uv = cat_origin * c.gResourceSizeInvPrev;
        Float4 z0 = temporal_gather(prev_z, cat_uv + make_float2(1.f, 1.f) * c.gResourceSizeInvPrev);
        Float4 z1 = temporal_gather(prev_z, cat_uv + make_float2(3.f, 1.f) * c.gResourceSizeInvPrev);
        Float4 z2 = temporal_gather(prev_z, cat_uv + make_float2(1.f, 3.f) * c.gResourceSizeInvPrev);
        Float4 z3 = temporal_gather(prev_z, cat_uv + make_float2(3.f, 3.f) * c.gResourceSizeInvPrev);
        Float3 prevViewZ0 = abs(z0.yzw() * c.gViewZScale), prevViewZ1 = abs(z1.xzw() * c.gViewZScale);
        Float3 prevViewZ2 = abs(z2.xyw() * c.gViewZScale), prevViewZ3 = abs(z3.xyz() * c.gViewZScale);
        Float3 smbNavg = make_float3(0.f);
        Float normal_sum = 0.f;
        Float4 normal_valid = make_float4(prevViewZ0.z < c.gDenoisingRange, prevViewZ1.y < c.gDenoisingRange,
                                         prevViewZ2.y < c.gDenoisingRange, prevViewZ3.x < c.gDenoisingRange);
        Uint2 previous_normal_origin = make_uint2(smb.origin);
        for (uint k = 0u; k < 4u; ++k) {
            Int2 q = make_int2(previous_normal_origin + make_uint2(k % 2u, k / 2u));
            smbNavg += NRD_FrontEnd_UnpackNormalAndRoughness(prev_normals.read(q)).xyz() * normal_valid[k];
            normal_sum += normal_valid[k];
        }
        smbNavg /= select(normal_sum == 0.f, 1.f, normal_sum);
        smbNavg = RotateVector(c.gWorldPrevToWorld, smbNavg);
        Float smbParallaxInPixels1 = temporal_parallax(Xprev + c.gCameraDelta.xyz(),
            select(c.gOrthoMode == 0.f, smbPixelUv, pixelUv), c.gWorldToClipPrev, c.gRectSize);
        Float smbParallaxInPixels2 = temporal_parallax(Xprev - c.gCameraDelta.xyz(),
            select(c.gOrthoMode == 0.f, pixelUv, smbPixelUv), c.gWorldToClip, c.gRectSize);
        Float smbParallaxInPixelsMax = max(smbParallaxInPixels1, smbParallaxInPixels2);
        Float smbParallaxInPixelsMin = min(smbParallaxInPixels1, smbParallaxInPixels2);
        Float pixelSize = PixelRadiusToWorld(c.gUnproject, c.gOrthoMode, 1.f, viewZ);
        Float frustumSize = GetFrustumSize(c.gMinRectDimMulUnproject, c.gOrthoMode, viewZ);
        Float disocclusionThresholdMix = 0.f;
        $if(materialID == c.gStrandMaterialID) {
            disocclusionThresholdMix = pixelSize / (pixelSize + c.gStrandThickness);
        };
        $if(c.gHasDisocclusionThresholdMix != 0u) { disocclusionThresholdMix = threshold_mix.read_in_bounds(pixel); };
        Float disocclusionThreshold = Lerp(c.gDisocclusionThreshold, c.gDisocclusionThresholdAlternate, disocclusionThresholdMix);
        Float smallParallax = saturate((smbParallaxInPixelsMax - .25f) / -.25f);
        Float thresholdAngle = cos(Float{89.f} * 3.14159265358979323846f / 180.f) - .25f * smallParallax;
        Float3 V = GetViewVector(c, X);
        Float NoV = abs(dot(N, V));
        Float NoVstrict = Lerp(NoV, 1.f, saturate(smbParallaxInPixelsMax / 30.f));
        Float4 smbDisocclusionThreshold = make_float4(GetDisocclusionThreshold(disocclusionThreshold, frustumSize, NoVstrict));
        smbDisocclusionThreshold *= (dot(smbNavg, Navg) > thresholdAngle).cast<float>();
        smbDisocclusionThreshold *= temporal_screen_bilinear(smb.origin, c.gRectSizePrev);
        smbDisocclusionThreshold -= 1e-6f;
        Float3 Xvprev = AffineTransform(c.gWorldToViewPrev, Xprev);
        Float3 smbOcclusion0 = make_float3(abs(prevViewZ0 - Xvprev.z) <= smbDisocclusionThreshold.x);
        Float3 smbOcclusion1 = make_float3(abs(prevViewZ1 - Xvprev.z) <= smbDisocclusionThreshold.y);
        Float3 smbOcclusion2 = make_float3(abs(prevViewZ2 - Xvprev.z) <= smbDisocclusionThreshold.z);
        Float3 smbOcclusion3 = make_float3(abs(prevViewZ3 - Xvprev.z) <= smbDisocclusionThreshold.w);
        Uint4 internal0 = make_uint4(temporal_gather(prev_internal, cat_uv + make_float2(1.f, 1.f) * c.gResourceSizeInvPrev));
        Uint4 internal1 = make_uint4(temporal_gather(prev_internal, cat_uv + make_float2(3.f, 1.f) * c.gResourceSizeInvPrev));
        Uint4 internal2 = make_uint4(temporal_gather(prev_internal, cat_uv + make_float2(1.f, 3.f) * c.gResourceSizeInvPrev));
        Uint4 internal3 = make_uint4(temporal_gather(prev_internal, cat_uv + make_float2(3.f, 3.f) * c.gResourceSizeInvPrev));
        Float3 mat0 = make_float3(UnpackInternalData(internal0.y).z, UnpackInternalData(internal0.z).z, UnpackInternalData(internal0.w).z);
        Float3 mat1 = make_float3(UnpackInternalData(internal1.x).z, UnpackInternalData(internal1.z).z, UnpackInternalData(internal1.w).z);
        Float3 mat2 = make_float3(UnpackInternalData(internal2.x).z, UnpackInternalData(internal2.y).z, UnpackInternalData(internal2.w).z);
        Float3 mat3 = make_float3(UnpackInternalData(internal3.x).z, UnpackInternalData(internal3.y).z, UnpackInternalData(internal3.z).z);
        Float minMaterialID = min(c.gSpecMinMaterial, c.gDiffMinMaterial);
        smbOcclusion0 *= make_float3(make_float3(max(materialID, minMaterialID)) == max(mat0, make_float3(minMaterialID)));
        smbOcclusion1 *= make_float3(make_float3(max(materialID, minMaterialID)) == max(mat1, make_float3(minMaterialID)));
        smbOcclusion2 *= make_float3(make_float3(max(materialID, minMaterialID)) == max(mat2, make_float3(minMaterialID)));
        smbOcclusion3 *= make_float3(make_float3(max(materialID, minMaterialID)) == max(mat3, make_float3(minMaterialID)));
        Float4 smbOcclusion = make_float4(smbOcclusion0.z, smbOcclusion1.y, smbOcclusion2.y, smbOcclusion3.x);
        Float4 smbOcclusionWeights = smb.weights * smbOcclusion;
        Bool smbAllowCatRom = dot(smbOcclusion0 + smbOcclusion1 + smbOcclusion2 + smbOcclusion3, make_float3(1.f)) > 11.5f;
        Float fbits = dot(smbOcclusion, make_float4(1.f, 2.f, 4.f, 8.f));
        Float2 internalData00 = UnpackInternalData(internal0.w).xy(), internalData10 = UnpackInternalData(internal1.z).xy();
        Float2 internalData01 = UnpackInternalData(internal2.y).xy(), internalData11 = UnpackInternalData(internal3.x).xy();
        Float diffAccumSpeed = temporal_apply(make_float4(internalData00.x, internalData10.x, internalData01.x, internalData11.x), smbOcclusionWeights);
        Float smbSpecAccumSpeed = temporal_apply(make_float4(internalData00.y, internalData10.y, internalData01.y, internalData11.y), smbOcclusionWeights);
        Float3 smbVprev = GetViewVectorPrev(c, Xprev, c.gCameraDelta.xyz());
        Float NoVprev = abs(dot(N, smbVprev));
        Float sizeQuality = (NoVprev + .001f) / (NoV + .001f);
        sizeQuality = Lerp(.1f, 1.f, saturate(sizeQuality * sizeQuality));
        Float smbFootprintQuality = sqrt(saturate(temporal_bilinear_value(smbOcclusion, smb))) * sizeQuality;

        Float specHistoryConfidence = smbFootprintQuality;
        $if(c.gHasHistoryConfidence != 0u) { specHistoryConfidence *= spec_confidence.read_in_bounds(pixel); };
        smbSpecAccumSpeed *= Lerp(specHistoryConfidence, 1.f, 1.f / (1.f + smbSpecAccumSpeed));
        smbSpecAccumSpeed = min(smbSpecAccumSpeed, c.gMaxAccumulatedFrameNum);
        Float4 spec = in_spec.read_in_bounds(pixel);

        // Curvature along predicted parallax. Both near edges and high-parallax
        // flattened normals follow the original camera-relative construction.
        Float curvature = 0.f;
        {
            Float2 deltaUv;
            if (ta_edge_probe) {
                Float2 uv_for_zero_parallax = select(c.gOrthoMode == 0.f, smbPixelUv, pixelUv);
                Float2 projected_uv = GetScreenUv(c.gWorldToClipPrev, Xprev + c.gCameraDelta.xyz());
                deltaUv = uv_for_zero_parallax - projected_uv;
                $if(c.gFrameIndex == 6u && all(pixel == make_int2(5, 2))) {
                    SignalImage diagnostic{p.outputs.t7};
                    const std::array<Float, 6> values{uv_for_zero_parallax.x, uv_for_zero_parallax.y,
                                                     projected_uv.x, projected_uv.y, smbParallaxInPixels1, c.gRectSize.y};
                    for (uint i = 0u; i != values.size(); ++i) {
                        Uint raw_bits = as<uint>(values[i]);
                        Uint4 bytes = (make_uint4(raw_bits) >> make_uint4(0u, 8u, 16u, 24u)) & 255u;
                        diagnostic.write(make_int2(i, 0u), make_float4(bytes));
                    }
                };
            } else {
                deltaUv = select(c.gOrthoMode == 0.f, smbPixelUv, pixelUv) - GetScreenUv(c.gWorldToClipPrev, Xprev + c.gCameraDelta.xyz());
            }
            deltaUv *= c.gRectSize;
            deltaUv /= max(smbParallaxInPixels1, 1.f / 256.f);
            Float3 x10, x01;
            for (uint axis = 0u; axis < 2u; ++axis) {
                Float2 offset = axis == 0u ? make_float2(1.f, 0.f) : make_float2(0.f, 1.f);
                Float2 curvature_uv = ocarina::fma(make_float2(pixel) + .5f, c.gRectSizeInv, offset * c.gRectSizeInv);
                Float3 xv = ReconstructViewPosition(curvature_uv, c.gFrustum, 1.f, c.gOrthoMode);
                Float3 x = RotateVector(c.gViewToWorld, xv);
                Float3 v = GetViewVector(c, x);
                Float3 o = select(c.gOrthoMode == 0.f, make_float3(0.f), x);
                Float3 intersection = o + v * dot(X - o, N) / dot(N, v);
                if (axis == 0u) { x10 = intersection; } else { x01 = intersection; }
            }
            Float3 n10 = cached_normal.read((local.y + 1u) * 10u + local.x + 2u).xyz();
            Float3 n01 = cached_normal.read((local.y + 2u) * 10u + local.x + 1u).xyz();
            Float2 w = abs(deltaUv) + 1.f / 256.f;
            w /= w.x + w.y;
            Float3 x = x10 * w.x + x01 * w.y;
            Float3 n = normalize(n10 * w.x + n01 * w.y);
            Float deltaUvLenFixed = smbParallaxInPixelsMin;
            deltaUvLenFixed *= ocarina::fma(c.gFramerateScale, temporal_bayer(make_uint2(pixel), c.gFrameIndex), 1.f);
            Float2 motionUvHigh = ocarina::fma(make_float2(pixel) + .5f, c.gRectSizeInv, deltaUvLenFixed * deltaUv * c.gRectSizeInv);
            motionUvHigh = (floor(motionUvHigh * c.gRectSize) + .5f) * c.gRectSizeInv;
            $if(deltaUvLenFixed > 1.f && temporal_in_screen(motionUvHigh) != 0.f) {
                Float2 uvScaled = min(motionUvHigh * c.gResolutionScale, c.gResolutionScale - .5f * c.gResourceSizeInv);
                Float zHigh = UnpackViewZ(c, view_z.sample(uvScaled, false));
                Float3 xHigh = RotateVector(c.gViewToWorld, ReconstructViewPosition(motionUvHigh, c.gFrustum, zHigh, c.gOrthoMode));
                Float3 nHigh = NRD_FrontEnd_UnpackNormalAndRoughness(normals.sample(uvScaled, false)).xyz();
                Bool same = abs(zHigh - viewZ) / max(zHigh, viewZ) < .1f;
                n = select(same, nHigh, n); x = select(same, xHigh, x);
            };
            Float3 edge = x - X;
            curvature = dot(n - N, edge) * (1.f / max(dot(edge, edge), 1e-15f));
            if (ta_probe) {
                auto &values = *curvature_probe;
                values[0] = edge.x; values[1] = edge.y; values[2] = edge.z;
                values[3] = n.x; values[4] = n.y; values[5] = n.z;
                values[6] = dot(n - N, edge); values[7] = dot(edge, edge);
            }
        }
        Float4 dominantDirection = GetSpecularDominantDirection(N, V, roughness);
        Float3 Xvirtual = xvirtual_probe ?
            GetXvirtualProbe(hitDistForTracking, curvature, X, Xprev, N, V, roughness, *xvirtual_values) :
            GetXvirtual(dominantDirection, hitDistForTracking, curvature, X, Xprev, N, V);
        Float XvirtualLength = length(Xvirtual);
        Float2 vmbPixelUv = GetScreenUv(c.gWorldToClipPrev, Xvirtual);
        vmbPixelUv = select(materialID == c.gCameraAttachedReflectionMaterialID, smbPixelUv, vmbPixelUv);
        Float2 vmbDelta = vmbPixelUv - smbPixelUv;
        Float vmbPixelsTraveled = length(vmbDelta * c.gRectSize);
        auto vmb = temporal_bilinear(vmbPixelUv, c.gRectSizePrev);
        Float2 vmbGatherUv = (vmb.origin + 1.f) * c.gResourceSizeInvPrev;
        Float2 relaxedRoughnessWeightParams = GetRelaxedRoughnessWeightParams(roughness * roughness, c.gRoughnessFraction, .003f);
        Float4 vmbRoughness = prev_normals.gather<2u>(vmbGatherUv).wzxy();
        Float4 roughnessWeight = temporal_nonexp_sigma(vmbRoughness * vmbRoughness, relaxedRoughnessWeightParams, roughnessSigma);
        roughnessWeight = Lerp(temporal_smooth(1.f, 0.f, smbParallaxInPixelsMax), 1.f, roughnessWeight);
        Float virtualHistoryRoughnessBasedConfidence = temporal_bilinear_value(roughnessWeight, vmb);
        Float4 vmbNormalAndRoughness = NRD_FrontEnd_UnpackNormalAndRoughness(prev_normals.sample(
            temporal_stochastic_bilinear(vmbPixelUv, c.gRectSizePrev, rng) * c.gResolutionScalePrev, false));
        Float3 vmbN = RotateVector(c.gWorldPrevToWorld, vmbNormalAndRoughness.xyz());
        Float Dfactor = GetSpecularDominantFactor(NoV, roughness);
        Float virtualHistoryNormalBasedConfidence = 1.f / (1.f + .5f * Dfactor * saturate(length(N - vmbN) - temporal_normal_ulp) * vmbPixelsTraveled);
        smbNavg = select(smbFootprintQuality == 0.f, vmbN, smbNavg);
        Float4 vmbOcclusionThreshold = make_float4(disocclusionThreshold * frustumSize * Lerp(.25f, 1.f, NoV));
        vmbOcclusionThreshold *= (dot(vmbN, N) > thresholdAngle).cast<float>();
        vmbOcclusionThreshold *= (dot(vmbN, smbNavg) > thresholdAngle).cast<float>();
        vmbOcclusionThreshold *= temporal_screen_bilinear(vmb.origin, c.gRectSizePrev);
        vmbOcclusionThreshold -= 1e-6f;
        Float4 vmbViewZ = abs(temporal_gather(prev_z, vmbGatherUv) * c.gViewZScale);
        Float3 vmbVv = ReconstructViewPosition(vmbPixelUv, c.gFrustumPrev, 1.f, 0.f);
        Float3 vmbV = RotateVectorInverse(c.gWorldToViewPrev, vmbVv);
        Float NoXcurr = dot(N, Xprev - c.gCameraDelta.xyz());
        Float4 NoXprev = (N.x * vmbV.x + N.y * vmbV.y) * select(c.gOrthoMode == 0.f, vmbViewZ, make_float4(c.gOrthoMode)) + N.z * vmbV.z * vmbViewZ;
        Float4 vmbOcclusion = make_float4(abs(NoXprev - NoXcurr) <= vmbOcclusionThreshold) * make_float4(roughnessWeight >= .5f);
        Uint4 vmbInternal = make_uint4(temporal_gather(prev_internal, vmbGatherUv));
        Float3 vi00 = UnpackInternalData(vmbInternal.x), vi10 = UnpackInternalData(vmbInternal.y);
        Float3 vi01 = UnpackInternalData(vmbInternal.z), vi11 = UnpackInternalData(vmbInternal.w);
        Float4 vmbMaterialID = make_float4(vi00.z, vi10.z, vi01.z, vi11.z);
        vmbOcclusion *= make_float4(make_float4(max(materialID, c.gSpecMinMaterial)) == max(vmbMaterialID, make_float4(c.gSpecMinMaterial)));
        fbits += dot(vmbOcclusion, make_float4(16.f, 32.f, 64.f, 128.f));
        Float4 vmbOcclusionWeights = vmb.weights * vmbOcclusion;
        Float vmbSpecAccumSpeed = temporal_apply(make_float4(vi00.y, vi10.y, vi01.y, vi11.y), vmbOcclusionWeights);
        Float vmbFootprintQuality = sqrt(saturate(temporal_bilinear_value(vmbOcclusion, vmb)));
        vmbSpecAccumSpeed *= Lerp(vmbFootprintQuality, 1.f, 1.f / (1.f + vmbSpecAccumSpeed));
        Bool vmbAllowCatRom = dot(vmbOcclusion, make_float4(1.f)) > 3.5f && smbAllowCatRom;
        Float curvatureAngleTan = pixelSize * abs(curvature);
        curvatureAngleTan *= max(vmbPixelsTraveled / max(NoV, .01f), 1.f);
        curvatureAngleTan *= 2.f;
        Float curvatureAngle = atan(curvatureAngleTan);
        Float percentOfVolume = .75f / (1.f + vmbSpecAccumSpeed);
        Float lobeTanHalfAngle = GetSpecularLobeTanHalfAngle(roughnessModified, percentOfVolume);
        Float lobeHalfAngle = max(atan(lobeTanHalfAngle), temporal_normal_ulp);
        Float normalWeight = GetEncodingAwareNormalWeight(N, vmbN, lobeHalfAngle, curvatureAngle, temporal_normal_ulp);
        normalWeight = Lerp(temporal_smooth(1.f, 0.f, vmbPixelsTraveled), 1.f, normalWeight);
        virtualHistoryNormalBasedConfidence = min(virtualHistoryNormalBasedConfidence, normalWeight);
        Float virtualHistoryAmount = temporal_smooth(.05f, .95f, Dfactor) * virtualHistoryNormalBasedConfidence;
        Float hitDistForTrackingPrev = prev_hit.sample(vmbPixelUv * c.gResolutionScalePrev);
        Float3 XvirtualPrev = GetXvirtual(dominantDirection, hitDistForTrackingPrev, curvature, X, Xprev, N, V);
        Float2 vmbPixelUvPrev = GetScreenUv(c.gWorldToClipPrev, XvirtualPrev);
        vmbPixelUvPrev = select(materialID == c.gCameraAttachedReflectionMaterialID, smbPixelUv, vmbPixelUvPrev);
        Float pixelSizeAtXvirtual = PixelRadiusToWorld(c.gUnproject, c.gOrthoMode, 1.f, XvirtualLength);
        Float r = (lobeTanHalfAngle + curvatureAngle) * min(hitDistForTracking, hitDistForTrackingPrev) / pixelSizeAtXvirtual;
        Float d = length((vmbPixelUvPrev - vmbPixelUv) * c.gRectSize);
        r = max(r, .1f);
        Float virtualHistoryParallaxBasedConfidence = saturate((d - r) / -r);
        Float stepBetweenTaps = min(vmbPixelsTraveled * c.gFramerateScale, 2.f) + vmbPixelsTraveled;
        vmbDelta *= 1.f / sqrt(max(dot(vmbDelta, vmbDelta), 1e-15f));
        vmbDelta /= c.gRectSizePrev;
        relaxedRoughnessWeightParams = GetRelaxedRoughnessWeightParams(vmbNormalAndRoughness.w * vmbNormalAndRoughness.w, c.gRoughnessFraction, .003f);
        // REBLUR_VIRTUAL_MOTION_PREV_PREV_WEIGHT_ITERATION_NUM == 1.
        {
            Float2 uvPrev = vmbPixelUv + vmbDelta * stepBetweenTaps;
            Float4 nrPrev = NRD_FrontEnd_UnpackNormalAndRoughness(prev_normals.sample(
                temporal_stochastic_bilinear(uvPrev, c.gRectSizePrev, rng) * c.gResolutionScalePrev, false));
            Float2 w;
            w.x = GetEncodingAwareNormalWeight(vmbNormalAndRoughness.xyz(), nrPrev.xyz(), lobeHalfAngle, curvatureAngle * (1.f + stepBetweenTaps), temporal_normal_ulp);
            w.y = temporal_nonexp_sigma(nrPrev.w * nrPrev.w, relaxedRoughnessWeightParams, roughnessSigma);
            w = Lerp(1.f, w, saturate(stepBetweenTaps));
            w = select(temporal_in_screen(uvPrev) != 0.f, w, make_float2(1.f));
            virtualHistoryNormalBasedConfidence = min(virtualHistoryNormalBasedConfidence, w.x);
            virtualHistoryRoughnessBasedConfidence = min(virtualHistoryRoughnessBasedConfidence, w.y);
        }
        Float virtualHistoryConfidenceForSmbRelaxation = virtualHistoryNormalBasedConfidence * virtualHistoryRoughnessBasedConfidence;
        Float virtualHistoryConfidence = virtualHistoryConfidenceForSmbRelaxation * virtualHistoryParallaxBasedConfidence;
        virtualHistoryAmount *= virtualHistoryRoughnessBasedConfidence;
        if (ta_blend_probe) {
            $if(c.gFrameIndex == 4u && all(pixel == make_int2(6, 68))) {
                SignalImage diagnostic{p.outputs.t7};
                const std::array<Float, 6> values{roughnessSigma, virtualHistoryNormalBasedConfidence,
                    virtualHistoryRoughnessBasedConfidence, virtualHistoryParallaxBasedConfidence,
                    virtualHistoryConfidenceForSmbRelaxation, virtualHistoryAmount};
                for (uint i = 0u; i != values.size(); ++i) {
                    Uint raw_bits = as<uint>(values[i]);
                    Uint4 bytes = (make_uint4(raw_bits) >> make_uint4(0u, 8u, 16u, 24u)) & 255u;
                    diagnostic.write(make_int2(i, 0u), make_float4(bytes));
                }
            };
        }
        Float2 smbSampleUv = saturate(smbPixelUv);
        Float2 smbSpecSampleOrigin = floor(ocarina::fma(smbSampleUv, c.gRectSizePrev, make_float2(-.5f)));
        Float4 smbSpecHistory = temporal_sample_custom(history_spec, smbSampleUv, c.gRectSizePrev, c.gResourceSizeInvPrev, smbOcclusionWeights, smbAllowCatRom, &smbSpecSampleOrigin);
        Float smbSpecFastHistory = temporal_sample_fast(history_spec_fast, smbSpecSampleOrigin, smbOcclusionWeights);
        Float surfaceHistoryConfidence;
        {
            Float a = atan(smbParallaxInPixelsMax * pixelSize / length(X));
            Float speed = 1.f / (1.f + smbSpecAccumSpeed);
            Float h = Lerp(smbSpecHistory.w, spec.w, speed) * hitDistNormalization;
            Float tana0 = GetSpecularLobeTanHalfAngle(roughnessModified, .75f);
            tana0 *= Lerp(NoV, 1.f, roughnessModified);
            tana0 *= speed;
            tana0 /= GetHitDistFactor(h, frustumSize) + 1e-6f;
            Float a0 = max(atan(tana0), temporal_normal_ulp);
            surfaceHistoryConfidence = pow(saturate((a - a0) / -a0), 4.f);
        }
        Float2 maxResponsiveFrameNum;
        {
            Float responsiveFactor = RemapRoughnessToResponsiveFactor(c, roughness);
            Float smc = GetSpecMagicCurve(roughnessModified);
            Float2 f = make_float2(dot(N, normalize(smbNavg)), dot(N, vmbN));
            f = Lerp(smc, 1.f, responsiveFactor) * pow(saturate(f), make_float2(Lerp(32.f, 1.f, smc) * (1.f - responsiveFactor)));
            maxResponsiveFrameNum = max(c.gMaxAccumulatedFrameNum * f, make_float2(c.gHistoryFixFrameNum));
        }
        Float smbMaxFrameNum = min(c.gMaxAccumulatedFrameNum * surfaceHistoryConfidence, maxResponsiveFrameNum.x);
        Float smbBoostedMaxFrameNum = max(smbMaxFrameNum, c.gHistoryFixFrameNum * (1.f - virtualHistoryConfidenceForSmbRelaxation));
        Float smbSpecAccumSpeedBoosted = min(smbSpecAccumSpeed, smbBoostedMaxFrameNum);
        Float vmbMaxFrameNum = min(c.gMaxAccumulatedFrameNum * virtualHistoryConfidence, maxResponsiveFrameNum.y);
        smbSpecAccumSpeed = min(smbSpecAccumSpeed, smbMaxFrameNum);
        vmbSpecAccumSpeed = min(vmbSpecAccumSpeed, vmbMaxFrameNum);
        Float magic = select(vmbSpecAccumSpeed > smbSpecAccumSpeed, 8.f, .5f);
        virtualHistoryAmount *= 1.f + (vmbSpecAccumSpeed - smbSpecAccumSpeed) / (magic * max(vmbSpecAccumSpeed, smbSpecAccumSpeed) + 1.f);
        virtualHistoryAmount = saturate(virtualHistoryAmount);
        Float2 vmbSampleUv = saturate(vmbPixelUv);
        Float2 vmbSpecSampleOrigin = floor(ocarina::fma(vmbSampleUv, c.gRectSizePrev, make_float2(-.5f)));
        Float4 vmbSpecHistory = temporal_sample_custom(history_spec, vmbSampleUv, c.gRectSizePrev, c.gResourceSizeInvPrev, vmbOcclusionWeights, vmbAllowCatRom, &vmbSpecSampleOrigin);
        Float vmbSpecFastHistory = temporal_sample_fast(history_spec_fast, vmbSpecSampleOrigin, vmbOcclusionWeights);
        smbSpecHistory = ClampNegativeToZero(smbSpecHistory);
        vmbSpecHistory = ClampNegativeToZero(vmbSpecHistory);
        Float smbSpeed = 1.f / (1.f + smbSpecAccumSpeed), vmbSpeed = 1.f / (1.f + vmbSpecAccumSpeed);
        Float4 smbSpec = MixHistoryAndCurrent(c, smbSpecHistory, spec, smbSpeed, roughnessModified);
        Float4 vmbSpec = MixHistoryAndCurrent(c, vmbSpecHistory, spec, vmbSpeed, roughnessModified);
        Float4 specResult = Lerp(smbSpec, vmbSpec, virtualHistoryAmount);
        Float specAccumSpeed = Lerp(smbSpecAccumSpeedBoosted, vmbSpecAccumSpeed, virtualHistoryAmount);
        Float4 specHistory = Lerp(smbSpecHistory, vmbSpecHistory, virtualHistoryAmount);
        Float specMaxRelativeIntensity = c.gFireflySuppressorMinRelativeScale + 38.f / (specAccumSpeed + 1.f);
        Float specAntifireflyBase = specAccumSpeed * c.gMaxBlurRadius;
        Float specAntifireflyFactor = specAntifireflyBase * .1f;
        specAntifireflyFactor /= ocarina::fma(specAntifireflyBase, .1f, 1.f);
        Float specLumaResult = GetLuma(specResult);
        Float specLumaClamped = min(specLumaResult, GetLuma(specHistory) * specMaxRelativeIntensity);
        specLumaClamped = Lerp(specLumaResult, specLumaClamped, specAntifireflyFactor);
        specResult = ChangeLuma(specResult, specLumaClamped);
        out_spec.write_in_bounds(pixel, specResult);
        Float smbFastSpeed = GetNonLinearAccumSpeed(c, smbSpecAccumSpeed, c.gMaxFastAccumulatedFrameNum, surfaceHistoryConfidence, Bool{true});
        Float vmbFastSpeed = GetNonLinearAccumSpeed(c, vmbSpecAccumSpeed, c.gMaxFastAccumulatedFrameNum, virtualHistoryConfidence, Bool{true});
        Float specFastResult = Lerp(Lerp(smbSpecFastHistory, GetLuma(spec), smbFastSpeed), Lerp(vmbSpecFastHistory, GetLuma(spec), vmbFastSpeed), virtualHistoryAmount);
        Float specFastClamped = min(specFastResult, GetLuma(specHistory) * specMaxRelativeIntensity * 4.f);
        specFastResult = Lerp(specFastResult, specFastClamped, specAntifireflyFactor);
        out_spec_fast.write_in_bounds(pixel, specFastResult);
        out_data2.write_in_bounds(pixel, PackData2(fbits, curvature, virtualHistoryAmount, smbAllowCatRom));

        Float diffHistoryConfidence = smbFootprintQuality;
        $if(c.gHasHistoryConfidence != 0u) { diffHistoryConfidence *= diff_confidence.read_in_bounds(pixel); };
        diffAccumSpeed *= Lerp(diffHistoryConfidence, 1.f, 1.f / (1.f + diffAccumSpeed));
        diffAccumSpeed = min(diffAccumSpeed, c.gMaxAccumulatedFrameNum);
        Float4 diff = in_diff.read_in_bounds(pixel);
        Float2 smbDiffSampleOrigin = floor(ocarina::fma(smbSampleUv, c.gRectSizePrev, make_float2(-.5f)));
        Float4 smbDiffHistory = temporal_sample_custom(history_diff, smbSampleUv, c.gRectSizePrev, c.gResourceSizeInvPrev, smbOcclusionWeights, smbAllowCatRom, &smbDiffSampleOrigin);
        Float smbDiffFastHistory = temporal_sample_fast(history_diff_fast, smbDiffSampleOrigin, smbOcclusionWeights);
        smbDiffHistory = ClampNegativeToZero(smbDiffHistory);
        Float diffSpeed = 1.f / (1.f + diffAccumSpeed);
        Float4 diffResult = MixHistoryAndCurrent(c, smbDiffHistory, diff, diffSpeed);
        Float diffMaxRelativeIntensity = c.gFireflySuppressorMinRelativeScale + 38.f / (diffAccumSpeed + 1.f);
        Float diffAntifireflyBase = diffAccumSpeed * c.gMaxBlurRadius;
        Float diffAntifireflyFactor = diffAntifireflyBase * .1f;
        diffAntifireflyFactor /= ocarina::fma(diffAntifireflyBase, .1f, 1.f);
        Float diffLumaResult = GetLuma(diffResult);
        Float diffLumaClamped = min(diffLumaResult, GetLuma(smbDiffHistory) * diffMaxRelativeIntensity);
        diffLumaClamped = Lerp(diffLumaResult, diffLumaClamped, diffAntifireflyFactor);
        diffResult = ChangeLuma(diffResult, diffLumaClamped);
        out_diff.write_in_bounds(pixel, diffResult);
        Float diffFastSpeed = 1.f / (1.f + min(diffAccumSpeed, c.gMaxFastAccumulatedFrameNum));
        Float diffFastResult = Lerp(smbDiffFastHistory, GetLuma(diff), diffFastSpeed);
        Float diffFastClamped = min(diffFastResult, GetLuma(smbDiffHistory) * diffMaxRelativeIntensity * 4.f);
        diffFastResult = Lerp(diffFastResult, diffFastClamped, diffAntifireflyFactor);
        out_diff_fast.write_in_bounds(pixel, diffFastResult);
        out_data1.write_in_bounds(pixel, PackData1(diffAccumSpeed, specAccumSpeed));
        if (ta_probe) {
            $if(all(pixel == make_int2(probe_x, probe_y))) {
                SignalImage diagnostic{p.outputs.t7};
                if (xvirtual_probe) {
                    (*xvirtual_values)[61] = pixelUv.x;
                    (*xvirtual_values)[62] = pixelUv.y;
                    for (uint index = 0u; index != xvirtual_values->size(); ++index) {
                        Uint bits = as<uint>((*xvirtual_values)[index]);
                        Uint4 bytes = (make_uint4(bits) >> make_uint4(0u, 8u, 16u, 24u)) & 255u;
                        diagnostic.write(make_int2(index, 0u), make_float4(bytes));
                    }
                } else {
                const auto &curve = *curvature_probe;
                Uint packedBits; Bool packedCatRom;
                Float packedCurvature = UnpackData2(PackData2(fbits, curvature, virtualHistoryAmount, smbAllowCatRom), packedBits, packedCatRom).y;
                Float4 planeDistance = abs(NoXprev - NoXcurr);
                const std::array<Float, 63> values{
                    pixelUv.x, pixelUv.y, Xv.x, Xv.y, Xv.z, X.x, X.y, X.z,
                    Xprev.x, Xprev.y, Xprev.z, N.x, N.y, N.z, Navg.x, Navg.y,
                    Navg.z, V.x, V.y, V.z, roughness, hitDistForTracking,
                    curve[0], curve[1], curve[2], curve[3], curve[4], curve[5], curve[6], curve[7],
                    curvature, packedCurvature, Xvirtual.x, Xvirtual.y, Xvirtual.z,
                    vmbPixelUv.x, vmbPixelUv.y, vmb.origin.x, vmb.origin.y, vmb.fraction.x, vmb.fraction.y,
                    roughnessWeight.x, roughnessWeight.y, roughnessWeight.z, roughnessWeight.w,
                    planeDistance.x, planeDistance.y, planeDistance.z, planeDistance.w,
                    vmbOcclusionThreshold.x, vmbOcclusionThreshold.y, vmbOcclusionThreshold.z, vmbOcclusionThreshold.w,
                    vmbOcclusion.x, vmbOcclusion.y, vmbOcclusion.z, vmbOcclusion.w,
                    vmbViewZ.x, vmbViewZ.y, vmbViewZ.z, vmbViewZ.w, smbPixelUv.x, smbPixelUv.y
                };
                for (uint i = 0u; i != values.size(); ++i) {
                    Uint bits = as<uint>(values[i]);
                    Uint4 bytes = (make_uint4(bits) >> make_uint4(0u, 8u, 16u, 24u)) & 255u;
                    diagnostic.write(make_int2(i, 0u), make_float4(bytes));
                }
                }
            };
        }
    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-TemporalAccumulation");
}

PassShader compile_stabilization(Device &device) {
    const char *probe = std::getenv("REBLUR_EDSL_EDGE_PROBE");
    const bool edge_probe = probe && std::string_view(probe) == "1";
    const bool xvirtual_chain_probe = probe && std::string_view(probe) == "4";
    const bool clip_chain_probe = probe && std::string_view(probe) == "5";
    const bool full_replay_chain_probe = probe && std::string_view(probe) == "6";
    const bool replay_chain_probe = probe && (std::string_view(probe) == "3" || xvirtual_chain_probe || clip_chain_probe || full_replay_chain_probe);
    const bool chain_probe = probe && (std::string_view(probe) == "2" || replay_chain_probe);
    Kernel kernel = [edge_probe, chain_probe, replay_chain_probe, xvirtual_chain_probe, clip_chain_probe, full_replay_chain_probe](Var<Dispatch> p) {
        configure_block(8u, 16u);
        auto &c = p.constants;
        Image<float, Format::R8Unorm> tiles{p.inputs.t0};
        NormalImage normals{p.inputs.t1};
        SignalImage base_color{p.inputs.t2};
        Image<float, Format::R32Float> view_z{p.inputs.t3};
        Image<float2, Format::RG8Unorm> data1_image{p.inputs.t4};
        Image<uint, Format::R32Uint> data2_image{p.inputs.t5};
        SignalImage in_diff{p.inputs.t6}, in_spec{p.inputs.t7};
        ScalarImage diff_stabilized{p.inputs.t8}, spec_stabilized{p.inputs.t9}, in_hit{p.inputs.t10};
        SignalImage motion{p.outputs.t0};
        Image<uint, Format::R16Uint> internal{p.outputs.t1};
        SignalImage out_diff{p.outputs.t2}, out_spec{p.outputs.t3};
        ScalarImage out_diff_stabilized{p.outputs.t4}, out_spec_stabilized{p.outputs.t5};
        Uint2 local = thread_idx().xy();
        Uint2 groups = (make_uint2(c.gRectSize) + make_uint2(7u, 15u)) / make_uint2(8u, 16u);
        Int2 pixel = make_int2((groups - 1u - block_idx().xy()) * make_uint2(8u, 16u) + local);
        Int2 origin = pixel - make_int2(local);
        SharedArray<float, 180> cached_diff_luma, cached_spec_luma;
        Float is_sky = tiles.read(pixel >> 4);
        $if(is_sky == 0.f) {
            for (uint iteration = 0u; iteration < 2u; ++iteration) {
                Uint entry = local.y * 8u + local.x + 128u * iteration;
                $if(entry < 180u) {
                    Int2 q = clamp(origin + make_int2(entry % 10u, entry / 10u) - 1, make_int2(0), c.gRectSizeMinusOne);
                    cached_diff_luma.write(entry, GetLuma(in_diff.read_in_bounds(q)));
                    cached_spec_luma.write(entry, GetLuma(in_spec.read_in_bounds(q)));
                };
            }
        };
        call("GroupMemoryBarrierWithGroupSync");
        $if(is_sky != 0.f || any(pixel > c.gRectSizeMinusOne)) { $return(); };
        Float viewZ = UnpackViewZ(c, view_z.read_in_bounds(pixel));
        $if(viewZ > c.gDenoisingRange) { $return(); };
        Float2 pixelUv = (make_float2(pixel) + .5f) * c.gRectSizeInv;
        Float3 Xv = ReconstructViewPosition(pixelUv, c.gFrustum, viewZ, c.gOrthoMode);
        Float3 X = RotateVector(c.gViewToWorld, Xv);
        Float4 inMv = motion.read_in_bounds(pixel);
        Float3 mv = inMv.xyz() * c.gMvScale.xyz();
        Float3 Xprev = X;
        Float2 smbPixelUv = ocarina::fma(make_float2(pixel) + .5f, c.gRectSizeInv, mv.xy());
        $if(c.gMvScale.w == 0.f) {
            $if(c.gMvScale.z == 0.f) { mv.z = AffineTransform(c.gWorldToViewPrev, X).z - viewZ; };
            Float3 xvprev = ReconstructViewPosition(smbPixelUv, c.gFrustumPrev, viewZ + mv.z, c.gOrthoMode);
            Xprev = RotateVectorInverse(c.gWorldToViewPrev, xvprev) + c.gCameraDelta.xyz();
        } $else {
            Xprev += mv; smbPixelUv = GetScreenUv(c.gWorldToClipPrev, Xprev);
        };
        Float materialID;
        Float4 nr = NRD_FrontEnd_UnpackNormalAndRoughness(normals.read_in_bounds(pixel), materialID);
        Float3 N = nr.xyz(); Float roughness = nr.w;
        Uint bits; Bool smbAllowCatRom;
        Float2 data1 = UnpackData1(data1_image.read_in_bounds(pixel));
        Float2 data2 = UnpackData2(data2_image.read_in_bounds(pixel), bits, smbAllowCatRom);
        auto smb = temporal_bilinear(smbPixelUv, c.gRectSizePrev);
        Float4 smbOcclusion = make_float4((bits & make_uint4(1u, 2u, 4u, 8u)) != 0u);
        Float4 smbWeights = smb.weights * smbOcclusion;
        Float smbQuality = sqrt(saturate(temporal_bilinear_value(smbOcclusion, smb)));
        Uint center = (local.y + 1u) * 10u + local.x + 1u;
        Float diffLuma = cached_diff_luma.read(center);
        Float diffM1 = diffLuma, diffM2 = diffLuma * diffLuma;
        Float diffMin = 1e6f, diffMax = -1e6f;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0) { continue; }
                Float d = cached_diff_luma.read((center.cast<int>() + y * 10 + x).cast<uint>());
                diffM1 += d; diffM2 = ocarina::fma(d, d, diffM2);
                diffMin = min(diffMin, d); diffMax = max(diffMax, d);
            }
        }
        Float diffMomentSum = diffM1;
        diffM1 = call<float>("oc_nrd_round_mul", diffM1, 1.f / 9.f);
        Float diffSigma = sqrt(abs(ocarina::fma(diffM2, 1.f / 9.f, -(diffM1 * diffM1))));
        $if(c.gMaxBlurRadius != 0.f) { diffLuma = clamp(diffLuma, diffMin, diffMax); };
        Float2 smbSampleUv = saturate(smbPixelUv);
        Float smbDiffHistory = temporal_sample_custom(diff_stabilized, smbSampleUv, c.gRectSizePrev, c.gResourceSizeInvPrev, smbWeights, smbAllowCatRom);
        smbDiffHistory = max(smbDiffHistory, 0.f);
        Float diffAntilag = ComputeAntilag(c, smbDiffHistory, diffMomentSum, diffSigma, smbQuality * data1.x);
        Float2 diffTemporal = GetTemporalAccumulationParams(c, smbQuality, data1.x);
        Float diffHistoryWeight = diffTemporal.x * diffAntilag;
        diffHistoryWeight *= (pixelUv.x >= c.gSplitScreen).cast<float>();
        diffHistoryWeight *= (smbPixelUv.x >= c.gSplitScreenPrev).cast<float>();
        Float diffSigmaScaled = diffSigma * diffTemporal.y;
        smbDiffHistory = clamp(smbDiffHistory, ocarina::fma(diffMomentSum, 1.f / 9.f, -diffSigmaScaled), ocarina::fma(diffMomentSum, 1.f / 9.f, diffSigmaScaled));
        Float diffLumaStabilized = Lerp(diffLuma, smbDiffHistory, min(diffHistoryWeight, c.gStabilizationStrength));
        out_diff.write_in_bounds(pixel, ChangeLuma(in_diff.read_in_bounds(pixel), diffLumaStabilized));
        out_diff_stabilized.write_in_bounds(pixel, diffLumaStabilized);
        data1.x += 1.f;
        data1.x = Lerp(min(data1.x, c.gHistoryFixFrameNum), data1.x, diffAntilag);

        Float specLuma = cached_spec_luma.read(center);
        Float specM1 = specLuma, specM2 = specLuma * specLuma;
        Float specMin = 1e6f, specMax = -1e6f;
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0) { continue; }
                Float s = cached_spec_luma.read((center.cast<int>() + y * 10 + x).cast<uint>());
                specM1 += s; specM2 = ocarina::fma(s, s, specM2);
                specMin = min(specMin, s); specMax = max(specMax, s);
            }
        }
        Float specMomentSum = specM1;
        specM1 = call<float>("oc_nrd_round_mul", specM1, 1.f / 9.f);
        Float specSigma = sqrt(abs(ocarina::fma(specM2, 1.f / 9.f, -(specM1 * specM1))));
        $if(c.gMaxBlurRadius != 0.f) { specLuma = clamp(specLuma, specMin, specMax); };

        Float virtualHistoryAmount = data2.x, curvature = data2.y;
        Float4 spec = in_spec.read_in_bounds(pixel);
        Float hitDistForTracking = spec.w * _REBLUR_GetHitDistanceNormalization(viewZ, c.gHitDistParams, roughness);
        $if(c.gSpecPrepassBlurRadius != 0.f) { hitDistForTracking = min(hitDistForTracking, in_hit.read_in_bounds(pixel)); };
        Float3 V = GetViewVector(c, X);
        Float NoV = abs(dot(N, V));
        Float3 Xvirtual = GetXvirtual(c, hitDistForTracking, curvature, X, Xprev, N, V, roughness);
        Float2 vmbPixelUv = GetScreenUv(c.gWorldToClipPrev, Xvirtual);
        vmbPixelUv = select(materialID == c.gCameraAttachedReflectionMaterialID, pixelUv, vmbPixelUv);
        $if(c.gSpecProbabilityThresholdsForMvModification.x < 1.f) {
            Float4 bc = base_color.read_in_bounds(pixel);
            Float3 albedo = bc.xyz() * saturate(1.f - bc.w);
            Float3 rf0 = Lerp(make_float3(.04f), bc.xyz(), bc.w);
            Float3 fenv = temporal_environment(rf0, NoV, roughness);
            Float lumSpec = dot(fenv, make_float3(.2126f, .7152f, .0722f));
            Float lumDiff = dot(albedo * (1.f - fenv), make_float3(.2126f, .7152f, .0722f));
            Float specProb = lumSpec / (lumDiff + lumSpec + 1e-6f);
            Float f = temporal_smooth(c.gSpecProbabilityThresholdsForMvModification.x, c.gSpecProbabilityThresholdsForMvModification.y, specProb);
            f *= 1.f - GetSpecMagicCurve(roughness);
            f *= 1.f - sqrt(saturate(abs(curvature)));
            $if(f != 0.f) {
                Float3 specMv = Xvirtual - X;
                $if(c.gMvScale.w == 0.f) {
                    specMv.xy() = vmbPixelUv - pixelUv;
                    specMv.z = AffineTransform(c.gWorldToViewPrev, Xvirtual).z - viewZ;
                };
                mv.xy() = specMv.xy() / c.gMvScale.xy();
                mv.z = select(c.gMvScale.z == 0.f, inMv.z, specMv.z / c.gMvScale.z);
                inMv.xyz() = Lerp(inMv.xyz(), mv, f);
                motion.write_in_bounds(pixel, inMv);
            };
        };
        Float smbSpecHistory = temporal_sample_custom(spec_stabilized, smbSampleUv, c.gRectSizePrev, c.gResourceSizeInvPrev, smbWeights, smbAllowCatRom);
        auto vmb = temporal_bilinear(vmbPixelUv, c.gRectSizePrev);
        Float4 vmbOcclusion = make_float4((bits & make_uint4(16u, 32u, 64u, 128u)) != 0u);
        Float4 vmbWeights = vmb.weights * vmbOcclusion;
        Bool vmbAllowCatRom = dot(vmbOcclusion, make_float4(1.f)) > 3.5f && smbAllowCatRom;
        Float vmbQuality = sqrt(saturate(temporal_bilinear_value(vmbOcclusion, vmb)));
        Float vmbSpecHistory = temporal_sample_custom(spec_stabilized, saturate(vmbPixelUv), c.gRectSizePrev,
            c.gResourceSizeInvPrev, vmbWeights, vmbAllowCatRom);
        Float vmbSpecHistoryRaw = vmbSpecHistory;
        if (chain_probe) {
            $if(c.gFrameIndex == (full_replay_chain_probe ? 22u : replay_chain_probe ? 1u : 6u) &&
                all(pixel == (full_replay_chain_probe ? make_int2(1050, 636) : replay_chain_probe ? make_int2(44, 51) : make_int2(5, 2)))) {
                SignalImage diagnostic{p.outputs.t6};
                const auto values = [&] {
                    if (xvirtual_chain_probe) {
                        Float4 clip = ProjectiveTransform(c.gWorldToClipPrev, Xvirtual);
                        return std::array<Float, 4>{Xvirtual.x, Xvirtual.y, Xvirtual.z, clip.w};
                    }
                    if (clip_chain_probe) {
                        Float4 clip = ProjectiveTransform(c.gWorldToClipPrev, Xvirtual);
                        return std::array<Float, 4>{clip.x, clip.y, clip.w, vmbPixelUv.x};
                    }
                    return std::array<Float, 4>{vmbPixelUv.x, vmbPixelUv.y, vmbQuality, vmbSpecHistoryRaw};
                }();
                for (uint i = 0u; i != values.size(); ++i) {
                    Uint raw_bits = as<uint>(values[i]);
                    Uint4 bytes = (make_uint4(raw_bits) >> make_uint4(0u, 8u, 16u, 24u)) & 255u;
                    diagnostic.write(make_int2(i, 0u), make_float4(bytes));
                }
            };
        }
        smbSpecHistory = max(smbSpecHistory, 0.f); vmbSpecHistory = max(vmbSpecHistory, 0.f);
        Float specLumaHistory = Lerp(smbSpecHistory, vmbSpecHistory, virtualHistoryAmount);
        Float footprintQuality = Lerp(smbQuality, vmbQuality, virtualHistoryAmount);
        Float specAntilag = ComputeAntilag(c, specLumaHistory, specMomentSum, specSigma, footprintQuality * data1.y);
        Float2 specTemporal = GetTemporalAccumulationParams(c, footprintQuality, data1.y);
        Float specHistoryWeight = specTemporal.x * specAntilag;
        specHistoryWeight *= (pixelUv.x >= c.gSplitScreen).cast<float>();
        specHistoryWeight *= select(virtualHistoryAmount != 1.f, (smbPixelUv.x >= c.gSplitScreenPrev).cast<float>(), 1.f);
        specHistoryWeight *= select(virtualHistoryAmount != 0.f, (vmbPixelUv.x >= c.gSplitScreenPrev).cast<float>(), 1.f);
        Float responsiveFactor = RemapRoughnessToResponsiveFactor(c, roughness);
        Float smc = GetSpecMagicCurve(roughness);
        Float acceleration = Lerp(smc, 1.f, .5f + responsiveFactor * .5f);
        specHistoryWeight *= select(materialID == c.gStrandMaterialID, .5f, acceleration);
        Float specSigmaScaled = specSigma * specTemporal.y;
        specLumaHistory = clamp(specLumaHistory, ocarina::fma(specMomentSum, 1.f / 9.f, -specSigmaScaled), ocarina::fma(specMomentSum, 1.f / 9.f, specSigmaScaled));
        Float specLumaStabilized = Lerp(specLuma, specLumaHistory, min(specHistoryWeight, c.gStabilizationStrength));
        spec = ChangeLuma(spec, specLumaStabilized);
        out_spec.write_in_bounds(pixel, spec);
        out_spec_stabilized.write_in_bounds(pixel, specLumaStabilized);
        data1.y += 1.f;
        data1.y = Lerp(min(data1.y, c.gHistoryFixFrameNum), data1.y, specAntilag);
        internal.write_in_bounds(pixel, PackInternalData(data1.x, data1.y, materialID));
        if (edge_probe) {
            $if(all(pixel == make_int2(0))) {
                // Diagnostic only: Pack replaces motion before the next frame,
                // and neither stabilization history nor Unpack consumes this write.
                Float2 edge = ocarina::fma(vmbPixelUv, c.gRectSizePrev, make_float2(-.5f));
                motion.write(pixel, make_float4(edge * 1e6f, vmbQuality, vmbSpecHistory));
            };
        }

    };
    return device.compile(kernel, "NRD-EDSL-REBLUR-TemporalStabilization");
}
}
