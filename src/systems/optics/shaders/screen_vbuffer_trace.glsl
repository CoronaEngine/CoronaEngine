// The same hierarchy is used by diffuse GI and specular reflection.
uvec2 screenSize() {return pushConsts.gbufferSize;}
mat4 screenView() {return readMat4(pushConsts.uniformBufferIndex,44u);}
mat4 screenProjection() {return readMat4(pushConsts.uniformBufferIndex,60u);}
mat4 screenInverseProjection() {return readMat4(pushConsts.uniformBufferIndex,76u);}
uint screenLevels() {return readUint(pushConsts.shadowInfoBufferIndex,96u);}
uvec2 screenLevelSize(uint level) {
    uint scale=1u<<level;
    return (screenSize()+scale-1u)/scale;
}
uint screenLevelOffset(uint level) {
    uint offset=0u;
    // Level zero is the existing depth image, never duplicated at 8K.
    for(uint i=1u;i<level;++i) {uvec2 s=screenLevelSize(i);offset+=s.x*s.y;}
    return offset;
}
float screenDepth(uint level,ivec2 p) {
    if(level==0u) {
        uint depth=(pushConsts.featureFlags&32u)!=0u?readUint(pushConsts.shadowInfoBufferIndex,85u):pushConsts.depthImageIndex;
        float d=texelFetch(textures[nonuniformEXT(depth)],p,0).r;
        return d>0.?viewPosition(p,d,screenInverseProjection()).z:1e30;
    }
    return readFloat(readUint(pushConsts.shadowInfoBufferIndex,92u),screenLevelOffset(level)+uint(p.y)*screenLevelSize(level).x+uint(p.x));
}
#include "screen_trace.glsl"
#include "screen_history.glsl"
