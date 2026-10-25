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
    uint instanceCount;
    uint materialCount;
    uint vpBufferIndex;
    uint uniformBufferIndex;
    uint outputImageIndex;
    float radius;
    float bias;
    float power;
    uint sampleCount;
} pushConsts;

const vec3 kSamples[16] = vec3[](
    vec3( 0.5381,  0.1856,  0.4319),
    vec3( 0.1379,  0.2486,  0.4430),
    vec3( 0.3371,  0.5679,  0.0057),
    vec3(-0.6999, -0.0451,  0.0019),
    vec3( 0.0689, -0.1598,  0.8547),
    vec3( 0.0560,  0.0069,  0.1843),
    vec3(-0.0147,  0.1402,  0.0762),
    vec3( 0.0100, -0.1924,  0.0344),
    vec3(-0.3577, -0.5301,  0.4358),
    vec3(-0.3169,  0.1063,  0.0158),
    vec3( 0.0104, -0.5869,  0.0046),
    vec3(-0.0897, -0.4940,  0.3287),
    vec3( 0.7119, -0.0154,  0.0918),
    vec3(-0.0533,  0.0596,  0.5411),
    vec3( 0.0352, -0.0631,  0.5460),
    vec3(-0.4776,  0.2847,  0.0271)
);

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

mat4 readMat4(uint bufIdx, uint offset)
{
    mat4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = readFloat(bufIdx, offset + uint(c * 4 + r));
    return m;
}

bool finiteFloat(float v)
{
    return !isnan(v) && !isinf(v);
}

bool finiteVec2(vec2 v)
{
    return finiteFloat(v.x) && finiteFloat(v.y);
}

bool finiteVec3(vec3 v)
{
    return finiteFloat(v.x) && finiteFloat(v.y) && finiteFloat(v.z);
}

bool finiteMat4(mat4 m)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!finiteFloat(m[c][r])) return false;
    return true;
}

struct InstanceInfo
{
    mat4 modelMatrix;
    uint vertexBufferIndex;
    uint indexBufferIndex;
    uint materialID;
    uint objectID;
    uint indexCount;
    uint vertexCount;
    uint maxIndex;
    uint flags;
};

InstanceInfo loadInstanceInfo(uint instanceID)
{
    uint base = instanceID * 24u;
    InstanceInfo info;
    info.modelMatrix = readMat4(pushConsts.instanceInfoBufferIndex, base);
    info.vertexBufferIndex = readUint(pushConsts.instanceInfoBufferIndex, base + 16u);
    info.indexBufferIndex = readUint(pushConsts.instanceInfoBufferIndex, base + 17u);
    info.materialID = readUint(pushConsts.instanceInfoBufferIndex, base + 18u);
    info.objectID = readUint(pushConsts.instanceInfoBufferIndex, base + 19u);
    info.indexCount = readUint(pushConsts.instanceInfoBufferIndex, base + 20u);
    info.vertexCount = readUint(pushConsts.instanceInfoBufferIndex, base + 21u);
    info.maxIndex = readUint(pushConsts.instanceInfoBufferIndex, base + 22u);
    info.flags = readUint(pushConsts.instanceInfoBufferIndex, base + 23u);
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
    worldNormal = vec3(0.0, 1.0, 0.0);

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

    uint instanceID = instanceID_1based - 1u;
    if (instanceID >= pushConsts.instanceCount) {
        return false;
    }

    InstanceInfo inst = loadInstanceInfo(instanceID);
    uint indexBase = primitiveID * 3u;
    if (inst.indexCount == 0u || inst.vertexCount == 0u || indexBase + 2u >= inst.indexCount) {
        return false;
    }

    uint i0 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 0u);
    uint i1 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 1u);
    uint i2 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 2u);
    if (i0 >= inst.vertexCount || i1 >= inst.vertexCount || i2 >= inst.vertexCount ||
        i0 > inst.maxIndex || i1 > inst.maxIndex || i2 > inst.maxIndex) {
        return false;
    }

    Vertex v0 = loadVertex(inst.vertexBufferIndex, i0);
    Vertex v1 = loadVertex(inst.vertexBufferIndex, i1);
    Vertex v2 = loadVertex(inst.vertexBufferIndex, i2);
    if (!finiteVec3(v0.position) || !finiteVec3(v1.position) || !finiteVec3(v2.position) ||
        !finiteVec3(v0.normal) || !finiteVec3(v1.normal) || !finiteVec3(v2.normal)) {
        return false;
    }

    vec3 worldPos0 = (inst.modelMatrix * vec4(v0.position, 1.0)).xyz;
    vec3 worldPos1 = (inst.modelMatrix * vec4(v1.position, 1.0)).xyz;
    vec3 worldPos2 = (inst.modelMatrix * vec4(v2.position, 1.0)).xyz;
    if (!finiteMat4(inst.modelMatrix) ||
        !finiteVec3(worldPos0) || !finiteVec3(worldPos1) || !finiteVec3(worldPos2)) {
        return false;
    }

    mat4 viewProjMatrix = readMat4(pushConsts.vpBufferIndex, 0u);
    vec2 resolution = vec2(pushConsts.gbufferSize);
    float w0, w1, w2;
    vec2 s0 = worldToScreen(worldPos0, viewProjMatrix, resolution, w0);
    vec2 s1 = worldToScreen(worldPos1, viewProjMatrix, resolution, w1);
    vec2 s2 = worldToScreen(worldPos2, viewProjMatrix, resolution, w2);
    if (!finiteMat4(viewProjMatrix) ||
        !finiteVec2(s0) || !finiteVec2(s1) || !finiteVec2(s2) ||
        !finiteFloat(w0) || !finiteFloat(w1) || !finiteFloat(w2) ||
        abs(w0) < 1e-6 || abs(w1) < 1e-6 || abs(w2) < 1e-6) {
        return false;
    }

    vec2 pixelPos = vec2(pixel) + vec2(0.5);
    float area = edgeFunction(s0, s1, s2);
    if (!finiteFloat(area) || abs(area) < 1e-6) {
        return false;
    }

    float b0 = edgeFunction(s1, s2, pixelPos) / area;
    float b1 = edgeFunction(s2, s0, pixelPos) / area;
    float b2 = edgeFunction(s0, s1, pixelPos) / area;
    if (!finiteFloat(b0) || !finiteFloat(b1) || !finiteFloat(b2)) {
        return false;
    }

    float inv_w0 = 1.0 / w0;
    float inv_w1 = 1.0 / w1;
    float inv_w2 = 1.0 / w2;
    float inv_w_sum = b0 * inv_w0 + b1 * inv_w1 + b2 * inv_w2;
    if (!finiteFloat(inv_w0) || !finiteFloat(inv_w1) || !finiteFloat(inv_w2) ||
        !finiteFloat(inv_w_sum) || abs(inv_w_sum) < 1e-6) {
        return false;
    }

    vec3 bary;
    bary.x = (b0 * inv_w0) / inv_w_sum;
    bary.y = (b1 * inv_w1) / inv_w_sum;
    bary.z = (b2 * inv_w2) / inv_w_sum;
    if (!finiteVec3(bary)) {
        return false;
    }

    worldPos = bary.x * worldPos0 + bary.y * worldPos1 + bary.z * worldPos2;
    mat3 normalMatrix = transpose(inverse(mat3(inst.modelMatrix)));
    worldNormal = normalize(normalMatrix *
        (bary.x * v0.normal + bary.y * v1.normal + bary.z * v2.normal));
    return finiteVec3(worldPos) && finiteVec3(worldNormal) && dot(worldNormal, worldNormal) > 0.0;
}

