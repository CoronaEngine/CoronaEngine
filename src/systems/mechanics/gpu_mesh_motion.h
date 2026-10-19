#pragma once

// Included in gpu_collision.cpp's implementation namespace. All final tests
// are triangle/triangle; node AABBs only prune candidates for those tests.
inline M::Vec3 rotate_motion_vertex(const M::Vec3& vertex, const M::Vec3& pivot,
                                    const M::Vec3& axis, const Float& angle) {
    const auto v=M::sub(vertex,pivot);
    return M::add(pivot,M::add(M::add(M::mul(v,cos(angle)),M::mul(M::cross(axis,v),sin(angle))),
        M::mul(axis,M::dot(axis,v)*(Float(1)-cos(angle)))));
}

inline Float triangle_separation(const std::array<M::Vec3,3>& a, const std::array<M::Vec3,3>& b) {
    std::array<M::Vec3,3> ea,eb;
    for(int i=0;i<3;++i) { ea[i]=M::sub(a[(i+1)%3],a[i]); eb[i]=M::sub(b[(i+1)%3],b[i]); }
    const auto na=M::cross(ea[0],ea[1]), nb=M::cross(eb[0],eb[1]);
    Float separation;
    separation=-huge;
    auto axis_test=[&](const M::Vec3& raw) {
        Float len; len=M::length(raw);
        $IF(len>Float(1e-8f)) {
            const auto axis=M::mul(raw,Float(1)/len);
            Float amin,amax,bmin,bmax;
            amin=M::dot(a[0],axis); amax=amin; bmin=M::dot(b[0],axis); bmax=bmin;
            for(int i=1;i<3;++i) { amin=min(amin,M::dot(a[i],axis)); amax=max(amax,M::dot(a[i],axis));
                bmin=min(bmin,M::dot(b[i],axis)); bmax=max(bmax,M::dot(b[i],axis)); }
            separation=max(separation,max(bmin-amax,amin-bmax));
        }
    };
    axis_test(na); axis_test(nb);
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) axis_test(M::cross(ea[i],eb[j]));
    for(int i=0;i<3;++i) { axis_test(M::cross(na,ea[i])); axis_test(M::cross(nb,eb[i])); }
    return separation;
}

inline Float triangle_rotation_toi(const std::array<M::Vec3,3>& a, const std::array<M::Vec3,3>& b,
                                  const M::Vec3& pivot, const M::Vec3& axis, const Float& angle) {
    Float radius,amin,amax,bmin,bmax;
    radius=0.f; amin=M::dot(a[0],axis); amax=amin; bmin=M::dot(b[0],axis); bmax=bmin;
    for(int i=0;i<3;++i) {
        radius=max(radius,M::length(M::cross(axis,M::sub(a[i],pivot))));
        amin=min(amin,M::dot(a[i],axis)); amax=max(amax,M::dot(a[i],axis));
        bmin=min(bmin,M::dot(b[i],axis)); bmax=max(bmax,M::dot(b[i],axis));
    }
    Float time,speed;
    time=2.f; speed=radius*abs(angle);
    const auto na=M::cross(M::sub(a[1],a[0]),M::sub(a[2],a[0]));
    const auto nb=M::cross(M::sub(b[1],b[0]),M::sub(b[2],b[0]));
    // Projection on the rotation axis cannot change. This also permits turning
    // on a resting horizontal triangle without treating that contact as a wall.
    $IF((speed>Float(1e-8f)) && (max(bmin-amax,amin-bmax)<Float(-1e-6f)) &&
        (M::dot(na,na)>Float(1e-18f)) && (M::dot(nb,nb)>Float(1e-18f))) {
        Uint step; Bool active; Float t;
        step=0u; active=true; t=0.f;
        $WHILE(active && (step<Uint(32u))) {
            std::array<M::Vec3,3> rotated;
            for(int i=0;i<3;++i) rotated[i]=rotate_motion_vertex(a[i],pivot,axis,angle*t);
            const auto gap=triangle_separation(rotated,b);
            Float advance; advance=max(Float(0),(gap-Float(.0005f))/speed)*Float(.9f);
            // Conservative advancement: angular speed bounds every vertex's
            // approach to this exact separating plane. No angular sampling can
            // jump through a thin triangle. Exhaustion retains the safe prefix.
            $IF(advance<Float(1e-5f)) { time=t; active=false; }
            $ELSE {
                $IF(t+advance>=Float(1)) { time=2.f; active=false; }
                $ELSE { t=t+advance; time=t; }
            }
            step=step+Uint(1u);
        }
    }
    return time;
}

