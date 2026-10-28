#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_GOOGLE_include_directive : require

layout (local_size_x = 8, local_size_y = 8) in;

#include "vbuffer_bindings.glsl"
#include "vbuffer_shading.glsl"
#include "screen_vbuffer_trace.glsl"
bool traceScreen(Surface source, vec3 direction, uint depthIndex, uint visibilityIndex, bool transmitting, out Surface hit, out float confidence) {
    confidence = 0.0;
    if(!transmitting && screenLevels()>0u) {
        ivec2 hp;
        if(!traceScreenHierarchy(source.position,source.normal,direction,pushConsts.ssrMaxDistance,pushConsts.ssrThickness,hp,confidence))return false;
        // Reject cheaply before material programs or texture fetches.
        Surface geometry;
        if(!decodeSurfaceFrom(hp,visibilityIndex,true,geometry) || dot(geometry.normal,direction)>=-.05 ||
           length(geometry.position-source.position)<=.05)return false;
        return decodeSurfaceFrom(hp,visibilityIndex,false,hit);
    }
    mat4 view = readMat4(pushConsts.uniformBufferIndex, 44u);
    mat4 projection = readMat4(pushConsts.uniformBufferIndex, 60u);
    mat4 inverseProjection = readMat4(pushConsts.uniformBufferIndex, 76u);
    vec3 origin = source.position + (transmitting?direction:source.normal) * max(0.001, pushConsts.ssrThickness * 0.25);
    vec3 a = (view * vec4(origin,1)).xyz;
    vec3 ray = mat3(view) * direction;
    float distance = transmitting ? max(pushConsts.ssrMaxDistance,readFloat(pushConsts.directLightingBufferIndex,7u)*float(readUint(pushConsts.directLightingBufferIndex,0u))*2.0) : pushConsts.ssrMaxDistance;
    // Clip rays before the camera near plane, avoiding negative/zero clip W.
    float nearPlane = abs(viewPosition(ivec2(pushConsts.gbufferSize / 2u), 1.0, inverseProjection).z);
    if (ray.z < 0.0) distance = min(distance, (a.z - nearPlane * 1.01) / -ray.z);
    if (distance <= 0.02) return false;
    vec3 b = a + ray * distance;
    vec4 ca = projection * vec4(a,1), cb = projection * vec4(b,1);
    if (ca.w <= 1e-5 || cb.w <= 1e-5) return false;
    vec2 pa = (ca.xy / ca.w * 0.5 + 0.5) * vec2(pushConsts.gbufferSize);
    vec2 pb = (cb.xy / cb.w * 0.5 + 0.5) * vec2(pushConsts.gbufferSize);
    vec2 delta = pb - pa;
    // Clip to the viewport before budgeting steps. A ray ending near the camera
    // can project thousands of pixels outside it; sampling that invisible span
    // would skip all useful intersections in the first step.
    if (any(lessThan(pa,vec2(0))) || any(greaterThanEqual(pa,vec2(pushConsts.gbufferSize)))) return false;
    float screenEnd = 1.0;
    for (int axis=0; axis<2; ++axis) {
        if (delta[axis] > 1e-6) screenEnd = min(screenEnd,(float(pushConsts.gbufferSize[axis])-0.5-pa[axis])/delta[axis]);
        else if (delta[axis] < -1e-6) screenEnd = min(screenEnd,(0.5-pa[axis])/delta[axis]);
    }
    screenEnd = clamp(screenEnd,0.0,1.0);
    float ka = 1.0 / ca.w, kb = 1.0 / cb.w;
    vec3 qa = a * ka, qb = b * kb;
    pb = mix(pa,pb,screenEnd);
    qb = mix(qa,qb,screenEnd);
    kb = mix(ka,kb,screenEnd);
    delta = pb-pa;
    float pixels = max(abs(delta.x), abs(delta.y));
    if (pixels < 1.0) {
        // A normally incident transmitted ray may stay in one screen pixel.
        ivec2 ip=ivec2(pa);
        float depth=texelFetch(textures[nonuniformEXT(depthIndex)],ip,0).r;
        float z=viewPosition(ip,depth,inverseProjection).z;
        float along=abs(ray.z)>1e-7?(z-a.z)/ray.z:-1;
        if(transmitting && depth>0 && along>0 && along<distance && decodeSurfaceFrom(ip,visibilityIndex,true,hit)) {
            confidence=screenConfidence(pa/vec2(pushConsts.gbufferSize));return confidence>0;
        }
        return false;
    }
    uint steps = uint(clamp(ceil(pixels / 4.0), 1.0, float(pushConsts.ssrMaxSteps)));
    float previousT = 0.0;
    float previousZ = a.z;
    ivec2 firstPixel = ivec2(pa);
    float firstDepth = texelFetch(textures[nonuniformEXT(depthIndex)],firstPixel,0).r;
    bool previousValid = firstDepth > 0.0;
    float previousGap = a.z - viewPosition(firstPixel,firstDepth,inverseProjection).z;
    for (uint i = 1u; i <= steps; ++i) {
        float t = float(i) / float(steps);
        vec2 pixel = mix(pa,pb,t);
        if (any(lessThan(pixel,vec2(0))) || any(greaterThanEqual(pixel,vec2(pushConsts.gbufferSize)))) return false;
        ivec2 ip = ivec2(pixel);
        float z = mix(qa,qb,t).z / mix(ka,kb,t);
        float depth = texelFetch(textures[nonuniformEXT(depthIndex)],ip,0).r;
        if (depth > 0.0) {
            float sceneZ = viewPosition(ip,depth,inverseProjection).z;
            float thickness = max(pushConsts.ssrThickness, abs(sceneZ) * 0.001);
            float gap = z-sceneZ;
            bool bracketed = previousValid && ((previousGap < 0.0) != (gap < 0.0));
            if (bracketed || (max(z,previousZ) >= sceneZ && min(z,previousZ) <= sceneZ + thickness)) {
                float lo = previousT, hi = t;
                bool startsBehind = previousValid && previousGap >= 0.0;
                // Refine the first crossing, then reject silhouettes and backfaces.
                // A parallel glass slab can project a long world-space ray to
                // only a few pixels. Five bisections leave depth errors larger
                // than the hit tolerance and cause alternating environment stripes.
                for (int j = 0; j < (transmitting ? 12 : 5); ++j) {
                    float mid = (lo+hi)*0.5;
                    ivec2 mp = ivec2(clamp(mix(pa,pb,mid),vec2(0),vec2(pushConsts.gbufferSize)-1.0));
                    float md = texelFetch(textures[nonuniformEXT(depthIndex)],mp,0).r;
                    float mz = mix(qa,qb,mid).z / mix(ka,kb,mid);
                    float sz = viewPosition(mp,md,inverseProjection).z;
                    // Bracket the signed ray/scene depth gap. Scene depth can
                    // change faster than ray Z on a slope, including rays toward
                    // the eye; ray direction alone cannot select the half interval.
                    bool crossed = (mz >= sz) != startsBehind;
                    if (md > 0.0 && crossed) hi=mid; else lo=mid;
                }
                vec2 hp = mix(pa,pb,hi);
                ivec2 hitPixel = ivec2(hp);
                float hd = texelFetch(textures[nonuniformEXT(depthIndex)],hitPixel,0).r;
                float hz = mix(qa,qb,hi).z / mix(ka,kb,hi);
                float hitZ = viewPosition(hitPixel,hd,inverseProjection).z;
                float gap = hz - hitZ;
                thickness = max(pushConsts.ssrThickness, abs(hitZ)*0.001);
                if (hd > 0.0 && gap >= -thickness*0.25 && gap <= thickness &&
                    decodeSurfaceFrom(hitPixel,visibilityIndex,transmitting,hit) && dot(hit.normal,direction) < -0.05 &&
                    length(hit.position-source.position) > 0.05) {
                    vec2 uv = hp / vec2(pushConsts.gbufferSize);
                    confidence = screenConfidence(uv) * (1.0-smoothstep(0.75,1.0,hi));
                    return confidence > 0.0;
                }
            }
            previousGap = gap;
        }
        previousValid = depth > 0.0;
        previousT = t;
        previousZ = z;
    }
    return false;
}

