#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

// Bindless resource pools
layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 1, binding = 0) readonly buffer SSBOPool { uint data[]; } ssbos[];
layout (set = 2, binding = 0, rgba16f) uniform image2D imagesRGBA16[];
layout (set = 2, binding = 0, rgba32ui) uniform uimage2D imagesRGBA32UI[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint visibilityImageIndex;
    uint depthImageIndex;
    uint instanceInfoBufferIndex;
    uint materialTableBufferIndex;
    uint vpBufferIndex;
    uint finalOutputImage;
    uint uniformBufferIndex;
    uint skyIrradianceSHBufferIndex;  // 9 vec3 SH coeffs of sky radiance (sky-driven ambient)
    vec3 lightColor;
    float ambientIntensity;
    vec3 sun_dir;
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

vec3 DisneyBRDF(vec3 WorldPos, vec3 Normal, vec3 Tangent, vec3 Bitangent,
    vec3 lightColor, vec3 albedo, MaterialInfo matl)
{
    vec3 N = normalize(Normal);
    vec3 X = normalize(Tangent);
    vec3 Y = normalize(Bitangent);
    vec3 eyePos = readVec3(pushConsts.uniformBufferIndex, 36u);
    vec3 V = normalize(eyePos - WorldPos);
    vec3 L = normalize(pushConsts.sun_dir);
    vec3 H = normalize(V + L);

    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 0.0);
    float ndoth = max(dot(N, H), 0.0);
    float ldoth = max(dot(L, H), 0.0);

    // Sky-driven ambient (SH9). evalSkySH returns the directional irradiance
    // E(n) from the atmospheric model; Lambertian exitance = albedo·E(n)/π.
    // The SH already carries the physical sky magnitude, so the old
    // ambientIntensity fudge (sun_intensity·0.02 ≈ 0.2) is dropped — it was
    // darkening the fill ~5× and crushing interiors.
    vec3 skyE = evalSkySH(N, pushConsts.skyIrradianceSHBufferIndex);
    // Average-sky (DC) fill. Without GI, surfaces the sky cannot reach directly
    // — down-facing, indoor, occluded — get near-zero directional irradiance and
    // read as black. Floor the irradiance at a fraction of the average sky
    // (c4·L00 = DC irradiance) so those faces keep a soft ambient base.
    vec3 skyDC = vec3(0.886227) * readVec3(pushConsts.skyIrradianceSHBufferIndex, 0u);
    vec3 irradiance = max(skyE, skyDC * 0.5);
    vec3 ambient = albedo * irradiance * (1.0 / PI);

    if (ndotl <= 0.0) return ambient;

    // --- Derived color values ---
    float Cdlum = luminance(albedo);
    vec3 Ctint = Cdlum > 0.0 ? albedo / Cdlum : vec3(1.0);
    vec3 Cspec0 = mix(matl.specular * 0.08 * mix(vec3(1.0), Ctint, matl.specularTint),
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
    vec3 F = mix(Cspec0, vec3(1.0), FH);

    // === Sheen lobe ===
    vec3 Fsheen = FH * matl.sheen * Csheen;

    // === Clearcoat lobe (fixed IOR 1.5, F0 = 0.04) ===
    float Dr = GTR1(ndoth, mix(0.1, 0.001, matl.clearcoatGloss));
    float Fr = mix(0.04, 1.0, FH);
    float Gr = SmithGGX(0.25, ndotl, ndotv);

    // === Combine all lobes ===
    vec3 diffuse = (1.0 / PI) * (mix(Fd, ss, matl.subsurface) * albedo + Fsheen)
                   * (1.0 - matl.metallic);
    vec3 specular = Ds * F * G;
    vec3 clearcoat = vec3(0.25 * matl.clearcoat * Gr * Fr * Dr);

    return ambient + (diffuse + specular + clearcoat) * lightColor * ndotl;
}

// ============================================================================
// Main — VBuffer decode + PBR in a single pass
// ============================================================================

void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y) {
        return;
    }

    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);

    // --- Read depth: skip background pixels ---
    vec2 screenUV = vec2(float(pixel.x) / float(pushConsts.gbufferSize.x),
                         float(pixel.y) / float(pushConsts.gbufferSize.y));
    float depth = texture(textures[pushConsts.depthImageIndex], screenUV).r;

    if (depth >= (1.0 - 1e-3))
    {
        imageStore(imagesRGBA16[pushConsts.finalOutputImage], pixel, vec4(0.0));
        return;
    }

    // --- Read visibility buffer ---
    uvec4 vis = imageLoad(imagesRGBA32UI[pushConsts.visibilityImageIndex], pixel);
    uint instanceID_1based = vis.r;
    uint primitiveID = vis.g;

    if (instanceID_1based == 0u)
    {
        imageStore(imagesRGBA16[pushConsts.finalOutputImage], pixel, vec4(0.0));
        return;
    }

    uint instanceID = instanceID_1based - 1u;

    // --- Load instance and material info ---
    InstanceInfo inst = loadInstanceInfo(instanceID);
    MaterialInfo matl = loadMaterialInfo(inst.materialID);

    // --- Load triangle indices (uint16 packed in uint32 SSBO) ---
    uint i0 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 0u);
    uint i1 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 1u);
    uint i2 = readIndex16(inst.indexBufferIndex, primitiveID * 3u + 2u);

    // --- Load triangle vertices ---
    Vertex v0 = loadVertex(inst.vertexBufferIndex, i0);
    Vertex v1 = loadVertex(inst.vertexBufferIndex, i1);
    Vertex v2 = loadVertex(inst.vertexBufferIndex, i2);

    // --- Transform to world space ---
    vec3 worldPos0 = (inst.modelMatrix * vec4(v0.position, 1.0)).xyz;
    vec3 worldPos1 = (inst.modelMatrix * vec4(v1.position, 1.0)).xyz;
    vec3 worldPos2 = (inst.modelMatrix * vec4(v2.position, 1.0)).xyz;

    // --- Compute screen-space positions for barycentric ---
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
        imageStore(imagesRGBA16[pushConsts.finalOutputImage], pixel, vec4(0.0));
        return;
    }

    float b0 = edgeFunction(s1, s2, pixelPos) / area;
    float b1 = edgeFunction(s2, s0, pixelPos) / area;
    float b2 = edgeFunction(s0, s1, pixelPos) / area;

    // Perspective-correct interpolation
    float inv_w0 = 1.0 / w0;
    float inv_w1 = 1.0 / w1;
    float inv_w2 = 1.0 / w2;
    float inv_w_sum = b0 * inv_w0 + b1 * inv_w1 + b2 * inv_w2;

    vec3 bary;
    bary.x = (b0 * inv_w0) / inv_w_sum;
    bary.y = (b1 * inv_w1) / inv_w_sum;
    bary.z = (b2 * inv_w2) / inv_w_sum;

    // --- Interpolate vertex attributes ---
    vec3 interpPos = bary.x * worldPos0 + bary.y * worldPos1 + bary.z * worldPos2;

    mat3 normalMatrix = transpose(inverse(mat3(inst.modelMatrix)));
    vec3 interpNormal = normalize(normalMatrix *
        (bary.x * v0.normal + bary.y * v1.normal + bary.z * v2.normal));

    vec2 interpUV = bary.x * v0.texCoord + bary.y * v1.texCoord + bary.z * v2.texCoord;

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
        T = normalize(edge1);
        B = normalize(cross(interpNormal, T));
    }
    // Re-orthogonalize
    T = normalize(T - dot(T, interpNormal) * interpNormal);
    B = normalize(cross(interpNormal, T));

    // --- Sample material ---
    vec4 baseColor = matl.materialColor;
    if (matl.textureDescriptor != 0u)
    {
        vec4 texSample = texture(textures[nonuniformEXT(matl.textureDescriptor)], interpUV);
        baseColor.rgb *= texSample.rgb;
    }

    // --- Disney Principled BRDF lighting ---
    vec3 renderResult = DisneyBRDF(interpPos, interpNormal, T, B,
        pushConsts.lightColor, baseColor.rgb, matl);
    renderResult = max(renderResult, vec3(0.01, 0.01, 0.01));

    imageStore(imagesRGBA16[pushConsts.finalOutputImage], pixel, vec4(renderResult, 1.0));
}
