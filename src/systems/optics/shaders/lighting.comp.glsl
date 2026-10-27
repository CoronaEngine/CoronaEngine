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
    float transmission; float ior;
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
    mat.transmission=(mat.flags&256u)!=0u?clamp(mat.subsurface,0,1):0;
    if((mat.flags&32u)!=0u)mat.subsurface=0;
    float sqrtF0=sqrt(clamp(mat.specular*.08,0,.9996));
    mat.ior=(1.0+sqrtF0)/max(1.0-sqrtF0,1e-4);
    if((mat.flags&32u)!=0u) {
        mat.ior=max(mat.specularTint,.001);
        mat.specularTint=0;
    }
    return mat;
}

// Imported scene expressions are immutable postfix programs. Per-image UV,
// encoding and wrapping are preserved instead of baking a repeating UV tile.
ivec2 appearanceTexel(ivec2 p, ivec2 size, uint wrap) {
    if (wrap == 1u) return clamp(p,ivec2(0),size-1);
    if (wrap == 2u) {
        p=((p%(2*size))+2*size)%(2*size);
        return min(p,2*size-1-p);
    }
    return ((p%size)+size)%size;
}
vec3 appearancePixel(uint tex, ivec2 p, ivec2 size, uint wrap) {
    if (wrap==3u && (any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,size)))) return vec3(0);
    return texelFetch(textures[nonuniformEXT(tex)],appearanceTexel(p,size,wrap),0).rgb;
}
void evaluateAppearance(inout MaterialInfo matl, vec2 uv, out vec3 color, out vec3 normalMap) {
    uint buf=matl.occlusionDescriptor;
    vec3 values[128];
    uint count=min(readUint(buf,0u),128u);
    for (uint i=0u;i<count;++i) {
        uint b=16u+i*24u;
        uint op=readUint(buf,b);
        uint a=readUint(buf,b+1u), c=readUint(buf,b+2u), t=readUint(buf,b+3u);
        vec3 value=readVec3(buf,b+4u);
        vec2 q=vec2(dot(readVec3(buf,b+8u),vec3(uv,1)),dot(readVec3(buf,b+11u),vec3(uv,1)));
        if (op==1u) {
            uint tex=readUint(buf,b+7u),wrap=readUint(buf,b+17u);
            ivec2 size=textureSize(textures[nonuniformEXT(tex)],0);
            vec2 p=q*vec2(size)-0.5;
            ivec2 at=ivec2(floor(p));vec2 f=fract(p);
            if (readUint(buf,b+18u)==1u) value=appearancePixel(tex,ivec2(floor(q*vec2(size))),size,wrap);
            else value=mix(mix(appearancePixel(tex,at,size,wrap),appearancePixel(tex,at+ivec2(1,0),size,wrap),f.x),
                mix(appearancePixel(tex,at+ivec2(0,1),size,wrap),appearancePixel(tex,at+ivec2(1,1),size,wrap),f.x),f.y);
        } else if (op==2u) value=values[a]*values[c];
        else if (op==3u) value=mix(values[a],values[c],values[t].x);
        else if (op==4u) value=sqrt(max(values[a],vec3(0)));
        else if (op==5u) {
            ivec2 cell=ivec2(floor(q*readFloat(buf,b+19u)));
            value=((cell.x+cell.y)&1)==0 ? values[a]:values[c];
        }
        values[i]=value;
    }
    color=values[readUint(buf,1u)];matl.roughness=values[readUint(buf,2u)].x;
    matl.metallic=values[readUint(buf,3u)].x;matl.sheenTint=values[readUint(buf,4u)].x;
    matl.emissive=values[readUint(buf,5u)];normalMap=values[readUint(buf,6u)];
    matl.clearcoat=values[readUint(buf,7u)].x;matl.clearcoatGloss=1.0-values[readUint(buf,8u)].x;
    matl.normalDescriptor=readUint(buf,9u); // boolean for an expression normal, not a texture descriptor
    matl.transmission=clamp(values[readUint(buf,10u)].x,0,1);
    matl.ior=clamp(values[readUint(buf,11u)].x,.001,10000);
    matl.occlusionDescriptor=0u;
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

float dielectricFresnel(float cosine,float etaI,float etaT) {
    float c=clamp(abs(cosine),0,1),s2=(etaI/etaT)*(etaI/etaT)*(1-c*c);
    if(s2>=1)return 1;
    float t=sqrt(max(0,1-s2));
    float rs=(etaI*c-etaT*t)/max(etaI*c+etaT*t,1e-6);
    float rp=(etaT*c-etaI*t)/max(etaT*c+etaI*t,1e-6);
    return .5*(rs*rs+rp*rp);
}

vec3 DisneyBRDF(vec3 V, vec3 Normal, vec3 Tangent, vec3 Bitangent, vec3 L,
    vec3 lightColor, vec3 albedo, MaterialInfo matl, float shadowFactor)
{
    vec3 N = normalize(Normal);
    vec3 X = normalize(Tangent);
    vec3 Y = normalize(Bitangent);
    L = normalize(L);
    vec3 H = V + L;
    H *= inversesqrt(max(dot(H,H), 1e-12));

    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 0.0);
    float ndoth = max(dot(N, H), 0.0);
    float ldoth = max(dot(L, H), 0.0);

    bool importedMR = (matl.flags & 32u) != 0u;
    float specularWeight = importedMR ? matl.sheenTint : 1.0;
    vec3 dielectricF0 = clamp(matl.specularColor * matl.specular * 0.08, vec3(0), vec3(1)) * specularWeight;
    if (ndotl <= 0.0 || ndotv <= 0.0) return vec3(0);

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
    // Glass retains its reflection lobe on either side of the interface.
    // shadeDirect supplies etaT/etaI for the viewing side, including eta < 1.
    float glassWeight=matl.transmission*(1.0-matl.metallic);
    F=mix(F,vec3(dielectricFresnel(ldoth,1.0,matl.ior)),glassWeight);

    // === Sheen lobe ===
    vec3 Fsheen = FH * matl.sheen * Csheen;

    // === Clearcoat lobe (fixed IOR 1.5, F0 = 0.04) ===
    float Dr = GTR1(ndoth, mix(0.1, 0.001, matl.clearcoatGloss));
    float Fr = mix(0.04, 1.0, FH);
    float Gr = SmithGGX(0.25, ndotl, ndotv);

    // === Combine all lobes ===
    vec3 diffuse = (1.0 / PI) * (mix(Fd, ss, matl.subsurface) * albedo + Fsheen)
                   * (1.0 - matl.metallic) * (1.0 - matl.transmission);
    if (importedMR) {
        vec3 dielectricF = mix(dielectricF0, vec3(specularWeight), FH);
        diffuse *= 1.0 - max(dielectricF.r, max(dielectricF.g, dielectricF.b));
    }
    vec3 specular = Ds * F * G;
    vec3 clearcoat = vec3(0.25 * matl.clearcoat * Gr * Fr * Dr);

    return (diffuse + specular + clearcoat) * lightColor * ndotl * shadowFactor;
}