// Four-tap joint upsample from bounded-resolution irradiance. Full resolution
// material and surface data continue to come exclusively from the VBuffer.
vec3 filteredScreenGi(ivec2 pixel,Surface receiver) {
    uint info=pushConsts.shadowInfoBufferIndex,descriptor=readUint(info,89u),guide=readUint(info,100u);
    float stride=float(readUint(info,95u)),sum=0.;vec3 result=vec3(0);
    vec2 q=(vec2(pixel)-floor(stride*.5))/stride;
    ivec2 base=ivec2(floor(q));vec2 f=fract(q);
    vec3 receiverView=(screenView()*vec4(receiver.position,1)).xyz;
    vec3 normalView=mat3(screenView())*receiver.geometricNormal;
    float tolerance=max(receiverView.z*.01,readFloat(pushConsts.directLightingBufferIndex,7u)*.002);
    uint stamp=surfaceStamp(receiver.objectID,receiver.geometricNormal);
    for(int i=0;i<4;++i) {
        ivec2 d=ivec2(i&1,i>>1),p=base+d;
        if(any(lessThan(p,ivec2(0)))||any(greaterThanEqual(p,screenGiSize())))continue;
        uvec2 g=imageLoad(imagesRG32UI[nonuniformEXT(guide)],p).rg;
        float z=uintBitsToFloat(g.x);if(z<=0. || !sameSurface(stamp,g.y))continue;
        vec3 v=screenViewPosition(screenGiPixel(p),1.);v*=z/v.z;
        vec2 w=mix(vec2(1)-f,f,vec2(d));
        float plane=abs(dot(v-receiverView,normalView));
        float weight=max(w.x*w.y,1e-3)*exp(-plane/max(tolerance,1e-5));
        result+=imageLoad(imagesRGBA16[nonuniformEXT(descriptor)],p).rgb*weight;sum+=weight;
    }
    float edge=screenConfidence((vec2(pixel)+.5)/vec2(pushConsts.gbufferSize));
    return sum>1e-6?result/sum*edge:vec3(0);
}

