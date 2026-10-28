// Compact temporal validation only. No albedo/roughness/material GBuffer.
vec2 octEncode(vec3 n) {
    n/=abs(n.x)+abs(n.y)+abs(n.z);
    return n.z>=0.?n.xy:(1.-abs(n.yx))*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
}
vec3 octDecode(vec2 f) {
    vec3 n=vec3(f,1.-abs(f.x)-abs(f.y));
    float t=clamp(-n.z,0.,1.);n.xy+=mix(vec2(t),vec2(-t),greaterThanEqual(n.xy,vec2(0)));
    return normalize(n);
}
uint surfaceStamp(uint objectID,vec3 normal) {
    // Hash rather than truncate handles; depth/normal also validate history.
    uint h=objectID*0x9e3779b9u;h^=h>>16u;
    uvec2 n=uvec2(round((octEncode(normal)*.5+.5)*255.));
    return ((h&65535u)<<16u)|(n.y<<8u)|n.x;
}
vec3 stampNormal(uint stamp) {
    return octDecode(vec2(stamp&255u,(stamp>>8u)&255u)*(2./255.)-1.);
}
bool sameSurface(uint a,uint b) {return (a>>16u)==(b>>16u) && dot(stampNormal(a),stampNormal(b))>.9;}
ivec2 screenGiSize() {return ivec2(readUint(pushConsts.shadowInfoBufferIndex,93u),readUint(pushConsts.shadowInfoBufferIndex,94u));}
ivec2 screenGiPixel(ivec2 p) {
    int stride=int(readUint(pushConsts.shadowInfoBufferIndex,95u));
    return min(p*stride+stride/2,ivec2(pushConsts.gbufferSize)-1);
}
