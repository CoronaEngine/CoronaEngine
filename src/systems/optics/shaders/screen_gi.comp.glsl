#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require
#include "native_policy.glsl"
layout(local_size_x=8,local_size_y=8) in;
#include "vbuffer_bindings.glsl"
#include "vbuffer_shading.glsl"
#include "screen_vbuffer_trace.glsl"

uint randomBits(uint v) {v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16);}
vec2 temporalPair(uint index,uint seed) {
    // Digitally shifted 2D Sobol sequence. Keep the per-pixel shift fixed:
    // hashing the frame discards stratification across the history window.
    uint y=0u,direction=0x80000000u;
    for(uint bits=index;bits!=0u;bits>>=1u) {
        if((bits&1u)!=0u)y^=direction;
        direction^=direction>>1u;
    }
    uvec2 v=uvec2(bitfieldReverse(index),y)^uvec2(randomBits(seed),randomBits(seed^0x9e3779b9u));
    // 23-bit bin centers remain strictly inside (0,1) in float precision.
    return (vec2(v>>9u)+.5)*(1./8388608.);
}
#include "rsm_gather.glsl"
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(p,screenGiSize())))return;
    ivec2 pixel=screenGiPixel(p);
    Surface receiver;vec3 correction=vec3(0);uvec2 guide=uvec2(0);bool valid=false;
    if(decodeSurface(pixel,receiver) && receiver.material.transmission<=0. &&
       receiver.material.metallic<.95 && receiver.material.lightingEnabled>.5) {
        float z=(screenView()*vec4(receiver.position,1)).z;
        guide=uvec2(floatBitsToUint(z),surfaceStamp(receiver.objectID,receiver.geometricNormal));valid=true;
        {
            vec3 n=receiver.geometricNormal;
            vec3 tangent=normalize(cross(abs(n.y)<.95?vec3(0,1,0):vec3(1,0,0),n));
            vec3 bitangent=cross(n,tangent);
            uint frame=readUint(pushConsts.shadowInfoBufferIndex,97u);
            uint seed=randomBits(uint(p.y)*uint(screenGiSize().x)+uint(p.x));
            uint samples=readUint(pushConsts.shadowInfoBufferIndex,98u)!=0u?uint(NATIVE_GI_STABLE_SAMPLES):uint(NATIVE_GI_RESET_SAMPLES);
            vec3 rsm=gatherRsm(receiver,seed^0x9e3779b9u,frame);
            for(uint i=0u;i<samples;++i) {
                // Cosine-weighted diffuse hemisphere, not a specular reflection.
                // Successive frames extend the same stratified sequence.
                vec2 u=temporalPair(frame*uint(NATIVE_GI_STABLE_SAMPLES)+i,seed);
                float r=sqrt(u.x),phi=2.*PI*u.y;
                vec3 direction=tangent*(r*cos(phi))+bitangent*(r*sin(phi))+n*sqrt(1.-u.x);
                ivec2 hp;float confidence;
                if(!traceScreenHierarchy(receiver.position,n,direction,pushConsts.ssrMaxDistance,pushConsts.ssrThickness,hp,confidence))continue;
                Surface hit;
                if(!decodeSurfaceFrom(hp,pushConsts.visibilityImageIndex,true,hit) ||
                   dot(hit.normal,direction)>=-.05 || length(hit.position-receiver.position)<=.05)continue;
                if(!decodeSurface(hp,hit) || hit.material.transmission>0.)continue;
                // Diffuse solar transport uses the same Lambertian approximation
                // as RSM flux. The screen/path partition replaces its visible
                // contribution instead of adding a second copy of the bounce.
                float weight=screenPathWeight(receiver.position,hit.position);
                vec3 diffuse=hit.baseColor*(1.-hit.material.metallic)/PI;
                vec3 incoming=pushConsts.lightColor*max(dot(hit.geometricNormal,pushConsts.sun_dir),0.)*
                    computeSunShadow(hit.position,hit.geometricNormal)+environmentIrradiance(hit.geometricNormal);
                vec3 emitted=((hit.material.flags&128u)!=0u || dot(hit.emissionNormal,-direction)>0.)?hit.material.emissive:vec3(0);
                vec3 radiance=hit.material.lightingEnabled>.5?diffuse*incoming+emitted:hit.baseColor+emitted;
                correction+=weight*(radiance-environmentRadiance(direction));
            }
            correction=correction*(PI/float(samples))+rsm;
        }
    }
    if(!finiteVec3(correction))correction=vec3(0);
    imageStore(imagesRGBA16[pushConsts.finalOutputImage],p,vec4(clamp(correction,vec3(-65504),vec3(65504)),valid?1.:0.));
    imageStore(imagesRG32UI[readUint(pushConsts.shadowInfoBufferIndex,100u)],p,uvec4(guide,0,0));
}
