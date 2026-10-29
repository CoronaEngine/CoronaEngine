#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 1, binding = 0) readonly buffer SSBOPool { uint data[]; } ssbos[];
layout (set = 2, binding = 0, r16f) uniform image2D imagesR16[];
layout (set = 2, binding = 0, rg32ui) uniform uimage2D imagesRGBA32UI[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint visibilityImageIndex;
    uint depthImageIndex;
    uint instanceInfoBufferIndex;
    uint vpBufferIndex;
    uint uniformBufferIndex;
    uint shadowInfoBufferIndex;
    uint outputImageIndex;
    vec3 sun_dir;
} pushConsts;

float readFloat(uint bufIdx, uint offset)
{
    return uintBitsToFloat(ssbos[nonuniformEXT(bufIdx)].data[offset]);
}

uint readUint(uint bufIdx, uint offset)
{
    return ssbos[nonuniformEXT(bufIdx)].data[offset];
}

uint readIndex16(uint bufIdx, uint index16)
{
    uint wordIndex = index16 >> 1u;
    uint word = ssbos[nonuniformEXT(bufIdx)].data[wordIndex];
    return (index16 & 1u) == 0u ? (word & 0xFFFFu) : (word >> 16u);
}

vec3 readVec3(uint bufIdx, uint offset)
{
    return vec3(readFloat(bufIdx, offset),
                readFloat(bufIdx, offset + 1u),
                readFloat(bufIdx, offset + 2u));
}

vec2 readVec2(uint bufIdx, uint offset)
{
    return vec2(readFloat(bufIdx, offset),
                readFloat(bufIdx, offset + 1u));
}

vec4 readVec4(uint bufIdx, uint offset)
{
    return vec4(readFloat(bufIdx, offset),
                readFloat(bufIdx, offset + 1u),
                readFloat(bufIdx, offset + 2u),
                readFloat(bufIdx, offset + 3u));
}

mat4 readMat4(uint bufIdx, uint offset)
{
    mat4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = readFloat(bufIdx, offset + uint(c * 4 + r));
    return m;
}

struct InstanceInfo
{
    mat4 modelMatrix;
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint materialID;
    uint objectID;
};

InstanceInfo loadInstanceInfo(uint instanceID)
{
    uint base = instanceID * 20u;
    InstanceInfo info;
    info.modelMatrix = readMat4(pushConsts.instanceInfoBufferIndex, base);
    info.vertexBufferIndex = readUint(pushConsts.instanceInfoBufferIndex, base + 16u);
    info.indexBufferIndex = readUint(pushConsts.instanceInfoBufferIndex, base + 17u);
    info.materialID = readUint(pushConsts.instanceInfoBufferIndex, base + 18u);
    info.objectID = readUint(pushConsts.instanceInfoBufferIndex, base + 19u);
    return info;
}

struct Vertex
{
    vec3 position;
    vec3 normal;
    vec2 texCoord;
};

Vertex loadVertex(uint vertexBufferIndex, uint vertexID)
{
    uint base = vertexID * 8u;
    Vertex v;
    v.position = readVec3(vertexBufferIndex, base);
    v.normal = readVec3(vertexBufferIndex, base + 3u);
    v.texCoord = readVec2(vertexBufferIndex, base + 6u);
    return v;
}

