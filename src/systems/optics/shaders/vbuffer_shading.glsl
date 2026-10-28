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

#include "lpv_sample.glsl"
vec3 shadeDirectWithIrradiance(Surface s,float ao,vec3 viewDirection,bool includeEmission,out vec3 irradiance) {
    irradiance=vec3(0);
    vec3 emission=includeEmission && ((s.material.flags&128u)!=0u || dot(s.emissionNormal,viewDirection)>0)?s.material.emissive:vec3(0);
    if(s.material.lightingEnabled<=.5)return s.baseColor+emission;
    if(s.material.transmission>0 && dot(viewDirection,s.outwardNormal)<0)
        s.material.ior=1.0/s.material.ior;
    vec3 color=emission;
    if(max(pushConsts.lightColor.r,max(pushConsts.lightColor.g,pushConsts.lightColor.b))>0.)
        color+=DisneyBRDF(viewDirection,s.normal,s.tangent,s.bitangent,pushConsts.sun_dir,
            pushConsts.lightColor,s.baseColor,s.material,computeSunShadow(s.position,s.geometricNormal));
    irradiance=lpvIrradiance(s.position,s.geometricNormal);
    color+=s.baseColor*(1.0-s.material.metallic)*(1.0-s.material.transmission)*irradiance/PI*ao*s.material.occlusionStrength;
    return color;
}
vec3 shadeDirect(Surface s,float ao,vec3 viewDirection,bool includeEmission) {
    vec3 irradiance;
    return shadeDirectWithIrradiance(s,ao,viewDirection,includeEmission,irradiance);
}
