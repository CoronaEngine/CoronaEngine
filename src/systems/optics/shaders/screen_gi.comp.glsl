#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require
layout(local_size_x=8,local_size_y=8) in;
#include "vbuffer_bindings.glsl"
#include "vbuffer_shading.glsl"
#include "screen_vbuffer_trace.glsl"

uint randomBits(uint v) {v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16);}
vec2 randomPair(uint index,uint seed) {
    return vec2(randomBits(seed+index*2u),randomBits(seed+index*2u+1u))*(1./4294967296.);
}
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    if(any(greaterThanEqual(p,screenGiSize())))return;
    ivec2 pixel=screenGiPixel(p);
    Surface receiver;vec3 correction=vec3(0);uvec2 guide=uvec2(0);bool valid=false;
    if(decodeSurface(pixel,receiver) && receiver.material.transmission<=0. &&
       receiver.material.metallic<.95 && receiver.material.lightingEnabled>.5) {
        float z=(screenView()*vec4(receiver.position,1)).z;
        guide=uvec2(floatBitsToUint(z),surfaceStamp(receiver.objectID,receiver.geometricNormal));valid=true;
        float edge=screenConfidence((vec2(pixel)+.5)/vec2(pushConsts.gbufferSize));
        if(edge>0.) {
            vec3 n=receiver.geometricNormal;
            vec3 tangent=normalize(cross(abs(n.y)<.95?vec3(0,1,0):vec3(1,0,0),n));
            vec3 bitangent=cross(n,tangent);
            uint frame=readUint(pushConsts.shadowInfoBufferIndex,97u);
            uint seed=randomBits(uint(p.y)*uint(screenGiSize().x)+uint(p.x))^randomBits(frame);
            uint samples=readUint(pushConsts.shadowInfoBufferIndex,98u)!=0u?4u:8u;
            for(uint i=0u;i<samples;++i) {
                // Cosine-weighted diffuse hemisphere, not a specular reflection.
                // Stratification plus frame scrambling gives new paths every frame.
                vec2 u=randomPair(i,seed);u.x=(float(i)+u.x)/float(samples);
                float r=sqrt(u.x),phi=2.*PI*u.y;
                vec3 direction=tangent*(r*cos(phi))+bitangent*(r*sin(phi))+n*sqrt(1.-u.x);
                ivec2 hp;float confidence;
                if(!traceScreenHierarchy(receiver.position,n,direction,pushConsts.ssrMaxDistance,pushConsts.ssrThickness,hp,confidence))continue;
                Surface hit;
                if(!decodeSurfaceFrom(hp,pushConsts.visibilityImageIndex,true,hit) ||
                   dot(hit.normal,direction)>=-.05 || length(hit.position-receiver.position)<=.05)continue;
                if(!decodeSurface(hp,hit) || hit.material.transmission>0. || hit.material.metallic>=.95 ||
                   max(hit.material.emissive.x,max(hit.material.emissive.y,hit.material.emissive.z))>0.)continue;
                // Evaluate from the hit toward the receiver, preserving Principled
                // direction dependence without a view-colour cache or feedback.
                correction+=confidence*(shadeDirect(hit,1.,-direction,false)-sampleLpv(receiver.position,n,direction,false));
            }
            correction*=PI/float(samples);
        }
    }
    if(!finiteVec3(correction))correction=vec3(0);
    imageStore(imagesRGBA16[pushConsts.finalOutputImage],p,vec4(clamp(correction,vec3(-65504),vec3(65504)),valid?1.:0.));
    imageStore(imagesRG32UI[readUint(pushConsts.shadowInfoBufferIndex,100u)],p,uvec4(guide,0,0));
}
