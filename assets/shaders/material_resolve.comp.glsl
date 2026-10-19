#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

// Bindless resource pools
layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 1, binding = 0) readonly buffer SSBOPool { uint data[]; } ssbos[];
layout (set = 2, binding = 0, rgba16f) uniform image2D imagesRGBA16[];
layout (set = 2, binding = 0, rg32ui) uniform uimage2D imagesRGBA32UI[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint visibilityImageIndex;
    uint depthImageIndex;
    uint instanceInfoBufferIndex;
    uint materialTableBufferIndex;
    uint instanceCount;
    uint materialCount;
    uint vpBufferIndex;
    uint resolvedPositionImageIndex;
    uint resolvedBaseColorImageIndex;
    uint resolvedNormalImageIndex;
    uint resolvedObjectIDImageIndex;
    uint disableAlbedoSample;
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

// Read a 16-bit index from a uint32 SSBO (indices are packed as uint16 pairs)
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

bool finiteVec4(vec4 v)
{
    return finiteFloat(v.x) && finiteFloat(v.y) && finiteFloat(v.z) && finiteFloat(v.w);
}

bool finiteMat4(mat4 m)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!finiteFloat(m[c][r])) return false;
    return true;
}

vec2 safeMaterialUV(vec2 uv)
{
    return fract(uv);
}

vec4 fetchMaterialTexture(uint descriptor, vec2 uv)
{
    ivec2 texSize = textureSize(textures[nonuniformEXT(descriptor)], 0);
    if (texSize.x <= 0 || texSize.y <= 0) {
        return vec4(1.0);
    }

    vec2 wrapped = safeMaterialUV(uv);
    ivec2 coord = ivec2(clamp(floor(wrapped * vec2(texSize)),
                              vec2(0.0),
                              vec2(texSize) - vec2(1.0)));
    return texelFetch(textures[nonuniformEXT(descriptor)], coord, 0);
}

// ============================================================================
// InstanceInfo layout (96 bytes = 24 uints):
//   [0..15]  mat4 modelMatrix
//   [16]     uint vertexBufferIndex
//   [17]     uint indexBufferIndex
//   [18]     uint materialID
//   [19]     uint objectID
//   [20]     uint indexCount
//   [21]     uint vertexCount
//   [22]     uint maxIndex
//   [23]     uint flags
// ============================================================================

struct InstanceInfo
{
    mat4  modelMatrix;
    uint  vertexBufferIndex;
    uint  indexBufferIndex;
    uint  materialID;
    uint  objectID;
    uint  indexCount;
    uint  vertexCount;
    uint  maxIndex;
    uint  flags;
};

InstanceInfo loadInstanceInfo(uint instanceID)
{
    uint base = instanceID * 24u;
    InstanceInfo info;
    info.modelMatrix       = readMat4(pushConsts.instanceInfoBufferIndex, base);
    info.vertexBufferIndex = readUint(pushConsts.instanceInfoBufferIndex, base + 16u);
    info.indexBufferIndex  = readUint(pushConsts.instanceInfoBufferIndex, base + 17u);
    info.materialID        = readUint(pushConsts.instanceInfoBufferIndex, base + 18u);
    info.objectID          = readUint(pushConsts.instanceInfoBufferIndex, base + 19u);
    info.indexCount        = readUint(pushConsts.instanceInfoBufferIndex, base + 20u);
    info.vertexCount       = readUint(pushConsts.instanceInfoBufferIndex, base + 21u);
    info.maxIndex          = readUint(pushConsts.instanceInfoBufferIndex, base + 22u);
    info.flags             = readUint(pushConsts.instanceInfoBufferIndex, base + 23u);
    return info;
}

// ============================================================================
// MaterialInfo layout (64 bytes = 16 uints):
//   [0]     uint  textureDescriptor
//   [1]     float metallic
//   [2]     float roughness
//   [3..11] material parameters
//   [12..15] vec4 materialColor
// ============================================================================

struct MaterialInfo
{
    uint  textureDescriptor;
    float metallic;
    float roughness;
    vec4  materialColor;
};

