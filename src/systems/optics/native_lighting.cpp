#include "native_lighting.h"
#include "native_lpv_data.h"
#include <horizon/core/logging.h>
#include <cstring>
#include <numbers>

namespace Corona::Systems::OpticsDetail {
namespace {
using namespace LpvData;
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
// CPU evaluation supplies environment reprojection and low-frequency voxel
// reflectance/emission. Visible surfaces keep their full GPU material expression.
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

struct EnvironmentMap { unsigned width=512,height=256; std::vector<float> pixels;double integral{};std::array<Vec,direction_count> boundary{};std::array<double,direction_count> weights{}; };
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
        out.integral+=luminance(color)*texel_solid_angle(y,out.width,out.height);
        auto travel=mul(world,-1);unsigned best=0;float alignment=-2;
        for(unsigned d=0;d<direction_count;++d)if(float a=dot(travel,directions[d].vector);a>alignment){alignment=a;best=d;}
        auto angle=texel_solid_angle(y,out.width,out.height);
        out.boundary[best]=add(out.boundary[best],mul(color,float(angle)));out.weights[best]+=angle;
    }
    for(unsigned d=0;d<direction_count;++d)if(out.weights[d]>0)out.boundary[d]=mul(out.boundary[d],float(1/out.weights[d]));
    return out;
}

struct Emitter {Vec position,normal,emission;float area;bool two_sided;};
}

