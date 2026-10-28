#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require
layout(local_size_x=8,local_size_y=8) in;
layout(set=1,binding=0) readonly buffer Buffer {uint data[];} ssbos[];
layout(set=2,binding=0,rgba16f) uniform image2D images[];
layout(set=2,binding=0,rg32ui) uniform uimage2D guides[];
layout(push_constant) uniform PushConsts {
    uvec2 gbufferSize;
    uint uniformBufferIndex;
    uint shadowInfoBufferIndex;
    uint inputIndex;
    uint outputIndex;
    uint mode; // 0 temporal, 1 and 2 edge-aware spatial iterations
} pushConsts;
uint readUint(uint b,uint i) {return ssbos[nonuniformEXT(b)].data[i];}
float readFloat(uint b,uint i) {return uintBitsToFloat(readUint(b,i));}
mat4 readMat4(uint b,uint i) {mat4 m;for(uint c=0u;c<4u;++c)for(uint r=0u;r<4u;++r)m[c][r]=readFloat(b,i+c*4u+r);return m;}
#include "screen_history.glsl"
float edgeConfidence(vec2 uv) {float e=min(min(uv.x,uv.y),min(1.-uv.x,1.-uv.y));return smoothstep(.02,.15,e);}
vec3 viewPositionAt(ivec2 p,float z) {
    vec4 v=readMat4(pushConsts.uniformBufferIndex,76u)*vec4((vec2(screenGiPixel(p))+.5)/vec2(pushConsts.gbufferSize)*2.-1.,1,1);
    return v.xyz*(z/v.z);
}
bool inside(ivec2 p) {return all(greaterThanEqual(p,ivec2(0))) && all(lessThan(p,screenGiSize()));}
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(!inside(p))return;
    uint info=pushConsts.shadowInfoBufferIndex,guide=readUint(info,100u);
    uvec2 g=imageLoad(guides[guide],p).rg;float z=uintBitsToFloat(g.x);
    vec4 value=imageLoad(images[pushConsts.inputIndex],p);
    if(z<=0.) {imageStore(images[pushConsts.outputIndex],p,vec4(0));return;}
    vec3 viewPosition=viewPositionAt(p,z),normal=stampNormal(g.y);
    if(pushConsts.mode==0u) {
        vec4 history=vec4(0);float weight=0.;
        vec3 world=(readMat4(info,140u)*vec4(viewPosition,1)).xyz;
        vec4 clip=readMat4(info,108u)*vec4(world,1);
        vec2 previousUv=clip.xy/max(clip.w,1e-10)*.5+.5;
        if(readUint(info,98u)!=0u && clip.w>0. && all(greaterThanEqual(previousUv,vec2(0))) && all(lessThan(previousUv,vec2(1)))) {
            float expectedZ=(readMat4(info,124u)*vec4(world,1)).z;
            float stride=float(readUint(info,95u));
            vec2 q=(previousUv*vec2(pushConsts.gbufferSize)-.5-floor(stride*.5))/stride;
            ivec2 base=ivec2(floor(q));vec2 f=fract(q);
            for(int i=0;i<4;++i) {
                ivec2 d=ivec2(i&1,i>>1),h=base+d;if(!inside(h))continue;
                uvec2 previous=imageLoad(guides[readUint(info,101u)],h).rg;
                float previousZ=uintBitsToFloat(previous.x);
                if(previousZ<=0. || !sameSurface(g.y,previous.y) || abs(expectedZ-previousZ)>max(.001,expectedZ*.01))continue;
                vec2 w=mix(vec2(1)-f,f,vec2(d));
                history+=imageLoad(images[readUint(info,99u)],h)*w.x*w.y;weight+=w.x*w.y;
            }
        }
        value.a=1.;
        if(weight>.05) {
            history/=weight;
            // Clip signed irradiance history, not final colour/albedo. Permit
            // local variance so sparse diffuse rays converge instead of sticking.
            vec3 lo=value.rgb,hi=value.rgb,mean=vec3(0),second=vec3(0);float count=0.;
            for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x) {
                ivec2 q=p+ivec2(x,y);if(!inside(q))continue;
                uvec2 ng=imageLoad(guides[guide],q).rg;
                if(ng.x==0u || !sameSurface(g.y,ng.y) || abs(uintBitsToFloat(ng.x)-z)>z*.03)continue;
                vec3 v=imageLoad(images[pushConsts.inputIndex],q).rgb;
                lo=min(lo,v);hi=max(hi,v);mean+=v;second+=v*v;count+=1.;
            }
            mean/=max(count,1.);vec3 sigma=sqrt(max(second/max(count,1.)-mean*mean,vec3(0)));
            history.rgb=clamp(history.rgb,lo-sigma,hi+sigma);
            vec2 uv=(vec2(screenGiPixel(p))+.5)/vec2(pushConsts.gbufferSize);
            float motion=length((previousUv-uv)*vec2(screenGiSize()));
            float lengthLimit=mix(32.,4.,smoothstep(0.,8.,motion));
            value.a=min(history.a+1.,lengthLimit);
            value.rgb=mix(history.rgb,value.rgb,1./value.a);
        }
    } else {
        vec3 viewNormal=mat3(readMat4(pushConsts.uniformBufferIndex,44u))*normal;
        float edge=edgeConfidence((vec2(screenGiPixel(p))+.5)/vec2(pushConsts.gbufferSize));
        float stride=float(pushConsts.mode)*mix(3.,1.,edge),sum=0.;vec3 result=vec3(0);
        for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x) {
            ivec2 q=p+ivec2(round(vec2(x,y)*stride));if(!inside(q))continue;
            uvec2 ng=imageLoad(guides[guide],q).rg;float nz=uintBitsToFloat(ng.x);
            if(nz<=0. || !sameSurface(g.y,ng.y))continue;
            float plane=abs(dot(viewPositionAt(q,nz)-viewPosition,viewNormal));
            float w=exp(-float(x*x+y*y)*.35-plane/max(z*.015,.001))*pow(max(dot(stampNormal(ng.y),normal),0.),32.);
            result+=imageLoad(images[pushConsts.inputIndex],q).rgb*w;sum+=w;
        }
        value.rgb=sum>1e-6?result/sum:value.rgb;
    }
    if(any(isnan(value)) || any(isinf(value)))value=vec4(0);
    imageStore(images[pushConsts.outputIndex],p,clamp(value,vec4(-65504),vec4(65504)));
}