struct Surface {
    vec3 position;
    vec3 emissionNormal;
    vec3 outwardNormal;
    vec3 geometricNormal;
    vec3 normal;
    vec3 tangent;
    vec3 bitangent;
    vec3 baseColor;
    MaterialInfo material;
    uint objectID;
};

bool decodeSurfaceFrom(ivec2 pixel, uint visibilityIndex, bool geometryOnly, out Surface surface)
{
    // --- Read visibility buffer ---
    uvec4 vis = imageLoad(imagesRG32UI[visibilityIndex], pixel);
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
    mat4 viewProjMatrix = readMat4(pushConsts.uniformBufferIndex,60u)*readMat4(pushConsts.uniformBufferIndex,44u);
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

    // Exit normals and opaque depth hits need no material expression evaluation,
    // texture fetches, tangent frames or normal derivatives. In particular, do
    // not evaluate a 128-node material program at every exit refinement.
    if (geometryOnly) {
        surface.position=interpPos;
        surface.normal=interpNormal;
        surface.outwardNormal=interpNormal;
        surface.objectID=inst.objectID;
        if ((matl.flags&16u)!=0u && dot(surface.normal,readVec3(pushConsts.uniformBufferIndex,36u)-interpPos)<0)
            surface.normal=-surface.normal;
        surface.geometricNormal=surface.normal;
        surface.material=matl;
        return finiteVec3(interpPos) && finiteVec3(interpNormal);
    }

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
    vec3 expressionNormal=vec3(0.5,0.5,1);
    if ((matl.flags & 64u)!=0u) evaluateAppearance(matl,interpUV,baseColor.rgb,expressionNormal);
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
        vec3 mapped = ((matl.flags & 64u)!=0u ? expressionNormal :
            fetchMaterialTexture(matl.normalDescriptor, interpUV, uvDx, uvDy).xyz) * 2.0 - 1.0;
        mapped.xy *= matl.normalScale;
        if (finiteVec3(mapped) && dot(mapped,mapped) > 1e-8)
            interpNormal = normalize(mat3(T,B,interpNormal) * normalize(mapped));
        T -= interpNormal * dot(T,interpNormal);
        if (dot(T,T) < 1e-10) T = cross(abs(interpNormal.z) < 0.9 ? vec3(0,0,1) : vec3(0,1,0),interpNormal);
        T = normalize(T);
        B = normalize(cross(interpNormal,T)) * tangentSign;
    }
    surface.outwardNormal=interpNormal;
    if ((matl.flags & 16u) != 0u && dot(geometricNormal,readVec3(pushConsts.uniformBufferIndex,36u)-interpPos) < 0.0) {
        geometricNormal = -geometricNormal;
        interpNormal = -interpNormal;
        B = -B;
    }
    surface.position = interpPos;
    surface.emissionNormal = normalize(normalMatrix*v0.normal);
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
bool decodeSurface(ivec2 pixel,out Surface surface) {
    return decodeSurfaceFrom(pixel,pushConsts.visibilityImageIndex,false,surface);
}

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
        if (d <= 0.0) continue;
        vec3 delta = viewPosition(q,d,inverseProjection) - p;
        float distance2 = dot(delta,delta);
        if (distance2 < 1e-8 || distance2 >= radius*radius) continue;
        float distance = sqrt(distance2);
        float horizon = max(dot(n,delta) / distance - 0.05, 0.0);
        occlusion += horizon * (1.0 - distance2/(radius*radius));
    }
    return clamp(1.0 - pushConsts.ssaoStrength * occlusion * (2.0/12.0), 0.0, 1.0);
}

