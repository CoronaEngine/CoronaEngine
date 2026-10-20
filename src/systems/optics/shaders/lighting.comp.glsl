#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

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
    uint vpBufferIndex;
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

vec4 fetchMaterialTexture(uint descriptor, vec2 uv, vec2 dx, vec2 dy)
{
    // Explicit analytic gradients are required in compute. Sampling LOD 0 with
    // texelFetch caused nearest-neighbour shimmer and ignored texture mipmaps.
    return textureGrad(textures[nonuniformEXT(descriptor)], uv, dx, dy);
}

// ============================================================================
// InstanceInfo layout (96 bytes = 24 uints)
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
// MaterialInfo layout (128 bytes = 32 uints)
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
    float lightingEnabled;
    vec4  materialColor;
    uint normalDescriptor;
    uint metallicDescriptor;
    uint roughnessDescriptor;
    uint occlusionDescriptor;
    uint emissiveDescriptor;
    uint specularDescriptor;
    uint specularColorDescriptor;
    uint flags;
    vec3 emissive;
    float normalScale;
    vec3 specularColor;
    float occlusionStrength;
};

MaterialInfo loadMaterialInfo(uint materialID)
{
    uint base = materialID * 32u;
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
    mat.lightingEnabled   = readFloat(pushConsts.materialTableBufferIndex, base + 11u);
    mat.materialColor     = readVec4(pushConsts.materialTableBufferIndex, base + 12u);
    mat.normalDescriptor = readUint(pushConsts.materialTableBufferIndex, base + 16u);
    mat.metallicDescriptor = readUint(pushConsts.materialTableBufferIndex, base + 17u);
    mat.roughnessDescriptor = readUint(pushConsts.materialTableBufferIndex, base + 18u);
    mat.occlusionDescriptor = readUint(pushConsts.materialTableBufferIndex, base + 19u);
    mat.emissiveDescriptor = readUint(pushConsts.materialTableBufferIndex, base + 20u);
    mat.specularDescriptor = readUint(pushConsts.materialTableBufferIndex, base + 21u);
    mat.specularColorDescriptor = readUint(pushConsts.materialTableBufferIndex, base + 22u);
    mat.flags = readUint(pushConsts.materialTableBufferIndex, base + 23u);
    mat.emissive = readVec3(pushConsts.materialTableBufferIndex, base + 24u);
    mat.normalScale = readFloat(pushConsts.materialTableBufferIndex, base + 27u);
    mat.specularColor = readVec3(pushConsts.materialTableBufferIndex, base + 28u);
    mat.occlusionStrength = readFloat(pushConsts.materialTableBufferIndex, base + 31u);
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

// ============================================================================
// Disney Principled BRDF
// Reference: Brent Burley, "Physically Based Shading at Disney", SIGGRAPH 2012
// ============================================================================

#define PI 3.14159265359

float sqr(float x) { return x * x; }

float luminance(vec3 color)
{
    return dot(color, vec3(0.299, 0.587, 0.114));
}

float SchlickFresnel(float x)
{
    x = clamp(1.0 - x, 0.0, 1.0);
    float x2 = x * x;
    return x2 * x2 * x; // pow(1 - x, 5)
}

// Isotropic GTR with gamma == 1 (used for clearcoat)
float GTR1(float ndoth, float a)
{
    float a2 = a * a;
    float t = 1.0 + (a2 - 1.0) * ndoth * ndoth;
    return (a2 - 1.0) / (PI * log(a2) * t);
}

// Anisotropic GTR with gamma == 2 (equivalent to anisotropic GGX)
float AnisotropicGTR2(float ndoth, float hdotx, float hdoty, float ax, float ay)
{
    return 1.0 / (PI * ax * ay * sqr(sqr(hdotx / ax) + sqr(hdoty / ay) + sqr(ndoth)));
}

// Isotropic Smith GGX geometric attenuation (used for clearcoat)
float SmithGGX(float alphaSquared, float ndotl, float ndotv)
{
    float a = ndotv * sqrt(alphaSquared + ndotl * (ndotl - alphaSquared * ndotl));
    float b = ndotl * sqrt(alphaSquared + ndotv * (ndotv - alphaSquared * ndotv));
    return 0.5 / (a + b);
}

// Anisotropic Smith GGX geometric attenuation
float AnisotropicSmithGGX(float ndots, float sdotx, float sdoty, float ax, float ay)
{
    return 1.0 / (ndots + sqrt(sqr(sdotx * ax) + sqr(sdoty * ay) + sqr(ndots)));
}

// UBO layout offsets (in uint units):
//   [0..2]   lightPosition (vec3)
//   [3]      padding
//   [4..19]  lightViewMatrix (mat4)
//   [20..35] lightProjMatrix (mat4)
//   [36..38] eyePosition (vec3)
//   [39]     padding
//   [40..42] eyeDir (vec3)
//   [43]     padding
//   [44..59] eyeViewMatrix (mat4)
//   [60..75] eyeProjMatrix (mat4)
//   [76..91] eyeInvProjMatrix (mat4)

// Evaluate sky-driven irradiance from 9 SH coefficients in the SSBO pool.
// Uses the Ramamoorthi-Hanrahan analytic form which already folds in the
// cosine (Lambertian) convolution, so the result is diffuse irradiance / PI
// ready to multiply by albedo. Returns radiance in the same units the sky
// pass emits (scaled by sky_intensity at projection time).
vec3 evalSkySH(vec3 n, uint shBuf)
{
    vec3 L0  = readVec3(shBuf, 0u);
    vec3 L1  = readVec3(shBuf, 3u);
    vec3 L2  = readVec3(shBuf, 6u);
    vec3 L3  = readVec3(shBuf, 9u);
    vec3 L4  = readVec3(shBuf, 12u);
    vec3 L5  = readVec3(shBuf, 15u);
    vec3 L6  = readVec3(shBuf, 18u);
    vec3 L7  = readVec3(shBuf, 21u);
    vec3 L8  = readVec3(shBuf, 24u);

    const float c1 = 0.429043;
    const float c2 = 0.511664;
    const float c3 = 0.743125;
    const float c4 = 0.886227;
    const float c5 = 0.247708;

    float x = n.x, y = n.y, z = n.z;
    vec3 irradiance =
          c1 * L8 * (x * x - y * y)
        + c3 * L6 * (z * z)
        + c4 * L0
        - c5 * L6
        + 2.0 * c1 * (L4 * x * y + L7 * x * z + L5 * y * z)
        + 2.0 * c2 * (L3 * x + L1 * y + L2 * z);

    return max(irradiance, vec3(0.0));
}

float sampleShadowMap(uint shadowMap, vec2 uv, float receiverDepth, float bias)
{
    float shadowMapSize = readFloat(pushConsts.shadowInfoBufferIndex, 72u);
    // Compare individual texels before filtering. Interpolating depth first
    // invents occluders between surfaces and makes subtexel motion shimmer.
    ivec2 size = textureSize(textures[nonuniformEXT(shadowMap)], 0);
    vec2 texel = uv * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(texel));
    vec2 fraction = fract(texel);
    float lit = 0.0;
    for (int y = -1; y <= 2; ++y) {
        float wy = y == -1 ? 1.0-fraction.y : y == 2 ? fraction.y : 1.0;
        for (int x = -1; x <= 2; ++x) {
            float wx = x == -1 ? 1.0-fraction.x : x == 2 ? fraction.x : 1.0;
            ivec2 samplePixel = clamp(base + ivec2(x,y), ivec2(0), size-1);
            float caster = texelFetch(textures[nonuniformEXT(shadowMap)], samplePixel, 0).r;
            lit += wx * wy * float(receiverDepth-bias <= caster);
        }
    }
    return lit / 9.0;
}

