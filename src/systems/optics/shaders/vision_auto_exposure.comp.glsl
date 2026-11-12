#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require

// Slang's serialized GLSL module loses gl_WorkGroupSize in loop expressions.
#define EXPOSURE_WORKGROUP_SIZE 64

layout(local_size_x = EXPOSURE_WORKGROUP_SIZE) in;
layout(set = 1, binding = 0) buffer SSBOPool { uint data[]; } ssbos[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint srcBufferIndex;
    uint outputBufferIndex;
    float minimumGain;
    float maximumGain;
    uint bloomBufferIndex;
    uvec2 bloomSize;
    float bloomIntensity;
} pushConsts;

#include "vision_bloom_sampling.glsl"

shared float logLuminanceSums[EXPOSURE_WORKGROUP_SIZE];

vec3 loadHdr(uvec2 pixel)
{
    uint base = (pixel.y * pushConsts.gbufferSize.x + pixel.x) * 4u;
    uint src = nonuniformEXT(pushConsts.srcBufferIndex);
    vec3 hdr = vec3(uintBitsToFloat(ssbos[src].data[base]),
                    uintBitsToFloat(ssbos[src].data[base + 1u]),
                    uintBitsToFloat(ssbos[src].data[base + 2u]));
    if (pushConsts.bloomIntensity > 0.0) {
        hdr = compositeVisionBloom(hdr, pixel, pushConsts.gbufferSize,
                                   pushConsts.bloomBufferIndex, pushConsts.bloomSize, pushConsts.bloomIntensity);
    }
    return hdr;
}

vec3 sampleHdr(uvec2 meterPixel, uvec2 meterSize)
{
    vec2 pos = (vec2(meterPixel) + 0.5) * vec2(pushConsts.gbufferSize) / vec2(meterSize) - 0.5;
    uvec2 lo = uvec2(floor(pos));
    uvec2 hi = min(lo + 1u, pushConsts.gbufferSize - 1u);
    vec2 t = fract(pos);
    return mix(mix(loadHdr(lo), loadHdr(uvec2(hi.x, lo.y)), t.x),
               mix(loadHdr(uvec2(lo.x, hi.y)), loadHdr(hi), t.x), t.y);
}

void main()
{
    // RTXPT meters bilinearly sampled HDR on a power-of-two grid. Read the
    // center of each stratum so the same log-luminance meter fits one workgroup.
    uvec2 meterSize = uvec2(1u) << uvec2(findMSB(pushConsts.gbufferSize));
    uvec2 strata = min(meterSize, uvec2(64u));
    uint sampleCount = strata.x * strata.y;
    uint tid = gl_LocalInvocationIndex;

    float sum = 0.0;
    for (uint i = tid; i < sampleCount; i += uint(EXPOSURE_WORKGROUP_SIZE)) {
        uvec2 stratum = uvec2(i % strata.x, i / strata.x);
        uvec2 meterPixel = (stratum * meterSize + meterSize / 2u) / strata;
        vec3 hdr = sampleHdr(meterPixel, meterSize);
        sum += log2(max(0.0001, dot(hdr, vec3(0.299, 0.587, 0.114))));
    }

    logLuminanceSums[tid] = sum;
    barrier();
    for (uint step = uint(EXPOSURE_WORKGROUP_SIZE) / 2u; step > 0u; step /= 2u) {
        if (tid < step) {
            logLuminanceSums[tid] += logLuminanceSums[tid + step];
        }
        barrier();
    }

    if (tid == 0u) {
        float luminance = exp2(logLuminanceSums[0] / float(sampleCount));
        float gain = clamp(0.042 / luminance, pushConsts.minimumGain, pushConsts.maximumGain);
        ssbos[nonuniformEXT(pushConsts.outputBufferIndex)].data[0] = floatBitsToUint(gain);
    }
}