// Link-aware interpolation stays on a connected side of the receiver surface.
float lpvEdge(uint info,uint level,ivec3 p,int axis,int sign,int n) {
    if(sign<0)p[axis]-=1;
    if(any(lessThan(p,ivec3(0)))||any(greaterThanEqual(p,ivec3(n))))return 0;
    uint cell=level*uint(n*n*n)+uint((p.z*n+p.y)*n+p.x);
    return clamp(readFloat(info,256u+cell*208u+196u+uint(axis)*4u),0,1);
}
float lpvConnection(uint info,uint level,ivec3 a,ivec3 b,int n) {
    if(all(equal(a,b)))return 1;
    const ivec3 orders[6]=ivec3[6](ivec3(0,1,2),ivec3(0,2,1),ivec3(1,0,2),ivec3(1,2,0),ivec3(2,0,1),ivec3(2,1,0));
    float best=0;
    for(int order=0;order<6;++order){ivec3 p=a;float visibility=1;
        for(int k=0;k<3;++k){int axis=orders[order][k],sign=b[axis]>p[axis]?1:-1;
            if(p[axis]!=b[axis]){visibility*=1-lpvEdge(info,level,p,axis,sign,n);p[axis]+=sign;}}
        best=max(best,visibility);
        // Every link transmission is in [0,1]. Once an unobstructed path is
        // found no remaining permutation can improve it (the result is exact).
        if(best>=1)return 1;
    }
    return best;
}
vec3 sampleLpvLevel(uint level,vec3 position,vec3 normal,vec3 direction,bool diffuse) {
    vec3 query=-direction;float xq=query.x,yq=query.y,zq=query.z;
    float basis[9]=float[9](.282094792,.488602512*xq,.488602512*yq,.488602512*zq,
        1.092548431*xq*yq,1.092548431*yq*zq,.315391565*(3*zq*zq-1),
        1.092548431*xq*zq,.546274215*(xq*xq-yq*yq));
    if(diffuse){basis[0]*=PI;for(int k=1;k<4;++k)basis[k]*=2*PI/3;for(int k=4;k<9;++k)basis[k]*=PI/4;}
    uint info=pushConsts.directLightingBufferIndex;
    int n=int(readUint(info,0u));float size=readFloat(info,227u+level*4u);
    uint radiance=readUint(pushConsts.shadowInfoBufferIndex,75u);
    vec3 origin=readVec3(info,224u+level*4u),samplePosition=position+normal*size*.25;
    vec3 q=(samplePosition-origin)/size-.5,t=fract(q);ivec3 base=ivec3(floor(q));
    float weights[8];float best=-1;ivec3 anchor=base;
    for(int corner=0;corner<8;++corner){ivec3 d=ivec3(corner&1,(corner>>1)&1,(corner>>2)&1),p=base+d;
        vec3 w=mix(vec3(1)-t,t,vec3(d));float value=w.x*w.y*w.z;
        if(any(lessThan(p,ivec3(0)))||any(greaterThanEqual(p,ivec3(n)))||dot(origin+(vec3(p)+.5)*size-position,normal)<=size*1e-5)value=0;
        weights[corner]=value;if(value>best){best=value;anchor=p;}
    }
    vec3 result=vec3(0);float weight=0;
    for(int corner=0;corner<8;++corner){if(weights[corner]<=0)continue;
        ivec3 p=base+ivec3(corner&1,(corner>>1)&1,(corner>>2)&1);
        float w=weights[corner]*lpvConnection(info,level,anchor,p,n);uint cell=level*uint(n*n*n)+uint((p.z*n+p.y)*n+p.x);
        for(uint c=0u;c<3u;++c){float value=0;for(uint k=0u;k<9u;++k)value+=readFloat(radiance,cell*27u+c*9u+k)*basis[k];result[c]+=w*max(value,0);}
        weight+=w;
    }
    return weight>1e-10?result/weight:vec3(0);
}
vec3 sampleLpv(vec3 position,vec3 normal,vec3 direction,bool diffuse) {
    uint info=pushConsts.directLightingBufferIndex;
    float n=float(readUint(info,0u));uint levels=readUint(info,12u);
    // Blend radiance, never add overlapping levels. The four-cell overlap is
    // wider than the interpolation footprint and fades before a child boundary.
    vec3 result=vec3(0);float remaining=1;
    for(int candidate=int(levels)-1;candidate>=0;--candidate) {
        uint level=uint(candidate);
        vec3 q=(position-readVec3(info,224u+level*4u))/readFloat(info,227u+level*4u);
        vec3 edge=min(q,vec3(n)-q);
        float weight=level==0u?1:smoothstep(1.5,4.5,min(edge.x,min(edge.y,edge.z)));
        if(weight>0)result+=remaining*weight*sampleLpvLevel(level,position,normal,direction,diffuse);
        remaining*=1-weight;if(remaining<1e-5)break;
    }
    return result;
}
vec3 lpvIrradiance(vec3 position,vec3 normal) {
    return sampleLpv(position,normal,normal,true);
}
vec3 shadeDirectWithIrradiance(Surface s,float ao,vec3 viewDirection,bool includeEmission,out vec3 irradiance) {
    irradiance=vec3(0);
    vec3 emission=includeEmission && ((s.material.flags&128u)!=0u || dot(s.emissionNormal,viewDirection)>0)?s.material.emissive:vec3(0);
    if(s.material.lightingEnabled<=.5)return s.baseColor+emission;
    if(s.material.transmission>0 && dot(viewDirection,s.outwardNormal)<0)
        s.material.ior=1.0/s.material.ior;
    vec3 color=emission+DisneyBRDF(viewDirection,s.normal,s.tangent,s.bitangent,pushConsts.sun_dir,
        pushConsts.lightColor,s.baseColor,s.material,computeSunShadow(s.position,s.geometricNormal));
    irradiance=lpvIrradiance(s.position,s.geometricNormal);
    color+=s.baseColor*(1.0-s.material.metallic)*(1.0-s.material.transmission)*irradiance/PI*ao*s.material.occlusionStrength;
    return color;
}
vec3 shadeDirect(Surface s,float ao,vec3 viewDirection,bool includeEmission) {
    vec3 irradiance;
    return shadeDirectWithIrradiance(s,ao,viewDirection,includeEmission,irradiance);
}

