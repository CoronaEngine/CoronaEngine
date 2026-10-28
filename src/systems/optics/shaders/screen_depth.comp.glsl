#version 460
#extension GL_EXT_nonuniform_qualifier : enable
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=0) uniform sampler2D textures[];
layout(set=1,binding=0) buffer Buffer {uint data[];} buffers[];
layout(push_constant) uniform PushConsts {
    uvec2 size;
    uint depthIndex;
    uint pyramidIndex;
    uint uniformIndex;
    uint level;
} pc;
uvec2 levelSize(uint i) {uint scale=1u<<i;return (pc.size+scale-1u)/scale;}
uint offset(uint level) {uint o=0u;for(uint i=1u;i<level;++i){uvec2 s=levelSize(i);o+=s.x*s.y;}return o;}
void main() {
    uvec2 p=gl_GlobalInvocationID.xy,size=levelSize(pc.level);
    if(any(greaterThanEqual(p,size)))return;
    float nearest=1e30;
    for(uint i=0u;i<4u;++i) {
        uvec2 q=p*2u+uvec2(i&1u,i>>1u),previous=levelSize(pc.level-1u);
        if(any(greaterThanEqual(q,previous)))continue;
        float z=1e30;
        if(pc.level==1u) {
            float depth=texelFetch(textures[pc.depthIndex],ivec2(q),0).r;
            if(depth>0.) {
                vec4 clip=vec4((vec2(q)+.5)/vec2(pc.size)*2.-1.,depth,1);
                vec4 view=vec4(0);
                for(uint c=0u;c<4u;++c)for(uint r=0u;r<4u;++r)
                    view[r]+=uintBitsToFloat(buffers[pc.uniformIndex].data[76u+c*4u+r])*clip[c];
                z=view.z/view.w;
            }
        } else z=uintBitsToFloat(buffers[pc.pyramidIndex].data[offset(pc.level-1u)+q.y*previous.x+q.x]);
        nearest=min(nearest,z);
    }
    buffers[pc.pyramidIndex].data[offset(pc.level)+p.y*size.x+p.x]=floatBitsToUint(nearest);
}
