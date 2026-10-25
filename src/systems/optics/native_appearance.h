#pragma once

#include <corona/shared_data_hub.h>
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/image.h>
#include <nlohmann/json.hpp>
#include <bit>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace Corona::Systems::OpticsDetail {

// Immutable expression programs use the same original files/UVs as Vision.
// Images are linear float RGBA, so HDR and data maps never pass through sRGB.
struct NativeAppearanceImage {
    Horizon::HardwareImage image;
    Memory::GpuMemToken memory;
};
struct NativeAppearance {
    Horizon::HardwareBuffer program;
    std::vector<std::shared_ptr<NativeAppearanceImage>> images;
};
class NativeAppearanceCache {
    using Json = nlohmann::json;
    struct Entry {
        std::weak_ptr<const Materials::Principled> owner;
        std::shared_ptr<NativeAppearance> value;
    };
    std::unordered_map<std::size_t, Entry> entries_;
    std::unordered_map<std::string, std::weak_ptr<NativeAppearanceImage>> images_;

    std::shared_ptr<NativeAppearanceImage> image(const Json& node) {
        const auto path=node.at("path").get<std::string>();
        const bool srgb=node.value("encoding", "srgb")=="srgb";
        const auto key=path+(srgb?"|srgb":"|linear");
        if (auto old=images_[key].lock()) return old;
        auto& manager=Resource::ResourceManager::get_instance();
        auto id=manager.import_sync(std::filesystem::u8path(path));
        auto source=manager.acquire_read<Resource::Image>(id);
        if (!source || source->get_width()<=0 || source->get_height()<=0)
            throw std::runtime_error("Native appearance image unavailable: "+path);
        const auto count=size_t(source->get_width())*source->get_height();
        const auto channels=source->get_channels();
        std::vector<float> pixels(count*4);
        for (size_t i=0;i<count;++i) for (int c=0;c<4;++c) {
            const int channel=c<3 && channels<3 ? 0 : c==3 && channels==2 ? 1 : c;
            float value=channel>=channels ? (c==3?1.f:0.f) : source->get_float_data()
                ? source->get_float_data()[i*channels+channel] : source->get_data()[i*channels+channel]/255.f;
            if (c<3 && srgb) value=value<=.04045f ? value/12.92f : std::pow((value+.055f)/1.055f,2.4f);
            pixels[i*4+c]=value;
        }
        auto result=std::make_shared<NativeAppearanceImage>();
        result->image=Horizon::HardwareImage(Horizon::HardwareImageDesc::texture_2d(
            source->get_width(),source->get_height(),Horizon::Format::RGBA32_FLOAT,
            Horizon::ImageUsage_Sampled|Horizon::ImageUsage_TransferDst,"appearance.linear"));
        Horizon::HardwareBufferDesc staging_desc;
        staging_desc.element_count=pixels.size(); staging_desc.element_size=sizeof(float);
        staging_desc.usage=Horizon::BufferUsage_TransferSrc;
        staging_desc.cpu_access=Horizon::CpuAccessMode::Write;
        const Horizon::HardwareBuffer staging(staging_desc,std::as_bytes(std::span(pixels)));
        Horizon::HardwareExecutor executor;
        auto receipt=executor.stream()<<result->image.copy_from(staging)<<Horizon::commit();
        executor.wait_idle(receipt);
        result->memory=Memory::GpuMemToken(Memory::ResKind::Texture,pixels.size()*sizeof(float));
        images_[key]=result;
        return result;
    }
public:
    std::shared_ptr<NativeAppearance> obtain(const std::shared_ptr<const Materials::Principled>& owner) {
        if (!owner || !owner->expressions) return {};
        std::erase_if(entries_,[](const auto& item){return item.second.owner.expired();});
        std::erase_if(images_,[](const auto& item){return item.second.expired();});
        if (auto found=entries_.find(owner->revision);found!=entries_.end()) return found->second.value;
        auto result=std::make_shared<NativeAppearance>();
        std::vector<std::array<uint32_t,24>> nodes;
        std::function<uint32_t(const Json&)> emit=[&](const Json& value)->uint32_t {
            std::array<uint32_t,24> n{};
            auto put=[&](unsigned slot,float v){n[slot]=std::bit_cast<uint32_t>(v);};
            if (!value.is_object()) {
                for (unsigned c=0;c<3;++c) put(4+c,value.is_number()?value.get<float>():value.at(value.size()==1?0:c).get<float>());
            } else {
                const auto kind=value.at("kind").get<std::string>();
                if (kind=="image") {
                    n[0]=1; auto texture=image(value);n[7]=texture->image.store_descriptor();result->images.push_back(texture);
                    auto wrap=value.value("wrap","repeat");n[17]=wrap=="clamp"?1:wrap=="mirror"?2:wrap=="black"?3:0;
                    n[18]=value.value("filter","bilinear")=="nearest"?1:0;
                } else {
                    n[0]=kind=="multiply"?2:kind=="mix"?3:kind=="sqrt"?4:5;
                    n[1]=emit(value.at("a"));
                    if (value.contains("b")) n[2]=emit(value.at("b"));
                    if (value.contains("weight")) n[3]=emit(value.at("weight"));
                    put(19,value.value("frequency",2.f));
                }
                const auto uv=value.value("uv",Json::array({1,0,0,0,1,0,0,0,1}));
                for (unsigned c=0;c<9;++c) put(8+c,uv[c].get<float>());
            }
            if (nodes.size()>=128) throw std::runtime_error("Native appearance exceeds 128 expression nodes");
            nodes.push_back(n);return static_cast<uint32_t>(nodes.size()-1);
        };
        std::vector<uint32_t> words(16);
        const auto& p=*owner->expressions;
        const std::pair<const char*,Json> slots[]={{"base_color",Json::array({.5,.5,.5})},{"roughness",.5},
            {"metallic",0.},{"specular",0.},{"emission",Json::array({0.,0.,0.})},
            {"normal",Json::array({.5,.5,1.})},{"coat",0.},{"coat_roughness",.1}};
        for (unsigned i=0;i<std::size(slots);++i) words[i+1]=emit(p.value(slots[i].first,slots[i].second));
        words[0]=static_cast<uint32_t>(nodes.size());
        words[9]=p.contains("normal")?1:0;
        for (const auto& n:nodes) words.insert(words.end(),n.begin(),n.end());
        Horizon::HardwareBufferDesc desc;
        desc.element_count=words.size();desc.element_size=sizeof(uint32_t);
        desc.usage=Horizon::BufferUsage_Storage;desc.cpu_access=Horizon::CpuAccessMode::Write;
        desc.debug_name="appearance.expressions";
        result->program=Horizon::HardwareBuffer(desc,std::as_bytes(std::span(words)));
        entries_[owner->revision]={owner,result};
        return result;
    }
};
} // namespace Corona::Systems::OpticsDetail