// Screen-space reflection using perspective-correct ray segments and depth
// crossings. Re-shade the hit from VBuffer: never read the output being written.
// This avoids colour ping-pong, a GBuffer, a Hi-Z pyramid and reflection history.
float screenConfidence(vec2 uv) {
    float edge=min(min(uv.x,uv.y),min(1-uv.x,1-uv.y));
    return smoothstep(.02,.15,edge);
}
bool traceScreen(Surface source, vec3 direction, uint depthIndex, uint visibilityIndex, bool transmitting, out Surface hit, out float confidence) {
    confidence = 0.0;
    mat4 view = readMat4(pushConsts.uniformBufferIndex, 44u);
    mat4 projection = readMat4(pushConsts.uniformBufferIndex, 60u);
    mat4 inverseProjection = readMat4(pushConsts.uniformBufferIndex, 76u);
    vec3 origin = source.position + (transmitting?direction:source.normal) * max(0.001, pushConsts.ssrThickness * 0.25);
    vec3 a = (view * vec4(origin,1)).xyz;
    vec3 ray = mat3(view) * direction;
    float distance = transmitting ? max(pushConsts.ssrMaxDistance,readFloat(pushConsts.directLightingBufferIndex,7u)*float(readUint(pushConsts.directLightingBufferIndex,0u))*2.0) : pushConsts.ssrMaxDistance;
    // Clip rays before the camera near plane, avoiding negative/zero clip W.
    float nearPlane = abs(viewPosition(ivec2(pushConsts.gbufferSize / 2u), 1.0, inverseProjection).z);
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
    if (pixels < 1.0) {
        // A normally incident transmitted ray may stay in one screen pixel.
        ivec2 ip=ivec2(pa);
        float depth=texelFetch(textures[nonuniformEXT(depthIndex)],ip,0).r;
        float z=viewPosition(ip,depth,inverseProjection).z;
        float along=abs(ray.z)>1e-7?(z-a.z)/ray.z:-1;
        if(transmitting && depth>0 && along>0 && along<distance && decodeSurfaceFrom(ip,visibilityIndex,true,hit)) {
            confidence=screenConfidence(pa/vec2(pushConsts.gbufferSize));return confidence>0;
        }
        return false;
    }
    uint steps = uint(clamp(ceil(pixels / 4.0), 1.0, float(pushConsts.ssrMaxSteps)));
    float previousT = 0.0;
    float previousZ = a.z;
    ivec2 firstPixel = ivec2(pa);
    float firstDepth = texelFetch(textures[nonuniformEXT(depthIndex)],firstPixel,0).r;
    bool previousValid = firstDepth > 0.0;
    float previousGap = a.z - viewPosition(firstPixel,firstDepth,inverseProjection).z;
    for (uint i = 1u; i <= steps; ++i) {
        float t = float(i) / float(steps);
        vec2 pixel = mix(pa,pb,t);
        if (any(lessThan(pixel,vec2(0))) || any(greaterThanEqual(pixel,vec2(pushConsts.gbufferSize)))) return false;
        ivec2 ip = ivec2(pixel);
        float z = mix(qa,qb,t).z / mix(ka,kb,t);
        float depth = texelFetch(textures[nonuniformEXT(depthIndex)],ip,0).r;
        if (depth > 0.0) {
            float sceneZ = viewPosition(ip,depth,inverseProjection).z;
            float thickness = max(pushConsts.ssrThickness, abs(sceneZ) * 0.001);
            float gap = z-sceneZ;
            bool bracketed = previousValid && ((previousGap < 0.0) != (gap < 0.0));
            if (bracketed || (max(z,previousZ) >= sceneZ && min(z,previousZ) <= sceneZ + thickness)) {
                float lo = previousT, hi = t;
                bool startsBehind = previousValid && previousGap >= 0.0;
                // Refine the first crossing, then reject silhouettes and backfaces.
                // A parallel glass slab can project a long world-space ray to
                // only a few pixels. Five bisections leave depth errors larger
                // than the hit tolerance and cause alternating environment stripes.
                for (int j = 0; j < (transmitting ? 12 : 5); ++j) {
                    float mid = (lo+hi)*0.5;
                    ivec2 mp = ivec2(clamp(mix(pa,pb,mid),vec2(0),vec2(pushConsts.gbufferSize)-1.0));
                    float md = texelFetch(textures[nonuniformEXT(depthIndex)],mp,0).r;
                    float mz = mix(qa,qb,mid).z / mix(ka,kb,mid);
                    float sz = viewPosition(mp,md,inverseProjection).z;
                    // Bracket the signed ray/scene depth gap. Scene depth can
                    // change faster than ray Z on a slope, including rays toward
                    // the eye; ray direction alone cannot select the half interval.
                    bool crossed = (mz >= sz) != startsBehind;
                    if (md > 0.0 && crossed) hi=mid; else lo=mid;
                }
                vec2 hp = mix(pa,pb,hi);
                ivec2 hitPixel = ivec2(hp);
                float hd = texelFetch(textures[nonuniformEXT(depthIndex)],hitPixel,0).r;
                float hz = mix(qa,qb,hi).z / mix(ka,kb,hi);
                float hitZ = viewPosition(hitPixel,hd,inverseProjection).z;
                float gap = hz - hitZ;
                thickness = max(pushConsts.ssrThickness, abs(hitZ)*0.001);
                if (hd > 0.0 && gap >= -thickness*0.25 && gap <= thickness &&
                    decodeSurfaceFrom(hitPixel,visibilityIndex,transmitting,hit) && dot(hit.normal,direction) < -0.05 &&
                    length(hit.position-source.position) > 0.05) {
                    vec2 uv = hp / vec2(pushConsts.gbufferSize);
                    confidence = screenConfidence(uv) * (1.0-smoothstep(0.75,1.0,hi));
                    return confidence > 0.0;
                }
            }
            previousGap = gap;
        }
        previousValid = depth > 0.0;
        previousT = t;
        previousZ = z;
    }
    return false;
}