std::shared_ptr<NativeLighting> NativeLightingCache::obtain(const SceneDevice& scene,
    const EnvironmentDevice& env,std::size_t signature,Vec eye,Vec forward) {
    signature^=env.authored_lighting_revision+0x9e3779b97f4a7c15ULL+(signature<<6)+(signature>>2);
    for (auto actor:scene.actor_handles) signature^=actor+0x9e3779b97f4a7c15ULL+(signature<<6)+(signature>>2);
    if(value_ && signature_==signature && value_->layouts==cascade_layouts(value_->bounds_low,value_->bounds_high,eye,forward)) return value_;
    auto result=std::make_shared<NativeLighting>();result->authored_environment=bool(env.authored_lighting);
    auto& hub=SharedDataHub::instance();auto& manager=Resource::ResourceManager::get_instance();
    Images images;std::array<Grid,cascade_count> grids;std::vector<Emitter> emitters;
    Vec low{},high{};low.fill(std::numeric_limits<float>::max());high.fill(-std::numeric_limits<float>::max());
    uint32_t triangle_count=0;
    // The first pass only computes bounds; the second voxelizes source meshes.
    // No persistent world-space triangle copy or acceleration structure exists.
    for(unsigned pass=0;pass<2;++pass) {

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
                Vec emission{},albedo{.8f,.8f,.8f};bool two_sided=false;float opacity=1.f;
                if(pass==1) {
                    Resource::MaterialParameters material;
                    if(optics->principled) material=optics->principled->surface;
                    else if(mesh_index<geometry->mesh_handles.size() && geometry->mesh_handles[mesh_index].material)
                        material=geometry->mesh_handles[mesh_index].material->source;
                    if(!optics->principled && optics->material_overrides && mesh_index<geometry->mesh_handles.size()) {
                        const auto& mesh=geometry->mesh_handles[mesh_index];
                        const std::string name=mesh.material?mesh.material->source.name:std::string{};
                        for(bool specific:{false,true})for(const auto& row:*optics->material_overrides)
                            if((row.index.has_value() || row.material!="*")==specific && row.matches(mesh.materialIndex,name))row.apply(material);
                    }
                    for(unsigned k=0;k<3;++k)albedo[k]=material.base_color[k]*(1-material.metallic);
                    emission=material.emissive;
                    two_sided=optics->principled?optics->principled->emission_two_sided:material.double_sided;
                    if(optics->principled && optics->principled->expressions) {
                        const auto& e=*optics->principled->expressions;
                        if(e.contains("base_color")) albedo=images.evaluate(e.at("base_color"),.5f,.5f,true);
                        if(e.contains("metallic")) albedo=mul(albedo,1-std::clamp(images.evaluate(e.at("metallic"))[0],0.f,1.f));
                        if(e.contains("emission")) emission=images.evaluate(e.at("emission"),.5f,.5f,true);
                    }
                    Vec transmission_color=albedo;
                    for(auto& v:transmission_color)v=std::isfinite(v)?std::clamp(v,0.f,1.f):0.f;
                    for(auto& v:albedo)v=std::isfinite(v)?std::clamp(v,0.f,.98f):0.f;
                    if(optics->principled) {
                        float transmission=std::clamp(optics->principled->transmission,0.f,1.f);
                        opacity=1-transmission*luminance(transmission_color);
                        // Preserve the diffuse/transmitted energy split in the
                        // visibility field. This is not a caustic or refraction solver.
                        albedo=mul(albedo,(1-transmission)/std::max(opacity,1e-6f));
                    }
                }
                auto matrix=multiply(world,instance.transform);
                for(size_t i=0;i+2<indices.size();i+=3) {
                    if(indices[i]>=vertices.size() || indices[i+1]>=vertices.size() || indices[i+2]>=vertices.size()) continue;
                    const auto& a=vertices[indices[i]];const auto& b=vertices[indices[i+1]];const auto& c=vertices[indices[i+2]];
                    auto pa=transform(matrix,a.position),pb=transform(matrix,b.position),pc=transform(matrix,c.position);
                    auto cross_n=cross(sub(pb,pa),sub(pc,pa));float area=.5f*std::sqrt(dot(cross_n,cross_n));
                    if(!std::isfinite(area) || area<=1e-20f)continue;
                    if(pass==0) {for(auto p:{pa,pb,pc})for(unsigned k=0;k<3;++k){low[k]=std::min(low[k],p[k]);high[k]=std::max(high[k],p[k]);}continue;}
                    // A clear dielectric is not an opaque LPV wall. This only
                    // permits low-frequency light through; no caustics or
                    // colored transparent shadows are implied.
                    if(opacity>1e-4f)
                        for(auto& grid:grids)grid.voxelize(pa,pb,pc,albedo,area,opacity);
                    ++triangle_count;
                    if(luminance(emission)>0) {
                        auto n=normalized(cross_n);auto authored_n=transform(ktm::transpose(ktm::inverse(matrix)),a.normal,0);
                        if(dot(n,authored_n)<0)n=mul(n,-1);
                        // Stratified triangle samples keep large emitters from collapsing to a point.
                        unsigned steps=std::clamp(unsigned(std::ceil(std::sqrt(area)/grids.back().size)),1u,32u);
                        unsigned count=steps*steps;
                        for(unsigned j=0;j<count;++j) {
                            float root=std::sqrt((j+.5f)/count),v=std::fmod(j*.61803398875f+.5f,1.f);
                            auto pos=add(pa,add(mul(sub(pb,pa),root*(1-v)),mul(sub(pc,pa),root*v)));
                            emitters.push_back({pos,n,emission,area/count,two_sided});
                        }
                    }
                }
            }
        }
    }
    if(pass==0) {
        if(low[0]>high[0]){low={-1,-1,-1};high={1,1,1};}
        result->bounds_low=low;result->bounds_high=high;
        result->layouts=cascade_layouts(low,high,eye,forward);
        for(unsigned level=0;level<cascade_count;++level) {
            grids[level].origin=result->layouts[level].origin;
            grids[level].size=result->layouts[level].size;
        }
    }
    }
    for(auto& grid:grids)grid.finish();
    constexpr float pi=std::numbers::pi_v<float>;
    for(auto& grid:grids)for(const auto& e:emitters) {
        auto flux=mul(e.emission,pi*e.area/(grid.size*grid.size));grid.inject(e.position,e.normal,flux);
        if(e.two_sided)grid.inject(e.position,mul(e.normal,-1),flux);
    }
    auto map=environment_map(env.authored_lighting?env.authored_lighting->at("environments"):Json::array(),images);
    // Incoming environment irradiance enters only boundary cells, then geometry
    // blocks its transport. Additional authored directionals use the same route.
    auto boundary=map.boundary;
    unsigned directionals=0;
    if(env.authored_lighting)for(const auto& light:env.authored_lighting->at("directionals")) {
        if(directionals++==0)continue; // only the primary sun owns the RSM/CSM
        auto direction=normalized(light.at("direction").get<Vec>()),color=images.evaluate(light.at("color"));
        unsigned best=0;float alignment=-2;
        for(unsigned d=0;d<direction_count;++d)if(float a=dot(direction,directions[d].vector);a>alignment){alignment=a;best=d;}
        boundary[best]=add(boundary[best],mul(color,1/directions[best].weight));
    }
    std::vector<uint32_t> words(header_words+cascade_count*cell_count*cell_words);
    words[0]=resolution;words[1]=cell_count;for(unsigned k=0;k<3;++k)words[4+k]=bits(grids[0].origin[k]);words[7]=bits(grids[0].size);
    words[12]=cascade_count;
    words[13]=env.authored_lighting?1:0;
    for(unsigned d=0;d<direction_count;++d)for(unsigned c=0;c<3;++c)words[32+d*3+c]=bits(boundary[d][c]);
    for(unsigned level=0;level<cascade_count;++level) {
        const auto& grid=grids[level];
        for(unsigned k=0;k<3;++k)words[layout_offset+level*4+k]=bits(grid.origin[k]);
        words[layout_offset+level*4+3]=bits(grid.size);
        std::memcpy(words.data()+header_words+level*cell_count*cell_words,grid.cells.data(),grid.cells.size()*sizeof(Cell));
    }
    result->environment=Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(map.width,map.height,Horizon::Format::RGBA32_FLOAT,Horizon::ImageUsage_Sampled|Horizon::ImageUsage_TransferDst,"native.environment"));
    Horizon::HardwareBufferDesc staging_desc;staging_desc.element_count=map.pixels.size();staging_desc.element_size=sizeof(float);staging_desc.usage=Horizon::BufferUsage_TransferSrc;staging_desc.cpu_access=Horizon::CpuAccessMode::Write;
    Horizon::HardwareBuffer staging(staging_desc,std::as_bytes(std::span(map.pixels)));Horizon::HardwareExecutor executor;
    auto receipt=executor.stream()<<result->environment.copy_from(staging)<<Horizon::commit();executor.wait_completed(receipt);
    words[8]=result->environment.store_descriptor();
    Horizon::HardwareBufferDesc desc;desc.element_count=words.size();desc.element_size=4;desc.usage=Horizon::BufferUsage_Storage|Horizon::BufferUsage_TransferDst;desc.cpu_access=Horizon::CpuAccessMode::None;desc.debug_name="native.lpv.geometry_sources";
    result->data=Horizon::HardwareBuffer(desc);
    staging_desc.element_count=words.size();staging_desc.element_size=sizeof(uint32_t);
    Horizon::HardwareBuffer data_staging(staging_desc,std::as_bytes(std::span(words)));
    receipt=executor.stream()<<Horizon::CopyBufferCommand{data_staging,result->data,0,0,words.size()*sizeof(uint32_t)}<<Horizon::commit();
    executor.wait_completed(receipt);
    result->memory=Memory::GpuMemToken(Memory::ResKind::Texture,map.pixels.size()*sizeof(float));
    result->geometry_memory=Memory::GpuMemToken(Memory::ResKind::Mesh,words.size()*sizeof(uint32_t));
    result->triangles=triangle_count;result->emitters=static_cast<uint32_t>(emitters.size());result->directionals=directionals;
    result->voxel_size=grids.back().size;
    for(const auto& grid:grids)result->occupied+=static_cast<uint32_t>(std::count_if(grid.cells.begin(),grid.cells.end(),[](const Cell& c){return c.occupied>0;}));
    CFW_LOG_INFO("Native cascaded LPV: {} triangles, {} levels, {} occupied / {} cells, finest cell {}, {} emitter samples; no scene ray traversal",
        result->triangles,cascade_count,result->occupied,cascade_count*cell_count,result->voxel_size,result->emitters);
    signature_=signature;value_=result;return result;
}
} // namespace Corona::Systems::OpticsDetail
