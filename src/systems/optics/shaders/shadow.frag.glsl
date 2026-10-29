#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require
layout(set=0,binding=0) uniform sampler2D textures[];
layout(set=1,binding=0) readonly buffer SSBOPool {uint data[];} ssbos[];
layout(push_constant) uniform RsmConsts {
    mat4 lightViewProjModel;
    vec4 modelRow0;
    vec4 modelRow1;
    vec4 modelRow2;
    uint materialTableBufferIndex;
    uint materialID;
    uint shadowInfo;
    uint cascade;
} rsmConsts;
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=2) in vec2 texCoord;
layout(location=0) out vec4 rsmPosition;
layout(location=1) out vec4 rsmNormal;
layout(location=2) out vec4 rsmFlux;
// Both stages share the byte layout; distinct reflection names avoid C++
// multiple-inheritance ambiguity in Horizon's generated raster bindings.
#define pushConsts rsmConsts
#include "bindless_read.glsl"
#include "material_decode.glsl"
void main() {
    MaterialInfo material=loadMaterialInfo(pushConsts.materialID);
    vec3 albedo=material.materialColor.rgb,unusedNormal;
    if((material.flags&64u)!=0u)evaluateAppearance(material,texCoord,albedo,unusedNormal);
    if(material.textureDescriptor!=0u)
        albedo*=texture(textures[nonuniformEXT(material.textureDescriptor)],texCoord).rgb;
    if(material.metallicDescriptor!=0u)
        material.metallic*=texture(textures[nonuniformEXT(material.metallicDescriptor)],texCoord)[material.flags&3u];
    vec3 normal=length(worldNormal)>1e-10?normalize(worldNormal):normalize(cross(dFdx(worldPosition),dFdy(worldPosition)));
    vec3 sun=readVec3(pushConsts.shadowInfo,80u),color=readVec3(pushConsts.shadowInfo,76u);
    if((material.flags&16u)!=0u && dot(normal,sun)<0.)normal=-normal;
    float cosine=max(dot(normal,sun),0.);
    float projectedArea=readFloat(pushConsts.shadowInfo,159u+pushConsts.cascade*4u);
    // Flux / PI per texel. The sunlight cosine cancels the conversion from
    // projected area to surface area; applying it twice darkens grazing walls.
    vec3 flux=cosine>1e-4 && material.lightingEnabled>.5
        ? clamp(albedo,vec3(0),vec3(1))*color*(1.-clamp(material.metallic,0.,1.))*(1.-material.transmission)*projectedArea/3.14159265359 : vec3(0);
    if(!finiteVec3(flux))flux=vec3(0);
    rsmPosition=vec4(worldPosition,1);
    rsmNormal=vec4(normal,min(projectedArea/max(cosine,1e-4),65504.));
    rsmFlux=vec4(clamp(flux,vec3(0),vec3(65504)),1);
}