// Screen samples replace the corresponding LPV radiance instead of adding a
// second GI estimate. A miss has exactly zero correction, including offscreen.
vec3 screenGiCorrection(ivec2 pixel,Surface receiver) {
    if(receiver.material.transmission>0 || receiver.material.metallic>.95)return vec3(0);
    float edge=screenConfidence((vec2(pixel)+.5)/vec2(pushConsts.gbufferSize));
    if(edge<=0)return vec3(0);
    vec3 n=receiver.geometricNormal;
    vec3 tangent=normalize(cross(abs(n.y)<.95?vec3(0,1,0):vec3(1,0,0),n));
    vec3 bitangent=cross(n,tangent),correction=vec3(0);
    // Fixed sample count and deterministic rotation, shared by every scene.
    float rotation=fract(sin(dot(vec2(pixel),vec2(12.9898,78.233)))*43758.5453)*2*PI;
    for(int sampleIndex=0;sampleIndex<32;++sampleIndex) {
        float r=sqrt((float(sampleIndex)+.5)/32),phi=float(sampleIndex)*2.39996323+rotation;
        vec3 direction=normalize(tangent*(r*cos(phi))+bitangent*(r*sin(phi))+n*sqrt(1-r*r));
        Surface hit;float confidence;
        if(traceScreen(receiver,direction,pushConsts.depthImageIndex,pushConsts.visibilityImageIndex,false,hit,confidence) &&
           hit.material.transmission<=0 && hit.material.metallic<.95 &&
           max(hit.material.emissive.x,max(hit.material.emissive.y,hit.material.emissive.z))<=0) {
            vec3 coarse=sampleLpv(receiver.position,n,direction,false);
            // Emitters already enter the LPV directly. Sampling tiny bright
            // emitters with a diffuse screen estimator introduces fireflies;
            // keep their LPV estimate and refine reflected light only.
            vec3 detailed=shadeDirect(hit,1,-direction,false);
            correction+=confidence*(detailed-coarse);
        }
    }
    return correction*(PI/32)*edge;
}
vec3 filteredScreenGi(ivec2 pixel,Surface receiver) {
    uint descriptor=readUint(pushConsts.shadowInfoBufferIndex,89u);
    float confidence=screenConfidence((vec2(pixel)+.5)/vec2(pushConsts.gbufferSize));
    if(confidence<=0)return vec3(0);
    // The footprint grows smoothly toward the edge; normal/plane weights keep
    // foreground and background from bleeding across silhouettes.
    float stride=mix(8.,2.,confidence),sum=0;
    float distanceToEye=length(receiver.position-readVec3(pushConsts.uniformBufferIndex,36u));
    float tolerance=max(distanceToEye*.02,readFloat(pushConsts.directLightingBufferIndex,7u)*.01);
    vec3 result=vec3(0);
    for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x) {
        ivec2 p=pixel+ivec2(round(vec2(x,y)*stride));
        if(any(lessThan(p,ivec2(0)))||any(greaterThanEqual(p,ivec2(pushConsts.gbufferSize))))continue;
        p=(p/2)*2;
        Surface neighbor;
        if(!decodeSurfaceFrom(p,pushConsts.visibilityImageIndex,true,neighbor))continue;
        // Bilateral weights use position and the geometric normal only. The
        // same conservative flag used by the opaque raster marks any possible
        // transmission, including expression textures; evaluate those fully.
        if((neighbor.material.flags&256u)!=0u) {
            if(!decodeSurface(p,neighbor) || neighbor.material.transmission>0)continue;
        }
        float normalWeight=pow(max(dot(receiver.geometricNormal,neighbor.geometricNormal),0),16);
        float plane=abs(dot(neighbor.position-receiver.position,receiver.geometricNormal));
        float weight=exp(-float(x*x+y*y)/4.-plane/max(tolerance,1e-6))*normalWeight;
        result+=weight*imageLoad(imagesRGBA16[nonuniformEXT(descriptor)],p).rgb;sum+=weight;
    }
    return sum>0?result/sum*confidence:vec3(0);
}