float computeSunShadow(vec3 worldPos, vec3 normal)
{
    if (pushConsts.shadowEnabled == 0u) {
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
    uint shadowMap = cascadeIndex == 0u ? pushConsts.shadowMap0 :
        cascadeIndex == 1u ? pushConsts.shadowMap1 :
        cascadeIndex == 2u ? pushConsts.shadowMap2 : pushConsts.shadowMap3;

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

uint computeShadowCascadeIndex(vec3 worldPos)
{
    vec4 cascadeSplits = readVec4(pushConsts.shadowInfoBufferIndex, 64u);
    vec3 eyePosition = readVec3(pushConsts.uniformBufferIndex, 36u);
    vec3 eyeDir = normalize(readVec3(pushConsts.uniformBufferIndex, 40u));
    float viewDepth = dot(worldPos - eyePosition, eyeDir);
    if (viewDepth < 0.0 || viewDepth > cascadeSplits.w) {
        return 4u;
    }
    if (viewDepth <= cascadeSplits.x) {
        return 0u;
    }
    if (viewDepth <= cascadeSplits.y) {
        return 1u;
    }
    if (viewDepth <= cascadeSplits.z) {
        return 2u;
    }
    return 3u;
}

vec3 shadowCascadeDebugColor(uint cascadeIndex)
{
    if (cascadeIndex == 0u) return vec3(1.0, 0.18, 0.12);
    if (cascadeIndex == 1u) return vec3(0.15, 0.85, 0.22);
    if (cascadeIndex == 2u) return vec3(0.16, 0.45, 1.0);
    if (cascadeIndex == 3u) return vec3(1.0, 0.82, 0.12);
    return vec3(0.08, 0.08, 0.08);
}

vec3 DisneyBRDF(vec3 V, vec3 Normal, vec3 Tangent, vec3 Bitangent,
    vec3 lightColor, vec3 albedo, MaterialInfo matl, float shadowFactor, float ambientOcclusion)
{
    vec3 N = normalize(Normal);
    vec3 X = normalize(Tangent);
    vec3 Y = normalize(Bitangent);
    vec3 L = normalize(pushConsts.sun_dir);
    vec3 H = V + L;
    H *= inversesqrt(max(dot(H,H), 1e-12));

    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 0.0);
    float ndoth = max(dot(N, H), 0.0);
    float ldoth = max(dot(L, H), 0.0);

    bool importedMR = (matl.flags & 32u) != 0u;
    float specularWeight = importedMR ? matl.sheenTint : 1.0;
    vec3 dielectricF0 = clamp(matl.specularColor * matl.specular * 0.08, vec3(0), vec3(1)) * specularWeight;
    // Sky-driven ambient (SH9). evalSkySH returns the directional irradiance
    // E(n) from the atmospheric model; Lambertian exitance = albedo·E(n)/π.
    // Perf toggle (skyAmbientEnabled): when off, skip the whole SH ambient —
    // both the per-pixel evaluation and the DC fill — so its cost is measurable
    // in isolation. The C++ side also skips the projection dispatch.
    vec3 irradiance = vec3(0.0);
    if ((pushConsts.featureFlags & 4u) != 0u) {
        vec3 skyE = evalSkySH(N, pushConsts.skyIrradianceSHBufferIndex);
        // Average-sky (DC) fill. Without GI, surfaces the sky cannot reach
        // directly — down-facing, indoor, occluded — get near-zero directional
        // irradiance and read as black. Floor the irradiance at a fraction of
        // the average sky (c4·L00 = DC irradiance) so those faces keep a soft
        // ambient base.
        vec3 skyDC = vec3(0.886227) * readVec3(pushConsts.skyIrradianceSHBufferIndex, 0u);
        irradiance = max(skyE, skyDC * 0.5);
    }
    vec3 ambient = albedo * (1.0 - matl.metallic) * irradiance * (1.0 / PI);
    ambient *= clamp(ambientOcclusion, 0.0, 1.0);
    if (importedMR) ambient *= 1.0 - max(dielectricF0.r, max(dielectricF0.g, dielectricF0.b));

    if (ndotl <= 0.0) return ambient;

    // --- Derived color values ---
    float Cdlum = luminance(albedo);
    vec3 Ctint = Cdlum > 0.0 ? albedo / Cdlum : vec3(1.0);
    vec3 Cspec0 = mix(dielectricF0 * mix(vec3(1.0), Ctint, matl.specularTint),
                      albedo, matl.metallic);
    vec3 Csheen = mix(vec3(1.0), Ctint, matl.sheenTint);

    // === Diffuse lobe (Disney retro-reflective diffuse) ===
    float FL = SchlickFresnel(ndotl);
    float FV = SchlickFresnel(ndotv);
    float Fss90 = ldoth * ldoth * matl.roughness;
    float Fd90 = 0.5 + 2.0 * Fss90;
    float Fd = mix(1.0, Fd90, FL) * mix(1.0, Fd90, FV);

    // Subsurface approximation (Hanrahan-Krueger)
    float Fss = mix(1.0, Fss90, FL) * mix(1.0, Fss90, FV);
    float ss = 1.25 * (Fss * (1.0 / (ndotl + ndotv + 1e-4) - 0.5) + 0.5);

    // === Specular lobe (anisotropic GGX) ===
    float aspect = sqrt(1.0 - matl.anisotropic * 0.9);
    float alphaSquared = matl.roughness * matl.roughness;
    float ax = max(0.001, alphaSquared / aspect);
    float ay = max(0.001, alphaSquared * aspect);
    float Ds = AnisotropicGTR2(ndoth, dot(H, X), dot(H, Y), ax, ay);

    float GalphaSquared = sqr(0.5 + matl.roughness * 0.5);
    float Gax = max(0.001, GalphaSquared / aspect);
    float Gay = max(0.001, GalphaSquared * aspect);
    float G = AnisotropicSmithGGX(ndotl, dot(L, X), dot(L, Y), Gax, Gay)
            * AnisotropicSmithGGX(ndotv, dot(V, X), dot(V, Y), Gax, Gay);

    float FH = SchlickFresnel(ldoth);
    vec3 F = mix(Cspec0, vec3(mix(specularWeight, 1.0, matl.metallic)), FH);

    // === Sheen lobe ===
    vec3 Fsheen = FH * matl.sheen * Csheen;

    // === Clearcoat lobe (fixed IOR 1.5, F0 = 0.04) ===
    float Dr = GTR1(ndoth, mix(0.1, 0.001, matl.clearcoatGloss));
    float Fr = mix(0.04, 1.0, FH);
    float Gr = SmithGGX(0.25, ndotl, ndotv);

    // === Combine all lobes ===
    vec3 diffuse = (1.0 / PI) * (mix(Fd, ss, matl.subsurface) * albedo + Fsheen)
                   * (1.0 - matl.metallic);
    if (importedMR) {
        vec3 dielectricF = mix(dielectricF0, vec3(specularWeight), FH);
        diffuse *= 1.0 - max(dielectricF.r, max(dielectricF.g, dielectricF.b));
    }
    vec3 specular = Ds * F * G;
    vec3 clearcoat = vec3(0.25 * matl.clearcoat * Gr * Fr * Dr);

    return ambient + (diffuse + specular + clearcoat) * lightColor * ndotl * shadowFactor;
}

struct Surface {
    vec3 position;
    vec3 geometricNormal;
    vec3 normal;
    vec3 tangent;
    vec3 bitangent;
    vec3 baseColor;
    MaterialInfo material;
    uint objectID;
};

bool decodeSurface(ivec2 pixel, out Surface surface)
{
    // --- Read visibility buffer ---
    uvec4 vis = imageLoad(imagesRG32UI[pushConsts.visibilityImageIndex], pixel);
    uint instanceID_1based = vis.r;
    uint primitiveID = vis.g;

    if (instanceID_1based == 0u)
    {
        return false;
    }

    uint instanceID = instanceID_1based - 1u;
    if (instanceID >= pushConsts.instanceCount)
    {
        return false;
    }

    // --- Load instance and material info ---
    InstanceInfo inst = loadInstanceInfo(instanceID);
    if (inst.materialID >= pushConsts.materialCount)
    {
        return false;
    }
    MaterialInfo matl = loadMaterialInfo(inst.materialID);

    // --- Load triangle indices (uint16 packed in uint32 SSBO) ---
    if (inst.indexCount < 3u || primitiveID >= inst.indexCount / 3u) return false;
    uint indexBase = primitiveID * 3u;
    if (inst.indexCount == 0u || inst.vertexCount == 0u || indexBase + 2u >= inst.indexCount)
    {
        return false;
    }
    uint i0 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 0u);
    uint i1 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 1u);
    uint i2 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 2u);
    if (i0 >= inst.vertexCount || i1 >= inst.vertexCount || i2 >= inst.vertexCount ||
        i0 > inst.maxIndex || i1 > inst.maxIndex || i2 > inst.maxIndex)
    {
        return false;
    }

    // --- Load triangle vertices ---
    Vertex v0 = loadVertex(inst.vertexBufferIndex, i0);
    Vertex v1 = loadVertex(inst.vertexBufferIndex, i1);
    Vertex v2 = loadVertex(inst.vertexBufferIndex, i2);
    if (!finiteVec3(v0.position) || !finiteVec3(v1.position) || !finiteVec3(v2.position) ||
        !finiteVec3(v0.normal) || !finiteVec3(v1.normal) || !finiteVec3(v2.normal) ||
        !finiteVec2(v0.texCoord) || !finiteVec2(v1.texCoord) || !finiteVec2(v2.texCoord))
    {
        return false;
    }

    // --- Transform to world space ---
    vec3 worldPos0 = (inst.modelMatrix * vec4(v0.position, 1.0)).xyz;
    vec3 worldPos1 = (inst.modelMatrix * vec4(v1.position, 1.0)).xyz;
    vec3 worldPos2 = (inst.modelMatrix * vec4(v2.position, 1.0)).xyz;
    if (!finiteMat4(inst.modelMatrix) ||
        !finiteVec3(worldPos0) || !finiteVec3(worldPos1) || !finiteVec3(worldPos2))
    {
        return false;
    }

    // Homogeneous edge equations recover perspective barycentrics without
    // dividing each vertex by W. A clipped triangle remains shadeable when a
    // vertex lies on or behind the eye plane; rasterization already clips it.
    mat4 viewProjMatrix = readMat4(pushConsts.vpBufferIndex, 0u);
    vec2 resolution = vec2(pushConsts.gbufferSize);
    vec4 clip0 = viewProjMatrix * vec4(worldPos0, 1.0);
    vec4 clip1 = viewProjMatrix * vec4(worldPos1, 1.0);
    vec4 clip2 = viewProjMatrix * vec4(worldPos2, 1.0);
    if (!finiteMat4(viewProjMatrix) || !finiteVec4(clip0) || !finiteVec4(clip1) || !finiteVec4(clip2)) return false;
    vec2 ndc = (vec2(pixel) + 0.5) * (2.0 / resolution) - 1.0;
    vec3 clipW = vec3(clip0.w, clip1.w, clip2.w);
    vec3 edgeX = vec3(clip0.x, clip1.x, clip2.x) - ndc.x * clipW;
    vec3 edgeY = vec3(clip0.y, clip1.y, clip2.y) - ndc.y * clipW;
    vec3 weights = cross(edgeX, edgeY);
    float weightSum = dot(weights, vec3(1));
    if (!finiteFloat(weightSum) || abs(weightSum) < 1e-20) return false;
    vec3 bary = weights / weightSum;
    if (!finiteVec3(bary)) return false;
    vec3 weightDx = cross(-clipW, edgeY) * (2.0 / resolution.x);
    vec3 weightDy = cross(edgeX, -clipW) * (2.0 / resolution.y);
    vec3 baryDx = (weightDx - bary * dot(weightDx, vec3(1))) / weightSum;
    vec3 baryDy = (weightDy - bary * dot(weightDy, vec3(1))) / weightSum;

    // --- Interpolate vertex attributes ---
    vec3 interpPos = bary.x * worldPos0 + bary.y * worldPos1 + bary.z * worldPos2;

    mat3 model = mat3(inst.modelMatrix);
    vec3 c0 = cross(model[1], model[2]);
    float determinant = dot(model[0], c0);
    if (abs(determinant) < 1e-12) return false;
    // Cofactors give inverse-transpose direction without a matrix division.
    mat3 normalMatrix = mat3(c0, cross(model[2], model[0]), cross(model[0], model[1])) * sign(determinant);
    vec3 rawNormal = normalMatrix * (bary.x*v0.normal + bary.y*v1.normal + bary.z*v2.normal);
    float normalLength = length(rawNormal);
    if (normalLength < 1e-12) return false;
    vec3 interpNormal = rawNormal / normalLength;

    vec2 interpUV = bary.x * v0.texCoord + bary.y * v1.texCoord + bary.z * v2.texCoord;
    vec2 uvDx = v0.texCoord*baryDx.x + v1.texCoord*baryDx.y + v2.texCoord*baryDx.z;
    vec2 uvDy = v0.texCoord*baryDy.x + v1.texCoord*baryDy.y + v2.texCoord*baryDy.z;
    if (!finiteVec2(uvDx) || !finiteVec2(uvDy)) return false;
    if (!finiteVec3(interpPos) || !finiteVec3(interpNormal) || !finiteVec2(interpUV))
    {
        return false;
    }

    // --- Compute tangent frame from triangle edges and UVs ---
    vec3 edge1 = worldPos1 - worldPos0;
    vec3 edge2 = worldPos2 - worldPos0;
    vec2 dUV1 = v1.texCoord - v0.texCoord;
    vec2 dUV2 = v2.texCoord - v0.texCoord;

    float denom = dUV1.x * dUV2.y - dUV2.x * dUV1.y;
    vec3 T, B;
    if (abs(denom) > 1e-6)
    {
        float inv = 1.0 / denom;
        T = normalize(inv * (dUV2.y * edge1 - dUV1.y * edge2));
        B = normalize(inv * (-dUV2.x * edge1 + dUV1.x * edge2));
    }
    else
    {
        // Fallback: construct arbitrary tangent frame from normal
        T = normalize(cross(abs(interpNormal.z) < 0.9 ? vec3(0,0,1) : vec3(0,1,0), interpNormal));
        B = normalize(cross(interpNormal, T));
    }
    // Preserve the UV orientation, including mirrored UV islands. Discarding
    // this sign reverses the green normal-map channel on half of a model.
    float tangentSign = dot(cross(interpNormal, T), B) < 0.0 ? -1.0 : 1.0;
    // Re-orthogonalize
    T -= dot(T, interpNormal) * interpNormal;
    if (dot(T,T) < 1e-10) T = cross(abs(interpNormal.z) < 0.9 ? vec3(0,0,1) : vec3(0,1,0), interpNormal);
    T = normalize(T);
    B = normalize(cross(interpNormal, T)) * tangentSign;

    // --- Sample material ---
    vec4 baseColor = matl.materialColor;
    if (matl.textureDescriptor != 0u && pushConsts.disableAlbedoSample == 0u)
    {
        vec4 texSample = fetchMaterialTexture(matl.textureDescriptor, interpUV, uvDx, uvDy);
        if (finiteVec4(texSample)) {
            baseColor.rgb *= texSample.rgb;
        }
    }

    // Material data textures are LINEAR views. Colour/emissive/specular RGB
    // are sRGB views; the sampler decodes those before applying linear factors.
    vec4 metalSample = vec4(1);
    if (matl.metallicDescriptor != 0u) {
        metalSample = fetchMaterialTexture(matl.metallicDescriptor, interpUV, uvDx, uvDy);
        if (finiteVec4(metalSample)) matl.metallic *= metalSample[matl.flags & 3u];
    }
    if (matl.roughnessDescriptor != 0u) {
        vec4 sampleValue = matl.roughnessDescriptor == matl.metallicDescriptor ? metalSample :
            fetchMaterialTexture(matl.roughnessDescriptor, interpUV, uvDx, uvDy);
        if (finiteVec4(sampleValue)) matl.roughness *= sampleValue[(matl.flags >> 2u) & 3u];
    }
    float materialAO = 1.0;
    if (matl.occlusionDescriptor != 0u) {
        vec4 sampleValue = matl.occlusionDescriptor == matl.metallicDescriptor ? metalSample :
            fetchMaterialTexture(matl.occlusionDescriptor, interpUV, uvDx, uvDy);
        if (finiteVec4(sampleValue)) materialAO = mix(1.0, sampleValue.r, matl.occlusionStrength);
    }
    matl.occlusionStrength = clamp(materialAO, 0.0, 1.0);
    if (matl.emissiveDescriptor != 0u) {
        vec4 sampleValue = fetchMaterialTexture(matl.emissiveDescriptor, interpUV, uvDx, uvDy);
        if (finiteVec4(sampleValue)) matl.emissive *= sampleValue.rgb;
    }
    if (matl.specularDescriptor != 0u) {
        vec4 sampleValue = fetchMaterialTexture(matl.specularDescriptor, interpUV, uvDx, uvDy);
        if (finiteVec4(sampleValue)) {
            if ((matl.flags & 32u) != 0u) matl.sheenTint *= sampleValue.a;
            else matl.specular *= sampleValue.a;
        }
    }
    if (matl.specularColorDescriptor != 0u) {
        vec4 sampleValue = fetchMaterialTexture(matl.specularColorDescriptor, interpUV, uvDx, uvDy);
        if (finiteVec4(sampleValue)) matl.specularColor *= sampleValue.rgb;
    }

    matl.metallic = clamp(matl.metallic, 0.0, 1.0);
    // Analytic normal derivatives from the same VBuffer triangle. Bound the
    // specular lobe by its pixel footprint without a normal/history image.
    vec3 normalDx = normalMatrix*(v0.normal*baryDx.x + v1.normal*baryDx.y + v2.normal*baryDx.z);
    vec3 normalDy = normalMatrix*(v0.normal*baryDy.x + v1.normal*baryDy.y + v2.normal*baryDy.z);
    normalDx = (normalDx-interpNormal*dot(interpNormal,normalDx))/normalLength;
    normalDy = (normalDy-interpNormal*dot(interpNormal,normalDy))/normalLength;
    float variance = finiteVec3(normalDx) && finiteVec3(normalDy)
        ? min(0.18, 0.5*(dot(normalDx,normalDx)+dot(normalDy,normalDy))) : 0.0;
    matl.roughness = sqrt(clamp(matl.roughness*matl.roughness + variance, 0.045*0.045, 1.0));
    matl.anisotropic = clamp(matl.anisotropic, 0.0, 0.99);
    vec3 geometricNormal = interpNormal;
    if (matl.normalDescriptor != 0u && matl.normalScale != 0.0) {
        vec3 mapped = fetchMaterialTexture(matl.normalDescriptor, interpUV, uvDx, uvDy).xyz * 2.0 - 1.0;
        mapped.xy *= matl.normalScale;
        if (finiteVec3(mapped) && dot(mapped,mapped) > 1e-8)
            interpNormal = normalize(mat3(T,B,interpNormal) * normalize(mapped));
        T -= interpNormal * dot(T,interpNormal);
        if (dot(T,T) < 1e-10) T = cross(abs(interpNormal.z) < 0.9 ? vec3(0,0,1) : vec3(0,1,0),interpNormal);
        T = normalize(T);
        B = normalize(cross(interpNormal,T)) * tangentSign;
    }
    if ((matl.flags & 16u) != 0u && dot(geometricNormal,readVec3(pushConsts.uniformBufferIndex,36u)-interpPos) < 0.0) {
        geometricNormal = -geometricNormal;
        interpNormal = -interpNormal;
        B = -B;
    }
    surface.position = interpPos;
    surface.geometricNormal = geometricNormal;
    surface.normal = interpNormal;
    surface.tangent = T;
    surface.bitangent = B;
    surface.baseColor = max(baseColor.rgb, vec3(0));
    surface.material = matl;
    surface.objectID = inst.objectID;
    return finiteVec3(surface.baseColor);
}

