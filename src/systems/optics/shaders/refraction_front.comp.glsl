#version 460
#extension GL_EXT_nonuniform_qualifier : enable
layout(local_size_x=16,local_size_y=16) in;
layout(push_constant) uniform PushConsts {
    uint visibilityIndex;
    uint depthIndex;
    uint instances;
    uint outputIndex;
    uvec2 size;
    uint instanceCount;
} pushConsts;
layout(set=0,binding=0) uniform sampler2D textures[];
layout(set=1,binding=0) readonly buffer Buffer { uint data[]; } buffers[];
layout(set=2,binding=0,rg32ui) uniform uimage2D images[];
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(p,ivec2(pushConsts.size))))return;
    uint visible=imageLoad(images[pushConsts.visibilityIndex],p).r;
    uint owner=0u;
    if(visible>0u && visible<=pushConsts.instanceCount)
        owner=buffers[pushConsts.instances].data[(visible-1u)*24u+19u];
    float depth=texelFetch(textures[pushConsts.depthIndex],p,0).r;
    imageStore(images[pushConsts.outputIndex],p,uvec4(owner,floatBitsToUint(depth),0u,0u));
}