// Canonical Corona lat-long orientation, shared with sky.comp and Vision.
vec3 refractionEnvironment(vec3 direction,Surface source) {
    if((pushConsts.featureFlags&16u)!=0u) {
        uint tex=readUint(pushConsts.directLightingBufferIndex,8u);
        float phi=atan(-direction.z,direction.x);if(phi<0)phi+=2.0*PI;
        return max(textureLod(textures[nonuniformEXT(tex)],vec2(phi/(2.0*PI),acos(clamp(direction.y,-1,1))/PI),0).rgb,vec3(0));
    }
    return sampleLpv(source.position,source.geometricNormal,direction,false);
}
bool screenPixel(vec3 world,out ivec2 pixel) {
    vec4 clip=readMat4(pushConsts.uniformBufferIndex,60u)*readMat4(pushConsts.uniformBufferIndex,44u)*vec4(world,1);
    if(clip.w<=1e-6)return false;
    vec2 uv=clip.xy/clip.w*.5+.5;
    if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1))))return false;
    pixel=ivec2(uv*vec2(pushConsts.gbufferSize));return true;
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
    float glassWeight=s.material.transmission*(1.0-s.material.metallic);
    if(glassWeight>0) {
        bool entering=dot(v,s.outwardNormal)>0;
        float f=dielectricFresnel(dot(v,s.outwardNormal),entering?1:s.material.ior,entering?s.material.ior:1);
        fresnel=mix(fresnel,vec3(f),glassWeight);
    }
    // SH radiance is a low-frequency, geometry-occluded fallback for SSR misses
    // and rough metals. Keeping the receiver offset along its geometric normal
    // avoids sampling the other side of a wall when the reflection is grazing.
    // SSR replaces this approximation when an actual screen-space hit exists.
    vec3 radiance=sampleLpv(s.position,s.geometricNormal,r,false);
    if(glassWeight>0)radiance=mix(radiance,refractionEnvironment(r,s),glassWeight);
    if ((pushConsts.featureFlags & 2u) != 0u && roughness < pushConsts.ssrMaxRoughness &&
        (glassWeight>0 || max(fresnel.x,max(fresnel.y,fresnel.z)) > 0.02)) {
        Surface hit;
        float confidence;
        // A foreground glass pane must not hide an opaque reflection target
        // from the depth search. The auxiliary raster already reveals it; no
        // recursive glass shading or scene intersection is needed.
        bool opaqueSearch=glassWeight>0 && (pushConsts.featureFlags&32u)!=0u;
        uint depth=opaqueSearch?readUint(pushConsts.shadowInfoBufferIndex,85u):pushConsts.depthImageIndex;
        uint visibility=opaqueSearch?readUint(pushConsts.shadowInfoBufferIndex,84u):pushConsts.visibilityImageIndex;
        if (traceScreen(s,r,depth,visibility,false,hit,confidence) && hit.material.transmission<=0.0) {
            confidence *= 1.0-smoothstep(pushConsts.ssrMaxRoughness*0.6,pushConsts.ssrMaxRoughness,roughness);
            // The hit sends radiance back along the reflected ray, not toward
            // the main camera. This matters for specular/metallic hit materials.
            radiance = mix(radiance,shadeDirect(hit,1.0,-r,true),confidence);
        }
    }
    // Roughness changes the angular distribution, not the energy of a uniform
    // environment. Do not apply the legacy metal/opaque SSR attenuation or SSAO
    // darkening to the glass reflection lobe.
    float attenuation=(1.0-0.65*roughness)*mix(1.0,ao*s.material.occlusionStrength,roughness);
    return radiance*fresnel*mix(attenuation,1.0,glassWeight);
}