float edgeFunction(vec2 a, vec2 b, vec2 p)
{
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

vec2 worldToScreen(vec3 worldPos, mat4 viewProjMatrix, vec2 resolution, out float clipW)
{
    vec4 clip = viewProjMatrix * vec4(worldPos, 1.0);
    clipW = clip.w;
    vec2 ndc = clip.xy / clip.w;
    return (ndc * 0.5 + 0.5) * resolution;
}

bool decodeWorldPositionNormal(ivec2 pixel, out vec3 worldPos, out vec3 worldNormal)
{
    worldPos = vec3(0.0);
    worldNormal = vec3(0.0);

    uvec4 vis = imageLoad(imagesRGBA32UI[nonuniformEXT(pushConsts.visibilityImageIndex)], pixel);
    uint instanceID_1based = vis.r;
    uint primitiveID = vis.g;
    if (instanceID_1based == 0u) {
        return false;
    }

    float depth = texelFetch(textures[nonuniformEXT(pushConsts.depthImageIndex)], pixel, 0).r;
    if (depth <= 0.0) {
        return false;
    }

    InstanceInfo inst = loadInstanceInfo(instanceID_1based - 1u);
    uint i0 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 0u);
    uint i1 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 1u);
    uint i2 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 2u);

    Vertex v0 = loadVertex(inst.vertexBufferIndex, i0);
    Vertex v1 = loadVertex(inst.vertexBufferIndex, i1);
    Vertex v2 = loadVertex(inst.vertexBufferIndex, i2);

    vec3 worldPos0 = (inst.modelMatrix * vec4(v0.position, 1.0)).xyz;
    vec3 worldPos1 = (inst.modelMatrix * vec4(v1.position, 1.0)).xyz;
    vec3 worldPos2 = (inst.modelMatrix * vec4(v2.position, 1.0)).xyz;

    mat4 viewProjMatrix = readMat4(pushConsts.vpBufferIndex, 0u);
    vec2 resolution = vec2(pushConsts.gbufferSize);
    float w0, w1, w2;
    vec2 s0 = worldToScreen(worldPos0, viewProjMatrix, resolution, w0);
    vec2 s1 = worldToScreen(worldPos1, viewProjMatrix, resolution, w1);
    vec2 s2 = worldToScreen(worldPos2, viewProjMatrix, resolution, w2);

    vec2 pixelPos = vec2(pixel) + vec2(0.5);
    float area = edgeFunction(s0, s1, s2);
    if (abs(area) < 1e-6) {
        return false;
    }

    float b0 = edgeFunction(s1, s2, pixelPos) / area;
    float b1 = edgeFunction(s2, s0, pixelPos) / area;
    float b2 = edgeFunction(s0, s1, pixelPos) / area;

    float inv_w0 = 1.0 / w0;
    float inv_w1 = 1.0 / w1;
    float inv_w2 = 1.0 / w2;
    float inv_w_sum = b0 * inv_w0 + b1 * inv_w1 + b2 * inv_w2;
    if (abs(inv_w_sum) < 1e-6) {
        return false;
    }

    vec3 bary;
    bary.x = (b0 * inv_w0) / inv_w_sum;
    bary.y = (b1 * inv_w1) / inv_w_sum;
    bary.z = (b2 * inv_w2) / inv_w_sum;

    worldPos = bary.x * worldPos0 + bary.y * worldPos1 + bary.z * worldPos2;
    mat3 normalMatrix = transpose(inverse(mat3(inst.modelMatrix)));
    worldNormal = normalize(normalMatrix *
        (bary.x * v0.normal + bary.y * v1.normal + bary.z * v2.normal));
    return dot(worldNormal, worldNormal) > 0.0;
}

float sampleShadowMap(uint shadowMap, vec2 uv, float receiverDepth, float bias)
{
    float shadowMapSize = readFloat(pushConsts.shadowInfoBufferIndex, 72u);
    vec2 texelSize = vec2(1.0 / max(shadowMapSize, 1.0));
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float casterDepth = texture(
                textures[nonuniformEXT(shadowMap)],
                uv + vec2(float(x), float(y)) * texelSize).r;
            lit += (receiverDepth - bias) <= casterDepth ? 1.0 : 0.0;
        }
    }
    return lit / 9.0;
}

float computeSunShadow(vec3 worldPos, vec3 normal)
{
    if (readUint(pushConsts.shadowInfoBufferIndex, 74u) == 0u) {
        return 1.0;
    }

    vec4 cascadeSplits = readVec4(pushConsts.shadowInfoBufferIndex, 64u);
    vec3 eyePosition = readVec3(pushConsts.uniformBufferIndex, 36u);
    vec3 eyeDir = normalize(readVec3(pushConsts.uniformBufferIndex, 40u));
    float viewDepth = dot(worldPos - eyePosition, eyeDir);
    if (viewDepth < 0.0 || viewDepth > cascadeSplits.w) {
        return 1.0;
    }

    uint cascadeIndex = 3u;
    if (viewDepth <= cascadeSplits.x) {
        cascadeIndex = 0u;
    } else if (viewDepth <= cascadeSplits.y) {
        cascadeIndex = 1u;
    } else if (viewDepth <= cascadeSplits.z) {
        cascadeIndex = 2u;
    }

    mat4 lightViewProj = readMat4(pushConsts.shadowInfoBufferIndex, cascadeIndex * 16u);
    uint shadowMap = readUint(pushConsts.shadowInfoBufferIndex, 68u + cascadeIndex);

    vec4 lightClip = lightViewProj * vec4(worldPos, 1.0);
    if (abs(lightClip.w) < 1e-5) {
        return 1.0;
    }

    vec3 ndc = lightClip.xyz / lightClip.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 ||
        ndc.z < 0.0 || ndc.z > 1.0) {
        return 1.0;
    }

    vec3 L = normalize(pushConsts.sun_dir);
    float slope = 1.0 - max(dot(normalize(normal), L), 0.0);
    float shadowBias = readFloat(pushConsts.shadowInfoBufferIndex, 73u);
    float bias = max(shadowBias, shadowBias * (1.0 + slope * 2.0));
    return sampleShadowMap(shadowMap, uv, ndc.z, bias);
}

void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y) {
        return;
    }

    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    vec3 worldPos;
    vec3 worldNormal;
    float lit = 1.0;
    if (decodeWorldPositionNormal(pixel, worldPos, worldNormal)) {
        lit = computeSunShadow(worldPos, worldNormal);
    }

    imageStore(imagesR16[nonuniformEXT(pushConsts.outputImageIndex)], pixel,
               vec4(clamp(lit, 0.0, 1.0), 0.0, 0.0, 1.0));
}
