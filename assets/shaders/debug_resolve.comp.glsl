#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

// Bindless resource pools
layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 0, binding = 0) uniform usampler2D uintTextures[];
layout (set = 1, binding = 0) readonly buffer SSBOPool { uint data[]; } ssbos[];
layout (set = 2, binding = 0, rgba16f) uniform image2D imagesRGBA16[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint visibilityImageIndex;
    uint depthImageIndex;
    uint instanceInfoBufferIndex;
    uint materialTableBufferIndex;
    uint vpBufferIndex;
    uint outputImageIndex;
    uint debugMode;         // 0=BaseColor, 1=Normal, 2=WorldPosition, 3=ObjectID, 4=VisibilityBuffer
} pushConsts;

// ============================================================================
// SSBO helper functions — generic uint[] accessor for all buffer types
// ============================================================================

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

// ============================================================================
// InstanceInfo layout (80 bytes = 20 uints)
// ============================================================================

struct InstanceInfo
{
    mat4  modelMatrix;
    uint  vertexBufferIndex;
    uint  indexBufferIndex;
    uint  materialID;
    uint  objectID;
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

// ============================================================================
// MaterialInfo layout (64 bytes = 16 uints)
// ============================================================================

struct MaterialInfo
{
    uint  textureDescriptor;
    float metallic;
    float roughness;
    float subsurface;
    float specular;
    float specularTint;
    float anisotropic;
    float sheen;
    float sheenTint;
    float clearcoat;
    float clearcoatGloss;
    vec4  materialColor;
};

MaterialInfo loadMaterialInfo(uint materialID)
{
    uint base = materialID * 16u;
    MaterialInfo mat;
    mat.textureDescriptor = readUint(pushConsts.materialTableBufferIndex, base);
    mat.metallic          = readFloat(pushConsts.materialTableBufferIndex, base + 1u);
    mat.roughness         = readFloat(pushConsts.materialTableBufferIndex, base + 2u);
    mat.subsurface        = readFloat(pushConsts.materialTableBufferIndex, base + 3u);
    mat.specular          = readFloat(pushConsts.materialTableBufferIndex, base + 4u);
    mat.specularTint      = readFloat(pushConsts.materialTableBufferIndex, base + 5u);
    mat.anisotropic       = readFloat(pushConsts.materialTableBufferIndex, base + 6u);
    mat.sheen             = readFloat(pushConsts.materialTableBufferIndex, base + 7u);
    mat.sheenTint         = readFloat(pushConsts.materialTableBufferIndex, base + 8u);
    mat.clearcoat         = readFloat(pushConsts.materialTableBufferIndex, base + 9u);
    mat.clearcoatGloss    = readFloat(pushConsts.materialTableBufferIndex, base + 10u);
    // [11] padding
    mat.materialColor     = readVec4(pushConsts.materialTableBufferIndex, base + 12u);
    return mat;
}

// ============================================================================
// Vertex layout (32 bytes = 8 floats)
// ============================================================================

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
    v.normal   = readVec3(vertexBufferIndex, base + 3u);
    v.texCoord = readVec2(vertexBufferIndex, base + 6u);
    return v;
}

// ============================================================================
// Screen-space utilities
// ============================================================================

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

// ============================================================================
// Debug visualization helpers
// ============================================================================

// Golden-ratio based pseudo-color for integer IDs
vec3 pseudoColor(uint id)
{
    return vec3(
        fract(float(id) * 0.618033988749),
        fract(float(id) * 0.381966011251),
        fract(float(id) * 0.737096774194)
    );
}

// ============================================================================
// Main
// ============================================================================

void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y)
    {
        return;
    }

    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);

    // --- Read visibility buffer (raw) for VisibilityBuffer debug mode ---
    uvec4 vis = texelFetch(uintTextures[nonuniformEXT(pushConsts.visibilityImageIndex)], pixel, 0);
    uint instanceID_1based = vis.r;
    uint primitiveID = vis.g;

    // --- VisibilityBuffer mode: raw pseudo-color without full decode ---
    if (pushConsts.debugMode == 4u)
    {
        vec4 result = vec4(0.0);
        if (instanceID_1based != 0u)
        {
            result = vec4(pseudoColor(instanceID_1based ^ (primitiveID * 2654435761u)), 1.0);
        }
        imageStore(imagesRGBA16[pushConsts.outputImageIndex], pixel, result);
        return;
    }

    // --- Read depth: skip background pixels ---
    vec2 screenUV = vec2(float(pixel.x) / float(pushConsts.gbufferSize.x),
                         float(pixel.y) / float(pushConsts.gbufferSize.y));
    float depth = texture(textures[pushConsts.depthImageIndex], screenUV).r;

    if (depth >= (1.0 - 1e-3) || instanceID_1based == 0u)
    {
        imageStore(imagesRGBA16[pushConsts.outputImageIndex], pixel, vec4(0.0));
        return;
    }

    uint instanceID = instanceID_1based - 1u;

    // --- Load instance and material info ---
    InstanceInfo inst = loadInstanceInfo(instanceID);
    MaterialInfo matl = loadMaterialInfo(inst.materialID);

    // --- ObjectID mode: skip expensive vertex decode ---
    if (pushConsts.debugMode == 3u)
    {
        imageStore(imagesRGBA16[pushConsts.outputImageIndex], pixel,
                   vec4(pseudoColor(inst.objectID), 1.0));
        return;
    }

    // --- Full VBuffer decode for BaseColor / Normal / WorldPosition ---
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
    if (abs(area) < 1e-6)
    {
        imageStore(imagesRGBA16[pushConsts.outputImageIndex], pixel, vec4(0.0));
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

    vec3 interpPos = bary.x * worldPos0 + bary.y * worldPos1 + bary.z * worldPos2;

    mat3 normalMatrix = transpose(inverse(mat3(inst.modelMatrix)));
    vec3 interpNormal = normalize(normalMatrix *
        (bary.x * v0.normal + bary.y * v1.normal + bary.z * v2.normal));

    vec2 interpUV = bary.x * v0.texCoord + bary.y * v1.texCoord + bary.z * v2.texCoord;

    vec4 baseColor = matl.materialColor;
    if (matl.textureDescriptor != 0u)
    {
        vec4 texSample = texture(textures[nonuniformEXT(matl.textureDescriptor)], interpUV);
        baseColor.rgb *= texSample.rgb;
    }

    // --- Output based on debug mode ---
    vec4 result = vec4(0.0);
    switch (pushConsts.debugMode)
    {
        case 0u: // BaseColor
            result = baseColor;
            break;
        case 1u: // Normal (remapped to [0,1])
            result = vec4(interpNormal * 0.5 + 0.5, 1.0);
            break;
        case 2u: // WorldPosition
            result = vec4(interpPos, 1.0);
            break;
        default:
            result = vec4(0.0);
            break;
    }

    imageStore(imagesRGBA16[pushConsts.outputImageIndex], pixel, result);
}