MaterialInfo loadMaterialInfo(uint materialID)
{
    uint base = materialID * 16u;
    MaterialInfo mat;
    mat.textureDescriptor = readUint(pushConsts.materialTableBufferIndex, base);
    mat.metallic          = readFloat(pushConsts.materialTableBufferIndex, base + 1u);
    mat.roughness         = readFloat(pushConsts.materialTableBufferIndex, base + 2u);
    mat.materialColor     = readVec4(pushConsts.materialTableBufferIndex, base + 12u);
    return mat;
}

// ============================================================================
// Vertex layout (32 bytes = 8 floats):
//   [0..2]  vec3 position
//   [3..5]  vec3 normal
//   [6..7]  vec2 texCoord
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
// Edge function for barycentric coordinate computation
// ============================================================================

float edgeFunction(vec2 a, vec2 b, vec2 p)
{
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

// Transform world-space position to screen-space pixel coordinates
vec2 worldToScreen(vec3 worldPos, mat4 viewProjMatrix, vec2 resolution, out float clipW)
{
    vec4 clip = viewProjMatrix * vec4(worldPos, 1.0);
    clipW = clip.w;
    vec2 ndc = clip.xy / clip.w;
    // Vulkan NDC: Y top-down, matching screen coordinates
    return (ndc * 0.5 + 0.5) * resolution;
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

    // --- Read depth: skip background pixels ---
    vec2 screenUV = vec2(float(pixel.x) / float(pushConsts.gbufferSize.x),
                         float(pixel.y) / float(pushConsts.gbufferSize.y));
    float depth = textureLod(textures[nonuniformEXT(pushConsts.depthImageIndex)], screenUV, 0.0).r;

    if (depth >= (1.0 - 1e-3))
    {
        // Background pixel — write zero to resolved images
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    // --- Read visibility buffer ---
    uvec4 vis = imageLoad(imagesRGBA32UI[pushConsts.visibilityImageIndex], pixel);
    uint instanceID_1based = vis.r;
    uint primitiveID = vis.g;

    // Background check: clear value is 0, valid instanceIDs are 1-based
    if (instanceID_1based == 0u)
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    uint instanceID = instanceID_1based - 1u;
    if (instanceID >= pushConsts.instanceCount)
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    // --- Load instance and material info ---
    InstanceInfo inst = loadInstanceInfo(instanceID);
    if (inst.materialID >= pushConsts.materialCount)
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }
    MaterialInfo matl = loadMaterialInfo(inst.materialID);

    // --- Load triangle indices (uint16 packed in uint32 SSBO) ---
    uint indexBase = primitiveID * 3u;
    if (inst.indexCount == 0u || inst.vertexCount == 0u || indexBase + 2u >= inst.indexCount)
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }
    uint i0 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 0u);
    uint i1 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 1u);
    uint i2 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 2u);
    if (i0 >= inst.vertexCount || i1 >= inst.vertexCount || i2 >= inst.vertexCount ||
        i0 > inst.maxIndex || i1 > inst.maxIndex || i2 > inst.maxIndex)
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    // --- Load triangle vertices ---
    Vertex v0 = loadVertex(inst.vertexBufferIndex, i0);
    Vertex v1 = loadVertex(inst.vertexBufferIndex, i1);
    Vertex v2 = loadVertex(inst.vertexBufferIndex, i2);
    if (!finiteVec3(v0.position) || !finiteVec3(v1.position) || !finiteVec3(v2.position) ||
        !finiteVec3(v0.normal) || !finiteVec3(v1.normal) || !finiteVec3(v2.normal) ||
        !finiteVec2(v0.texCoord) || !finiteVec2(v1.texCoord) || !finiteVec2(v2.texCoord))
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    // --- Transform to world space ---
    vec3 worldPos0 = (inst.modelMatrix * vec4(v0.position, 1.0)).xyz;
    vec3 worldPos1 = (inst.modelMatrix * vec4(v1.position, 1.0)).xyz;
    vec3 worldPos2 = (inst.modelMatrix * vec4(v2.position, 1.0)).xyz;
    if (!finiteMat4(inst.modelMatrix) ||
        !finiteVec3(worldPos0) || !finiteVec3(worldPos1) || !finiteVec3(worldPos2))
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    // --- Compute screen-space positions for barycentric ---
    mat4 viewProjMatrix = readMat4(pushConsts.vpBufferIndex, 0u);
    vec2 resolution = vec2(pushConsts.gbufferSize);

    float w0, w1, w2;
    vec2 s0 = worldToScreen(worldPos0, viewProjMatrix, resolution, w0);
    vec2 s1 = worldToScreen(worldPos1, viewProjMatrix, resolution, w1);
    vec2 s2 = worldToScreen(worldPos2, viewProjMatrix, resolution, w2);
    if (!finiteMat4(viewProjMatrix) ||
        !finiteVec2(s0) || !finiteVec2(s1) || !finiteVec2(s2) ||
        !finiteFloat(w0) || !finiteFloat(w1) || !finiteFloat(w2) ||
        abs(w0) < 1e-6 || abs(w1) < 1e-6 || abs(w2) < 1e-6)
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    // Current pixel center
    vec2 pixelPos = vec2(pixel) + vec2(0.5);

    // Screen-space barycentric via edge functions
    float area = edgeFunction(s0, s1, s2);
    if (!finiteFloat(area) || abs(area) < 1e-6)
    {
        // Degenerate triangle
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    float b0 = edgeFunction(s1, s2, pixelPos) / area;
    float b1 = edgeFunction(s2, s0, pixelPos) / area;
    float b2 = edgeFunction(s0, s1, pixelPos) / area;
    if (!finiteFloat(b0) || !finiteFloat(b1) || !finiteFloat(b2))
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    // Perspective-correct interpolation
    float inv_w0 = 1.0 / w0;
    float inv_w1 = 1.0 / w1;
    float inv_w2 = 1.0 / w2;
    float inv_w_sum = b0 * inv_w0 + b1 * inv_w1 + b2 * inv_w2;
    if (!finiteFloat(inv_w0) || !finiteFloat(inv_w1) || !finiteFloat(inv_w2) ||
        !finiteFloat(inv_w_sum) || abs(inv_w_sum) < 1e-6)
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    vec3 bary;
    bary.x = (b0 * inv_w0) / inv_w_sum;
    bary.y = (b1 * inv_w1) / inv_w_sum;
    bary.z = (b2 * inv_w2) / inv_w_sum;
    if (!finiteVec3(bary))
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    // --- Interpolate vertex attributes ---
    vec3 interpPos = bary.x * worldPos0 + bary.y * worldPos1 + bary.z * worldPos2;

    // Normal: transform to world space with inverse transpose for non-uniform scaling
    mat3 normalMatrix = transpose(inverse(mat3(inst.modelMatrix)));
    vec3 interpNormal = normalize(normalMatrix *
        (bary.x * v0.normal + bary.y * v1.normal + bary.z * v2.normal));

    vec2 interpUV = bary.x * v0.texCoord + bary.y * v1.texCoord + bary.z * v2.texCoord;
    if (!finiteVec3(interpPos) || !finiteVec3(interpNormal) || !finiteVec2(interpUV))
    {
        imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(0.0));
        imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(0.0));
        return;
    }

    // --- Sample material ---
    vec4 baseColor = matl.materialColor;
    if (matl.textureDescriptor != 0u && pushConsts.disableAlbedoSample == 0u)
    {
        vec4 texSample = fetchMaterialTexture(matl.textureDescriptor, interpUV);
        if (finiteVec4(texSample)) {
            baseColor.rgb *= texSample.rgb;
        }
    }

    // --- Write resolved attributes ---
    imageStore(imagesRGBA16[pushConsts.resolvedPositionImageIndex],  pixel, vec4(interpPos, 1.0));
    imageStore(imagesRGBA16[pushConsts.resolvedBaseColorImageIndex], pixel, baseColor);
    imageStore(imagesRGBA16[pushConsts.resolvedNormalImageIndex],    pixel, vec4(interpNormal, 0.0));
    imageStore(imagesRGBA16[pushConsts.resolvedObjectIDImageIndex],  pixel, vec4(float(inst.objectID), 0.0, 0.0, 1.0));
}
