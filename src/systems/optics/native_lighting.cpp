#include "native_lighting.h"
#include "native_light_data.h"
#include <horizon/core/logging.h>
#include <cstring>
#include <numbers>

namespace Corona::Systems::OpticsDetail {
namespace {
using namespace LightData;
using Json=nlohmann::json;
using Matrix=ktm::fmat4x4;
Matrix multiply(const Matrix& a,const Matrix& b) {
    Matrix out{};
    for (unsigned c=0;c<4;++c) for (unsigned r=0;r<4;++r)
        for (unsigned k=0;k<4;++k) out[c][r]+=a[k][r]*b[c][k];
    return out;
}
Vec transform(const Matrix& m,Vec p,float w=1) {
    Vec out{};
    for (unsigned r=0;r<3;++r) out[r]=m[0][r]*p[0]+m[1][r]*p[1]+m[2][r]*p[2]+m[3][r]*w;
    return out;
}
// CPU evaluation is only used for the environment reprojection and emission
// importance weights. Actual surface/emitter textures use the GPU expression.
class Images {
    struct Image { unsigned width,height; std::vector<Vec> pixels; Vec average{}; };
    std::unordered_map<std::string,Image> images_;
    const Image& image(const Json& e) {
        const auto key=e.at("path").get<std::string>()+"|"+e.value("encoding","linear");
        if (auto found=images_.find(key);found!=images_.end()) return found->second;
        auto& manager=Resource::ResourceManager::get_instance();
        auto id=manager.import_sync(std::filesystem::u8path(e.at("path").get<std::string>()));
        auto source=manager.acquire_read<Resource::Image>(id);
        if (!source || !source->get_width() || !source->get_height()) throw std::runtime_error("Missing native light image: "+key);
        Image result{static_cast<unsigned>(source->get_width()),static_cast<unsigned>(source->get_height())};
        result.pixels.resize(size_t(result.width)*result.height);
        const auto channels=source->get_channels();std::array<double,3> sum{};
        for (size_t i=0;i<result.pixels.size();++i) for (unsigned c=0;c<3;++c) {
            const auto ch=channels<3?0:c;
            float v=source->get_float_data()?source->get_float_data()[i*channels+ch]:source->get_data()[i*channels+ch]/255.f;
            if (e.value("encoding","linear")=="srgb") v=v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);
            result.pixels[i][c]=std::isfinite(v)?std::max(v,0.f):0.f;sum[c]+=result.pixels[i][c];
        }
        for (unsigned c=0;c<3;++c) result.average[c]=float(sum[c]/result.pixels.size());
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
    Vec evaluate(const Json& e,float u=.5f,float v=.5f,bool average=false) {
        if (e.is_number()) { auto x=e.get<float>();return {x,x,x}; }
        if (e.is_array()) return {e[0].get<float>(),e[e.size()==1?0:1].get<float>(),e[e.size()==1?0:2].get<float>()};
        const auto kind=e.at("kind").get<std::string>();
        if (kind=="image") {
            const auto& im=image(e);if (average) return im.average;
            float x=(u-std::floor(u))*im.width-.5f,y=std::clamp(v,0.f,1.f)*im.height-.5f;
            int ix=int(std::floor(x)),iy=int(std::floor(y));
            auto pixel=[&](int px,int py) { px=(px%int(im.width)+int(im.width))%int(im.width);py=std::clamp(py,0,int(im.height)-1);return im.pixels[size_t(py)*im.width+px]; };
            float fx=x-ix,fy=y-iy;
            return add(mul(add(mul(pixel(ix,iy),1-fx),mul(pixel(ix+1,iy),fx)),1-fy),
                mul(add(mul(pixel(ix,iy+1),1-fx),mul(pixel(ix+1,iy+1),fx)),fy));
        }
        auto a=evaluate(e.at("a"),u,v,average);
        if (kind=="sqrt") { for (auto& x:a) x=std::sqrt(std::max(x,0.f));return a; }
        auto b=evaluate(e.at("b"),u,v,average);
        if (kind=="multiply") { for (unsigned c=0;c<3;++c) a[c]*=b[c];return a; }
        if (kind=="mix") {auto t=evaluate(e.at("weight"),u,v,average);for (unsigned c=0;c<3;++c) a[c]=a[c]*(1-t[c])+b[c]*t[c];return a;}
        return mul(add(a,b),.5f);
    }
};

struct EnvironmentMap { unsigned width=512,height=256; std::vector<float> pixels;std::vector<double> cdf;double integral{}; };
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
    out.pixels.resize(size_t(out.width)*out.height*4);out.cdf.reserve(size_t(out.width)*out.height);
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
        out.integral+=luminance(color)*texel_solid_angle(y,out.width,out.height);
        out.cdf.push_back(out.integral);
    }
    return out;
}

