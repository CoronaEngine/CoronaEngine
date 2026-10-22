#pragma once

#include <corona/resource/resource_manager.h>
#include <corona/resource/types/image.h>
#include <nlohmann/json.hpp>
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/import/node_desc.h"
#include <cmath>

namespace Corona::Systems::Vision {
using AppearanceJson=nlohmann::json;

inline ::vision::DataWrap vision_json(const AppearanceJson& value) {
    return ::vision::DataWrap::parse(value.dump());
}

inline ocarina::Image appearance_image(const std::string& path, const std::string& encoding) {
    auto& manager=Resource::ResourceManager::get_instance();
    auto id=manager.import_sync(std::filesystem::u8path(path));
    auto source=manager.acquire_read<Resource::Image>(id);
    if (!source || source->get_width()<=0 || source->get_height()<=0)
        throw std::runtime_error("Unable to read appearance image: "+path);
    auto result=ocarina::Image::create_empty(ocarina::PixelStorage::FLOAT4,
        ocarina::make_uint2(source->get_width(),source->get_height()));
    auto* pixels=result.pixel_ptr<float>();
    const auto* floats=source->get_float_data();
    const auto* bytes=source->get_data();
    const unsigned channels=source->get_channels();
    std::array<double,4> sums{};
    const size_t count=size_t(source->get_width())*source->get_height();
    for (size_t i=0;i<count;++i) for (unsigned c=0;c<4;++c) {
        float value=c>=channels ? (c==3?1.f:0.f) : floats ? floats[i*channels+c] : bytes[i*channels+c]/255.f;
        if (c<3 && encoding=="srgb") value=value<=.04045f ? value/12.92f : std::pow((value+.055f)/1.055f,2.4f);
        pixels[i*4+c]=value;
        sums[c]+=value;
    }
    result.path()=std::filesystem::u8path(path);
    auto& average=result.average<4>();
    for (unsigned c=0;c<4;++c) average[c]=float(sums[c]/count);
    return result;
}

inline void register_appearance_image(const AppearanceJson& node, ocarina::Image image) {
    ::vision::ShaderNodeDesc descriptor;
    descriptor.init(::vision::ParameterSet{vision_json(node)});
    auto* pipeline=::vision::Global::instance().pipeline();
    if (!pipeline) throw std::runtime_error("Image material requires an active Vision pipeline");
    pipeline->image_pool().register_source(descriptor,std::move(image));
    // Engine-built scenes can add images after Pipeline::prepare(). The normal
    // geometry refresh uploads descriptors, but does not upload image pixels.
    auto& texture=pipeline->image_pool().obtain_texture(descriptor,
        pipeline->geometry().bindless_array(),pipeline->device());
    texture.upload_immediately();
}

inline AppearanceJson appearance_slot(const AppearanceJson& value, unsigned channels=3) {
    auto slot=[&](AppearanceJson node) { return AppearanceJson{{"channels",channels==1 ? "x":"xyz"},{"node",std::move(node)}}; };
    auto constant=[&](AppearanceJson number) {
        if (number.is_number()) number=AppearanceJson::array({number});
        if (channels==3 && number.size()==1) number=AppearanceJson::array({number[0],number[0],number[0]});
        return slot({{"type","number"},{"param",{{"value",number}}}});
    };
    if (!value.is_object()) return constant(value);
    const auto kind=value.at("kind").get<std::string>();
    if (kind=="multiply") return slot({{"type","math"},{"param",{
        {"lhs",appearance_slot(value.at("a"),channels)},{"rhs",appearance_slot(value.at("b"),channels)}}}});
    if (kind=="mix") return slot({{"type","mix"},{"param",{
        {"t",appearance_slot(value.at("weight"),1)},{"A",appearance_slot(value.at("a"),channels)},
        {"B",appearance_slot(value.at("b"),channels)}}}});
    if (kind=="sqrt") return slot({{"type","gamma"},{"param",{
        {"color",appearance_slot(value.at("a"),3)},{"gamma",.5}}}});
    AppearanceJson node={{"type","image"},{"param",{{"color_space","linear"}}}};
    if (kind=="image") {
        const auto path=value.at("path").get<std::string>();
        node["param"]["fn"]=path;
        node["param"]["uv_transform"]=value.value("uv",AppearanceJson::array({1,0,0,0,1,0,0,0,1}));
        node["param"]["wrap"]=value.value("wrap","repeat");
        node["param"]["filter"]=value.value("filter","bilinear");
        // Include decoding in identity even though the supplied pixels are linear.
        node["param"]["source_encoding"]=value.value("encoding","srgb");
        ::vision::ShaderNodeDesc desc; desc.init(::vision::ParameterSet{vision_json(node)});
        auto* pipeline=::vision::Global::instance().pipeline();
        if (!pipeline) throw std::runtime_error("Image material requires an active Vision pipeline");
        if (!pipeline->image_pool().is_contain(desc.hash()))
            register_appearance_image(node,appearance_image(path,value.value("encoding","srgb")));
    } else if (kind=="checkerboard") {
        // Generated in memory, never written as a copied/baked source asset.
        node["param"]["fn"]="corona-checker-"+std::to_string(std::hash<std::string>{}(value.dump()));
        auto uv=value.value("uv",AppearanceJson::array({1,0,0,0,1,0,0,0,1}));
        for (unsigned c=0;c<6;++c) uv[c]=uv[c].get<float>()*value.value("frequency",2.f)*.5f;
        node["param"]["uv_transform"]=uv;
        auto image=ocarina::Image::create_empty(ocarina::PixelStorage::FLOAT4,ocarina::make_uint2(128));
        auto a=value.at("a"),b=value.at("b");
        if (a.is_number()) a=AppearanceJson::array({a,a,a});
        if (b.is_number()) b=AppearanceJson::array({b,b,b});
        if (!a.is_array() || !b.is_array()) throw std::runtime_error("Nested checker textures are unsupported");
        auto* pixels=image.pixel_ptr<float>();
        for (unsigned y=0;y<128;++y) for (unsigned x=0;x<128;++x) {
            const auto& color=((x/64+y/64)&1) ? b:a;
            for (unsigned c=0;c<4;++c) pixels[(y*128+x)*4+c]=c==3 ? 1.f:color[c].get<float>();
        }
        for (unsigned c=0;c<4;++c) image.average<4>()[c]=c==3 ? 1.f:(a[c].get<float>()+b[c].get<float>())*.5f;
        register_appearance_image(node,std::move(image));
    } else throw std::runtime_error("Unsupported appearance expression: "+kind);
    return slot(std::move(node));
}
} // namespace Corona::Systems::Vision
