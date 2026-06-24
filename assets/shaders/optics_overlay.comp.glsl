#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 1, binding = 0) readonly buffer SSBOPool { uint data[]; } ssbos[];
layout (set = 2, binding = 0, rgba16f) uniform image2D imagesRGBA16[];
layout (set = 2, binding = 0, rgba32ui) uniform uimage2D imagesRGBA32UI[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint visibilityImageIndex;
    uint instanceInfoBufferIndex;
    uint materialTableBufferIndex;
    uint vpBufferIndex;
    uint outputImage;
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
                readFloat(bufIdx, offset + 1),
                readFloat(bufIdx, offset + 2));
}

vec2 readVec2(uint bufIdx, uint offset)
{
    return vec2(readFloat(bufIdx, offset),
                readFloat(bufIdx, offset + 1));
}

vec4 readVec4(uint bufIdx, uint offset)
{
    return vec4(readFloat(bufIdx, offset),
                readFloat(bufIdx, offset + 1),
                readFloat(bufIdx, offset + 2),
                readFloat(bufIdx, offset + 3));
}

mat4 readMat4(uint bufIdx, uint offset)
{
    mat4 m;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            m[c][r] = readFloat(bufIdx, offset + c * 4 + r);
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
    info.modelMatrix       = readMat4(pushConsts.instanceInfoBufferIndex, base);
    info.vertexBufferIndex = readUint(pushConsts.instanceInfoBufferIndex, base + 16u);
    info.indexBufferIndex  = readUint(pushConsts.instanceInfoBufferIndex, base + 17u);
    info.materialID        = readUint(pushConsts.instanceInfoBufferIndex, base + 18u);
    info.objectID          = readUint(pushConsts.instanceInfoBufferIndex, base + 19u);
    return info;
}

struct MaterialInfo
{
    uint textureDescriptor;
    vec4 materialColor;
};

MaterialInfo loadMaterialInfo(uint materialID)
{
    uint base = materialID * 16u;
    MaterialInfo mat;
    mat.textureDescriptor = readUint(pushConsts.materialTableBufferIndex, base);
    mat.materialColor = readVec4(pushConsts.materialTableBufferIndex, base + 12u);
    return mat;
}

struct Vertex
{
    vec3 position;
    vec2 texCoord;
};

Vertex loadVertex(uint vertexBufferIndex, uint vertexID)
{
    uint base = vertexID * 8u;
    Vertex v;
    v.position = readVec3(vertexBufferIndex, base);
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

void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y) {
        return;
    }

    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    uvec4 vis = imageLoad(imagesRGBA32UI[pushConsts.visibilityImageIndex], pixel);
    uint instanceID_1based = vis.r;
    uint primitiveID = vis.g;

    if (instanceID_1based == 0u) {
        imageStore(imagesRGBA16[pushConsts.outputImage], pixel, vec4(0.0));
        return;
    }

    uint instanceID = instanceID_1based - 1u;
    InstanceInfo inst = loadInstanceInfo(instanceID);
    MaterialInfo matl = loadMaterialInfo(inst.materialID);

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
        imageStore(imagesRGBA16[pushConsts.outputImage], pixel, vec4(0.0));
        return;
    }

    float b0 = edgeFunction(s1, s2, pixelPos) / area;
    float b1 = edgeFunction(s2, s0, pixelPos) / area;
    float b2 = edgeFunction(s0, s1, pixelPos) / area;

    float inv_w0 = 1.0 / w0;
    float inv_w1 = 1.0 / w1;
    float inv_w2 = 1.0 / w2;
    float inv_w_sum = b0 * inv_w0 + b1 * inv_w1 + b2 * inv_w2;

    vec3 bary;
    bary.x = (b0 * inv_w0) / inv_w_sum;
    bary.y = (b1 * inv_w1) / inv_w_sum;
    bary.z = (b2 * inv_w2) / inv_w_sum;

    vec2 uv = bary.x * v0.texCoord + bary.y * v1.texCoord + bary.z * v2.texCoord;

    vec4 color = matl.materialColor;
    if (matl.textureDescriptor != 0u) {
        color *= texture(textures[nonuniformEXT(matl.textureDescriptor)], uv);
    }

    imageStore(imagesRGBA16[pushConsts.outputImage], pixel, color);
}