vec3 viewPosition(ivec2 pixel, float depth, mat4 inverseProjection) {
    vec2 uv = (vec2(pixel) + 0.5) / vec2(pushConsts.gbufferSize);
    vec4 p = inverseProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return p.xyz / p.w;
}

// Stateless, bounded screen-space obscurance. Only depth is read for neighbours;
// no full-resolution normal, AO, filter, or temporal-history image is needed.
float ambientOcclusion(ivec2 pixel, Surface surface) {
    if ((pushConsts.featureFlags & 1u) == 0u) return 1.0;
    mat4 view = readMat4(pushConsts.uniformBufferIndex, 44u);
    mat4 projection = readMat4(pushConsts.uniformBufferIndex, 60u);
    mat4 inverseProjection = readMat4(pushConsts.uniformBufferIndex, 76u);
    vec3 p = (view * vec4(surface.position, 1)).xyz;
    vec3 n = normalize(mat3(view) * surface.geometricNormal);
    const float radius = 0.6;
    float radiusPixels = clamp(abs(projection[1][1]) * radius * float(pushConsts.gbufferSize.y) / (2.0 * max(abs(p.z), 0.01)), 1.0, 192.0);
    float occlusion = 0.0;
    // Fixed low-discrepancy disk avoids frame-to-frame noise without a history.
    const vec2 disk[12] = vec2[12](
        vec2(0.204124145,0.000000000),
        vec2(-0.260699267,0.238821884),
        vec2(0.039904202,-0.454687792),
        vec2(0.328594540,0.428593391),
        vec2(-0.603011395,-0.106664226),
        vec2(0.571225035,-0.363366609),
        vec2(-0.191063596,0.710747050),
        vec2(-0.364378996,-0.701589586),
        vec2(0.790556672,0.288710031),
        vec2(-0.822442487,0.339492301),
        vec2(0.396471627,-0.847236832),
        vec2(0.292982443,0.934074206)
    );
    for (int i = 0; i < 12; ++i) {
        ivec2 q = pixel + ivec2(round(disk[i] * radiusPixels));
        if (any(lessThan(q,ivec2(0))) || any(greaterThanEqual(q,ivec2(pushConsts.gbufferSize))) || all(equal(q,pixel))) continue;
        float d = texelFetch(textures[nonuniformEXT(pushConsts.depthImageIndex)], q, 0).r;
        if (d >= 1.0) continue;
        vec3 delta = viewPosition(q,d,inverseProjection) - p;
        float distance2 = dot(delta,delta);
        if (distance2 < 1e-8 || distance2 >= radius*radius) continue;
        float distance = sqrt(distance2);
        float horizon = max(dot(n,delta) / distance - 0.05, 0.0);
        occlusion += horizon * (1.0 - distance2/(radius*radius));
    }
    return clamp(1.0 - pushConsts.ssaoStrength * occlusion * (2.0/12.0), 0.0, 1.0);
}

