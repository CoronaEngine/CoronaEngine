// Link-aware interpolation stays on a connected side of the receiver surface.
float lpvEdge(uint info,uint level,ivec3 p,int axis,int sign,int n) {
    if(sign<0)p[axis]-=1;
    if(any(lessThan(p,ivec3(0)))||any(greaterThanEqual(p,ivec3(n))))return 0;
    uint cell=level*uint(n*n*n)+uint((p.z*n+p.y)*n+p.x);
    return clamp(readFloat(info,256u+cell*208u+196u+uint(axis)*4u),0,1);
}
float lpvConnection(uint info,uint level,ivec3 a,ivec3 b,int n) {
    if(all(equal(a,b)))return 1;
    const ivec3 orders[6]=ivec3[6](ivec3(0,1,2),ivec3(0,2,1),ivec3(1,0,2),ivec3(1,2,0),ivec3(2,0,1),ivec3(2,1,0));
    float best=0;
    for(int order=0;order<6;++order){ivec3 p=a;float visibility=1;
        for(int k=0;k<3;++k){int axis=orders[order][k],sign=b[axis]>p[axis]?1:-1;
            if(p[axis]!=b[axis]){visibility*=1-lpvEdge(info,level,p,axis,sign,n);p[axis]+=sign;}}
        best=max(best,visibility);
        // Every link transmission is in [0,1]. Once an unobstructed path is
        // found no remaining permutation can improve it (the result is exact).
        if(best>=1)return 1;
    }
    return best;
}
vec3 sampleLpvLevel(uint level,vec3 position,vec3 normal,vec3 direction,bool diffuse) {
    vec3 query=-direction;float xq=query.x,yq=query.y,zq=query.z;
    float basis[9]=float[9](.282094792,.488602512*xq,.488602512*yq,.488602512*zq,
        1.092548431*xq*yq,1.092548431*yq*zq,.315391565*(3*zq*zq-1),
        1.092548431*xq*zq,.546274215*(xq*xq-yq*yq));
    if(diffuse){basis[0]*=PI;for(int k=1;k<4;++k)basis[k]*=2*PI/3;for(int k=4;k<9;++k)basis[k]*=PI/4;}
    uint info=pushConsts.directLightingBufferIndex;
    int n=int(readUint(info,0u));float size=readFloat(info,227u+level*4u);
    uint radiance=readUint(pushConsts.shadowInfoBufferIndex,75u);
    vec3 origin=readVec3(info,224u+level*4u),samplePosition=position+normal*size*.25;
    vec3 q=(samplePosition-origin)/size-.5,t=fract(q);ivec3 base=ivec3(floor(q));
    float weights[8];float best=-1;ivec3 anchor=base;
    for(int corner=0;corner<8;++corner){ivec3 d=ivec3(corner&1,(corner>>1)&1,(corner>>2)&1),p=base+d;
        vec3 w=mix(vec3(1)-t,t,vec3(d));float value=w.x*w.y*w.z;
        if(any(lessThan(p,ivec3(0)))||any(greaterThanEqual(p,ivec3(n)))||dot(origin+(vec3(p)+.5)*size-position,normal)<=size*1e-5)value=0;
        weights[corner]=value;if(value>best){best=value;anchor=p;}
    }
    vec3 result=vec3(0);float weight=0;
    for(int corner=0;corner<8;++corner){if(weights[corner]<=0)continue;
        ivec3 p=base+ivec3(corner&1,(corner>>1)&1,(corner>>2)&1);
        float w=weights[corner]*lpvConnection(info,level,anchor,p,n);uint cell=level*uint(n*n*n)+uint((p.z*n+p.y)*n+p.x);
        for(uint c=0u;c<3u;++c){float value=0;for(uint k=0u;k<9u;++k)value+=readFloat(radiance,cell*27u+c*9u+k)*basis[k];result[c]+=w*max(value,0);}
        weight+=w;
    }
    return weight>1e-10?result/weight:vec3(0);
}
vec3 sampleLpv(vec3 position,vec3 normal,vec3 direction,bool diffuse) {
    uint info=pushConsts.directLightingBufferIndex;
    float n=float(readUint(info,0u));uint levels=readUint(info,12u);
    // Blend radiance, never add overlapping levels. The four-cell overlap is
    // wider than the interpolation footprint and fades before a child boundary.
    vec3 result=vec3(0);float remaining=1;
    for(int candidate=int(levels)-1;candidate>=0;--candidate) {
        uint level=uint(candidate);
        vec3 q=(position-readVec3(info,224u+level*4u))/readFloat(info,227u+level*4u);
        vec3 edge=min(q,vec3(n)-q);
        float weight=level==0u?1:smoothstep(1.5,4.5,min(edge.x,min(edge.y,edge.z)));
        if(weight>0)result+=remaining*weight*sampleLpvLevel(level,position,normal,direction,diffuse);
        remaining*=1-weight;if(remaining<1e-5)break;
    }
    return result;
}
vec3 lpvIrradiance(vec3 position,vec3 normal) {
    return sampleLpv(position,normal,normal,true);
}
