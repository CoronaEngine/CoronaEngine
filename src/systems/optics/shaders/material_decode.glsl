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
