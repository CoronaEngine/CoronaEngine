// Shared hierarchical screen traversal for diffuse GI and glossy reflection.
// Linear positive view Z avoids reversing the min reduction for reversed-Z.
// The caller supplies screenSize(), screenView(), screenProjection(),
// screenInverseProjection(), screenDepth(level,p) and screenLevels().
float screenConfidence(vec2 uv) {
    float edge=min(min(uv.x,uv.y),min(1.0-uv.x,1.0-uv.y));
    return smoothstep(.02,.15,edge);
}
vec3 screenViewPosition(ivec2 pixel,float depth) {
    vec4 p=screenInverseProjection()*vec4((vec2(pixel)+.5)/vec2(screenSize())*2.-1.,depth,1);
    return p.xyz/p.w;
}
// Perspective-correct segment/cell traversal. Empty/front-of-surface cells
// advance to their boundary; potential crossings descend to a single pixel.
// There is a hard iteration budget; exhaustion is a miss and uses LPV.
bool traceScreenHierarchy(vec3 position,vec3 normal,vec3 direction,float maxDistance,
                          float thickness,out ivec2 hitPixel,out float confidence) {
    confidence=0.;hitPixel=ivec2(0);
    mat4 view=screenView(),projection=screenProjection();
    vec3 a=(view*vec4(position+normal*max(.001,thickness*.25),1)).xyz;
    vec3 ray=mat3(view)*direction;
    float nearPlane=abs(screenViewPosition(ivec2(screenSize()/2u),1.).z);
    float distance=maxDistance;
    if(ray.z<0.)distance=min(distance,(a.z-nearPlane*1.01)/-ray.z);
    if(distance<=.02)return false;
    vec3 b=a+ray*distance;
    vec4 ca=projection*vec4(a,1),cb=projection*vec4(b,1);
    if(ca.w<=1e-5 || cb.w<=1e-5)return false;
    vec2 size=vec2(screenSize()),pa=(ca.xy/ca.w*.5+.5)*size,pb=(cb.xy/cb.w*.5+.5)*size;
    if(any(lessThan(pa,vec2(0))) || any(greaterThanEqual(pa,size)))return false;
    vec2 delta=pb-pa;
    float end=1.;
    for(int axis=0;axis<2;++axis) {
        if(delta[axis]>1e-6)end=min(end,(size[axis]-.501-pa[axis])/delta[axis]);
        else if(delta[axis]<-1e-6)end=min(end,(.001-pa[axis])/delta[axis]);
    }
    end=clamp(end,0.,1.);
    float ka=1./ca.w,kb=mix(ka,1./cb.w,end);
    float qa=a.z*ka,qb=mix(qa,b.z/cb.w,end);
    pb=mix(pa,pb,end);delta=pb-pa;
    float pixels=max(abs(delta.x),abs(delta.y));
    if(pixels<1.)return false;
    float epsilon=.002/pixels,t=min(1.,1.5/pixels);
    uint level=min(3u,screenLevels()-1u);
    for(uint iteration=0u;iteration<128u && t<1.;++iteration) {
        vec2 p=pa+delta*t;
        if(any(lessThan(p,vec2(0))) || any(greaterThanEqual(p,size)))break;
        float cellSize=float(1u<<level);
        ivec2 cell=ivec2(floor(p/cellSize));
        vec2 boundary=(vec2(cell)+step(vec2(0),delta))*cellSize;
        vec2 dt=vec2(1e20);
        for(int axis=0;axis<2;++axis)
            if(abs(delta[axis])>1e-7)dt[axis]=(boundary[axis]-p[axis])/delta[axis];
        float next=min(1.,t+max(min(dt.x,dt.y),epsilon));
        float za=mix(qa,qb,t)/mix(ka,kb,t),zb=mix(qa,qb,next)/mix(ka,kb,next);
        float sceneZ=screenDepth(level,cell);
        float tolerance=max(thickness,sceneZ*.001);
        if(sceneZ<1e29 && max(za,zb)>=sceneZ-tolerance*.25) {
            if(level>0u) {--level;continue;}
            // Reject rays already behind this depth sheet, rather than accepting
            // silhouette discontinuities as intersections with thick geometry.
            if(min(za,zb)<=sceneZ+tolerance) {
                float denominator=(qb-qa)-sceneZ*(kb-ka);
                float crossing=abs(denominator)>1e-10?(sceneZ*ka-qa)/denominator:t;
                crossing=clamp(crossing,t,next);
                vec3 hitView=screenViewPosition(cell,1.);hitView*=sceneZ/hitView.z;
                if(length(hitView-(view*vec4(position,1)).xyz)>.05) {
                    hitPixel=cell;
                    confidence=screenConfidence((vec2(cell)+.5)/size)*(1.-smoothstep(.75,1.,crossing));
                    return confidence>0.;
                }
            }
        }
        t=next+epsilon;
        level=min(level+1u,screenLevels()-1u);
    }
    return false;
}
