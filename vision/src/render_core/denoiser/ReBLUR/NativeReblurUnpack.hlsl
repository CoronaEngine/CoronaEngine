#define NRD_HEADER_ONLY
#define NRD_NORMAL_ENCODING 2
#define NRD_ROUGHNESS_ENCODING 1
#include "NRD.hlsli"

// Keep this 96-byte layout identical to NativeReblurPack and the host constants.
struct NativeReblurConstants
{
    uint64_t guide;
    uint64_t rawDiffuse;
    uint64_t rawSpecular;
    uint64_t outputDiffuse;
    uint64_t outputSpecular;
    uint2 size;
    float4 hitDistanceParams;
    float4 previousViewDepthRow;
    float denoisingRange;
};
[[vk::push_constant]] ConstantBuffer<NativeReblurConstants> g_Constants;

Texture2D<float4> g_InDiffuse : register(t0);
Texture2D<float4> g_InSpecular : register(t1);

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
    float4 diffuse;
    float4 specular;
    [branch]
    if ((identity.w & 1u) != 0 && isfinite(position.w) &&
        position.w > 0.0 && position.w < g_Constants.denoisingRange)
    {
        float4 normalRoughness = vk::RawBufferLoad<float4>(g_Constants.guide + fieldBytes * 2 + elementOffset, 16);
        float3 diffuseFactor = vk::RawBufferLoad<float4>(g_Constants.guide + fieldBytes * 3 + elementOffset, 16).xyz;
        float3 specularFactor = vk::RawBufferLoad<float4>(g_Constants.guide + fieldBytes * 4 + elementOffset, 16).xyz;
        diffuse = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(g_InDiffuse.Load(int3(pixel, 0)));
        specular = REBLUR_BackEnd_UnpackRadianceAndNormHitDist(g_InSpecular.Load(int3(pixel, 0)));
        diffuse.xyz *= max(diffuseFactor, 1e-3);
        specular.xyz *= max(specularFactor, 1e-3);
        diffuse.w = REBLUR_GetHitDist(diffuse.w, position.w, g_Constants.hitDistanceParams, 1.0);
        specular.w = REBLUR_GetHitDist(specular.w, position.w, g_Constants.hitDistanceParams, normalRoughness.w);
        vk::RawBufferStore<float4>(g_Constants.outputDiffuse + elementOffset, diffuse, 16);
        vk::RawBufferStore<float4>(g_Constants.outputSpecular + elementOffset, specular, 16);
    }
    else
    {
        // Preserve Corona's sky/invalid output contract, including the raw w.
        // The production renderer filters in place. Preserve an aliased raw
        // signal without reading/writing it; copy each independent output only
        // when its address differs from the corresponding raw input.
        if (g_Constants.outputDiffuse != g_Constants.rawDiffuse)
        {
            diffuse = vk::RawBufferLoad<float4>(g_Constants.rawDiffuse + elementOffset, 16);
            vk::RawBufferStore<float4>(g_Constants.outputDiffuse + elementOffset, diffuse, 16);
        }
        if (g_Constants.outputSpecular != g_Constants.rawSpecular)
        {
            specular = vk::RawBufferLoad<float4>(g_Constants.rawSpecular + elementOffset, 16);
            vk::RawBufferStore<float4>(g_Constants.outputSpecular + elementOffset, specular, 16);
        }
    }
}
