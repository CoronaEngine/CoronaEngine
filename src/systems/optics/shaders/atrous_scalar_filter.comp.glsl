#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 2, binding = 0, r16f) uniform image2D imagesR16[];
layout (set = 2, binding = 0, rg32ui) uniform uimage2D imagesRGBA32UI[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint inputImageIndex;
    uint outputImageIndex;
    uint guideImageIndex;
    uint visibilityImageIndex;
    uint filterStep;
    float valueSigma;
    float normalPower;
    float normalThreshold;
    float depthSigmaScale;
    float depthSigmaMin;
} pushConsts;

const float kKernel[5] = float[](1.0, 4.0, 6.0, 4.0, 1.0);

float gaussianWeight(float diff, float sigma)
{
    float safeSigma = max(sigma, 1e-5);
    return exp(-(diff * diff) / (safeSigma * safeSigma));
}

void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y) {
        return;
    }

    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 maxPixel = ivec2(pushConsts.gbufferSize) - ivec2(1);

    float centerValue = texelFetch(textures[nonuniformEXT(pushConsts.inputImageIndex)], pixel, 0).r;
    vec4 centerGuide = texelFetch(textures[nonuniformEXT(pushConsts.guideImageIndex)], pixel, 0);
    if (centerGuide.w < 0.0) {
        imageStore(imagesR16[nonuniformEXT(pushConsts.outputImageIndex)], pixel,
                   vec4(centerValue, 0.0, 0.0, 1.0));
        return;
    }

    vec3 centerNormal = normalize(centerGuide.xyz);
    float centerDepth = centerGuide.w;
    uvec4 centerVis = imageLoad(imagesRGBA32UI[nonuniformEXT(pushConsts.visibilityImageIndex)], pixel);
    float depthSigma = max(pushConsts.depthSigmaMin, centerDepth * pushConsts.depthSigmaScale);

    float sumValue = 0.0;
    float sumWeight = 0.0;
    int step = int(max(pushConsts.filterStep, 1u));

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            ivec2 coord = clamp(pixel + ivec2(x * step, y * step), ivec2(0), maxPixel);
            vec4 sampleGuide = texelFetch(textures[nonuniformEXT(pushConsts.guideImageIndex)], coord, 0);
            if (sampleGuide.w < 0.0) {
                continue;
            }

            uvec4 sampleVis = imageLoad(imagesRGBA32UI[nonuniformEXT(pushConsts.visibilityImageIndex)], coord);
            if (sampleVis.r != centerVis.r) {
                continue;
            }

            vec3 sampleNormal = normalize(sampleGuide.xyz);
            float normalDot = dot(centerNormal, sampleNormal);
            if (normalDot < pushConsts.normalThreshold) {
                continue;
            }

            float sampleValue = texelFetch(textures[nonuniformEXT(pushConsts.inputImageIndex)], coord, 0).r;
            float kernelWeight = (kKernel[x + 2] * kKernel[y + 2]) * (1.0 / 256.0);
            float normalWeight = pow(max(normalDot, 0.0), max(pushConsts.normalPower, 0.0));
            float depthWeight = gaussianWeight(abs(centerDepth - sampleGuide.w), depthSigma);
            float valueWeight = gaussianWeight(abs(centerValue - sampleValue), pushConsts.valueSigma);
            float weight = kernelWeight * normalWeight * depthWeight * valueWeight;

            sumValue += sampleValue * weight;
            sumWeight += weight;
        }
    }

    float filtered = sumWeight > 1e-6 ? (sumValue / sumWeight) : centerValue;
    imageStore(imagesR16[nonuniformEXT(pushConsts.outputImageIndex)], pixel,
               vec4(clamp(filtered, 0.0, 1.0), 0.0, 0.0, 1.0));
}
