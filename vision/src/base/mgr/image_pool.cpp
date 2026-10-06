//
// Created by Zero on 26/09/2022.
//

#include "image_pool.h"
#include "pipeline.h"
#include "global.h"
#include "rhi/device.h"
#include "switch_profile.h"
#include <cstring>

namespace vision {
using namespace ocarina;

RegistrableTexture3D ImagePool::load_texture(const ShaderNodeDesc &desc,
                                             BindlessArray &bindless_array,
                                             Device &device) noexcept {
    Image image_io;
    const auto hash = desc.hash();
    const auto cached = source_cache_ ? source_cache_->find(hash) : SourceCache::iterator{};
    if (source_cache_ && cached != source_cache_->end()) {
        switch_profile::Scope profile{"ImagePool::reuse_source", "scene_reuse"};
        const auto &source = *cached->second;
        image_io = Image::create_empty(source.pixel_storage(), source.resolution());
        std::memcpy(image_io.pixel_ptr(), source.pixel_ptr(), source.size_in_bytes());
        image_io.path() = source.path();
        std::memcpy(&image_io.average<1>(), &source.average<1>(), source.channel_num() * sizeof(float));
    } else if (desc.sub_type == "constant") {
        image_io = Image::pure_color(desc["value"].as_float4(), ocarina::LINEAR, make_uint2(1));
    } else {
        string color_space = desc["color_space"].as_string();
        if (color_space.empty()) {
            string fn = desc.file_name();
            color_space = (fn.ends_with(".exr") || fn.ends_with(".hdr")) ? "linear" : "srgb";
        }
        ColorSpace cs = color_space == "linear" ? LINEAR : SRGB;
        fs::path fpath = desc.file_name();
        if (!fpath.is_absolute()) {
            fpath = Global::instance().scene_path() / fpath;
        }
        switch_profile::Scope profile{"ImagePool::read_source", "scene_images"};
        image_io = Image::load(fpath, cs);
    }
    if (source_cache_ && cached == source_cache_->end()) {
        auto source = Image::create_empty(image_io.pixel_storage(), image_io.resolution());
        std::memcpy(source.pixel_ptr(), image_io.pixel_ptr(), image_io.size_in_bytes());
        source.path() = image_io.path();
        std::memcpy(&source.average<1>(), &image_io.average<1>(), image_io.channel_num() * sizeof(float));
        source_cache_->emplace(hash, std::make_shared<const Image>(std::move(source)));
    }
    RegistrableTexture3D ret{bindless_array};
    ret.host_tex() = ocarina::move(image_io);
    ret.allocate_on_device(device, desc.file_name());
    ret.register_self();
    return ret;
}

RegistrableTexture3D &ImagePool::obtain_texture(const ShaderNodeDesc &desc,
                                                BindlessArray &bindless_array,
                                                Device &device) noexcept {
    uint64_t hash = desc.hash();
    if (!is_contain(hash)) {
        textures_.insert(make_pair(hash, load_texture(desc, bindless_array, device)));
    } else {
        auto iter = textures_.find(hash);
        if (iter->second.bindless_array() != &bindless_array) {
            textures_.erase(iter);
            textures_.insert(make_pair(hash, load_texture(desc, bindless_array, device)));
        } else {
            auto scene_path = Global::instance().scene_path();
            OC_INFO_FORMAT("image load: find {} from image pool", (scene_path / desc.file_name()).string().c_str());
        }
    }
    return textures_[hash];
}

void ImagePool::prepare(Stream &stream) noexcept {
    switch_profile::Scope profile{"images.prepare", "images"};
    for (auto &iter : textures_) {
        stream << iter.second.upload();
    }

    stream << synchronize() << commit();
}

ImagePool *ImagePool::s_image_pool = nullptr;

ImagePool &ImagePool::instance() {
    if (s_image_pool == nullptr) {
        s_image_pool = new ImagePool();
    }
    return *s_image_pool;
}

void ImagePool::destroy_instance() {
    if (s_image_pool) {
        delete s_image_pool;
        s_image_pool = nullptr;
    }
}

}// namespace vision
