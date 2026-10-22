#pragma once
#include "vision_texture_adapter.h"
#include <unordered_map>

namespace Corona::Systems::Vision {

// Reproject into the existing spherical light's direction convention in memory.
// This also combines environments into its single background registration slot.
inline AppearanceJson authored_environment_slot(const AppearanceJson& environments) {
    using namespace ocarina;
    AppearanceJson node={{"type","image"},{"param",{
        {"fn","corona-environment-"+std::to_string(std::hash<std::string>{}(environments.dump()))},
        {"color_space","linear"}}}};
    AppearanceJson result={{"channels","xyz"},{"node",node}};
    if (auto* pipeline=::vision::Global::instance().pipeline()) {
        ::vision::ShaderNodeDesc desc; desc.init(::vision::ParameterSet{vision_json(node)});
        if (pipeline->image_pool().is_contain(desc.hash())) return result;
    }
    if (std::all_of(environments.begin(),environments.end(),[](const auto& layer){ return !layer.at("color").is_object(); })) {
        std::array<float,3> color{};
        for (const auto& layer:environments) for (unsigned c=0;c<3;++c) {
            const auto& value=layer.at("color");
            color[c]+=value.is_number() ? value.template get<float>() : value[c].template get<float>();
        }
        return appearance_slot(color);
    }
    std::unordered_map<std::string,Image> images;
    unsigned width=1,height=1;
    std::function<void(const AppearanceJson&)> load=[&](const AppearanceJson& e) {
        if (!e.is_object()) return;
        if (e.value("kind","")=="image") {
            const auto path=e.at("path").get<std::string>();
            if (!images.contains(path)) {
                auto image=appearance_image(path,e.value("encoding","linear"));
                width=std::max(width,image.resolution().x);
                height=std::max(height,image.resolution().y);
                images.emplace(path,std::move(image));
            }
        }
        for (auto key:{"a","b","weight"}) if (e.contains(key)) load(e[key]);
    };
    for (const auto& environment:environments) load(environment.at("color"));
    std::function<float3(const AppearanceJson&,float,float)> evaluate;
    evaluate=[&](const AppearanceJson& e,float u,float v)->float3 {
        if (e.is_number()) return make_float3(e.get<float>());
        if (e.is_array()) return make_float3(e[0].get<float>(),e[1].get<float>(),e[2].get<float>());
        const auto kind=e.at("kind").get<std::string>();
        if (kind=="multiply") return evaluate(e.at("a"),u,v)*evaluate(e.at("b"),u,v);
        if (kind=="mix") {
            auto t=evaluate(e.at("weight"),u,v);
            return (make_float3(1.f)-t)*evaluate(e.at("a"),u,v)+t*evaluate(e.at("b"),u,v);
        }
        const auto& image=images.at(e.at("path").get<std::string>());
        const auto res=image.resolution();
        const float x=(u-std::floor(u))*res.x-.5f;
        const float y=std::clamp(v,0.f,1.f)*res.y-.5f;
        const int ix=int(std::floor(x)),iy=int(std::floor(y));
        auto pixel=[&](int px,int py) {
            px=(px%int(res.x)+int(res.x))%int(res.x);
            py=std::clamp(py,0,int(res.y)-1);
            const float* p=image.pixel_ptr<float>()+(size_t(py)*res.x+px)*4;
            return make_float3(p[0],p[1],p[2]);
        };
        const float fx=x-ix,fy=y-iy;
        return (pixel(ix,iy)*(1-fx)+pixel(ix+1,iy)*fx)*(1-fy)+
               (pixel(ix,iy+1)*(1-fx)+pixel(ix+1,iy+1)*fx)*fy;
    };
    struct Layer { const AppearanceJson* color; float4x4 inverse; bool equal_area; };
    std::vector<Layer> layers;
    for (const auto& e:environments) {
        auto m=make_float4x4(1.f);
        const auto& values=e.at("matrix");
        for (unsigned c=0;c<4;++c) for (unsigned r=0;r<4;++r) m[c][r]=values[r*4+c].get<float>();
        layers.push_back({&e.at("color"),inverse(m),e.value("projection","")=="equal_area"});
    }
    if (!images.empty()) { height=std::max(height,(width+1)/2); width=2*height; }
    auto output=Image::create_empty(PixelStorage::FLOAT4,make_uint2(width,height));
    std::array<double,3> sum{};
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const float theta=(y+.5f)/height*Pi,phi=(x+.5f)/width*2*Pi;
        // Existing Vision spherical: Z-polar -> Rx(-90)*Y-reflection -> F^-1.
        const auto world=make_float3(std::sin(theta)*std::cos(phi),std::cos(theta),-std::sin(theta)*std::sin(phi));
        auto color=make_float3(0.f);
        for (const auto& layer:layers) {
            // Host reprojection: avoid the similarly named DSL transform helper.
            float3 d=(layer.inverse*make_float4(world,0.f)).xyz();
            d/=std::sqrt(d.x*d.x+d.y*d.y+d.z*d.z);
            float u,v;
            if (layer.equal_area) {
                const float r=std::sqrt(std::max(0.f,1.f-std::abs(d.z)));
                const float angle=std::atan2(std::abs(d.y),std::abs(d.x))*2/Pi;
                v=angle*r; u=r-v;
                if (d.z<0) { std::swap(u,v);u=1-u;v=1-v; }
                u=(std::copysign(u,d.x)+1)*.5f;
                v=(std::copysign(v,d.y)+1)*.5f;
            } else {
                u=std::atan2(d.x,-d.z)/(2*Pi);
                v=std::acos(std::clamp(d.y,-1.f,1.f))/Pi;
            }
            color+=evaluate(*layer.color,u,v);
        }
        float* p=output.pixel_ptr<float>()+(size_t(y)*width+x)*4;
        for (unsigned c=0;c<3;++c) {p[c]=std::max(0.f,color[c]);sum[c]+=p[c];}
        p[3]=1.f;
    }
    for (unsigned c=0;c<3;++c) output.average<4>()[c]=float(sum[c]/(size_t(width)*height));
    output.average<4>()[3]=1.f;
    register_appearance_image(node,std::move(output));
    return result;
}
} // namespace Corona::Systems::Vision