inline void triangle_translation_toi(const std::array<M::Vec3,3>& a,
                                     const std::array<M::Vec3,3>& b,
                                     const M::Vec3& delta, Float& time, M::Vec3& normal) {
    std::array<M::Vec3,3> ea, eb;
    for(int i=0;i<3;++i) { ea[i]=M::sub(a[(i+1)%3],a[i]); eb[i]=M::sub(b[(i+1)%3],b[i]); }
    const auto na=M::cross(ea[0],ea[1]), nb=M::cross(eb[0],eb[1]);
    Float enter, leave, shallow;
    enter=-huge; leave=huge; shallow=huge;
    M::Vec3 entry_normal, overlap_normal;
    entry_normal.x=0.f; entry_normal.y=0.f; entry_normal.z=0.f;
    overlap_normal=entry_normal;
    Bool valid, initial;
    valid=(M::dot(na,na)>Float(1e-18f)) && (M::dot(nb,nb)>Float(1e-18f));
    initial=true;
    auto axis_test=[&](const M::Vec3& raw) {
        Float len;
        len=M::length(raw);
        $IF(len>Float(1e-8f)) {
            const auto axis=M::mul(raw,Float(1)/len);
            Float amin,amax,bmin,bmax,speed;
            amin=M::dot(a[0],axis); amax=amin; bmin=M::dot(b[0],axis); bmax=bmin;
            for(int i=1;i<3;++i) { amin=min(amin,M::dot(a[i],axis)); amax=max(amax,M::dot(a[i],axis));
                bmin=min(bmin,M::dot(b[i],axis)); bmax=max(bmax,M::dot(b[i],axis)); }
            // A millimetre contact tolerance on each separating axis, not a
            // replacement collision shape. Both windings and open meshes work.
            Float left,right;
            left=bmin-amax-Float(.001f); right=bmax-amin+Float(.001f);
            $IF((left>Float(0)) || (right<Float(0))) { initial=false; }
            Float overlap;
            overlap=min(-left,right);
            $IF(overlap<shallow) {
                shallow=overlap;
                overlap_normal=axis;
                $IF(-left<right) { overlap_normal=M::mul(axis,Float(-1)); }
            }
            speed=M::dot(delta,axis);
            $IF(abs(speed)<Float(1e-9f)) {
                $IF((left>Float(0)) || (right<Float(0))) { valid=false; }
            } $ELSE {
                Float t0,t1;
                t0=min(left/speed,right/speed); t1=max(left/speed,right/speed);
                $IF(t0>enter) {
                    enter=t0; entry_normal=axis;
                    $IF(speed>Float(0)) { entry_normal=M::mul(axis,Float(-1)); }
                }
                leave=min(leave,t1);
            }
        }
    };
    axis_test(na); axis_test(nb);
    for(int i=0;i<3;++i) for(int j=0;j<3;++j) axis_test(M::cross(ea[i],eb[j]));
    // Coplanar triangles need in-plane edge axes as well.
    for(int i=0;i<3;++i) { axis_test(M::cross(na,ea[i])); axis_test(M::cross(nb,eb[i])); }
    time=2.f; normal=entry_normal;
    $IF(valid && (enter<=leave) && (leave>=Float(0)) && (enter<=Float(1))) {
        $IF(initial) {
            // An existing resting contact must not block tangential travel or
            // movement away from the surface.
            normal=overlap_normal;
            // Within the contact tolerance, a triangle edge can have a smaller
            // overlap than the supporting face. Using that edge axis snags feet
            // on internal tessellation seams. Prefer the actual obstacle face
            // when the moving triangle rests entirely on one side of its plane.
            const auto face=M::mul(nb,Float(1)/max(Float(1e-9f),M::length(nb)));
            Float low,high;
            low=M::dot(M::sub(a[0],b[0]),face); high=low;
            for(int i=1;i<3;++i) {
                const auto distance=M::dot(M::sub(a[i],b[0]),face);
                low=min(low,distance); high=max(high,distance);
            }
            Float side,nearest;
            side=1.f; nearest=low;
            $IF(low+high<Float(0)) { side=-1.f; nearest=-high; }
            $IF((nearest>=Float(-.0012f)) && (nearest<=Float(.0012f))) {
                normal=M::mul(face,side);
                shallow=Float(.001f)-nearest;
            }
            $IF(shallow>Float(.0012f)) { time=-min(Float(.1f),shallow-Float(.001f)); }
            $ELSE { $IF(M::dot(delta,normal)<Float(-1e-8f)) { time=0.f; } }
        } $ELSE { time=max(Float(0),enter); }
    }
    // A resting mesh can enter a neighbouring floor triangle laterally without
    // initially overlapping it. Its in-plane edge axes must not turn the contact
    // tolerance into tiny vertical walls along every tessellation seam.
    const auto face=M::mul(nb,Float(1)/max(Float(1e-9f),M::length(nb)));
    Float face_low,face_high,face_speed;
    face_low=M::dot(M::sub(a[0],b[0]),face); face_high=face_low;
    for(int i=1;i<3;++i) {
        const auto distance=M::dot(M::sub(a[i],b[0]),face);
        face_low=min(face_low,distance); face_high=max(face_high,distance);
    }
    face_speed=M::dot(delta,face);
    $IF(((face_low>=Float(-.0012f)) && (face_speed>=Float(-1e-8f))) ||
        ((face_high<=Float(.0012f)) && (face_speed<=Float(1e-8f)))) { time=2.f; }
}

