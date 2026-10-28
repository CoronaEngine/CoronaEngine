// Bindless resource pools
layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 1, binding = 0) readonly buffer SSBOPool { uint data[]; } ssbos[];
layout (set = 2, binding = 0, rgba16f) uniform image2D imagesRGBA16[];
layout (set = 2, binding = 0, rg32ui) uniform uimage2D imagesRG32UI[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint visibilityImageIndex;
    uint depthImageIndex;
    uint instanceInfoBufferIndex;
    uint materialTableBufferIndex;
    uint instanceCount;
    uint materialCount;
    uint directLightingBufferIndex;
    uint finalOutputImage;
    uint uniformBufferIndex;
    uint skyIrradianceSHBufferIndex;  // 9 vec3 SH coeffs of sky radiance (sky-driven ambient)
    vec3 lightColor;
    uint featureFlags; // AO=1, SSR=2, sky ambient=4, cascade debug=8
    vec3 sun_dir;
    uint shadowEnabled;
    uint shadowInfoBufferIndex;
    uint shadowMap0;
    uint outputMode;
    uint shadowMap1;
    uint shadowMap2;
    float ssaoStrength;
    uint shadowMap3;
    uint disableAlbedoSample;
    float ssrMaxDistance;
    float ssrThickness;
    float ssrMaxRoughness;
    uint ssrMaxSteps;
} pushConsts;
