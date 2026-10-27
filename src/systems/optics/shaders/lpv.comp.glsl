#version 460
#extension GL_EXT_nonuniform_qualifier : enable
layout(local_size_x=8,local_size_y=8) in;
layout(set=0,binding=0) uniform sampler2D textures[];
layout(set=1,binding=0) buffer SSBOPool {uint data[];} ssbos[];
layout(push_constant) uniform PushConsts {
    uint info; uint injection; uint source; uint destination;
    uint accumulated; uint mode; uint cascade; uint stride;
    uint positionImage; uint normalImage; uint fluxImage; uint shadowInfo;
    uint uniformInfo; uint skySH; uint authored; uint disabled;
    vec3 sunDirection; float texelArea;
    uint level; uint parent; uint padding0; uint padding1;
} pushConsts;
const float PI=3.14159265359,Y0=.282094792,Y1=.488602512;
const uint HEADER=256u,CELL_WORDS=208u,CHANNELS=192u;
const ivec3 axes[6]=ivec3[6](ivec3(1,0,0),ivec3(-1,0,0),ivec3(0,1,0),ivec3(0,-1,0),ivec3(0,0,1),ivec3(0,0,-1));
// Fixed antipodal angular quadrature, shared with native_lpv_data.h.
const vec3 angularDirections[64]=vec3[64](
    vec3(0.999877922,0.000000000,0.015625000),
    vec3(-0.999877922,-0.000000000,-0.015625000),
    vec3(-0.736558335,0.674747770,0.046875000),
    vec3(0.736558335,-0.674747770,-0.046875000),
    vec3(0.087158514,-0.993126315,0.078125000),
    vec3(-0.087158514,0.993126315,-0.078125000),
    vec3(0.604788567,0.788839590,0.109375000),
    vec3(-0.604788567,-0.788839590,-0.109375000),
    vec3(-0.974928320,-0.172451092,0.140625000),
    vec3(0.974928320,0.172451092,-0.140625000),
    vec3(0.831199175,-0.528740877,0.171875000),
    vec3(-0.831199175,0.528740877,-0.171875000),
    vec3(-0.254192286,0.945582633,0.203125000),
    vec3(0.254192286,-0.945582633,-0.203125000),
    vec3(-0.448069042,-0.862729675,0.234375000),
    vec3(0.448069042,0.862729675,-0.234375000),
    vec3(0.905577520,0.330715458,0.265625000),
    vec3(-0.905577520,-0.330715458,-0.265625000),
    vec3(-0.882672684,0.364354453,0.296875000),
    vec3(0.882672684,-0.364354453,-0.296875000),
    vec3(0.400379470,-0.855587672,0.328125000),
    vec3(-0.400379470,0.855587672,-0.328125000),
    vec3(0.279289677,0.890419500,0.359375000),
    vec3(-0.279289677,-0.890419500,-0.359375000),
    vec3(-0.796470088,-0.461570696,0.390625000),
    vec3(0.796470088,0.461570696,-0.390625000),
    vec3(0.885507002,-0.194676230,0.421875000),
    vec3(-0.885507002,0.194676230,-0.421875000),
    vec3(-0.512697398,0.729259290,0.453125000),
    vec3(0.512697398,-0.729259290,-0.453125000),
    vec3(-0.112428924,-0.867606245,0.484375000),
    vec3(0.112428924,0.867606245,-0.484375000),
    vec3(0.655162245,0.552171434,0.515625000),
    vec3(-0.655162245,-0.552171434,-0.515625000),
    vec3(-0.836499335,0.034591874,0.546875000),
    vec3(0.836499335,-0.034591874,-0.546875000),
    vec3(0.578368093,-0.575553502,0.578125000),
    vec3(-0.578368093,0.575553502,-0.578125000),
    vec3(-0.036624373,0.792035835,0.609375000),
    vec3(0.036624373,-0.792035835,-0.609375000),
    vec3(-0.491971015,-0.589545698,0.640625000),
    vec3(0.491971015,0.589545698,-0.640625000),
    vec3(0.734049986,0.098765387,0.671875000),
    vec3(-0.734049986,-0.098765387,-0.671875000),
    vec3(-0.583684672,0.406112592,0.703125000),
    vec3(0.583684672,-0.406112592,-0.703125000),
    vec3(0.148971653,-0.662193934,0.734375000),
    vec3(-0.148971653,0.662193934,-0.734375000),
    vec3(0.319830069,0.558146116,0.765625000),
    vec3(-0.319830069,-0.558146116,-0.765625000),
    vec3(-0.575563831,-0.183620561,0.796875000),
    vec3(0.575563831,0.183620561,-0.796875000),
    vec3(0.508856394,-0.235104561,0.828125000),
    vec3(-0.508856394,0.235104561,-0.828125000),
    vec3(-0.197410744,0.471702881,0.859375000),
    vec3(0.197410744,-0.471702881,-0.859375000),
    vec3(-0.153907284,-0.427901457,0.890625000),
    vec3(0.153907284,0.427901457,-0.890625000),
    vec3(0.342999753,0.180271057,0.921875000),
    vec3(-0.342999753,-0.180271057,-0.921875000),
    vec3(-0.292582713,0.077123864,0.953125000),
    vec3(0.292582713,-0.077123864,-0.953125000),
    vec3(0.095233309,-0.148109676,0.984375000),
    vec3(-0.095233309,0.148109676,-0.984375000));