struct MeshSweepKernel {
    Floats va,vb,bounds,state,output;
    UInts ia,ib,nodes,leaves;
    Uint count,first,node_count,obstacle;
    H::ComputePipeline<> pipeline;
    explicit MeshSweepKernel(bool rotation_only=false) {
        count.as_push_constant(); first.as_push_constant(); node_count.as_push_constant(); obstacle.as_push_constant();
        pipeline=make_compute_pipeline([&] {
            Uint lane,ta;
            lane=dispatchThreadID()->x; ta=first+lane;
            $IF(lane<count) {
                const auto offset=load3(state.values,Uint(0u)), delta=load3(state.values,Uint(3u));
                const auto pivot=load3(state.values,Uint(20u)), axis=load3(state.values,Uint(6u));
                std::array<M::Vec3,3> a;
                for(std::uint32_t c=0;c<3;++c) {
                    a[c]=load3(va.values,ia.values[ta*Uint(3u)+Uint(c)]*Uint(3u));
                    if(!rotation_only) a[c]=M::add(rotate_motion_vertex(a[c],pivot,axis,state.values[23]),offset);
                }
                std::array<Float,3> lo,hi;
                lo[0]=min(min(a[0].x,a[1].x),a[2].x)+min(Float(0),delta.x)-Float(.002f);
                lo[1]=min(min(a[0].y,a[1].y),a[2].y)+min(Float(0),delta.y)-Float(.002f);
                lo[2]=min(min(a[0].z,a[1].z),a[2].z)+min(Float(0),delta.z)-Float(.002f);
                hi[0]=max(max(a[0].x,a[1].x),a[2].x)+max(Float(0),delta.x)+Float(.002f);
                hi[1]=max(max(a[0].y,a[1].y),a[2].y)+max(Float(0),delta.y)+Float(.002f);
                hi[2]=max(max(a[0].z,a[1].z),a[2].z)+max(Float(0),delta.z)+Float(.002f);
                if(rotation_only) {
                    Float reach; reach=0.f;
                    for(int c=0;c<3;++c) reach=max(reach,M::length(M::cross(axis,M::sub(a[c],pivot)))*abs(state.values[19]));
                    for(int c=0;c<3;++c) { lo[c]=lo[c]-reach; hi[c]=hi[c]+reach; }
                }
                Float best,best_b;
                best=2.f; best_b=-1.f;
                M::Vec3 normal;
                normal.x=0.f; normal.y=0.f; normal.z=0.f;
                Uint node;
                node=0u;
                Bool active;
                if(rotation_only) active=abs(state.values[19])>Float(1e-8f);
                else active=M::dot(delta,delta)>Float(1e-14f);
                $WHILE((node<node_count) && active) {
                    Bool overlap;
                    overlap=true;
                    for(std::uint32_t axis=0;axis<3;++axis)
                        $IF((lo[axis]>bounds.values[node*Uint(bounds_size)+Uint(axis+3u)]) ||
                            (hi[axis]<bounds.values[node*Uint(bounds_size)+Uint(axis)])) { overlap=false; }
                    Uint meta;
                    meta=node*Uint(node_size);
                    $IF(overlap) {
                        $IF(nodes.values[meta+Uint(4u)]==Uint(1u)) {
                            Uint k,end;
                            k=nodes.values[meta]; end=k+nodes.values[meta+Uint(1u)];
                            $WHILE(k<end) {
                                Uint tb;
                                tb=leaves.values[k];
                                std::array<M::Vec3,3> b;
                                for(std::uint32_t c=0;c<3;++c) b[c]=load3(vb.values,ib.values[tb*Uint(3u)+Uint(c)]*Uint(3u));
                                Float t;
                                M::Vec3 n;
                                Bool candidate;
                                candidate=(min(min(b[0].x,b[1].x),b[2].x)<=hi[0]) &&
                                    (max(max(b[0].x,b[1].x),b[2].x)>=lo[0]) &&
                                    (min(min(b[0].y,b[1].y),b[2].y)<=hi[1]) &&
                                    (max(max(b[0].y,b[1].y),b[2].y)>=lo[1]) &&
                                    (min(min(b[0].z,b[1].z),b[2].z)<=hi[2]) &&
                                    (max(max(b[0].z,b[1].z),b[2].z)>=lo[2]);
                                $IF(candidate) {
                                    if(rotation_only) {
                                        t=triangle_rotation_toi(a,b,pivot,axis,state.values[19]);
                                        n.x=0.f; n.y=0.f; n.z=0.f;
                                    } else triangle_translation_toi(a,b,delta,t,n);
                                    $IF(t<best) { best=t; normal=n; best_b=Float(tb); }
                                }
                                k=k+Uint(1u);
                            }
                            node=nodes.values[meta+Uint(2u)];
                        } $ELSE { node=nodes.values[meta+Uint(3u)]; }
                    } $ELSE { node=nodes.values[meta+Uint(2u)]; }
                }
                Uint o;
                o=lane*Uint(record_size);
                output.values[o]=best;
                output.values[o+Uint(1u)]=normal.x; output.values[o+Uint(2u)]=normal.y; output.values[o+Uint(3u)]=normal.z;
                output.values[o+Uint(4u)]=Float(obstacle); output.values[o+Uint(5u)]=Float(ta); output.values[o+Uint(6u)]=best_b;
                for(std::uint32_t i=7;i<record_size;++i) output.values[o+Uint(i)]=0.f;
            }
        },ktm::uvec3(threads,1,1));
    }
};

