#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require

// Zero-copy Vision resolve pass.
// Read Vision's linear float32 RGBA display source and write the RGBA16F
// display image. Apply the scene's display transform and encode sRGB here
// because the UNORM swapchain blit preserves the stored values.

layout (local_size_x = 8, local_size_y = 8) in;

// Bindless SSBO pool (matches lighting.comp.glsl set=1 layout).
layout (set = 1, binding = 0) readonly buffer SSBOPool { uint data[]; } ssbos[];
// Bindless storage-image pool (matches tonemap.comp.glsl set=2 rgba16 layout).
layout (set = 2, binding = 0, rgba16f) uniform image2D imagesRGBA16[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint  srcBufferIndex;   // imported Vision pre-tonemap buffer (float4 per pixel)
    uint  outputImage;      // engine finalOutputImage (RGBA16F)
    float exposure;         // Legacy exposure or Hable exposure multiplier.
    uint diagnostic;        // Geometry AOVs already contain display values.
    uint displayProfile;    // 0 = legacy, 1 = Hable manual, 2 = Hable auto.
    uint exposureBufferIndex;
    uint bloomBufferIndex;
    uvec2 bloomSize;
    float bloomIntensity;
} pushConsts;

#include "vision_bloom_sampling.glsl"

vec3 acesFilmicToneMapCurve(vec3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 hableUc2Curve(vec3 x)
{
    const float a = 0.22;
    const float b = 0.30;
    const float c = 0.10;
    const float d = 0.20;
    const float e = 0.01;
    const float f = 0.30;
    return ((x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f)) - e / f;
}

// 标准 sRGB OETF，必须与 Native tonemap.comp 的 linearToSrgb 完全一致。
vec3 linearToSrgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92,
               1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(0.0031308, c));
}

void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y) {
        return;
    }

    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    uint base = (gl_GlobalInvocationID.y * pushConsts.gbufferSize.x +
                 gl_GlobalInvocationID.x) * 4u;

    uint srcIdx = nonuniformEXT(pushConsts.srcBufferIndex);
    vec3 hdr = vec3(
        uintBitsToFloat(ssbos[srcIdx].data[base + 0u]),
        uintBitsToFloat(ssbos[srcIdx].data[base + 1u]),
        uintBitsToFloat(ssbos[srcIdx].data[base + 2u]));

    vec3 ldr = hdr;
    if (pushConsts.diagnostic == 0u) {
        if (pushConsts.displayProfile == 0u) {
            vec3 exposed = vec3(1.0) - exp(-hdr * pushConsts.exposure);
            ldr = linearToSrgb(acesFilmicToneMapCurve(exposed));
        } else {
            if (pushConsts.bloomIntensity > 0.0) {
                hdr = compositeVisionBloom(hdr, uvec2(pixel), pushConsts.gbufferSize,
                                           pushConsts.bloomBufferIndex, pushConsts.bloomSize, pushConsts.bloomIntensity);
            }
            float gain = pushConsts.exposure;
            if (pushConsts.displayProfile == 2u) {
                uint index = nonuniformEXT(pushConsts.exposureBufferIndex);
                gain *= uintBitsToFloat(ssbos[index].data[0]);
            }
            ldr = linearToSrgb(hableUc2Curve(hdr * gain * 2.0) / hableUc2Curve(vec3(11.2)));
        }
    }

    imageStore(imagesRGBA16[nonuniformEXT(pushConsts.outputImage)], pixel, vec4(ldr, 1.0));
}