// Canonical Corona lat-long orientation, shared with sky.comp and Vision.
vec3 refractionEnvironment(vec3 direction,Surface source) {
    if((pushConsts.featureFlags&16u)!=0u) {
        uint tex=readUint(pushConsts.directLightingBufferIndex,8u);
        float phi=atan(-direction.z,direction.x);if(phi<0)phi+=2.0*PI;
        return max(textureLod(textures[nonuniformEXT(tex)],vec2(phi/(2.0*PI),acos(clamp(direction.y,-1,1))/PI),0).rgb,vec3(0));
    }
    return sampleLpv(source.position,source.geometricNormal,direction,false);
}
bool screenPixel(vec3 world,out ivec2 pixel) {
    vec4 clip=readMat4(pushConsts.uniformBufferIndex,60u)*readMat4(pushConsts.uniformBufferIndex,44u)*vec4(world,1);
    if(clip.w<=1e-6)return false;
    vec2 uv=clip.xy/clip.w*.5+.5;
    if(any(lessThan(uv,vec2(0)))||any(greaterThanEqual(uv,vec2(1))))return false;
    pixel=ivec2(uv*vec2(pushConsts.gbufferSize));return true;
}

vec3 reflection(Surface s, float ao) {
    vec3 eye = readVec3(pushConsts.uniformBufferIndex,36u);
    vec3 v = normalize(eye-s.position);
    vec3 r = reflect(-v,s.normal);
    float roughness = s.material.roughness;
    float weight = (s.material.flags & 32u) != 0u ? s.material.sheenTint : 1.0;
    vec3 f0 = mix(clamp(s.material.specularColor * (0.08 * s.material.specular),vec3(0),vec3(1))*weight,s.baseColor,s.material.metallic);
    vec3 f90 = vec3(mix(weight, 1.0, s.material.metallic));
    vec3 fresnel = mix(f0, f90, pow(1.0-max(dot(s.normal,v),0.0),5.0));
    float glassWeight=s.material.transmission*(1.0-s.material.metallic);
    if(glassWeight>0) {
        bool entering=dot(v,s.outwardNormal)>0;
        float f=dielectricFresnel(dot(v,s.outwardNormal),entering?1:s.material.ior,entering?s.material.ior:1);
        fresnel=mix(fresnel,vec3(f),glassWeight);
    }
    // SH radiance is a low-frequency, geometry-occluded fallback for SSR misses
    // and rough metals. Keeping the receiver offset along its geometric normal
    // avoids sampling the other side of a wall when the reflection is grazing.
    // SSR replaces this approximation when an actual screen-space hit exists.
    vec3 radiance=sampleLpv(s.position,s.geometricNormal,r,false);
    if(glassWeight>0)radiance=mix(radiance,refractionEnvironment(r,s),glassWeight);
    if ((pushConsts.featureFlags & 2u) != 0u && roughness < pushConsts.ssrMaxRoughness &&
        (glassWeight>0 || max(fresnel.x,max(fresnel.y,fresnel.z)) > 0.02)) {
        Surface hit;
        float confidence;
        // A foreground glass pane must not hide an opaque reflection target
        // from the depth search. The auxiliary raster already reveals it; no
        // recursive glass shading or scene intersection is needed.
        bool opaqueSearch=glassWeight>0 && (pushConsts.featureFlags&32u)!=0u;
        uint depth=opaqueSearch?readUint(pushConsts.shadowInfoBufferIndex,85u):pushConsts.depthImageIndex;
        uint visibility=opaqueSearch?readUint(pushConsts.shadowInfoBufferIndex,84u):pushConsts.visibilityImageIndex;
        if (traceScreen(s,r,depth,visibility,false,hit,confidence) && hit.material.transmission<=0.0) {
            confidence *= 1.0-smoothstep(pushConsts.ssrMaxRoughness*0.6,pushConsts.ssrMaxRoughness,roughness);
            // The hit sends radiance back along the reflected ray, not toward
            // the main camera. This matters for specular/metallic hit materials.
            radiance = mix(radiance,shadeDirect(hit,1.0,-r,true),confidence);
        }
    }
    // Roughness changes the angular distribution, not the energy of a uniform
    // environment. Do not apply the legacy metal/opaque SSR attenuation or SSAO
    // darkening to the glass reflection lobe.
    float attenuation=(1.0-0.65*roughness)*mix(1.0,ao*s.material.occlusionStrength,roughness);
    return radiance*fresnel*mix(attenuation,1.0,glassWeight);
}

