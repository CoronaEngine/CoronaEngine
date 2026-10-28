// Single-bounce diffuse transport from the four solar reflective shadow maps.
// This runs only at the bounded GI resolution, never in the full-size resolve.
uint rsmDescriptor(uint cascade,uint channel) {
    return readUint(pushConsts.shadowInfoBufferIndex,156u+cascade*4u+channel);
}
bool rsmProjection(uint cascade,vec3 position,out vec2 uv) {
    vec4 p=readMat4(pushConsts.shadowInfoBufferIndex,cascade*16u)*vec4(position,1);
    vec3 ndc=p.xyz/p.w;uv=ndc.xy*.5+.5;
    return all(greaterThanEqual(uv,vec2(0))) && all(lessThan(uv,vec2(1))) && ndc.z>=0. && ndc.z<=1.;
}
// Split overlapping cascades by source, not receiver, so a surface contributes
// once even when it appears in all four light views.
bool rsmContains(uint cascade,vec3 source,vec3 normal) {
    vec2 uv;if(!rsmProjection(cascade,source,uv))return false;
    uint image=rsmDescriptor(cascade,0u);
    ivec2 p=ivec2(uv*vec2(textureSize(textures[nonuniformEXT(image)],0)));
    vec4 q=texelFetch(textures[nonuniformEXT(image)],p,0);
    float texel=sqrt(readFloat(pushConsts.shadowInfoBufferIndex,159u+cascade*4u));
    return q.w>0. && length(q.xyz-source)<texel*3. && abs(dot(q.xyz-source,normal))<max(.001,texel*.1);
}
float screenPathWeight(vec3 receiver,vec3 source) {
    vec4 a=screenProjection()*screenView()*vec4(receiver,1);
    vec4 b=screenProjection()*screenView()*vec4(source,1);
    if(a.w<=0. || b.w<=0.)return 0.;
    return screenConfidence(a.xy/a.w*.5+.5)*screenConfidence(b.xy/b.w*.5+.5)*
        (1.-smoothstep(pushConsts.ssrMaxDistance*.75,pushConsts.ssrMaxDistance,length(source-receiver)));
}
bool sourceOnScreen(vec3 source,float area) {
    vec3 view=(screenView()*vec4(source,1)).xyz;
    vec4 clip=screenProjection()*vec4(view,1);
    if(clip.w<=0.)return false;
    vec2 uv=clip.xy/clip.w*.5+.5;
    if(any(lessThan(uv,vec2(0))) || any(greaterThanEqual(uv,vec2(1))))return false;
    float z=screenDepth(0u,ivec2(uv*vec2(screenSize())));
    return abs(z-view.z)<max(.002,max(view.z*.001,sqrt(area)*.1));
}
// Only reject actual depth-sheet crossings. Being behind a foreground surface
// is not evidence that an off-screen segment intersects it.
float rsmScreenVisibility(vec3 receiver,vec3 source) {
    vec3 a=(screenView()*vec4(receiver,1)).xyz,b=(screenView()*vec4(source,1)).xyz;
    float previousGap=0.;bool previousValid=false;
    for(int i=1;i<=NATIVE_RSM_VISIBILITY_STEPS;++i) {
        float t=float(i)/float(NATIVE_RSM_VISIBILITY_STEPS+1);
        vec3 v=mix(a,b,t);vec4 clip=screenProjection()*vec4(v,1);
        vec2 uv=clip.xy/max(clip.w,1e-6)*.5+.5;
        if(clip.w<=0. || any(lessThan(uv,vec2(0))) || any(greaterThanEqual(uv,vec2(1)))) {previousValid=false;continue;}
        float z=screenDepth(0u,ivec2(uv*vec2(screenSize()))),gap=v.z-z;
        float thickness=max(pushConsts.ssrThickness,z*.001);
        if(z<1e29 && gap>0. && gap<thickness && previousValid && previousGap<0.)return 0.;
        previousGap=gap;previousValid=z<1e29;
    }
    return 1.;
}
vec3 gatherRsm(Surface receiver,uint seed) {
    if(pushConsts.shadowEnabled==0u)return vec3(0);
    vec3 result=vec3(0);
    const float radius=NATIVE_RSM_LOCAL_RADIUS;
    for(uint cascade=0u;cascade<uint(NATIVE_SHADOW_CASCADES);++cascade) {
        vec2 center;rsmProjection(cascade,receiver.position,center);
        uint posTex=rsmDescriptor(cascade,0u),normalTex=rsmDescriptor(cascade,1u),fluxTex=rsmDescriptor(cascade,2u);
        ivec2 size=textureSize(textures[nonuniformEXT(posTex)],0);
        for(uint i=0u;i<uint(NATIVE_RSM_SAMPLES_PER_CASCADE);++i) {
            vec2 u=randomPair(i,seed+cascade*7919u);
            // Equal mixture: local disk for nearby transport + full-map support.
            // The PDF includes both branches, including clipped disk samples.
            vec2 uv=(i&1u)==0u?center+radius*sqrt(u.x)*vec2(cos(2.*PI*u.y),sin(2.*PI*u.y)):u;
            if(any(lessThan(uv,vec2(0))) || any(greaterThanEqual(uv,vec2(1))))continue;
            ivec2 p=ivec2(uv*vec2(size));
            vec4 position=texelFetch(textures[nonuniformEXT(posTex)],p,0);
            if(position.w<=0.)continue;
            vec4 normalArea=texelFetch(textures[nonuniformEXT(normalTex)],p,0);
            vec3 delta=position.xyz-receiver.position;float distance2=dot(delta,delta);
            if(distance2<1e-8)continue;
            vec3 direction=delta*inversesqrt(distance2);
            float cosine=max(dot(receiver.geometricNormal,direction),0.)*max(dot(normalArea.xyz,-direction),0.);
            if(cosine<=0.)continue;
            bool duplicate=false;
            for(uint finer=0u;finer<cascade;++finer)if(rsmContains(finer,position.xyz,normalArea.xyz)){duplicate=true;break;}
            if(duplicate)continue;
            float coverage=sourceOnScreen(position.xyz,normalArea.w)?screenPathWeight(receiver.position,position.xyz):0.;
            if(coverage>=1.)continue;
            float pdf=.5+(dot(uv-center,uv-center)<radius*radius?.5/(PI*radius*radius):0.);
            vec3 flux=texelFetch(textures[nonuniformEXT(fluxTex)],p,0).rgb;
            // Finite patch regularization prevents the VPL singularity near a
            // source. Scale comes from the actual texel area, not a scene knob.
            float geometry=cosine/max(distance2,normalArea.w/PI);
            result+=flux*geometry*(float(size.x*size.y)/(pdf*float(NATIVE_RSM_SAMPLES_PER_CASCADE)))*
                (1.-coverage)*rsmScreenVisibility(receiver.position,position.xyz);
        }
    }
    return result;
}
