#define NRD_HEADER_ONLY
#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD.hlsli"

// Must match NativeReblurConstants on the host: 96 bytes, scalar member offsets.
struct NativeReblurConstants
{
    uint64_t guide;             // 0: SOA Guide, five 16-byte field planes
    uint64_t rawDiffuse;        // 8
    uint64_t rawSpecular;       // 16
    uint64_t outputDiffuse;     // 24
    uint64_t outputSpecular;    // 32
    uint2 size;                 // 40
    float4 hitDistanceParams;   // 48: NRD defaults (3, 0.1, 20, -25)
    float4 previousViewDepthRow;// 64: row 2 of the previous world-to-view matrix
    float denoisingRange;       // 80: view-space range shared with NRD
};
[[vk::push_constant]] ConstantBuffer<NativeReblurConstants> g_Constants;

[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_OutDiffuse : register(u0);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_OutSpecular : register(u1);
[[vk::image_format("rgb10a2")]] RWTexture2D<float4> g_OutNormalRoughness : register(u2);
RWTexture2D<float> g_OutViewZ : register(u3);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_OutMotion : register(u4);
[[vk::image_format("r8")]] RWTexture2D<float> g_OutDisocclusionMix : register(u5);

[numthreads(16, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= g_Constants.size))
        return;

    uint index = pixel.y * g_Constants.size.x + pixel.x;
    uint64_t fieldBytes = uint64_t(g_Constants.size.x) * g_Constants.size.y * 16;
    uint64_t elementOffset = uint64_t(index) * 16;
    float4 position = vk::RawBufferLoad<float4>(g_Constants.guide + elementOffset, 16);
    uint4 identity = vk::RawBufferLoad<uint4>(g_Constants.guide + fieldBytes + elementOffset, 16);
    bool valid = (identity.w & 1u) != 0 && isfinite(position.w) &&
                 position.w > 0.0 && position.w < g_Constants.denoisingRange;
    if (!valid)
    {
        // NRD ignores pixels beyond denoisingRange. Initialize every bound input
        // to avoid reading undefined scratch values in dispatch boundary tiles.
        g_OutDiffuse[pixel] = 0.0;
        g_OutSpecular[pixel] = 0.0;
        g_OutNormalRoughness[pixel] = 0.0;
        // NRD ClassifyTiles skips viewZ > range. Treat the exact boundary
        // conservatively as invalid and publish a marker strictly beyond it.
        g_OutViewZ[pixel] = max(NRD_INF, g_Constants.denoisingRange * 2.0);
        g_OutMotion[pixel] = 0.0;
        g_OutDisocclusionMix[pixel] = 0.0;
        return;
    }

    float4 normalRoughness = vk::RawBufferLoad<float4>(g_Constants.guide + fieldBytes * 2 + elementOffset, 16);
    float4 diffuseFactor = vk::RawBufferLoad<float4>(g_Constants.guide + fieldBytes * 3 + elementOffset, 16);
    float4 specularFactor = vk::RawBufferLoad<float4>(g_Constants.guide + fieldBytes * 4 + elementOffset, 16);
    float4 diffuse = vk::RawBufferLoad<float4>(g_Constants.rawDiffuse + elementOffset, 16);
    float4 specular = vk::RawBufferLoad<float4>(g_Constants.rawSpecular + elementOffset, 16);
    diffuse.xyz /= max(diffuseFactor.xyz, 1e-3);
    specular.xyz /= max(specularFactor.xyz, 1e-3);
    // Match the custom path before saturation turns +Inf into a valid hit.
    bool validDiffuseHit = abs(diffuse.w) < 3.402823466e38;
    bool validSpecularHit = abs(specular.w) < 3.402823466e38;
    float diffuseHit = validDiffuseHit ? REBLUR_FrontEnd_GetNormHitDist(diffuse.w, position.w, g_Constants.hitDistanceParams, 1.0) : 0.0;
    float specularHit = validSpecularHit ? REBLUR_FrontEnd_GetNormHitDist(specular.w, position.w, g_Constants.hitDistanceParams, normalRoughness.w) : 0.0;
    g_OutDiffuse[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(diffuse.xyz, diffuseHit, true);
    g_OutSpecular[pixel] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(specular.xyz, specularHit, true);
    float disocclusionMix = float((identity.w >> 8u) & 255u) / 255.0;
    g_OutDisocclusionMix[pixel] = disocclusionMix;
    // RTXPT applies the material mix to filtering roughness after normalizing
    // hit distance with the original material roughness above.
    g_OutNormalRoughness[pixel] = NRD_FrontEnd_PackNormalAndRoughness(normalRoughness.xyz, saturate(normalRoughness.w + disocclusionMix), 0.0);
    g_OutViewZ[pixel] = position.w;
    // Corona stores current-minus-previous pixels; NRD adds its motion vector
    // to the current UV. Host motionVectorScale.xy converts pixels to UVs.
    float previousViewDepth = dot(g_Constants.previousViewDepthRow, float4(position.xyz, 1.0));
    g_OutMotion[pixel] = float4(-float2(diffuseFactor.w, specularFactor.w), previousViewDepth - position.w, 0.0);
}
