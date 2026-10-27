#version 460
#extension GL_EXT_nonuniform_qualifier : enable
layout(set=1,binding=0) readonly buffer SSBOPool {uint data[];} ssbos[];
layout(push_constant) uniform RsmConsts {
    mat4 lightViewProjModel;
    vec4 modelRow0;
    vec4 modelRow1;
    vec4 modelRow2;
    uint lpvInfo;
    uint shadowInfo;
} rsmConsts;
layout(location=0) in vec3 worldPosition;
layout(location=1) in vec3 worldNormal;
layout(location=0) out vec4 rsmPosition;
layout(location=1) out vec4 rsmNormal;
layout(location=2) out vec4 rsmFlux;
float f(uint b,uint i){return uintBitsToFloat(ssbos[nonuniformEXT(b)].data[i]);}
vec3 v(uint b,uint i){return vec3(f(b,i),f(b,i+1),f(b,i+2));}
void main() {
    uint b=rsmConsts.lpvInfo;int n=int(ssbos[nonuniformEXT(b)].data[0]);
    uint level=0u;
    for(uint candidate=1u;candidate<ssbos[nonuniformEXT(b)].data[12];++candidate) {
        vec3 q=(worldPosition-v(b,224u+candidate*4u))/f(b,227u+candidate*4u);
        if(all(greaterThanEqual(q,vec3(1)))&&all(lessThan(q,vec3(n-1))))level=candidate;
    }
    ivec3 p=clamp(ivec3(floor((worldPosition-v(b,224u+level*4u))/f(b,227u+level*4u))),ivec3(0),ivec3(n-1));
    uint cell=uint((p.z*n+p.y)*n+p.x);
    vec3 normal=length(worldNormal)>1e-10?normalize(worldNormal):normalize(cross(dFdx(worldPosition),dFdy(worldPosition)));
    vec3 sun=v(rsmConsts.shadowInfo,80),color=v(rsmConsts.shadowInfo,76);
    // LPV uses the voxel's area-weighted diffuse reflectance, including texture
    // means. Full-resolution material/normal textures remain in the VBuffer.
    vec3 albedo=v(b,256u+(level*uint(n*n*n)+cell)*208u+1u);
    rsmPosition=vec4(worldPosition,1);
    rsmNormal=vec4(normal,1);
    rsmFlux=vec4(albedo*color*max(dot(normal,sun),0)/3.14159265359,1);
}
