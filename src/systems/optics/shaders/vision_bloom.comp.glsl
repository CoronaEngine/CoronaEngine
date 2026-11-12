#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 0) buffer SSBOPool { uint data[]; } ssbos[];

layout(push_constant) uniform PushConsts
{
    uvec2 inputSize;
    uvec2 outputSize;
    uint srcBufferIndex;
    uint outputBufferIndex;
    uint operation; // 0: HDR downsample, 1: half-float downsample, 2/3: horizontal/vertical blur.
} pushConsts;

#include "vision_bloom_sampling.glsl"

vec3 loadDownsampleSource(uvec2 pixel)
{
    if (pushConsts.operation == 0u) {
        uint base = (pixel.y * pushConsts.inputSize.x + pixel.x) * 4u;
        uint src = nonuniformEXT(pushConsts.srcBufferIndex);
        return vec3(uintBitsToFloat(ssbos[src].data[base]),
                    uintBitsToFloat(ssbos[src].data[base + 1u]),
                    uintBitsToFloat(ssbos[src].data[base + 2u]));
    }
    return loadVisionBloomTexel(pushConsts.srcBufferIndex, pushConsts.inputSize, pixel);
}

vec3 downsampleVisionBloom(uvec2 pixel)
{
    vec2 pos = (vec2(pixel) + 0.5) * vec2(pushConsts.inputSize) / vec2(pushConsts.outputSize) - 0.5;
    uvec2 lo = uvec2(floor(pos));
    uvec2 hi = min(lo + 1u, pushConsts.inputSize - 1u);
    vec2 t = fract(pos);
    return mix(mix(loadDownsampleSource(lo), loadDownsampleSource(uvec2(hi.x, lo.y)), t.x),
               mix(loadDownsampleSource(uvec2(lo.x, hi.y)), loadDownsampleSource(hi), t.x), t.y);
}

// RTXPT's radius of 8 full-resolution pixels yields sigma 2 at quarter size.
// Its paired bilinear taps expand to these Gaussian weights at integer offsets.
const float gaussianWeights[9] = float[9](
    0.1994711402, 0.1760326634, 0.1209853623, 0.0647587978, 0.0269954833,
    0.0087641502, 0.0022159242, 0.0004363413, 0.0000669151);

vec3 blurVisionBloom(ivec2 pixel)
{
    uint src = pushConsts.srcBufferIndex;
    uvec2 size = pushConsts.inputSize;
    ivec2 direction = pushConsts.operation == 2u ? ivec2(1, 0) : ivec2(0, 1);
    ivec2 last = ivec2(size) - 1;
    vec3 sum = loadVisionBloomTexel(src, size, uvec2(pixel)) * gaussianWeights[0];
    for (int tap = 1; tap <= 8; ++tap) {
        ivec2 offset = direction * tap;
        vec3 before = loadVisionBloomTexel(src, size, uvec2(max(pixel - offset, ivec2(0))));
        vec3 after = loadVisionBloomTexel(src, size, uvec2(min(pixel + offset, last)));
        sum += (before + after) * gaussianWeights[tap];
    }
    return sum;
}

void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    if (any(greaterThanEqual(pixel, pushConsts.outputSize))) {
        return;
    }

    vec3 color = pushConsts.operation < 2u ? downsampleVisionBloom(pixel) : blurVisionBloom(ivec2(pixel));
    uint base = (pixel.y * pushConsts.outputSize.x + pixel.x) * 2u;
    uint dst = nonuniformEXT(pushConsts.outputBufferIndex);
    ssbos[dst].data[base] = packHalf2x16(color.rg);
    ssbos[dst].data[base + 1u] = packHalf2x16(vec2(color.b, 1.0));
}