vec3 reconstructViewPos(vec2 uv, float depth)
{
    mat4 eyeInvProj = readMat4(pushConsts.uniformBufferIndex, 76u);
    vec4 clip = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 view = eyeInvProj * clip;
    return view.xyz / max(abs(view.w), 1e-6);
}

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
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
    if (!decodeWorldPositionNormal(pixel, worldPos, worldNormal)) {
        imageStore(imagesR16[nonuniformEXT(pushConsts.outputImageIndex)], pixel,
                   vec4(1.0, 0.0, 0.0, 1.0));
        return;
    }

    vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(pushConsts.gbufferSize);
    float depth = texelFetch(textures[nonuniformEXT(pushConsts.depthImageIndex)], pixel, 0).r;
    vec3 fragView = reconstructViewPos(uv, depth);

    mat4 eyeView = readMat4(pushConsts.uniformBufferIndex, 44u);
    vec3 viewNormal = normalize(mat3(eyeView) * worldNormal);

    float angle = hash12(vec2(pixel)) * 6.28318530718;
    vec3 randomVec = normalize(vec3(cos(angle), sin(angle), 0.0));
    vec3 tangent = randomVec - viewNormal * dot(randomVec, viewNormal);
    if (dot(tangent, tangent) < 1e-5) {
        tangent = abs(viewNormal.z) < 0.999
            ? normalize(cross(vec3(0.0, 0.0, 1.0), viewNormal))
            : normalize(cross(vec3(0.0, 1.0, 0.0), viewNormal));
    } else {
        tangent = normalize(tangent);
    }
    vec3 bitangent = normalize(cross(viewNormal, tangent));
    mat3 tbn = mat3(tangent, bitangent, viewNormal);

    mat4 eyeProj = readMat4(pushConsts.uniformBufferIndex, 60u);
    uint samples = clamp(pushConsts.sampleCount, 1u, 16u);
    float occlusion = 0.0;

    for (uint i = 0u; i < samples; ++i) {
        vec3 sampleView = fragView + (tbn * kSamples[i]) * pushConsts.radius;
        vec4 sampleClip = eyeProj * vec4(sampleView, 1.0);
        if (abs(sampleClip.w) < 1e-5) {
            continue;
        }

        vec3 sampleNdc = sampleClip.xyz / sampleClip.w;
        vec2 sampleUV = sampleNdc.xy * 0.5 + 0.5;
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 ||
            sampleUV.y < 0.0 || sampleUV.y > 1.0 ||
            sampleNdc.z < 0.0 || sampleNdc.z > 1.0) {
            continue;
        }

        float sampleDepth = textureLod(textures[nonuniformEXT(pushConsts.depthImageIndex)], sampleUV, 0.0).r;
        if (sampleDepth <= 0.0) {
            continue;
        }

        vec3 sampleDepthView = reconstructViewPos(sampleUV, sampleDepth);
        float rangeCheck = smoothstep(0.0, 1.0,
            pushConsts.radius / max(abs(fragView.z - sampleDepthView.z), 1e-4));
        occlusion += (sampleDepthView.z <= sampleView.z - pushConsts.bias) ? rangeCheck : 0.0;
    }

    float ao = 1.0 - occlusion / float(samples);
    ao = pow(clamp(ao, 0.0, 1.0), max(pushConsts.power, 0.001));
    imageStore(imagesR16[nonuniformEXT(pushConsts.outputImageIndex)], pixel,
               vec4(ao, 0.0, 0.0, 1.0));
}