// Wyman 2005, section 3: two image-space interfaces. We use rasterized
// front/furthest-surface distance instead of the optional precomputed dN.
// Unlike the paper's environment-only result, the exiting ray first searches
// the immutable opaque depth/color buffers for nearby scene geometry.
vec3 refraction(ivec2 pixel,Surface source) {
    float transmission=source.material.transmission*(1-source.material.metallic);
    if(transmission<=0 || (pushConsts.featureFlags&32u)==0u)return vec3(0);
    vec3 eye=readVec3(pushConsts.uniformBufferIndex,36u);
    vec3 incident=normalize(source.position-eye),normal=source.outwardNormal;
    bool entering=dot(incident,normal)<0;
    float ior=source.material.ior;
    vec3 facing=entering?normal:-normal;
    vec3 direction=refract(incident,facing,entering?1.0/ior:ior);
    float frontF=dielectricFresnel(dot(-incident,facing),entering?1:ior,entering?ior:1);
    if(dot(direction,direction)<1e-10)return vec3(0); // entry TIR is reflected by reflection().
    vec3 position=source.position;
    float exitF=0;
    vec3 internalReflection=vec3(0);
    Surface back;
    if(entering && decodeSurfaceFrom(pixel,readUint(pushConsts.shadowInfoBufferIndex,86u),true,back) && back.objectID==source.objectID) {
        float d=length(back.position-source.position);
        vec3 estimate=position+direction*d;
        Surface candidate;
        ivec2 projected;
        // Bounded local plane refinement; no scene triangles or acceleration structure.
        for(int iteration=0;iteration<2;++iteration) {
            if(!screenPixel(estimate,projected) || !decodeSurfaceFrom(projected,readUint(pushConsts.shadowInfoBufferIndex,86u),true,candidate) || candidate.objectID!=source.objectID)break;
            vec3 n=candidate.outwardNormal;
            float denom=dot(direction,n);
            if(denom<=.02)break;
            float distance=dot(candidate.position-source.position,n)/denom;
            if(distance<=0 || distance>d*2.0)break;
            back=candidate;estimate=source.position+direction*distance;
        }
        position=estimate;
        vec3 exitNormal=back.outwardNormal;
        if(dot(direction,exitNormal)<0)exitNormal=-exitNormal;
        exitF=dielectricFresnel(dot(direction,exitNormal),ior,1);
        // The rejected exit energy is reflection, not absorption. Keep it in
        // the same bounded environment/LPV fallback used for internal TIR.
        // This does not trace another interface or claim a nested-medium path.
        internalReflection=refractionEnvironment(reflect(direction,-exitNormal),source);
        vec3 outgoing=refract(direction,-exitNormal,ior);
        if(dot(outgoing,outgoing)<1e-10) {
            // Bounded TIR fallback: preserve reflected energy, without claiming
            // recursive interior paths or clamping the critical angle.
            return transmission*(1-frontF)*clamp(source.baseColor,vec3(0),vec3(1))*internalReflection;
        }
        direction=normalize(outgoing);
    }
    Surface start=source;start.position=position;
    Surface hit;float confidence;
    vec3 color=refractionEnvironment(direction,source);
    if(traceScreen(start,direction,readUint(pushConsts.shadowInfoBufferIndex,85u),readUint(pushConsts.shadowInfoBufferIndex,84u),true,hit,confidence)) {
        ivec2 hp;
        if(screenPixel(hit.position,hp))color=mix(color,imageLoad(imagesRGBA16[readUint(pushConsts.shadowInfoBufferIndex,88u)],hp).rgb,confidence);
    }
    vec3 transported=max(color,vec3(0))*(1-exitF)+internalReflection*exitF;
    return transported*clamp(source.baseColor,vec3(0),vec3(1))*transmission*(1-frontF);
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(pixel,ivec2(pushConsts.gbufferSize)))) return;
    uint mode = pushConsts.outputMode;
    if (mode==0u && (pushConsts.featureFlags&40u)==32u) {
        // The opaque resolve already shaded this exact surface. Reuse it in
        // the final pass instead of repeating LPV, SSAO, SSR and material work.
        uint visible=imageLoad(imagesRG32UI[pushConsts.visibilityImageIndex],pixel).r;
        if(visible>0u && visible<=pushConsts.instanceCount) {
            uint material=loadInstanceInfo(visible-1u).materialID;
            if(material<pushConsts.materialCount && (loadMaterialInfo(material).flags&256u)==0u) {
                imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,
                    imageLoad(imagesRGBA16[readUint(pushConsts.shadowInfoBufferIndex,88u)],pixel));
                return;
            }
        }
    }
    // Geometry diagnostics must not depend on normals, UVs or materials being
    // decodable. Otherwise a valid rasterized triangle with an invalid shading
    // attribute appears to be missing even in the object/visibility views.
    if (mode == 4u || mode == 5u) {
        uvec4 vis = imageLoad(imagesRG32UI[pushConsts.visibilityImageIndex],pixel);
        vec3 color = vec3(0);
        if (vis.x != 0u && vis.x <= pushConsts.instanceCount) {
            uint id = mode == 4u ? loadInstanceInfo(vis.x-1u).objectID : (vis.x*1664525u+vis.y);
            color = fract(vec3(0.1031,0.11369,0.13787)*float(id % 65521u));
        }
        imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,vec4(color,1));
        return;
    }
    Surface surface;
    if (!decodeSurface(pixel,surface)) {
        imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,vec4(0));
        return;
    }
    float ao = (mode == 0u || mode == 6u || mode == 7u) ? ambientOcclusion(pixel,surface) : 1.0;
    vec3 color;
    if (mode == 1u) color = surface.baseColor;
    else if (mode == 2u) color = surface.normal*0.5+0.5;
    else if (mode == 3u) color = fract(abs(surface.position)*0.1);
    else if (mode == 6u || mode == 7u) color = vec3(ao);
    else if (mode == 8u || mode == 9u) color = vec3(computeSunShadow(surface.position,surface.normal));
    else {
        vec3 eye = readVec3(pushConsts.uniformBufferIndex,36u);
        // A dielectric reflects from inside as well as outside. Face only the
        // shading frame; retain outwardNormal for Snell/Fresnel medium selection
        // and retain the original geometry in diagnostic outputs.
        if(surface.material.transmission>0 && dot(surface.geometricNormal,eye-surface.position)<0) {
            surface.geometricNormal=-surface.geometricNormal;
            surface.normal=-surface.normal;
            surface.bitangent=-surface.bitangent;
        }
        vec3 coarse;
        color = shadeDirectWithIrradiance(surface,ao,normalize(eye-surface.position),true,coarse);
        if (surface.material.lightingEnabled > 0.5) {
            if((pushConsts.featureFlags&64u)!=0u && surface.material.transmission<=0) {
                vec3 correction=max(coarse+filteredScreenGi(pixel,surface),vec3(0))-coarse;
                color+=surface.baseColor*(1-surface.material.metallic)*correction/PI*ao*surface.material.occlusionStrength;
            }
            color += reflection(surface,ao);
            color += refraction(pixel,surface);
        }
        if ((pushConsts.featureFlags & 8u) != 0u) color = shadowCascadeDebugColor(computeShadowCascadeIndex(surface.position));
    }
    if (!finiteVec3(color)) color=vec3(0);
    imageStore(imagesRGBA16[pushConsts.finalOutputImage],pixel,vec4(clamp(color,vec3(0),vec3(65504)),1));
}