vec3 directionOf(int i){return angularDirections[i];}
const float angularWeights[64]=float[64](0.18545264566,0.18545264566,0.18360403261,0.18360403261,0.20603140180,0.20603140180,0.21282124925,0.21282124925,0.19039847982,0.19039847982,0.18074022786,0.18074022786,0.20166881818,0.20166881818,0.21220114315,0.21220114315,0.19702384094,0.19702384094,0.18163553049,0.18163553049,0.19365422512,0.19365422512,0.21346855808,0.21346855808,0.20023951791,0.20023951791,0.18366960940,0.18366960940,0.19206956339,0.19206956339,0.20583139827,0.20583139827,0.20634814163,0.20634814163,0.18761892444,0.18761892444,0.18677456163,0.18677456163,0.20580184440,0.20580184440,0.20310482026,0.20310482026,0.19299351642,0.19299351642,0.18962572256,0.18962572256,0.19645112755,0.19645112755,0.20586908503,0.20586908503,0.19449384754,0.19449384754,0.19022179593,0.19022179593,0.19802303988,0.19802303988,0.19768137266,0.19768137266,0.19782837476,0.19782837476,0.19472847419,0.19472847419,0.19511041640,0.19511041640);
float angularWeight(int i){return angularWeights[i];}
float cosineIntegral(vec3 normal){float result=0;for(int i=0;i<64;++i){vec3 d=directionOf(i);result+=angularWeight(i)*max(dot(normal,d),0);}return result;}
uint u(uint b,uint i){return ssbos[nonuniformEXT(b)].data[i];}
float f(uint b,uint i){return uintBitsToFloat(u(b,i));}
vec3 v(uint b,uint i){return vec3(f(b,i),f(b,i+1),f(b,i+2));}
void put(uint b,uint i,float value){ssbos[nonuniformEXT(b)].data[i]=floatBitsToUint(value);}
void put3(uint b,uint i,vec3 value){for(uint c=0;c<3;++c)put(b,i+c,value[c]);}
uint indexOf(ivec3 p,int n){return uint((p.z*n+p.y)*n+p.x);}
// Direction-major storage coalesces neighboring GPU lanes; the immutable CPU
// geometry remains cell-major and is packed once during source initialization.
uint channelAt(uint cell,int direction,int n){return (uint(direction)*uint(n*n*n)+cell)*3u;}
bool inside(ivec3 p,int n){return all(greaterThanEqual(p,ivec3(0)))&&all(lessThan(p,ivec3(n)));}
vec4 link(ivec3 p,int axis,int sign,int n){if(sign<0)p[axis]-=1;if(!inside(p,n))return vec4(0);uint at=HEADER+(pushConsts.level*uint(n*n*n)+indexOf(p,n))*CELL_WORDS+196u+uint(axis)*4u;return vec4(f(pushConsts.info,at),v(pushConsts.info,at+1));}
// Core 32-bit atomics suffice; no atomic-float extension, fixed-point overflow,
// RTX features, or scene-intersection operation is needed.
void addFloat(uint i,float value) {
    uint old=u(pushConsts.injection,i);
    for(;;) {
        uint expected=old;
        float sum=clamp(uintBitsToFloat(expected)+value,-1e20,1e20);
        old=atomicCompSwap(ssbos[nonuniformEXT(pushConsts.injection)].data[i],expected,floatBitsToUint(sum));
        if(old==expected)break;
    }
}
vec3 skyRadiance(vec3 n) {
    uint b=pushConsts.skySH;float x=n.x,y=n.y,z=n.z;
    return max(vec3(0),v(b,0)*.282095 +
        v(b,3)*(.488603*y)+v(b,6)*(.488603*z)+v(b,9)*(.488603*x)+
        v(b,12)*(1.092548*x*y)+v(b,15)*(1.092548*y*z)+v(b,18)*(.315392*(3*z*z-1))+
        v(b,21)*(1.092548*x*z)+v(b,24)*(.546274*(x*x-y*y)));
}
vec3 boundaryRadiance(vec3 world,int direction,int n) {
    if(pushConsts.level==0u)return pushConsts.authored!=0u?v(pushConsts.info,32u+uint(direction)*3u):skyRadiance(-directionOf(direction));
    uint at=224u+(pushConsts.level-1u)*4u;
    vec3 q=(world-v(pushConsts.info,at))/f(pushConsts.info,at+3u)-.5;
    ivec3 base=ivec3(floor(q));vec3 t=fract(q),result=vec3(0);float total=0;
    for(int corner=0;corner<8;++corner) {
        ivec3 d=ivec3(corner&1,(corner>>1)&1,(corner>>2)&1),p=base+d;
        if(!inside(p,n))continue;
        vec3 w=mix(vec3(1)-t,t,vec3(d));float weight=w.x*w.y*w.z;
        result+=weight*v(pushConsts.parent,channelAt(indexOf(p,n),direction,n));total+=weight;
    }
    return total>0?result/total:vec3(0);
}
void main() {
    int n=int(u(pushConsts.info,0));uint layoutAt=224u+pushConsts.level*4u;
    float size=f(pushConsts.info,layoutAt+3u);vec3 origin=v(pushConsts.info,layoutAt);
    if(pushConsts.mode==1u) {
        // Integrate every valid RSM texel in this tile, rather than multiplying
        // one arbitrarily selected texel by stride^2. Only solar CSM owns RSMs.
        ivec2 first=ivec2(gl_GlobalInvocationID.xy)*int(pushConsts.stride);
        vec3 positionSum=vec3(0),normalSum=vec3(0),flux=vec3(0);float weightSum=0;
        for(uint y=0;y<pushConsts.stride;++y)for(uint x=0;x<pushConsts.stride;++x) {
            ivec2 pixel=first+ivec2(x,y);
            if(any(greaterThanEqual(pixel,textureSize(textures[nonuniformEXT(pushConsts.positionImage)],0))))continue;
            vec4 position=texelFetch(textures[nonuniformEXT(pushConsts.positionImage)],pixel,0);if(position.w<.5)continue;
            float depth=dot(position.xyz-v(pushConsts.uniformInfo,36),normalize(v(pushConsts.uniformInfo,40)));
            uint owner=3u;for(int i=2;i>=0;--i)if(depth<=f(pushConsts.shadowInfo,64+uint(i)))owner=uint(i);
            if(owner!=pushConsts.cascade || depth<0 || depth>f(pushConsts.shadowInfo,67))continue;
            vec3 normal=normalize(texelFetch(textures[nonuniformEXT(pushConsts.normalImage)],pixel,0).xyz);
            float cosine=max(dot(normal,pushConsts.sunDirection),0);if(cosine<.001)continue;
            vec3 energy=texelFetch(textures[nonuniformEXT(pushConsts.fluxImage)],pixel,0).rgb*
                (PI*pushConsts.texelArea/(cosine*size*size));
            float weight=dot(energy,vec3(.2126,.7152,.0722));
            positionSum+=position.xyz*weight;normalSum+=normal*weight;flux+=energy;weightSum+=weight;
        }
        if(weightSum<=1e-15 || length(normalSum)<1e-15)return;
        vec3 position=positionSum/weightSum,normal=normalize(normalSum);
        vec3 q=(position+normal*size*.25-origin)/size-.5,t=fract(q);ivec3 base=ivec3(floor(q));float sum=0;
        float weights[8];
        for(int corner=0;corner<8;++corner){ivec3 d=ivec3(corner&1,(corner>>1)&1,(corner>>2)&1),p=base+d;
            vec3 w=mix(vec3(1)-t,t,vec3(d));float a=w.x*w.y*w.z;
            if(!inside(p,n)||dot(origin+(vec3(p)+.5)*size-position,normal)<=size*1e-5)a=0;
            weights[corner]=a;sum+=a;
        }
        if(sum<=1e-10)return;
        float normalization=cosineIntegral(normal);
        for(int d=0;d<64;++d){vec3 dir=directionOf(d);vec3 value=flux*max(dot(normal,dir),0)/(normalization*dot(abs(dir),vec3(1)));
            for(int corner=0;corner<8;++corner){if(weights[corner]<=0)continue;
                ivec3 p=base+ivec3(corner&1,(corner>>1)&1,(corner>>2)&1);uint at=channelAt(indexOf(p,n),d,n);
                for(uint c=0;c<3;++c)addFloat(at+c,value[c]*weights[corner]/sum);
            }
        }
        return;
    }
    ivec3 p=ivec3(gl_GlobalInvocationID.x,gl_GlobalInvocationID.y%uint(n),gl_GlobalInvocationID.y/uint(n));
    if(!inside(p,n))return;
    uint cell=indexOf(p,n),at=cell*CHANNELS;
    if(pushConsts.mode==0u){for(uint k=0;k<CHANNELS;++k)put(pushConsts.injection,at+k,0);return;}
    if(pushConsts.mode==2u){for(int d=0;d<64;++d){uint offset=channelAt(cell,d,n);
        put3(pushConsts.destination,offset,vec3(0));
        vec3 source=v(pushConsts.info,HEADER+(pushConsts.level*uint(n*n*n)+cell)*CELL_WORDS+4u+uint(d)*3u)+v(pushConsts.injection,offset);
        put3(pushConsts.injection,offset,source);
    }return;}
    if(pushConsts.mode==4u) {
        // Project the bounded-iteration radiance, never sum repeated steady-state iterates.
        vec3 coeff[9];for(int k=0;k<9;++k)coeff[k]=vec3(0);
        if(pushConsts.disabled==0u)for(int d=0;d<64;++d){vec3 dir=directionOf(d);float x=dir.x,y=dir.y,z=dir.z;
            float basis[9]=float[9](Y0,Y1*x,Y1*y,Y1*z,1.092548431*x*y,1.092548431*y*z,
                .315391565*(3*z*z-1),1.092548431*x*z,.546274215*(x*x-y*y));
            vec3 incident=v(pushConsts.source,channelAt(cell,d,n));
            put3(pushConsts.parent,channelAt(cell,d,n),incident);
            vec3 value=incident*angularWeight(d);
            for(int k=0;k<9;++k)coeff[k]+=basis[k]*value;}
        for(uint c=0;c<3;++c)for(uint k=0;k<9;++k)put(pushConsts.accumulated,(pushConsts.level*uint(n*n*n)+cell)*27u+c*9u+k,coeff[k][c]);
        return;
    }
    // Upwind finite-volume transport. Angular channels retain their direction
    // in free space. Absorption/reflection occurs only at blocked voxel links.
    vec3 reflected[6];vec4 edges[6];
    for(int face=0;face<6;++face){reflected[face]=vec3(0);edges[face]=link(p,face/2,face%2==0?1:-1,n);}
    bool walls=false;for(int face=0;face<6;++face)walls=walls||edges[face].x>0;
    if(walls){
        for(int d=0;d<64;++d){vec3 dir=directionOf(d),value=v(pushConsts.source,channelAt(cell,d,n))*angularWeight(d);
            for(int face=0;face<6;++face)if(edges[face].x>0)reflected[face]+=value*max(dot(dir,vec3(axes[face])),0);}
        for(int face=0;face<6;++face)if(edges[face].x>0)reflected[face]*=edges[face].yzw/vec3(3.145915418,3.122227172,3.144544148)[face/2];
    }
    for(int d=0;d<64;++d) {
        vec3 dir=directionOf(d),w=abs(dir);w/=dot(w,vec3(1));
        vec3 result=vec3(0);
        if(pushConsts.disabled==0u){
            for(int axis=0;axis<3;++axis)if(w[axis]>0){
                int sign=dir[axis]>0?1:-1;ivec3 q=p;q[axis]-=sign;
                vec3 incoming=inside(q,n)?v(pushConsts.source,channelAt(indexOf(q,n),d,n)):
                    boundaryRadiance(origin+(vec3(q)+.5)*size,d,n);
                int face=axis*2+(sign>0?1:0);vec4 edge=edges[face];
                result+=w[axis]*mix(incoming,reflected[face],edge.x);
            }
            result+=v(pushConsts.injection,channelAt(cell,d,n));
        }
        put3(pushConsts.destination,channelAt(cell,d,n),result);
    }
}
