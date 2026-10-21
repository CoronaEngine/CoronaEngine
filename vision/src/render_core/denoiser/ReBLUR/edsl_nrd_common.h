#pragma once
#include "edsl_nrd_params.h"
#include <array>
namespace vision::reblur::edsl {
inline constexpr float NRD_EPS = 1e-6f;
inline constexpr float NRD_NORMAL_ENCODING_ERROR = .75f / 255.f;
inline constexpr float REBLUR_MAX_ACCUM_FRAME_NUM = 63.f;

// Ocarina's native lerp places the interpolation factor first.
template<typename A, typename B, typename T>
inline auto Lerp(const A &a, const B &b, const T &t) { return ocarina::lerp(t, a, b); }
template<typename T> inline auto PositiveRcp(const T &x) { return 1.f / max(x, 1e-15f); }
template<typename T> inline auto SmoothStep01(T x) { x = saturate(x); return x * x * (3.f - 2.f * x); }
template<typename A, typename B, typename T>
inline auto LinearStep(const A &a, const B &b, const T &x) { return saturate((x - a) / (b - a)); }
template<typename A, typename B, typename T>
inline auto SmoothStep(const A &a, const B &b, const T &x) { return SmoothStep01(LinearStep(a, b, x)); }
template<typename T> inline auto AcosApprox(const T &x) { return 1.4142135623730951f * sqrt(saturate(1.f - x)); }
template<typename T> inline auto AtanApprox(const T &x) { return .7853981633974483f * x - (abs(x) * x - x) * (.2447f + .0663f * abs(x)); }
template<typename T> inline auto GetStdDev(const T &m1, const T &m2) { return sqrt(abs(m2 - m1 * m1)); }
template<typename T> inline auto Denanify(Float weight, const T &value) { return select(weight == 0.f, T{0.f}, value); }
// Match the contractions in the original DXC shaders at quantized boundaries.
inline Float ExpApprox(Float x) { return rcp(ocarina::fma(x, x, -x) + 1.f); }
inline Float ComputeExponentialWeight(Float x, Float px, Float py) { return ExpApprox(-3.f * abs(ocarina::fma(x, px, py))); }
inline Float ComputeNonExponentialWeight(Float x, Float px, Float py) { return SmoothStep(1.f, 0.f, abs(ocarina::fma(x, px, py))); }
inline Float ComputeNonExponentialWeightWithSigma(Float x, Float px, Float py, Float sigma) { return SmoothStep(1.f, 0.f, abs(ocarina::fma(x, px, py)) - sigma * px); }
inline Float ComputeWeight(Float x, Float px, Float py) { return ComputeNonExponentialWeight(x, px, py); }
inline Float GetGaussianWeight(Float r) { return exp(-.66f * r * r); }
inline Float CompareMaterials(Float m0, Float m, Float minm) { return (max(m0, minm) == max(m, minm)).cast<float>(); }
inline Float UnpackViewZ(const Var<SharedConstants> &c, Float z) { return abs(z * c.gViewZScale); }
inline Float PixelRadiusToWorld(Float unproject, Float orthoMode, Float radius, Float z) { return radius * unproject * Lerp(z, 1.f, abs(orthoMode)); }
inline Float GetFrustumSize(Float scale, Float orthoMode, Float z) { return scale * Lerp(z, 1.f, abs(orthoMode)); }
inline Float GetHitDistFactor(Float distance, Float frustumSize) { return saturate(distance / frustumSize); }
inline Float IsInScreenNearest(Float2 uv) { return (all(uv > 0.f) && all(uv < 1.f)).cast<float>(); }
inline Float4 IsInScreenBilinear(Float2 origin, Float2 size) {
    Float4 p = make_float4(origin, origin) + make_float4(0.f, 0.f, 1.f, 1.f);
    Float4 r = make_float4(p >= 0.f) * make_float4(p < make_float4(size, size));
    return make_float4(r.x, r.z, r.x, r.z) * make_float4(r.y, r.y, r.w, r.w);
}
inline Float3 _NRD_SafeNormalize(Float3 v) { return v * rsqrt(dot(v, v) + 1e-9f); }
inline Float2 _NRD_EncodeUnitVector(Float3 v, bool signedEncoding = false) {
    v /= dot(abs(v), make_float3(1.f));
    Float2 signs = select(v.xy() >= 0.f, Float2{make_float2(1.f)}, Float2{make_float2(-1.f)});
    Float2 oct = (1.f - abs(v.yx())) * signs;
    Float2 p = select(v.z >= 0.f, Float2{v.xy()}, oct);
    return signedEncoding ? p : p * .5f + .5f;
}
inline Float3 _NRD_DecodeUnitVector(Float2 p, bool signedEncoding = false, bool normalizeResult = false) {
    if (!signedEncoding) p = p * 2.f - 1.f;
    Float3 n = make_float3(p, 1.f - abs(p.x) - abs(p.y));
    Float t = saturate(-n.z);
    Float2 signs = select(n.xy() >= 0.f, Float2{make_float2(1.f)}, Float2{make_float2(-1.f)});
    n = make_float3(n.xy() - t * signs, n.z);
    return normalizeResult ? normalize(n) : n;
}
inline Float4 NRD_FrontEnd_UnpackNormalAndRoughness(Float4 p, Float &material) {
    material = p.w * 3.f;
    return make_float4(_NRD_SafeNormalize(_NRD_DecodeUnitVector(p.xy())), p.z);
}
inline Float4 NRD_FrontEnd_UnpackNormalAndRoughness(Float4 p) { Float material; return NRD_FrontEnd_UnpackNormalAndRoughness(p, material); }
inline Float4 NRD_FrontEnd_PackNormalAndRoughness(Float3 n, Float roughness, Float material) { return make_float4(_NRD_EncodeUnitVector(n), roughness, saturate(material / 3.f)); }
inline Float _NRD_Luminance(Float3 color) { return dot(color, make_float3(.2126f, .7152f, .0722f)); }
inline Float3 _NRD_LinearToYCoCg(Float3 color) { return make_float3(dot(color, make_float3(.25f, .5f, .25f)), dot(color, make_float3(.5f, 0.f, -.5f)), dot(color, make_float3(-.25f, .5f, -.25f))); }
inline Float3 _NRD_YCoCgToLinear(Float3 color) { Float t = color.x - color.z; return max(make_float3(t + color.y, color.x + color.z, t - color.y), make_float3(0.f)); }
inline Float _REBLUR_GetHitDistanceNormalization(Float z, Float4 params, Float roughness) { return ocarina::fma(abs(z), params.y, params.x) * Lerp(1.f, params.z, saturate(exp2(params.w * roughness * roughness))); }
inline Float HitDistanceNormalization(Float z, Float4 params, Float roughness) { return _REBLUR_GetHitDistanceNormalization(z, params, roughness); }
inline Float4 PackRadiance(Float3 rgb, Float normalizedHit) {
    rgb = select(any(ocarina::isnan(rgb)) || any(ocarina::isinf(rgb)), Float3{make_float3(0.f)}, clamp(rgb, make_float3(0.f), make_float3(65504.f)));
    normalizedHit = select(ocarina::isnan(normalizedHit) || ocarina::isinf(normalizedHit), 0.f, saturate(normalizedHit));
    return make_float4(_NRD_LinearToYCoCg(rgb), normalizedHit);
}
inline Float3 UnpackRadiance(Float4 encoded) { return _NRD_YCoCgToLinear(encoded.xyz()); }
inline Float4 PackNormalRoughness(Float3 normal, Float roughness, Float material) { return NRD_FrontEnd_PackNormalAndRoughness(normal, roughness, material); }

inline Float3 RotateVector(Float4x4 m, Float3 v) { return make_float3x3(m) * v; }
inline Float3 RotateVector(Float3x3 m, Float3 v) { return m * v; }
inline Float3 RotateVectorInverse(Float4x4 m, Float3 v) { return transpose(make_float3x3(m)) * v; }
inline Float3 RotateVectorInverse(Float3x3 m, Float3 v) { return transpose(m) * v; }
inline Float2 RotateVector(Float4 r, Float2 v) { return v.x * r.xz() + v.y * r.yw(); }
inline Float3 AffineTransform(Float4x4 m, Float3 p) { return (m * make_float4(p, 1.f)).xyz(); }
inline Float4 ProjectiveTransform(Float4x4 m, Float3 p) { return m * make_float4(p, 1.f); }
inline Float3 ReconstructViewPosition(Float2 uv, Float4 frustum, Float z = 1.f, Float ortho = 0.f) { return make_float3(ocarina::fma(uv, frustum.zw(), frustum.xy()) * ocarina::fma(z, 1.f - abs(ortho), ortho), z); }
inline Float2 GetScreenUv(Float4x4 m, Float3 x, bool killBackprojection = false) {
    Float4 clip = ProjectiveTransform(m, x);
    Float2 uv = ocarina::fma(clip.xy() / clip.w, make_float2(.5f, -.5f), make_float2(.5f));
    if (killBackprojection) uv = select(clip.w < 0.f, make_float2(99999.f), uv);
    return uv;
}
inline Float3 GetViewVector(const Var<SharedConstants> &c, Float3 x, bool viewSpace = false) { Float3 ortho = viewSpace ? Float3{make_float3(0.f, 0.f, -1.f)} : Float3{c.gViewVectorWorld.xyz()}; return select(c.gOrthoMode == 0.f, normalize(-x), ortho); }
inline Float3 GetViewVectorPrev(const Var<SharedConstants> &c, Float3 x, Float3 delta) { return select(c.gOrthoMode == 0.f, normalize(delta - x), c.gViewVectorWorldPrev.xyz()); }
inline Float GetSpecMagicCurve(Float roughness, Float power = .25f) { return (1.f - exp2(-200.f * roughness * roughness)) * pow(saturate(roughness), power); }
inline Float GetSpecularLobeTanHalfAngle(Float roughness, Float volume = .75f) { volume = saturate(volume); return saturate(roughness) * sqrt(volume / (1.f - volume + 1e-6f)); }
inline Float GetSpecularDominantFactor(Float NoV, Float roughness) {
    roughness = saturate(roughness);
    Float logarithm = log(ocarina::fma(-39.0029f, roughness, 39.4115f));
    Float a = .298475f * logarithm;
    Float complement = ocarina::fma(-.298475f, logarithm, 1.f);
    return saturate(ocarina::fma(pow(saturate(1.f - NoV), 10.8649f), complement, a));
}
inline Float3 GetSpecularDominantDirectionWithFactor(Float3 n, Float3 v, Float f) { return normalize(Lerp(n, call<float3>("reflect", -v, n), f)); }
inline Float4 GetSpecularDominantDirection(Float3 n, Float3 v, Float roughness) { Float f = GetSpecularDominantFactor(abs(dot(n, v)), roughness); return make_float4(GetSpecularDominantDirectionWithFactor(n, v, f), f); }
// Reuse the identical dominant direction across TA hit-distance reprojections.
inline Float3 GetXvirtual(const Float4 &d, Float hit, Float curvature, Float3 x, Float3 prev, Float3 n, Float3 v) {
    Float sz = select(n.z >= 0.f, 1.f, -1.f);
    Float a = 1.f / (sz + n.z), ya = n.y * a, b = n.x * ya, nxsz = n.x * sz;
    Float3 t = make_float3(ocarina::fma(nxsz * n.x, a, -1.f), sz * b, nxsz);
    Float3 binormal = make_float3(b, ocarina::fma(n.y, ya, -sz), n.y);
    Float3 ray = d.xyz() * hit;
    // Match the original mul(basis, ray) intrinsic. The generic DSL matrix
    // operator introduces a transpose and changes its floating-point lowering.
    Float3 object = call<float3>("mul", make_float3x3(t, binormal, n), ray);
    object.z = -object.z;
    Float mag = 1.f / ocarina::fma(2.f * curvature, object.z, -1.f);
    Float f = length(x) * (1.f - abs(dot(n, v)));
    mag *= 1.f / ocarina::fma(f, max(curvature, 0.f), 1.f);
    Float3 iw = v * length(object * mag);
    Float closeness = saturate(length(iw) / (hit + NRD_EPS));
    return Lerp(prev, x, closeness * d.w) - iw * d.w;
}
inline Float3 GetXvirtual(const Var<SharedConstants> &, Float hit, Float curvature, Float3 x, Float3 prev, Float3 n, Float3 v, Float roughness) {
    Float4 d = GetSpecularDominantDirection(n, v, roughness);
    return GetXvirtual(d, hit, curvature, x, prev, n, v);
}
// Explicit diagnostic variant. The ordinary helper above and its generated AST
// stay unchanged when REBLUR_EDSL_XVIRTUAL_PROBE is unset. Field names live in
// the independent getxvirtual-chain-probe artifact, alongside the native copy.
inline Float3 GetXvirtualProbe(Float hit, Float curvature, Float3 x, Float3 prev,
                              Float3 n, Float3 v, Float roughness,
                              std::array<Float, 63> &values) {
    Float NoV = abs(dot(n, v));
    Float factor = GetSpecularDominantFactor(NoV, roughness);
    Float3 reflection = call<float3>("reflect", -v, n);
    Float3 mixed = Lerp(n, reflection, factor);
    Float3 direction = normalize(mixed);
    Float4 d = make_float4(direction, factor);
    Float sz = select(n.z >= 0.f, 1.f, -1.f);
    Float a = 1.f / (sz + n.z), ya = n.y * a, b = n.x * ya, nxsz = n.x * sz;
    Float3 t = make_float3(ocarina::fma(nxsz * n.x, a, -1.f), sz * b, nxsz);
    Float3 binormal = make_float3(b, ocarina::fma(n.y, ya, -sz), n.y);
    Float3 ray = d.xyz() * hit;
    Float3 object = call<float3>("mul", make_float3x3(t, binormal, n), ray);
    object.z = -object.z;
    Float initialMag = 1.f / ocarina::fma(2.f * curvature, object.z, -1.f);
    Float f = length(x) * (1.f - abs(dot(n, v)));
    Float mag = initialMag;
    mag *= 1.f / ocarina::fma(f, max(curvature, 0.f), 1.f);
    Float3 image = object * mag;
    Float imageLength = length(image);
    Float3 iw = v * imageLength;
    Float iwLength = length(iw);
    Float closeness = saturate(iwLength / (hit + NRD_EPS));
    Float3 origin = Lerp(prev, x, closeness * d.w);
    Float3 result = origin - iw * d.w;
    const std::array<Float, 61> captured{
        n.x, n.y, n.z, v.x, v.y, v.z, roughness, hit, curvature, NoV, factor,
        reflection.x, reflection.y, reflection.z, mixed.x, mixed.y, mixed.z,
        direction.x, direction.y, direction.z, sz, a, ya, b, nxsz,
        t.x, t.y, t.z, binormal.x, binormal.y, binormal.z,
        ray.x, ray.y, ray.z, object.x, object.y, object.z, initialMag, f, mag,
        image.x, image.y, image.z, imageLength, iw.x, iw.y, iw.z, iwLength, closeness,
        origin.x, origin.y, origin.z, result.x, result.y, result.z,
        x.x, x.y, x.z, prev.x, prev.y, prev.z};
    for (uint index = 0u; index != captured.size(); ++index) values[index] = captured[index];
    return result;
}
inline Float GetNormalWeightParam(Float accum, Float fraction, Float roughness = 1.f) { Float volume = .75f * Lerp(fraction, 1.f, accum); return 1.f / max(atan(GetSpecularLobeTanHalfAngle(roughness, volume)), NRD_NORMAL_ENCODING_ERROR); }
inline Float2 GetGeometryWeightParams(Float sensitivity, Float frustum, Float3 x, Float3 n, Float) { Float a = 1.f / (sensitivity * frustum); return make_float2(a, -dot(n, x) * a); }
inline Float2 GetHitDistanceWeightParams(Float hit, Float accum, Float roughness = 1.f) { Float norm = Lerp(.0005f, 1.f, min(accum, GetSpecMagicCurve(roughness))); Float a = 1.f / norm; return make_float2(a, -hit * a); }
inline Float2 GetRoughnessWeightParams(Float roughness, Float fraction, Float sensitivity = .01f) { Float a = 1.f / Lerp(sensitivity, 1.f, saturate(roughness * fraction)); return make_float2(a, -roughness * a); }
inline Float2 GetRelaxedRoughnessWeightParams(Float m, Float fraction = 1.f, Float sensitivity = .01f) { Float a = 1.f / Lerp(sensitivity, 1.f, Lerp(m * m, m, fraction)); return make_float2(a, -m * a); }
inline Float GetEncodingAwareNormalWeight(Float3 current, Float3 previous, Float maxAngle, Float curvatureAngle, Float threshold, bool remap = false) { Float w = SmoothStep01(1.f - (AcosApprox(dot(current, previous)) - curvatureAngle - threshold) / maxAngle); return remap ? SmoothStep(.05f, .95f, w) : w; }
inline Float GetDisocclusionThreshold(Float threshold, Float frustum, Float NoV) { return frustum * saturate(threshold / max(.01f, NoV)); }
inline Float GetDisocclusionWeight(Float z, Float z0, Float threshold) { return (threshold >= abs(z - z0)).cast<float>(); }
inline Float ComputeParallaxInPixels(Float3 x, Float2 zeroUv, Float4x4 matrix, Float2 size) { return length((GetScreenUv(matrix, x) - zeroUv) * size); }
inline Float GetColorCompressionExposureForSpatialPasses(Float roughness) { return .5f * (1.f - roughness) / (1.f + 1000.f * roughness * roughness) + (1.f - sqrt(saturate(roughness))) * .03f; }

inline Uint PackInternalData(Float diffuse, Float specular, Float material) { Uint d = (saturate(diffuse / 63.f) * 63.f + .5f).cast<uint>(); Uint s = (saturate(specular / 63.f) * 63.f + .5f).cast<uint>(); Uint m = (saturate(material / 15.f) * 15.f + .5f).cast<uint>(); return d | (s << 6u) | (m << 12u); }
inline Float3 UnpackInternalData(Uint p) { Float3 unpacked = make_float3((p & 63u).cast<float>(), ((p >> 6u) & 63u).cast<float>(), ((p >> 12u) & 15u).cast<float>()) * make_float3(1.f / 63.f, 1.f / 63.f, 1.f / 15.f); return unpacked * make_float3(63.f, 63.f, 15.f); }
inline Float2 PackData1(Float d, Float s) { return saturate(make_float2(d, s) / 63.f); }
inline Float2 UnpackData1(Float2 p) { return p * 63.f; }
inline Uint PackData2(Float bits, Float curvature, Float virtualHistory, Bool catrom) { Uint p = (bits + .5f).cast<uint>(); p |= (saturate(virtualHistory) * 127.f + .5f).cast<uint>() << 8u; p |= select(catrom, 1u << 15u, 0u); p |= call<uint>("f32tof16", curvature) << 16u; return p; }
inline Float2 UnpackData2(Uint p, Uint &bits, Bool &catrom) { bits = p & 255u; catrom = (p & (1u << 15u)) != 0u; Float curvature = call<float>("f16tof32", p >> 16u); return make_float2(((p >> 8u) & 127u).cast<float>() / 127.f, curvature); }
inline Float GetMinAllowedLimitForHitDistNonLinearAccumSpeed(const Var<SharedConstants> &c, Float roughness) { return 1.f / ocarina::fma(.5f * GetSpecMagicCurve(roughness), c.gMaxAccumulatedFrameNum, 1.f); }
inline Float GetFadeBasedOnAccumulatedFrames(const Var<SharedConstants> &c, Float accum) { return LinearStep(c.gHistoryFixFrameNum * 2.f / 3.f + 1e-6f, c.gHistoryFixFrameNum * 4.f / 3.f + 2e-6f, accum); }
inline Float GetNonLinearAccumSpeed(const Var<SharedConstants> &c, Float accum, Float maximum, Float confidence, Bool hasData) { Float f = max(1.f - confidence, 1.f / (1.f + min(accum, maximum))); return select(hasData, f, f * Lerp(1.f - c.gCheckerboardResolveAccumSpeed, 1.f, f)); }
inline Float RemapRoughnessToResponsiveFactor(const Var<SharedConstants> &c, Float roughness) { return SmoothStep01((roughness + NRD_EPS) / (c.gResponsiveAccumulationRoughnessThreshold + NRD_EPS)); }
inline Float GetLuma(Float4 value) { return value.x; }
inline Float ExtractHitDist(Float4 value) { return value.w; }
inline Float GetLumaScale(Float oldLuma, Float newLuma) { return (newLuma + NRD_EPS) / (oldLuma + NRD_EPS); }
inline Float4 ChangeLuma(Float4 value, Float newLuma) { return make_float4(value.xyz() * GetLumaScale(value.x, newLuma), value.w); }
inline Float4 ClampNegativeToZero(Float4 value) { return make_float4(_NRD_LinearToYCoCg(_NRD_YCoCgToLinear(value.xyz())), saturate(value.w)); }
inline Float4 MixHistoryAndCurrent(const Var<SharedConstants> &c, Float4 history, Float4 current, Float f, Float roughness = 1.f) { return make_float4(Lerp(history.xyz(), current.xyz(), f), Lerp(history.w, current.w, max(f, GetMinAllowedLimitForHitDistNonLinearAccumSpeed(c, roughness)))); }
inline Float ComputeAntilag(const Var<SharedConstants> &c, Float history, Float avg, Float sigma, Float accum) { Float s = sigma * c.gAntilagParams.x; Float hc = clamp(history, avg - s, avg + s); Float d = abs(history - hc) / (max(history, hc) + NRD_EPS); Float magic = c.gAntilagParams.y * c.gFramerateScale * c.gFramerateScale; return 1.f / (1.f + d * accum / magic); }
inline Float2 GetTemporalAccumulationParams(const Var<SharedConstants> &c, Float screenQuality, Float accum) { Float w = screenQuality; w *= accum / (1.f + accum); return make_float2(w, 1.f + 3.f * c.gFramerateScale * w); }
}