// Wyman 2005, section 3: two image-space interfaces. We use rasterized
// front/furthest-surface distance instead of the optional precomputed dN.
// Unlike the paper's environment-only result, the exiting ray first searches
// the immutable opaque depth/color buffers for nearby scene geometry.
vec3 refraction(ivec2 pixel,Surface source) {
    float transmission=source.material.transmission*(1-source.material.metallic);
    if(transmission<=0 || (pushConsts.featureFlags&32u)==0u)return vec3(0);
    vec3 eye=readVec3(pushConsts.uniformBufferIndex,36u);
    vec3 incident=normalize(source.position-eye),normal=source.outwardNormal;
    bool entering=dot(incident,normal)<0;
    float ior=source.material.ior;
    vec3 facing=entering?normal:-normal;
    vec3 direction=refract(incident,facing,entering?1.0/ior:ior);
    float frontF=dielectricFresnel(dot(-incident,facing),entering?1:ior,entering?ior:1);
    if(dot(direction,direction)<1e-10)return vec3(0); // entry TIR is reflected by reflection().
    vec3 position=source.position;
    float exitF=0;
    vec3 internalReflection=vec3(0);
    Surface back;
    if(entering && decodeSurfaceFrom(pixel,readUint(pushConsts.shadowInfoBufferIndex,86u),true,back) && back.objectID==source.objectID) {
        float d=length(back.position-source.position);
        vec3 estimate=position+direction*d;
        Surface candidate;
        ivec2 projected;
        // Bounded local plane refinement; no scene triangles or acceleration structure.
        for(int iteration=0;iteration<2;++iteration) {
            if(!screenPixel(estimate,projected) || !decodeSurfaceFrom(projected,readUint(pushConsts.shadowInfoBufferIndex,86u),true,candidate) || candidate.objectID!=source.objectID)break;
            vec3 n=candidate.outwardNormal;
            float denom=dot(direction,n);
            if(denom<=.02)break;
            float distance=dot(candidate.position-source.position,n)/denom;
            if(distance<=0 || distance>d*2.0)break;
            back=candidate;estimate=source.position+direction*distance;
        }
        position=estimate;
        vec3 exitNormal=back.outwardNormal;
        if(dot(direction,exitNormal)<0)exitNormal=-exitNormal;
        exitF=dielectricFresnel(dot(direction,exitNormal),ior,1);
        // The rejected exit energy is reflection, not absorption. Keep it in
        // the same bounded environment/LPV fallback used for internal TIR.
        // This does not trace another interface or claim a nested-medium path.
        internalReflection=refractionEnvironment(reflect(direction,-exitNormal),source);
        vec3 outgoing=refract(direction,-exitNormal,ior);
        if(dot(outgoing,outgoing)<1e-10) {
            // Bounded TIR fallback: preserve reflected energy, without claiming
            // recursive interior paths or clamping the critical angle.
            return transmission*(1-frontF)*clamp(source.baseColor,vec3(0),vec3(1))*internalReflection;
        }
        direction=normalize(outgoing);
    }
    Surface start=source;start.position=position;
    Surface hit;float confidence;
    vec3 color=refractionEnvironment(direction,source);
    if(traceScreen(start,direction,readUint(pushConsts.shadowInfoBufferIndex,85u),readUint(pushConsts.shadowInfoBufferIndex,84u),true,hit,confidence)) {
        ivec2 hp;
        if(screenPixel(hit.position,hp))color=mix(color,imageLoad(imagesRGBA16[readUint(pushConsts.shadowInfoBufferIndex,88u)],hp).rgb,confidence);
    }
    vec3 transported=max(color,vec3(0))*(1-exitF)+internalReflection*exitF;
    return transported*clamp(source.baseColor,vec3(0),vec3(1))*transmission*(1-frontF);
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(pixel,ivec2(pushConsts.gbufferSize)))) return;
    uint mode = pushConsts.outputMode;
    if((pushConsts.featureFlags&128u)!=0u) {
        Surface receiver;vec3 correction=vec3(0);
        if(all(equal(pixel&ivec2(1),ivec2(0))) && decodeSurface(pixel,receiver))correction=screenGiCorrection(pixel,receiver);
        if(!finiteVec3(correction))correction=vec3(0);
        imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,vec4(clamp(correction,vec3(-65504),vec3(65504)),1));
        return;
    }
    if (mode==0u && (pushConsts.featureFlags&40u)==32u) {
        // The opaque resolve already shaded this exact surface. Reuse it in
        // the final pass instead of repeating LPV, SSAO, SSR and material work.
        uint visible=imageLoad(imagesRG32UI[pushConsts.visibilityImageIndex],pixel).r;
        if(visible>0u && visible<=pushConsts.instanceCount) {
            uint material=loadInstanceInfo(visible-1u).materialID;
            if(material<pushConsts.materialCount && (loadMaterialInfo(material).flags&256u)==0u) {
                imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,
                    imageLoad(imagesRGBA16[readUint(pushConsts.shadowInfoBufferIndex,88u)],pixel));
                return;
            }
        }
    }
    // Geometry diagnostics must not depend on normals, UVs or materials being
    // decodable. Otherwise a valid rasterized triangle with an invalid shading
    // attribute appears to be missing even in the object/visibility views.
    if (mode == 4u || mode == 5u) {
        uvec4 vis = imageLoad(imagesRG32UI[pushConsts.visibilityImageIndex],pixel);
        vec3 color = vec3(0);
        if (vis.x != 0u && vis.x <= pushConsts.instanceCount) {
            uint id = mode == 4u ? loadInstanceInfo(vis.x-1u).objectID : (vis.x*1664525u+vis.y);
            color = fract(vec3(0.1031,0.11369,0.13787)*float(id % 65521u));
        }
        imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,vec4(color,1));
        return;
    }
    Surface surface;
    if (!decodeSurface(pixel,surface)) {
        imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,vec4(0));
        return;
    }
    float ao = (mode == 0u || mode == 6u || mode == 7u) ? ambientOcclusion(pixel,surface) : 1.0;
    vec3 color;
    if (mode == 1u) color = surface.baseColor;
    else if (mode == 2u) color = surface.normal*0.5+0.5;
    else if (mode == 3u) color = fract(abs(surface.position)*0.1);
    else if (mode == 6u || mode == 7u) color = vec3(ao);
    else if (mode == 8u || mode == 9u) color = vec3(computeSunShadow(surface.position,surface.normal));
    else {
        vec3 eye = readVec3(pushConsts.uniformBufferIndex,36u);
        // A dielectric reflects from inside as well as outside. Face only the
        // shading frame; retain outwardNormal for Snell/Fresnel medium selection
        // and retain the original geometry in diagnostic outputs.
        if(surface.material.transmission>0 && dot(surface.geometricNormal,eye-surface.position)<0) {
            surface.geometricNormal=-surface.geometricNormal;
            surface.normal=-surface.normal;
            surface.bitangent=-surface.bitangent;
        }
        vec3 coarse;
        color = shadeDirectWithIrradiance(surface,ao,normalize(eye-surface.position),true,coarse);
        if (surface.material.lightingEnabled > 0.5) {
            if((pushConsts.featureFlags&64u)!=0u && surface.material.transmission<=0) {
                vec3 correction=max(coarse+filteredScreenGi(pixel,surface),vec3(0))-coarse;
                color+=surface.baseColor*(1-surface.material.metallic)*correction/PI*ao*surface.material.occlusionStrength;
            }
            color += reflection(surface,ao);
            color += refraction(pixel,surface);
        }
        if ((pushConsts.featureFlags & 8u) != 0u) color = shadowCascadeDebugColor(computeShadowCascadeIndex(surface.position));
    }
    if (!finiteVec3(color)) color=vec3(0);
    imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,vec4(clamp(color,vec3(0),vec3(65504)),1));
}
