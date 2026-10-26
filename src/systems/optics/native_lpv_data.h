#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace Corona::Systems::OpticsDetail::LpvData {
using Vec = std::array<float,3>;
inline Vec add(Vec a,Vec b) {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
inline Vec sub(Vec a,Vec b) {for(unsigned i=0;i<3;++i)a[i]-=b[i];return a;}
inline Vec mul(Vec a,float b) {for(auto& v:a)v*=b;return a;}
inline float dot(Vec a,Vec b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline Vec cross(Vec a,Vec b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline Vec normalized(Vec a) {return mul(a,1.f/std::sqrt(std::max(dot(a,a),1e-30f)));}
inline float luminance(Vec a) {return std::max(0.f,.2126f*a[0]+.7152f*a[1]+.0722f*a[2]);}
inline uint32_t bits(float x) {return std::bit_cast<uint32_t>(x);}
inline double texel_solid_angle(unsigned row,unsigned w,unsigned h) {
    constexpr double pi=3.141592653589793;return 2*pi/w*(std::cos(pi*row/h)-std::cos(pi*(row+1)/h));
}
// One preset for every scene. Transport retains its angular channels until a
// surface interaction; only the final shading field is projected to RGB SH.
inline constexpr unsigned resolution=32,cell_count=resolution*resolution*resolution;
inline constexpr unsigned direction_count=64,header_words=256,cell_words=208;
inline constexpr unsigned sh_words=27,transport_words=direction_count*3;
inline constexpr unsigned propagation_steps=resolution*3,link_offset=196;
struct Direction {Vec vector;float weight;float sum;};
inline const std::array<Direction,direction_count> directions{{
    {{0.999877922f,0.000000000f,0.015625000f}, 0.18545264566f, 1.015502922f},
    {{-0.999877922f,-0.000000000f,-0.015625000f}, 0.18545264566f, 1.015502922f},
    {{-0.736558335f,0.674747770f,0.046875000f}, 0.18360403261f, 1.458181105f},
    {{0.736558335f,-0.674747770f,-0.046875000f}, 0.18360403261f, 1.458181105f},
    {{0.087158514f,-0.993126315f,0.078125000f}, 0.20603140180f, 1.158409829f},
    {{-0.087158514f,0.993126315f,-0.078125000f}, 0.20603140180f, 1.158409829f},
    {{0.604788567f,0.788839590f,0.109375000f}, 0.21282124925f, 1.503003157f},
    {{-0.604788567f,-0.788839590f,-0.109375000f}, 0.21282124925f, 1.503003157f},
    {{-0.974928320f,-0.172451092f,0.140625000f}, 0.19039847982f, 1.288004413f},
    {{0.974928320f,0.172451092f,-0.140625000f}, 0.19039847982f, 1.288004413f},
    {{0.831199175f,-0.528740877f,0.171875000f}, 0.18074022786f, 1.531815053f},
    {{-0.831199175f,0.528740877f,-0.171875000f}, 0.18074022786f, 1.531815053f},
    {{-0.254192286f,0.945582633f,0.203125000f}, 0.20166881818f, 1.402899919f},
    {{0.254192286f,-0.945582633f,-0.203125000f}, 0.20166881818f, 1.402899919f},
    {{-0.448069042f,-0.862729675f,0.234375000f}, 0.21220114315f, 1.545173718f},
    {{0.448069042f,0.862729675f,-0.234375000f}, 0.21220114315f, 1.545173718f},
    {{0.905577520f,0.330715458f,0.265625000f}, 0.19702384094f, 1.501917978f},
    {{-0.905577520f,-0.330715458f,-0.265625000f}, 0.19702384094f, 1.501917978f},
    {{-0.882672684f,0.364354453f,0.296875000f}, 0.18163553049f, 1.543902137f},
    {{0.882672684f,-0.364354453f,-0.296875000f}, 0.18163553049f, 1.543902137f},
    {{0.400379470f,-0.855587672f,0.328125000f}, 0.19365422512f, 1.584092142f},
    {{-0.400379470f,0.855587672f,-0.328125000f}, 0.19365422512f, 1.584092142f},
    {{0.279289677f,0.890419500f,0.359375000f}, 0.21346855808f, 1.529084177f},
    {{-0.279289677f,-0.890419500f,-0.359375000f}, 0.21346855808f, 1.529084177f},
    {{-0.796470088f,-0.461570696f,0.390625000f}, 0.20023951791f, 1.648665785f},
    {{0.796470088f,0.461570696f,-0.390625000f}, 0.20023951791f, 1.648665785f},
    {{0.885507002f,-0.194676230f,0.421875000f}, 0.18366960940f, 1.502058232f},
    {{-0.885507002f,0.194676230f,-0.421875000f}, 0.18366960940f, 1.502058232f},
    {{-0.512697398f,0.729259290f,0.453125000f}, 0.19206956339f, 1.695081688f},
    {{0.512697398f,-0.729259290f,-0.453125000f}, 0.19206956339f, 1.695081688f},
    {{-0.112428924f,-0.867606245f,0.484375000f}, 0.20583139827f, 1.464410169f},
    {{0.112428924f,0.867606245f,-0.484375000f}, 0.20583139827f, 1.464410169f},
    {{0.655162245f,0.552171434f,0.515625000f}, 0.20634814163f, 1.722958679f},
    {{-0.655162245f,-0.552171434f,-0.515625000f}, 0.20634814163f, 1.722958679f},
    {{-0.836499335f,0.034591874f,0.546875000f}, 0.18761892444f, 1.417966208f},
    {{0.836499335f,-0.034591874f,-0.546875000f}, 0.18761892444f, 1.417966208f},
    {{0.578368093f,-0.575553502f,0.578125000f}, 0.18677456163f, 1.732046595f},
    {{-0.578368093f,0.575553502f,-0.578125000f}, 0.18677456163f, 1.732046595f},
    {{-0.036624373f,0.792035835f,0.609375000f}, 0.20580184440f, 1.438035209f},
    {{0.036624373f,-0.792035835f,-0.609375000f}, 0.20580184440f, 1.438035209f},
    {{-0.491971015f,-0.589545698f,0.640625000f}, 0.20310482026f, 1.722141713f},
    {{0.491971015f,0.589545698f,-0.640625000f}, 0.20310482026f, 1.722141713f},
    {{0.734049986f,0.098765387f,0.671875000f}, 0.19299351642f, 1.504690374f},
    {{-0.734049986f,-0.098765387f,-0.671875000f}, 0.19299351642f, 1.504690374f},
    {{-0.583684672f,0.406112592f,0.703125000f}, 0.18962572256f, 1.692922265f},
    {{0.583684672f,-0.406112592f,-0.703125000f}, 0.18962572256f, 1.692922265f},
    {{0.148971653f,-0.662193934f,0.734375000f}, 0.19645112755f, 1.545540587f},
    {{-0.148971653f,0.662193934f,-0.734375000f}, 0.19645112755f, 1.545540587f},
    {{0.319830069f,0.558146116f,0.765625000f}, 0.20586908503f, 1.643601185f},
    {{-0.319830069f,-0.558146116f,-0.765625000f}, 0.20586908503f, 1.643601185f},
    {{-0.575563831f,-0.183620561f,0.796875000f}, 0.19449384754f, 1.556059392f},
    {{0.575563831f,0.183620561f,-0.796875000f}, 0.19449384754f, 1.556059392f},
    {{0.508856394f,-0.235104561f,0.828125000f}, 0.19022179593f, 1.572085955f},
    {{-0.508856394f,0.235104561f,-0.828125000f}, 0.19022179593f, 1.572085955f},
    {{-0.197410744f,0.471702881f,0.859375000f}, 0.19802303988f, 1.528488625f},
    {{0.197410744f,-0.471702881f,-0.859375000f}, 0.19802303988f, 1.528488625f},
    {{-0.153907284f,-0.427901457f,0.890625000f}, 0.19768137266f, 1.472433742f},
    {{0.153907284f,0.427901457f,-0.890625000f}, 0.19768137266f, 1.472433742f},
    {{0.342999753f,0.180271057f,0.921875000f}, 0.19782837476f, 1.445145810f},
    {{-0.342999753f,-0.180271057f,-0.921875000f}, 0.19782837476f, 1.445145810f},
    {{-0.292582713f,0.077123864f,0.953125000f}, 0.19472847419f, 1.322831577f},
    {{0.292582713f,-0.077123864f,-0.953125000f}, 0.19472847419f, 1.322831577f},
    {{0.095233309f,-0.148109676f,0.984375000f}, 0.19511041640f, 1.227717985f},
    {{-0.095233309f,0.148109676f,-0.984375000f}, 0.19511041640f, 1.227717985f}
}};
inline float cosine_integral(Vec n) {float sum=0;for(auto d:directions)sum+=d.weight*std::max(dot(n,d.vector),0.f);return sum;}
// Keep the first four words compatible with RSM material lookup. Links are
// coverage + RGB diffuse reflectance on the positive X/Y/Z dual-grid edges.
struct Cell {float occupied{};Vec albedo{};std::array<float,transport_words> source{};
    std::array<float,12> links{};};
static_assert(sizeof(Cell)==cell_words*sizeof(float));
struct Grid {
    Vec origin{};float size=1;
    std::vector<Cell> cells=std::vector<Cell>(cell_count);
    std::vector<float> weights=std::vector<float>(cell_count);
    bool inside(std::array<int,3> p) const {return p[0]>=0&&p[1]>=0&&p[2]>=0&&p[0]<int(resolution)&&p[1]<int(resolution)&&p[2]<int(resolution);}
    unsigned index(std::array<int,3> p) const {return (p[2]*resolution+p[1])*resolution+p[0];}
    std::array<int,3> coord(Vec p) const {auto q=mul(sub(p,origin),1/size);return {int(std::floor(q[0])),int(std::floor(q[1])),int(std::floor(q[2]))};}
    Vec center(std::array<int,3> p) const {return add(origin,mul({p[0]+.5f,p[1]+.5f,p[2]+.5f},size));}
    void fit(Vec low,Vec high) {
        auto d=sub(high,low);size=std::max({d[0],d[1],d[2],.01f})/float(resolution-6);
        origin=sub(mul(add(low,high),.5f),{size*resolution/2,size*resolution/2,size*resolution/2});
    }
    // Clip a triangle against a voxel/dual voxel, then integrate its actual
    // area. Full triangle areas would make the result depend on triangulation.
    static float clipped_area(Vec a,Vec b,Vec c,Vec middle,float half) {
        std::array<Vec,16> poly{},next{};poly[0]=a;poly[1]=b;poly[2]=c;unsigned count=3;
        for(unsigned axis=0;axis<3 && count>=3;++axis)for(float sign:{-1.f,1.f}) {
            unsigned n=0;float edge=middle[axis]+sign*half;
            for(unsigned i=0;i<count;++i) {
                Vec p=poly[i],q=poly[(i+1)%count];float dp=(p[axis]-edge)*sign,dq=(q[axis]-edge)*sign;
                if(dp<=0)next[n++]=p;
                if((dp<=0)!=(dq<=0))next[n++]=add(p,mul(sub(q,p),dp/(dp-dq)));
            }
            poly=next;count=n;
        }
        if(count<3)return 0;
        Vec total{};for(unsigned i=1;i+1<count;++i)total=add(total,cross(sub(poly[i],poly[0]),sub(poly[i+1],poly[0])));
        return .5f*std::sqrt(dot(total,total));
    }
    static bool overlaps(Vec a,Vec b,Vec c,Vec middle,float half) {return clipped_area(a,b,c,middle,half)>0;}
    void voxelize(Vec a,Vec b,Vec c,Vec albedo,float area) {
        Vec lo{},hi{};for(unsigned k=0;k<3;++k){lo[k]=std::min({a[k],b[k],c[k]});hi[k]=std::max({a[k],b[k],c[k]});}
        auto normal=normalized(cross(sub(b,a),sub(c,a)));
        // The regular voxel stores reflectance for RSM, but is never treated
        // as an entirely solid block. Transport is blocked only across links.
        auto l=coord(lo),h=coord(hi);for(unsigned k=0;k<3;++k){l[k]=std::clamp(l[k],0,int(resolution)-1);h[k]=std::clamp(h[k],0,int(resolution)-1);}
        for(int z=l[2];z<=h[2];++z)for(int y=l[1];y<=h[1];++y)for(int x=l[0];x<=h[0];++x) {
            std::array<int,3> p{x,y,z};float part=clipped_area(a,b,c,center(p),size*.50001f);if(part<=0)continue;
            auto i=index(p);cells[i].occupied+=part/(size*size);cells[i].albedo=add(cells[i].albedo,mul(albedo,part));weights[i]+=part;
        }
        for(unsigned axis=0;axis<3;++axis) {
            if(std::abs(normal[axis])<1e-6f)continue;
            Vec shift{};shift[axis]=size*.5f;auto low=coord(sub(lo,shift)),high=coord(sub(hi,shift));--low[axis];
            for(unsigned k=0;k<3;++k){low[k]=std::clamp(low[k],0,int(resolution)-1);high[k]=std::clamp(high[k],0,int(resolution)-1);}
            for(int z=low[2];z<=high[2];++z)for(int y=low[1];y<=high[1];++y)for(int x=low[0];x<=high[0];++x) {
                std::array<int,3> p{x,y,z};auto middle=add(center(p),shift);
                // A surface exactly on a dual-cell boundary belongs to one
                // slab only, rather than producing two opaque layers.
                if(hi[axis]<=middle[axis]-size*.5f)continue;
                float cover=clipped_area(a,b,c,middle,size*.50001f)*std::abs(normal[axis])/(size*size);
                auto& link=cells[index(p)].links;link[axis*4]+=cover;
                for(unsigned k=0;k<3;++k)link[axis*4+1+k]+=cover*albedo[k];
            }
        }
    }
    void finish() {
        for(unsigned i=0;i<cell_count;++i) {
            auto& cell=cells[i];if(weights[i]>0)cell.albedo=mul(cell.albedo,1/weights[i]);
            for(unsigned axis=0;axis<3;++axis) {auto at=axis*4;float cover=cell.links[at];
                if(cover>0)for(unsigned c=0;c<3;++c)cell.links[at+1+c]/=cover;
                cell.links[at]=std::clamp(cover,0.f,1.f);
            }
        }
    }
    void inject(Vec position,Vec normal,Vec flux,bool displace=true) {
        // Same-side normalized trilinear splat; no marching over occupied cells
        // and no source can disappear because a coarse cell contains geometry.
        auto sample=displace?add(position,mul(normal,size*.25f)):position;
        auto q=sub(mul(sub(sample,origin),1/size),{.5f,.5f,.5f});
        std::array<int,3> base{int(std::floor(q[0])),int(std::floor(q[1])),int(std::floor(q[2]))};
        Vec t{q[0]-base[0],q[1]-base[1],q[2]-base[2]};
        std::array<unsigned,8> indices{};std::array<float,8> w{};unsigned count=0;float sum=0;
        for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x) {
            std::array<int,3> p{base[0]+x,base[1]+y,base[2]+z};if(!inside(p))continue;
            if(displace && dot(sub(center(p),position),normal)<=size*1e-5f)continue;
            float weight=(x?t[0]:1-t[0])*(y?t[1]:1-t[1])*(z?t[2]:1-t[2]);
            indices[count]=index(p);w[count++]=weight;sum+=weight;
        }
        if(sum<=1e-10f)return;
        float normalization=cosine_integral(normal);
        for(unsigned d=0;d<direction_count;++d) {
            float angular=std::max(dot(normal,directions[d].vector),0.f)/(normalization*directions[d].sum);
            for(unsigned i=0;i<count;++i)for(unsigned c=0;c<3;++c)
                cells[indices[i]].source[d*3+c]+=flux[c]*angular*w[i]/sum;
        }
    }
};
} // namespace Corona::Systems::OpticsDetail::LpvData