struct Emitter { Triangle triangle;std::array<float,6> uv;Vec emission;float area;double weight;uint32_t program;bool two_sided; };
void append(std::vector<uint32_t>& out,Vec v) {for(float x:v) out.push_back(bits(x));}
}

std::shared_ptr<NativeLighting> NativeLightingCache::obtain(const SceneDevice& scene,
    const EnvironmentDevice& env,std::size_t signature,NativeAppearanceCache& appearances) {
    signature^=env.authored_lighting_revision+0x9e3779b97f4a7c15ULL+(signature<<6)+(signature>>2);
    for (auto actor:scene.actor_handles) signature^=actor+0x9e3779b97f4a7c15ULL+(signature<<6)+(signature>>2);
    if(value_ && signature_==signature) return value_;
    auto result=std::make_shared<NativeLighting>();result->authored_environment=bool(env.authored_lighting);
    auto& hub=SharedDataHub::instance();auto& manager=Resource::ResourceManager::get_instance();
    Images images;std::vector<Triangle> triangles;std::vector<Emitter> emitters;
    std::unordered_map<const Materials::Principled*,std::pair<Vec,uint32_t>> emission_cache;
    // Deliberately no camera visibility or residency filtering: an off-screen
    // lamp still illuminates the room, and an off-screen wall still blocks it.
    for(auto actor_handle:scene.actor_handles) {
        auto actor=hub.actor_storage().try_acquire_read(actor_handle);if(!actor || actor->follow_camera) continue;
        for(auto ph:actor->profile_handles) {
            auto profile=hub.profile_storage().try_acquire_read(ph);if(!profile) continue;
            auto optics=hub.optics_storage().try_acquire_read(profile->optics_handle);if(!optics || !optics->visible) continue;
            auto geometry=hub.geometry_storage().try_acquire_read(optics->geometry_handle);if(!geometry) continue;
            Resource::ReadHandle<Resource::Scene> source;
            if(auto model=hub.model_resource_storage().try_acquire_read(geometry->model_resource_handle);model && model->model_id)
                source=manager.acquire_read<Resource::Scene>(model->model_id);
            Matrix world=Matrix::from_eye();
            if(auto transform=hub.model_transform_storage().try_acquire_read(geometry->transform_handle)) world=transform->compute_matrix();
            Matrix correction=Matrix::from_eye();
            for(unsigned c=0;c<3;++c) {correction[c][c]=geometry->native_local_correction_scale;correction[3][c]=geometry->native_local_correction_offset[c];}
            world=multiply(world,correction);
            auto instances=geometry->mesh_instances;
            if(instances.empty() && source) for(const auto& instance:source->data.mesh_instances) {
                MeshInstanceDevice i;i.mesh_index=instance.mesh_index;
                for(unsigned c=0;c<4;++c) for(unsigned r=0;r<4;++r) i.transform[c][r]=instance.transform[c*4+r];instances.push_back(i);
            }
            if(instances.empty()) for(unsigned i=0;i<(source?source->data.meshes.size():geometry->mesh_handles.size());++i) {MeshInstanceDevice instance;instance.mesh_index=i;instances.push_back(instance);}
            for(const auto& instance:instances) {
                if(!instance.visible) continue;
                const auto mesh_index=instance.mesh_index;
                std::span<const Resource::Vertex> vertices;std::vector<Resource::Vertex> mapped_vertices;
                std::vector<uint32_t> indices;
                if(source && mesh_index<source->data.meshes.size()) {
                    vertices=source->get_mesh_vertices(mesh_index);
                    const auto& src=source->get_mesh_indices(mesh_index);indices.assign(src.begin(),src.end());
                    if(geometry->is_skinned && mesh_index<geometry->skinned_cpu_vertices.size() &&
                       geometry->skinned_cpu_vertices[mesh_index].size()==vertices.size_bytes()) {
                        mapped_vertices.resize(vertices.size());std::memcpy(mapped_vertices.data(),geometry->skinned_cpu_vertices[mesh_index].data(),vertices.size_bytes());vertices=mapped_vertices;
                    }
                } else if(mesh_index<geometry->mesh_handles.size()) {
                    const auto& mesh=geometry->mesh_handles[mesh_index];
                    const auto& vb=mesh.vertexBuffer;const auto& ib=mesh.indexBuffer;
                    auto* vp=vb.get_mapped_data();auto* ip=ib.get_mapped_data();
                    if(!vp || !ip) continue;
                    mapped_vertices.resize(vb.get_element_count()*vb.get_element_size()/sizeof(Resource::Vertex));
                    std::memcpy(mapped_vertices.data(),vp,mapped_vertices.size()*sizeof(Resource::Vertex));vertices=mapped_vertices;
                    for(size_t i=0;i<ib.get_element_count();++i) indices.push_back(ib.get_element_size()==2?static_cast<const uint16_t*>(ip)[i]:static_cast<const uint32_t*>(ip)[i]);
                }
                Vec emission{};uint32_t program=0;bool two_sided=false;
                if(optics->principled) {
                    const auto& p=optics->principled;two_sided=p->emission_two_sided;
                    auto found=emission_cache.find(p.get());
                    if(found==emission_cache.end()) {
                        emission=p->expressions && p->expressions->contains("emission")?images.evaluate(p->expressions->at("emission"),.5f,.5f,true):p->surface.emissive;
                        if(luminance(emission)>0) if(auto appearance=appearances.obtain(p)) {program=appearance->program.store_descriptor();result->appearances.push_back(std::move(appearance));}
                        found=emission_cache.emplace(p.get(),std::pair{emission,program}).first;
                    }
                    emission=found->second.first;program=found->second.second;
                } else if(mesh_index<geometry->mesh_handles.size()) {
                    const auto& m=geometry->mesh_handles[mesh_index];
                    if(m.material) {emission=m.material->source.emissive;two_sided=m.material->source.double_sided;}
                }
                auto matrix=multiply(world,instance.transform);
                for(size_t i=0;i+2<indices.size();i+=3) {
                    if(indices[i]>=vertices.size() || indices[i+1]>=vertices.size() || indices[i+2]>=vertices.size()) continue;
                    const auto& a=vertices[indices[i]];const auto& b=vertices[indices[i+1]];const auto& c=vertices[indices[i+2]];
                    Triangle t;t.p=transform(matrix,a.position);t.e1=sub(transform(matrix,b.position),t.p);t.e2=sub(transform(matrix,c.position),t.p);
                    const float area=.5f*std::sqrt(dot(cross(t.e1,t.e2),cross(t.e1,t.e2)));
                    if(!std::isfinite(area) || area<=1e-20f) continue;
                    if(luminance(emission)>0) {
                        // Imported flipped normals may be independent of winding.
                        auto n=transform(ktm::transpose(ktm::inverse(matrix)),a.normal,0);
                        auto uv=std::array<float,6>{a.tex_coords[0],a.tex_coords[1],b.tex_coords[0],b.tex_coords[1],c.tex_coords[0],c.tex_coords[1]};
                        if(dot(cross(t.e1,t.e2),n)<0) {std::swap(t.e1,t.e2);std::swap(uv[2],uv[4]);std::swap(uv[3],uv[5]);}
                        t.emitter=static_cast<uint32_t>(emitters.size()+1);
                        emitters.push_back({t,uv,emission,area,double(area)*luminance(emission)*(two_sided?2:1),program,two_sided});
                    }
                    triangles.push_back(t);
                }
            }
        }
    }
    std::vector<Node> nodes;nodes.reserve(triangles.size()/2);build_bvh(triangles,nodes,0,static_cast<uint32_t>(triangles.size()));
    std::vector<uint32_t> words(16);words[0]=static_cast<uint32_t>(nodes.size());words[1]=static_cast<uint32_t>(triangles.size());words[2]=static_cast<uint32_t>(emitters.size());
    words[4]=static_cast<uint32_t>(words.size());
    for(const auto& node:nodes) {append(words,node.low);words.push_back(node.first);append(words,node.high);words.push_back(node.count);words.push_back(node.escape);words.insert(words.end(),3,0);}
    words[5]=static_cast<uint32_t>(words.size());
    for(const auto& t:triangles) {append(words,t.p);words.push_back(t.emitter);append(words,t.e1);words.push_back(0);append(words,t.e2);words.push_back(0);}
    words[6]=static_cast<uint32_t>(words.size());double total=0,cdf=0;for(const auto& e:emitters) total+=e.weight;
    for(const auto& e:emitters) {
        cdf+=e.weight;append(words,e.triangle.p);words.push_back(e.two_sided?1:0);append(words,e.triangle.e1);words.push_back(bits(float(cdf/total)));
        append(words,e.triangle.e2);words.push_back(bits(float(e.weight/total/e.area)));
        for(float uv:e.uv) words.push_back(bits(uv));words.insert(words.end(),2,0);
        append(words,e.emission);words.push_back(e.program);words.insert(words.end(),8,0);
    }
    words[7]=static_cast<uint32_t>(words.size());
    if(env.authored_lighting) for(const auto& light:env.authored_lighting->at("directionals")) {
        // Authored direction is propagation, whereas the BRDF needs to-light.
        append(words,mul(normalized(light.at("direction").get<Vec>()),-1.f));words.push_back(0);append(words,images.evaluate(light.at("color")));words.push_back(0);++words[3];
    }
    auto map=environment_map(env.authored_lighting?env.authored_lighting->at("environments"):Json::array(),images);
    result->environment=Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(map.width,map.height,Horizon::Format::RGBA32_FLOAT,Horizon::ImageUsage_Sampled|Horizon::ImageUsage_TransferDst,"native.environment"));
    Horizon::HardwareBufferDesc staging_desc;staging_desc.element_count=map.pixels.size();staging_desc.element_size=sizeof(float);staging_desc.usage=Horizon::BufferUsage_TransferSrc;staging_desc.cpu_access=Horizon::CpuAccessMode::Write;
    Horizon::HardwareBuffer staging(staging_desc,std::as_bytes(std::span(map.pixels)));Horizon::HardwareExecutor executor;
    auto receipt=executor.stream()<<result->environment.copy_from(staging)<<Horizon::commit();executor.wait_completed(receipt);
    words[8]=result->environment.store_descriptor();words[9]=static_cast<uint32_t>(words.size());words[10]=map.width;words[11]=map.height;words[12]=bits(float(map.integral));words[13]=env.authored_lighting?1:0;
    for(double v:map.cdf) words.push_back(bits(map.integral>0?float(v/map.integral):0.f));
    // Shadow traversal randomly reads this large immutable buffer. Keep it in
    // device memory; host-visible storage makes every BVH walk cross PCIe.
    Horizon::HardwareBufferDesc desc;desc.element_count=words.size();desc.element_size=4;desc.usage=Horizon::BufferUsage_Storage|Horizon::BufferUsage_TransferDst;desc.cpu_access=Horizon::CpuAccessMode::None;desc.debug_name="native.direct_lighting";
    result->data=Horizon::HardwareBuffer(desc);
    staging_desc.element_count=words.size();staging_desc.element_size=sizeof(uint32_t);
    Horizon::HardwareBuffer data_staging(staging_desc,std::as_bytes(std::span(words)));
    receipt=executor.stream()<<Horizon::CopyBufferCommand{data_staging,result->data,0,0,words.size()*sizeof(uint32_t)}<<Horizon::commit();
    executor.wait_completed(receipt);
    result->memory=Memory::GpuMemToken(Memory::ResKind::Texture,map.pixels.size()*sizeof(float));
    result->geometry_memory=Memory::GpuMemToken(Memory::ResKind::Mesh,words.size()*sizeof(uint32_t));
    result->triangles=words[1];result->emitters=words[2];result->directionals=words[3];
    CFW_LOG_INFO("Native direct lighting: {} triangles, {} emissive triangles, {} directionals, environment integral {}",result->triangles,result->emitters,result->directionals,map.integral);
    signature_=signature;value_=result;return result;
}
} // namespace Corona::Systems::OpticsDetail
