//
// Created by Zero on 26/09/2022.
//

#include "image_pool.h"
#include "pipeline.h"
#include "global.h"
#include "rhi/device.h"
#include "switch_profile.h"
#include "dds_bc7.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace vision {
using namespace ocarina;

namespace {
bool pack_lossless_unorm8(const float *source, size_t count, uint8_t *target = nullptr) noexcept {
    for (size_t i = 0; i < count; ++i) {
        const float value = source[i];
        if (!std::isfinite(value) || value < 0.f || value > 1.f) return false;
        const float code = std::round(value * 255.f);
        if (code / 255.f != value) return false;
        if (target) target[i] = static_cast<uint8_t>(code);
    }
    return true;
}
}// namespace

void ImagePool::register_source(const ShaderNodeDesc& desc, Image image) {
    if (!source_cache_) source_cache_=std::make_shared<SourceCache>();
    source_cache_->try_emplace(desc.hash(),std::make_shared<const Image>(std::move(image)));
}

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
    std::vector<uint8_t> gpu_bytes;
    std::optional<DdsBC7Mip> gpu_mip;
    const string gpu_source = desc["gpu_source"].as_string();
    if (!gpu_source.empty() && device.supports_bc7_textures()) {
        fs::path path = gpu_source;
        if (!path.is_absolute()) path = Global::instance().scene_path() / path;
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (input && input.tellg() >= std::streamoff(148)) {
            gpu_bytes.resize(size_t(input.tellg()));
            input.seekg(0);
            if (input.read(reinterpret_cast<char *>(gpu_bytes.data()), std::streamsize(gpu_bytes.size()))) {
                gpu_mip = parse_dds_bc7(gpu_bytes);
                if (gpu_mip && (gpu_mip->width != image_io.resolution().x ||
                                gpu_mip->height != image_io.resolution().y))
                    gpu_mip.reset();
            }
        }
        if (!gpu_mip) OC_INFO_FORMAT("Invalid BC7 GPU source {}; using decoded image", path.string());
    }
    // Select BYTE4 only when it preserves every decoded linear value exactly.
    // Keep the FLOAT4 source cache and the existing HALF4 fallback unchanged.
    const auto extension = image_io.path().extension().string();
    if (!gpu_mip && image_io.pixel_storage() == PixelStorage::FLOAT4 &&
        (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
         extension == ".bmp" || extension == ".tga")) {
        const auto *source = image_io.pixel_ptr<float>();
        const size_t channels = size_t(image_io.pixel_num()) * 4u;
        const bool unorm8 = pack_lossless_unorm8(source, channels);
        auto compact = Image::create_empty(unorm8 ? PixelStorage::BYTE4 : PixelStorage::HALF4,
                                           image_io.resolution());
        if (unorm8) {
            (void)pack_lossless_unorm8(source, channels, compact.pixel_ptr<uint8_t>());
        } else {
            auto *target = compact.pixel_ptr<uint16_t>();
            for (size_t i = 0; i < channels; ++i) target[i] = float_to_half(source[i]);
        }
        compact.path() = image_io.path();
        std::memcpy(&compact.average<1>(), &image_io.average<1>(), 4u * sizeof(float));
        image_io = std::move(compact);
    }
    RegistrableTexture3D ret{bindless_array};
    ret.host_tex() = ocarina::move(image_io);
    compressed_textures_.erase(hash);
    if (gpu_mip) {
        // The CPU image remains decoded and linear. Hardware sRGB conversion
        // follows the source encoding, independent of the DDS's view tag.
        const auto storage = desc["source_encoding"].as_string() == "srgb"
                                 ? PixelStorage::BC7_SRGB : PixelStorage::BC7_UNORM;
        ret.device_tex() = device.create_texture3d(ret.host_tex().resolution(), storage, desc.file_name());
        ret.device_tex().upload_immediately(gpu_bytes.data() + gpu_mip->offset);
        compressed_textures_.insert(hash);
    } else {
        ret.allocate_on_device(device, desc.file_name());
    }
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
        if (!is_compressed_texture(iter.first)) stream << iter.second.upload();
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