struct MeshSlideKernel {
    Floats state,contact;
    Uint iteration,snap;
    H::ComputePipeline<> pipeline;
    MeshSlideKernel() {
        iteration.as_push_constant(); snap.as_push_constant();
        pipeline=make_compute_pipeline([&] {
            $IF(dispatchThreadID()->x==Uint(0u)) {
                $IF(snap==Uint(2u)) {
                    state.values[23]=state.values[19]*max(Float(0),min(Float(1),contact.values[0]));
                    $IF(contact.values[0]<Float(1)) {
                        state.values[11]=1.f;
                        state.values[15]=contact.values[4]; state.values[16]=contact.values[5]; state.values[17]=contact.values[6];
                    }
                } $ELSE {
                auto delta=load3(state.values,Uint(3u));
                Float fraction;
                fraction=max(Float(0),min(Float(1),contact.values[0]));
                $IF((snap==Uint(0u)) || ((fraction<Float(1)) && (contact.values[2]>=Float(.65f)))) {
                    const auto moved=M::add(load3(state.values,Uint(0u)),M::mul(delta,fraction));
                    state.values[0]=moved.x; state.values[1]=moved.y; state.values[2]=moved.z;
                }
                $IF(contact.values[0]<Float(0)) {
                    const auto correction=M::mul(load3(contact.values,Uint(1u)),Float(.0002f)-contact.values[0]);
                    state.values[0]=state.values[0]+correction.x;
                    state.values[1]=state.values[1]+correction.y;
                    state.values[2]=state.values[2]+correction.z;
                }
                delta=M::mul(delta,Float(1)-fraction);
                $IF(contact.values[0]<=Float(1)) {
                    auto normal=load3(contact.values,Uint(1u));
                    state.values[11]=1.f;
                    for(std::uint32_t i=0;i<3;++i) state.values[12u+i]=contact.values[1u+i];
                    state.values[15]=contact.values[4]; state.values[16]=contact.values[5]; state.values[17]=contact.values[6];
                    $IF(normal.y>=Float(.65f)) { state.values[9]=1.f; }
                    $IF(normal.y<Float(-.1f)) { state.values[10]=1.f; }
                    Uint base;
                    base=Uint(24u)+iteration*Uint(3u);
                    state.values[base]=normal.x; state.values[base+Uint(1u)]=normal.y; state.values[base+Uint(2u)]=normal.z;
                    Float old_y;
                    old_y=delta.y;
                    for(std::uint32_t pass=0;pass<6;++pass) for(std::uint32_t p=0;p<6;++p) {
                        $IF(Uint(p)<=iteration) {
                            const auto n=load3(state.values,Uint(24u+p*3u));
                            delta=M::sub(delta,M::mul(n,min(Float(0),M::dot(delta,n))));
                        }
                    }
                    $IF((normal.y>Float(0)) && (normal.y<Float(.65f)) && (delta.y>max(Float(0),old_y))) {
                        delta.y=max(Float(0),old_y);
                        M::Vec3 horizontal;
                        horizontal.x=normal.x; horizontal.y=0.f; horizontal.z=normal.z;
                        delta=M::sub(delta,M::mul(horizontal,min(Float(0),M::dot(delta,horizontal))/max(Float(1e-8f),M::dot(horizontal,horizontal))));
                    }
                    for(std::uint32_t p=0;p<6;++p) {
                        $IF((Uint(p)<=iteration) && (M::dot(delta,load3(state.values,Uint(24u+p*3u)))<Float(-1e-5f))) {
                            delta.x=0.f; delta.y=0.f; delta.z=0.f;
                        }
                    }
                }
                state.values[3]=delta.x; state.values[4]=delta.y; state.values[5]=delta.z;
                // The seventh pass probes real triangle support after sliding.
                $IF(iteration==Uint(5u)) {
                    state.values[3]=0.f; state.values[4]=state.values[18]; state.values[5]=0.f;
                    state.values[9]=0.f;
                }
                }
            }
        },ktm::uvec3(threads,1,1));
    }
};