vec3 shadeDirect(Surface s, float ao, vec3 viewDirection) {
    if (s.material.lightingEnabled <= 0.5) return s.baseColor + s.material.emissive;
    float shadow = computeSunShadow(s.position, s.geometricNormal);
    return s.material.emissive + DisneyBRDF(viewDirection,s.normal,s.tangent,s.bitangent,
        pushConsts.lightColor,s.baseColor,s.material,shadow,ao * s.material.occlusionStrength);
}

// Screen-space reflection using perspective-correct ray segments and depth
// crossings. Re-shade the hit from VBuffer: never read the output being written.
// This avoids colour ping-pong, a GBuffer, a Hi-Z pyramid and reflection history.
bool traceReflection(Surface source, vec3 direction, out Surface hit, out float confidence) {
    confidence = 0.0;
    mat4 view = readMat4(pushConsts.uniformBufferIndex, 44u);
    mat4 projection = readMat4(pushConsts.uniformBufferIndex, 60u);
    mat4 inverseProjection = readMat4(pushConsts.uniformBufferIndex, 76u);
    vec3 origin = source.position + source.normal * max(0.015, pushConsts.ssrThickness * 0.25);
    vec3 a = (view * vec4(origin,1)).xyz;
    vec3 ray = mat3(view) * direction;
    float distance = pushConsts.ssrMaxDistance;
    // Clip rays before the camera near plane, avoiding negative/zero clip W.
    float nearPlane = abs(viewPosition(ivec2(pushConsts.gbufferSize / 2u), 0.0, inverseProjection).z);
    if (ray.z < 0.0) distance = min(distance, (a.z - nearPlane * 1.01) / -ray.z);
    if (distance <= 0.02) return false;
    vec3 b = a + ray * distance;
    vec4 ca = projection * vec4(a,1), cb = projection * vec4(b,1);
    if (ca.w <= 1e-5 || cb.w <= 1e-5) return false;
    vec2 pa = (ca.xy / ca.w * 0.5 + 0.5) * vec2(pushConsts.gbufferSize);
    vec2 pb = (cb.xy / cb.w * 0.5 + 0.5) * vec2(pushConsts.gbufferSize);
    vec2 delta = pb - pa;
    // Clip to the viewport before budgeting steps. A ray ending near the camera
    // can project thousands of pixels outside it; sampling that invisible span
    // would skip all useful intersections in the first step.
    if (any(lessThan(pa,vec2(0))) || any(greaterThanEqual(pa,vec2(pushConsts.gbufferSize)))) return false;
    float screenEnd = 1.0;
    for (int axis=0; axis<2; ++axis) {
        if (delta[axis] > 1e-6) screenEnd = min(screenEnd,(float(pushConsts.gbufferSize[axis])-0.5-pa[axis])/delta[axis]);
        else if (delta[axis] < -1e-6) screenEnd = min(screenEnd,(0.5-pa[axis])/delta[axis]);
    }
    screenEnd = clamp(screenEnd,0.0,1.0);
    float ka = 1.0 / ca.w, kb = 1.0 / cb.w;
    vec3 qa = a * ka, qb = b * kb;
    pb = mix(pa,pb,screenEnd);
    qb = mix(qa,qb,screenEnd);
    kb = mix(ka,kb,screenEnd);
    delta = pb-pa;
    float pixels = max(abs(delta.x), abs(delta.y));
    if (pixels < 1.0) return false;
    uint steps = uint(clamp(ceil(pixels / 4.0), 1.0, float(pushConsts.ssrMaxSteps)));
    float previousT = 0.0;
    float previousZ = a.z;
    ivec2 firstPixel = ivec2(pa);
    float firstDepth = texelFetch(textures[nonuniformEXT(pushConsts.depthImageIndex)],firstPixel,0).r;
    bool previousValid = firstDepth < 1.0;
    float previousGap = a.z - viewPosition(firstPixel,firstDepth,inverseProjection).z;
    for (uint i = 1u; i <= steps; ++i) {
        float t = float(i) / float(steps);
        vec2 pixel = mix(pa,pb,t);
        if (any(lessThan(pixel,vec2(0))) || any(greaterThanEqual(pixel,vec2(pushConsts.gbufferSize)))) return false;
        ivec2 ip = ivec2(pixel);
        float z = mix(qa,qb,t).z / mix(ka,kb,t);
        float depth = texelFetch(textures[nonuniformEXT(pushConsts.depthImageIndex)],ip,0).r;
        if (depth < 1.0) {
            float sceneZ = viewPosition(ip,depth,inverseProjection).z;
            float thickness = max(pushConsts.ssrThickness, abs(sceneZ) * 0.001);
            float gap = z-sceneZ;
            bool bracketed = previousValid && ((previousGap < 0.0) != (gap < 0.0));
            if (bracketed || (max(z,previousZ) >= sceneZ && min(z,previousZ) <= sceneZ + thickness)) {
                float lo = previousT, hi = t;
                bool startsBehind = previousValid && previousGap >= 0.0;
                // Refine the first crossing, then reject silhouettes and backfaces.
                for (int j = 0; j < 5; ++j) {
                    float mid = (lo+hi)*0.5;
                    ivec2 mp = ivec2(clamp(mix(pa,pb,mid),vec2(0),vec2(pushConsts.gbufferSize)-1.0));
                    float md = texelFetch(textures[nonuniformEXT(pushConsts.depthImageIndex)],mp,0).r;
                    float mz = mix(qa,qb,mid).z / mix(ka,kb,mid);
                    float sz = viewPosition(mp,md,inverseProjection).z;
                    // Bracket the signed ray/scene depth gap. Scene depth can
                    // change faster than ray Z on a slope, including rays toward
                    // the eye; ray direction alone cannot select the half interval.
                    bool crossed = (mz >= sz) != startsBehind;
                    if (md < 1.0 && crossed) hi=mid; else lo=mid;
                }
                vec2 hp = mix(pa,pb,hi);
                ivec2 hitPixel = ivec2(hp);
                float hd = texelFetch(textures[nonuniformEXT(pushConsts.depthImageIndex)],hitPixel,0).r;
                float hz = mix(qa,qb,hi).z / mix(ka,kb,hi);
                float hitZ = viewPosition(hitPixel,hd,inverseProjection).z;
                float gap = hz - hitZ;
                thickness = max(pushConsts.ssrThickness, abs(hitZ)*0.001);
                if (hd < 1.0 && gap >= -thickness*0.25 && gap <= thickness &&
                    decodeSurface(hitPixel,hit) && dot(hit.normal,direction) < -0.05 &&
                    length(hit.position-source.position) > 0.05) {
                    vec2 uv = hp / vec2(pushConsts.gbufferSize);
                    float edge = min(min(uv.x,uv.y),min(1.0-uv.x,1.0-uv.y));
                    confidence = smoothstep(0.0,0.06,edge) * (1.0-smoothstep(0.75,1.0,hi));
                    return confidence > 0.0;
                }
            }
            previousGap = gap;
        }
        previousValid = depth < 1.0;
        previousT = t;
        previousZ = z;
    }
    return false;
}

