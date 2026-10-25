#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace Corona::Systems::OpticsDetail::LightData {
using Vec = std::array<float,3>;
inline Vec add(Vec a, Vec b) { for (unsigned i=0;i<3;++i) a[i]+=b[i]; return a; }
inline Vec sub(Vec a, Vec b) { for (unsigned i=0;i<3;++i) a[i]-=b[i]; return a; }
inline Vec mul(Vec a, float b) { for (auto& v:a) v*=b; return a; }
inline float dot(Vec a, Vec b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline Vec cross(Vec a, Vec b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
inline Vec normalized(Vec a) { return mul(a,1.f/std::sqrt(std::max(dot(a,a),1e-30f))); }
inline float luminance(Vec a) { return std::max(0.f,.2126f*a[0]+.7152f*a[1]+.0722f*a[2]); }
inline uint32_t bits(float x) { return std::bit_cast<uint32_t>(x); }
inline float number(uint32_t x) { return std::bit_cast<float>(x); }
inline uint32_t sample_budget(uint64_t pixels,uint64_t triangles) {
    const auto complexity=std::max<uint64_t>(1,triangles/500000);
    auto samples=std::clamp<uint64_t>(4194304/std::max<uint64_t>(1,pixels*complexity),2,64);
    return static_cast<uint32_t>(samples)&~1u; // equal environment/BRDF MIS counts
}

struct Triangle { Vec p, e1, e2; uint32_t emitter{}; }; // 1-based, zero for opaque blockers
struct Node { Vec low, high; uint32_t first{}, count{}, escape{}; };
// Preorder nodes have escape links so GPU traversal needs no per-ray stack.
inline void build_bvh(std::vector<Triangle>& triangles, std::vector<Node>& nodes,
                      uint32_t first, uint32_t count) {
    if (!count) return;
    Node node;
    node.low.fill(std::numeric_limits<float>::max());
    node.high.fill(-std::numeric_limits<float>::max());
    Vec centroid_low=node.low,centroid_high=node.high;
    for (uint32_t i=first;i<first+count;++i) {
        const auto& t=triangles[i];
        auto center=add(t.p,mul(add(t.e1,t.e2),1.f/3.f));
        for (auto p:{t.p,add(t.p,t.e1),add(t.p,t.e2)}) for (unsigned c=0;c<3;++c) {
            node.low[c]=std::min(node.low[c],p[c]);node.high[c]=std::max(node.high[c],p[c]);
        }
        for (unsigned c=0;c<3;++c) {
            centroid_low[c]=std::min(centroid_low[c],center[c]);centroid_high[c]=std::max(centroid_high[c],center[c]);
        }
    }
    const auto index=static_cast<uint32_t>(nodes.size()); nodes.push_back(node);
    if (count<=4) { nodes[index].first=first;nodes[index].count=count; }
    else {
        auto extent=sub(centroid_high,centroid_low);
        unsigned axis=std::max_element(extent.begin(),extent.end())-extent.begin();
        auto middle=first+count/2;
        // Binned surface-area heuristic avoids overlapping long room walls
        // and high-poly objects degrading to near-exhaustive shadow traversal.
        struct Bin { Vec low,high;uint32_t count{}; };
        auto empty=[] {Bin b;b.low.fill(std::numeric_limits<float>::max());b.high.fill(-std::numeric_limits<float>::max());return b;};
        auto merge=[](Bin& a,const Bin& b) {if(!b.count)return;a.count+=b.count;for(unsigned c=0;c<3;++c){a.low[c]=std::min(a.low[c],b.low[c]);a.high[c]=std::max(a.high[c],b.high[c]);}};
        auto area=[](const Bin& a) {if(!a.count)return 0.f;auto d=sub(a.high,a.low);return d[0]*d[1]+d[0]*d[2]+d[1]*d[2];};
        float best=std::numeric_limits<float>::max(),split=0;unsigned split_axis=axis;
        for(unsigned c=0;c<3;++c) {
            if(extent[c]<1e-12f) continue;
            std::array<Bin,16> bins,left,right;for(auto& bin:bins)bin=empty();
            for(uint32_t i=first;i<first+count;++i) {
                const auto& t=triangles[i];float center=t.p[c]+(t.e1[c]+t.e2[c])/3;
                auto b=std::min(15u,static_cast<unsigned>(std::max(0.f,(center-centroid_low[c])/extent[c]*16)));
                ++bins[b].count;
                for(auto p:{t.p,add(t.p,t.e1),add(t.p,t.e2)})for(unsigned k=0;k<3;++k){bins[b].low[k]=std::min(bins[b].low[k],p[k]);bins[b].high[k]=std::max(bins[b].high[k],p[k]);}
            }
            Bin l=empty(),r=empty();for(unsigned b=0;b<16;++b){merge(l,bins[b]);left[b]=l;merge(r,bins[15-b]);right[15-b]=r;}
            for(unsigned b=0;b<15;++b) if(left[b].count && right[b+1].count) {
                float cost=area(left[b])*left[b].count+area(right[b+1])*right[b+1].count;
                if(cost<best){best=cost;split_axis=c;split=centroid_low[c]+extent[c]*float(b+1)/16;}
            }
        }
        if(best<std::numeric_limits<float>::max()) middle=static_cast<uint32_t>(std::partition(triangles.begin()+first,triangles.begin()+first+count,
            [=](const auto& t){return t.p[split_axis]+(t.e1[split_axis]+t.e2[split_axis])/3<split;})-triangles.begin());
        if(middle==first || middle==first+count || best==std::numeric_limits<float>::max()) {
            middle=first+count/2;
            std::nth_element(triangles.begin()+first,triangles.begin()+middle,triangles.begin()+first+count,
                [axis](const auto& a,const auto& b){return a.p[axis]+(a.e1[axis]+a.e2[axis])/3 < b.p[axis]+(b.e1[axis]+b.e2[axis])/3;});
        }
        build_bvh(triangles,nodes,first,middle-first);build_bvh(triangles,nodes,middle,first+count-middle);
    }
    nodes[index].escape=static_cast<uint32_t>(nodes.size());
}

inline double texel_solid_angle(unsigned row, unsigned width, unsigned height) {
    constexpr double pi=3.14159265358979323846;
    return (2*pi/width)*(std::cos(pi*row/height)-std::cos(pi*(row+1)/height));
}
} // namespace Corona::Systems::OpticsDetail::LightData
