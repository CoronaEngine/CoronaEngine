#include "native_lighting.h"
#include <horizon/core/logging.h>
#include <cstring>
#include <numbers>
#include <array>

namespace Corona::Systems::OpticsDetail {
namespace {
using Vec=std::array<float,3>;
Vec add(Vec a,Vec b) {for(unsigned i=0;i<3;++i)a[i]+=b[i];return a;}
Vec mul(Vec a,float b) {for(auto& v:a)v*=b;return a;}
float dot(Vec a,Vec b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
Vec normalized(Vec a) {return mul(a,1.f/std::sqrt(std::max(dot(a,a),1e-30f)));}
uint32_t bits(float v) {return std::bit_cast<uint32_t>(v);}

using Json=nlohmann::json;
using Matrix=ktm::fmat4x4;
Vec transform(const Matrix& m,Vec p,float w=1) {
    Vec out{};
    for (unsigned r=0;r<3;++r) out[r]=m[0][r]*p[0]+m[1][r]*p[1]+m[2][r]*p[2]+m[3][r]*w;
    return out;
}
// Only environment reprojection reads texels on the CPU, under resource read
// handles. Scene geometry and material textures never enter a CPU light volume.
class Images {
    struct Image {
        unsigned width,height; Resource::ReadHandle<Resource::Image> source;
        bool srgb{};
        Vec pixel(size_t i) const {
            Vec result{};const auto channels=source->get_channels();
            for(unsigned c=0;c<3;++c){auto ch=channels<3?0:c;
                float v=source->get_float_data()?source->get_float_data()[i*channels+ch]:source->get_data()[i*channels+ch]/255.f;
                if(srgb)v=v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);
                result[c]=std::isfinite(v)?std::max(v,0.f):0.f;}
            return result;
        }
    };
    std::unordered_map<std::string,Image> images_;
    const Image& image(const Json& e) {
        const auto key=e.at("path").get<std::string>()+"|"+e.value("encoding","linear");
        if (auto found=images_.find(key);found!=images_.end()) return found->second;
        auto& manager=Resource::ResourceManager::get_instance();
        auto id=manager.import_sync(std::filesystem::u8path(e.at("path").get<std::string>()));
        auto source=manager.acquire_read<Resource::Image>(id);
        if (!source || !source->get_width() || !source->get_height()) throw std::runtime_error("Missing native light image: "+key);
        Image result{static_cast<unsigned>(source->get_width()),static_cast<unsigned>(source->get_height()),std::move(source),e.value("encoding","linear")=="srgb"};
        return images_.emplace(key,std::move(result)).first->second;
    }
public:
    void resolution(const Json& e,unsigned& width,unsigned& height) {
        if (!e.is_object()) return;
        if (e.at("kind")=="image") {
            const auto& im=image(e);width=std::max(width,im.width);height=std::max(height,im.height);
        }
        for (const char* child:{"a","b","weight"}) if(e.contains(child)) resolution(e.at(child),width,height);
    }
    Vec evaluate(const Json& e,float u=.5f,float v=.5f) {
        if (e.is_number()) { auto x=e.get<float>();return {x,x,x}; }
        if (e.is_array()) return {e[0].get<float>(),e[e.size()==1?0:1].get<float>(),e[e.size()==1?0:2].get<float>()};
        const auto kind=e.at("kind").get<std::string>();
        if (kind=="image") {
            const auto& im=image(e);
            float x=(u-std::floor(u))*im.width-.5f,y=std::clamp(v,0.f,1.f)*im.height-.5f;
            int ix=int(std::floor(x)),iy=int(std::floor(y));
            auto pixel=[&](int px,int py) { px=(px%int(im.width)+int(im.width))%int(im.width);py=std::clamp(py,0,int(im.height)-1);return im.pixel(size_t(py)*im.width+px); };
            float fx=x-ix,fy=y-iy;
            return add(mul(add(mul(pixel(ix,iy),1-fx),mul(pixel(ix+1,iy),fx)),1-fy),
                mul(add(mul(pixel(ix,iy+1),1-fx),mul(pixel(ix+1,iy+1),fx)),fy));
        }
        auto a=evaluate(e.at("a"),u,v);
        if (kind=="sqrt") { for (auto& x:a) x=std::sqrt(std::max(x,0.f));return a; }
        auto b=evaluate(e.at("b"),u,v);
        if (kind=="multiply") { for (unsigned c=0;c<3;++c) a[c]*=b[c];return a; }
        if (kind=="mix") {auto t=evaluate(e.at("weight"),u,v);for (unsigned c=0;c<3;++c) a[c]=a[c]*(1-t[c])+b[c]*t[c];return a;}
        return mul(add(a,b),.5f);
    }
};

struct EnvironmentMap {
    unsigned width=512,height=256;
    std::vector<float> pixels;
    std::array<Vec,9> sh{};
};
EnvironmentMap environment_map(const Json& environments,Images& images) {
    struct Layer { Json color; Matrix inverse;bool equal_area; };
    std::vector<Layer> layers;
    for (const auto& e:environments) {
        Matrix m{};for(unsigned c=0;c<4;++c) for(unsigned r=0;r<4;++r) m[c][r]=e.at("matrix")[r*4+c].get<float>();
        layers.push_back({e.at("color"),ktm::inverse(m),e.value("projection","")=="equal_area"});
    }
    EnvironmentMap out;
    if (layers.empty()) out.width=out.height=1;
    // Match the shared/Vision reprojection resolution. A fixed thumbnail can
    // miss a tiny bright HDR sun entirely and change the direct illumination.
    for (const auto& layer:layers) images.resolution(layer.color,out.width,out.height);
    if (!layers.empty()) {out.height=std::max(out.height,(out.width+1)/2);out.width=out.height*2;}
    out.pixels.resize(size_t(out.width)*out.height*4);
    constexpr float pi=std::numbers::pi_v<float>;
    for (unsigned y=0;y<out.height;++y) for(unsigned x=0;x<out.width;++x) {
        // Canonical Corona world coordinates, identical to Vision reprojection.
        const float theta=(y+.5f)/out.height*pi,phi=(x+.5f)/out.width*2*pi;
        Vec world{std::sin(theta)*std::cos(phi),std::cos(theta),-std::sin(theta)*std::sin(phi)},color{};
        for(const auto& layer:layers) {
            auto d=normalized(transform(layer.inverse,world,0));float u,v;
            if(layer.equal_area) {
                const float r=std::sqrt(std::max(0.f,1.f-std::abs(d[2])));
                v=std::atan2(std::abs(d[1]),std::abs(d[0]))*2/pi*r;u=r-v;
                if(d[2]<0) {std::swap(u,v);u=1-u;v=1-v;}
                u=(std::copysign(u,d[0])+1)*.5f;v=(std::copysign(v,d[1])+1)*.5f;
            } else {u=std::atan2(d[0],-d[2])/(2*pi);v=std::acos(std::clamp(d[1],-1.f,1.f))/pi;}
            color=add(color,images.evaluate(layer.color,u,v));
        }
        for(unsigned c=0;c<3;++c) out.pixels[(size_t(y)*out.width+x)*4+c]=std::max(0.f,color[c]);
        out.pixels[(size_t(y)*out.width+x)*4+3]=1;
        const double angle=2.0*pi/out.width*(std::cos(double(y)/out.height*pi)-std::cos(double(y+1)/out.height*pi));
        const auto [wx,wy,wz]=world;
        const float basis[9]={.282094792f,.488602512f*wy,.488602512f*wz,.488602512f*wx,
            1.092548431f*wx*wy,1.092548431f*wy*wz,.315391565f*(3*wz*wz-1),
            1.092548431f*wx*wz,.546274215f*(wx*wx-wy*wy)};
        for(unsigned i=0;i<9;++i)out.sh[i]=add(out.sh[i],mul(color,float(angle)*basis[i]));
    }
    return out;
}

} // namespace

struct NativeLightingCache::State {
    std::size_t revision=~std::size_t(0);
    std::shared_ptr<const nlohmann::json> owner;
    Horizon::HardwareExecutor executor;
};

std::shared_ptr<NativeLighting> NativeLightingCache::obtain(const EnvironmentDevice& env) {
    if(!state_)state_=std::make_shared<State>();
    auto& cache=*state_;
    if(value_ && cache.revision==env.authored_lighting_revision && cache.owner==env.authored_lighting)
        return value_;
    Images images;
    auto map=environment_map(env.authored_lighting?env.authored_lighting->at("environments"):Json::array(),images);
    auto result=std::make_shared<NativeLighting>();
    result->authored_environment=bool(env.authored_lighting);
    result->environment=Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(map.width,map.height,
        Horizon::Format::RGBA32_FLOAT,Horizon::ImageUsage_Sampled|Horizon::ImageUsage_TransferDst,"native.environment"));
    result->environment_memory=std::make_shared<Memory::GpuMemToken>(Memory::ResKind::Texture,map.pixels.size()*sizeof(float));
    Horizon::HardwareBufferDesc staging;
    staging.element_count=map.pixels.size();staging.element_size=sizeof(float);
    staging.usage=Horizon::BufferUsage_TransferSrc;staging.cpu_access=Horizon::CpuAccessMode::Write;
    result->uploads.emplace_back(staging,std::as_bytes(std::span(map.pixels)));
    // Environment descriptor plus radiance SH9; there is no scene-sized allocation.
    std::array<uint32_t,64> words{};
    words[8]=result->environment.store_descriptor();
    for(unsigned i=0;i<9;++i)for(unsigned c=0;c<3;++c)words[16+i*3+c]=bits(map.sh[i][c]);
    Horizon::HardwareBufferDesc metadata;
    metadata.element_count=words.size();metadata.element_size=sizeof(uint32_t);
    metadata.usage=Horizon::BufferUsage_Storage;metadata.cpu_access=Horizon::CpuAccessMode::Write;
    metadata.debug_name="native.environment.metadata";
    result->data=Horizon::HardwareBuffer(metadata,std::as_bytes(std::span(words)));
    result->ready=cache.executor.stream()<<result->environment.copy_from(result->uploads.back())<<Horizon::commit();
    cache.revision=env.authored_lighting_revision;cache.owner=env.authored_lighting;
    value_=result;
    return result;
}
} // namespace Corona::Systems::OpticsDetail