vec3 reflection(Surface s, float ao) {
    vec3 eye = readVec3(pushConsts.uniformBufferIndex,36u);
    vec3 v = normalize(eye-s.position);
    vec3 r = reflect(-v,s.normal);
    float roughness = s.material.roughness;
    float weight = (s.material.flags & 32u) != 0u ? s.material.sheenTint : 1.0;
    vec3 f0 = mix(clamp(s.material.specularColor * (0.08 * s.material.specular),vec3(0),vec3(1))*weight,s.baseColor,s.material.metallic);
    vec3 f90 = vec3(mix(weight, 1.0, s.material.metallic));
    vec3 fresnel = mix(f0, f90, pow(1.0-max(dot(s.normal,v),0.0),5.0));
    // SH provides a bounded low-frequency fallback at screen edges and misses.
    vec3 radiance = (pushConsts.featureFlags & 4u) != 0u ? max(evalSkySH(r,pushConsts.skyIrradianceSHBufferIndex),vec3(0)) / PI : vec3(0);
    if ((pushConsts.featureFlags & 2u) != 0u && roughness < pushConsts.ssrMaxRoughness && max(fresnel.x,max(fresnel.y,fresnel.z)) > 0.02) {
        Surface hit;
        float confidence;
        if (traceReflection(s,r,hit,confidence)) {
            confidence *= 1.0-smoothstep(pushConsts.ssrMaxRoughness*0.6,pushConsts.ssrMaxRoughness,roughness);
            // The hit sends radiance back along the reflected ray, not toward
            // the main camera. This matters for specular/metallic hit materials.
            radiance = mix(radiance,shadeDirect(hit,1.0,-r),confidence);
        }
    }
    return radiance * fresnel * (1.0-0.65*roughness) * mix(1.0,ao * s.material.occlusionStrength,roughness);
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(pixel,ivec2(pushConsts.gbufferSize)))) return;
    Surface surface;
    if (!decodeSurface(pixel,surface)) {
        imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,vec4(0));
        return;
    }
    uint mode = pushConsts.outputMode;
    float ao = (mode == 0u || mode == 6u || mode == 7u) ? ambientOcclusion(pixel,surface) : 1.0;
    vec3 color;
    if (mode == 1u) color = surface.baseColor;
    else if (mode == 2u) color = surface.normal*0.5+0.5;
    else if (mode == 3u) color = fract(abs(surface.position)*0.1);
    else if (mode == 4u || mode == 5u) {
        uvec4 vis = imageLoad(imagesRG32UI[pushConsts.visibilityImageIndex],pixel);
        uint id = mode == 4u ? surface.objectID : (vis.x * 1664525u + vis.y);
        color = fract(vec3(0.1031,0.11369,0.13787)*float(id % 65521u));
    }
    else if (mode == 6u || mode == 7u) color = vec3(ao);
    else if (mode == 8u || mode == 9u) color = vec3(computeSunShadow(surface.position,surface.normal));
    else {
        vec3 eye = readVec3(pushConsts.uniformBufferIndex,36u);
        color = shadeDirect(surface,ao,normalize(eye-surface.position));
        if (surface.material.lightingEnabled > 0.5) color += reflection(surface,ao);
        if ((pushConsts.featureFlags & 8u) != 0u) color = shadowCascadeDebugColor(computeShadowCascadeIndex(surface.position));
    }
    if (!finiteVec3(color)) color=vec3(0);
    imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,vec4(clamp(color,vec3(0),vec3(65504)),1));
}
