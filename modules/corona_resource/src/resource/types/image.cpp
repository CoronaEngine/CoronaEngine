#include "corona/resource/types/image.h"

#include <iomanip>

#include "corona/resource/resource_manager.h"

#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define TINYEXR_IMPLEMENTATION
#include <astcenc.h>

#include <iostream>

#include "horizon/core/logging.h"
#include "tinyexr.h"

namespace Corona::Resource {

Image::Image(const std::filesystem::path& path) : IResource(path) {}

// 纹理压缩 (BC1/BC3/ASTC)
void Image::compress(CompressedData::Format format) {
    if (pixel_data_.empty()) {
        throw std::runtime_error("No pixel data");
    }

    // 准备 RGBA 数据（所有压缩格式都需要 RGBA 输入）
    std::vector<unsigned char> rgba_data;
    if (channels_ == 4) {
        rgba_data = pixel_data_;
    } else if (channels_ == 3) {
        rgba_data.resize(static_cast<size_t>(width_) * height_ * 4);
        for (int i = 0; i < width_ * height_; ++i) {
            rgba_data[i * 4 + 0] = pixel_data_[i * 3 + 0];
            rgba_data[i * 4 + 1] = pixel_data_[i * 3 + 1];
            rgba_data[i * 4 + 2] = pixel_data_[i * 3 + 2];
            rgba_data[i * 4 + 3] = 255;
        }
    } else if (channels_ == 1) {
        // 灰度图转 RGBA
        rgba_data.resize(static_cast<size_t>(width_) * height_ * 4);
        for (int i = 0; i < width_ * height_; ++i) {
            rgba_data[i * 4 + 0] = pixel_data_[i];
            rgba_data[i * 4 + 1] = pixel_data_[i];
            rgba_data[i * 4 + 2] = pixel_data_[i];
            rgba_data[i * 4 + 3] = 255;
        }
    } else {
        throw std::runtime_error("Unsupported channel count for compression");
    }

    compressed_data_.emplace();
    compressed_data_->format = format;
    compressed_data_->mip_levels = 1;

    size_t original_size = rgba_data.size();
    const char* format_name = nullptr;

    if (format == CompressedData::Format::ASTC_4x4) {
        // ===== ASTC 4x4 压缩 =====
        format_name = "ASTC_4x4";

        constexpr unsigned int block_x = 4;
        constexpr unsigned int block_y = 4;
        constexpr unsigned int block_z = 1;
        const astcenc_profile profile = ASTCENC_PRF_LDR;
        const float quality = ASTCENC_PRE_MEDIUM;
        const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

        // 初始化 ASTC 配置
        astcenc_config config;
        astcenc_error status = astcenc_config_init(profile, block_x, block_y, block_z, quality, 0, &config);
        if (status != ASTCENC_SUCCESS) {
            throw std::runtime_error(std::string("ASTC config init failed: ") + astcenc_get_error_string(status));
        }

        // 创建压缩上下文
        astcenc_context* context = nullptr;
        status = astcenc_context_alloc(&config, 1, &context);
        if (status != ASTCENC_SUCCESS) {
            throw std::runtime_error(std::string("ASTC context alloc failed: ") + astcenc_get_error_string(status));
        }

        // 设置图像数据
        astcenc_image image;
        image.dim_x = static_cast<unsigned int>(width_);
        image.dim_y = static_cast<unsigned int>(height_);
        image.dim_z = 1;
        image.data_type = ASTCENC_TYPE_U8;
        uint8_t* slices = rgba_data.data();
        image.data = reinterpret_cast<void**>(&slices);

        // 计算输出大小（每块 16 字节）
        unsigned int block_count_x = (static_cast<unsigned int>(width_) + block_x - 1) / block_x;
        unsigned int block_count_y = (static_cast<unsigned int>(height_) + block_y - 1) / block_y;
        size_t comp_len = static_cast<size_t>(block_count_x) * block_count_y * 16;

        compressed_data_->data.resize(comp_len);

        // 执行压缩
        status = astcenc_compress_image(context, &image, &swizzle,
                                        compressed_data_->data.data(), comp_len, 0);
        astcenc_context_free(context);

        if (status != ASTCENC_SUCCESS) {
            compressed_data_.reset();
            throw std::runtime_error(std::string("ASTC compress failed: ") + astcenc_get_error_string(status));
        }

    } else {
        // ===== BC1/BC3 (DXT) 压缩 =====
        format_name = (format == CompressedData::Format::BC1) ? "BC1" : "BC3";

        int block_width = (width_ + 3) / 4;
        int block_height = (height_ + 3) / 4;
        size_t block_count = static_cast<size_t>(block_width) * static_cast<size_t>(block_height);
        size_t bytes_per_block = (format == CompressedData::Format::BC1) ? 8 : 16;

        compressed_data_->data.resize(block_count * bytes_per_block);

        unsigned char block[64];
        auto* dst = compressed_data_->data.data();

        for (int by = 0; by < block_height; ++by) {
            for (int bx = 0; bx < block_width; ++bx) {
                std::memset(block, 0, sizeof(block));
                for (int py = 0; py < 4; ++py) {
                    int y = by * 4 + py;
                    if (y >= height_) {
                        continue;
                    }
                    int x = bx * 4;
                    int copy_pixels = std::min(4, width_ - x);
                    if (copy_pixels <= 0) {
                        continue;
                    }
                    int src_idx = (y * width_ + x) * 4;
                    int dst_idx = py * 16;
                    std::memcpy(&block[dst_idx], &rgba_data[src_idx], static_cast<size_t>(copy_pixels) * 4);
                }

                int alpha = (format == CompressedData::Format::BC3) ? 1 : 0;
                stb_compress_dxt_block(dst, block, alpha, STB_DXT_HIGHQUAL);
                dst += bytes_per_block;
            }
        }
    }

    // 输出压缩统计信息
    size_t compressed_size = compressed_data_->data.size();
    double ratio = static_cast<double>(original_size) / static_cast<double>(compressed_size);

    CFW_LOG_DEBUG("[Image::compress] {}x{} | Format: {} | Before: {} bytes | After: {} bytes | Ratio: {:.2f}:1",
                  width_, height_, format_name, original_size, compressed_size, ratio);
}

// ASTC Compress

bool Image::is_compressed() const {
    return compressed_data_.has_value();
}

const CompressedData& Image::get_compressed_data() const {
    if (!compressed_data_.has_value()) {
        throw std::runtime_error("Image is not compressed");
    }
    return compressed_data_.value();
}

void Image::set_data(unsigned char* data, int width, int height, int channels) {
    if (width <= 0 || height <= 0 || channels <= 0) {
        throw std::runtime_error("Invalid image dimensions");
    }
    width_ = width;
    height_ = height;
    channels_ = channels;

    size_t data_size = static_cast<size_t>(width) * height * channels;
    pixel_data_.assign(data, data + data_size);
}

void Image::set_float_data(float* data, int width, int height, int channels) {
    if (width <= 0 || height <= 0 || channels <= 0) {
        throw std::runtime_error("Invalid float image dimensions");
    }
    width_ = width;
    height_ = height;
    channels_ = channels;

    size_t data_size = static_cast<size_t>(width) * height * channels;
    float_pixel_data_.assign(data, data + data_size);
}

unsigned char* Image::get_data() const {
    return pixel_data_.empty() ? nullptr : const_cast<unsigned char*>(pixel_data_.data());
}

float* Image::get_float_data() const {
    return float_pixel_data_.empty() ? nullptr : const_cast<float*>(float_pixel_data_.data());
}

int Image::get_width() const {
    return width_;
}

int Image::get_height() const {
    return height_;
}

int Image::get_channels() const {
    return channels_;
}

ImageParser::ImageParser() {
    register_extension(".png", [this](const auto& path, ResourceCache& cache) { return parse_stb_image(path); });
    register_extension(".jpg", [this](const auto& path, ResourceCache& cache) { return parse_stb_image(path); });
    register_extension(".jpeg", [this](const auto& path, ResourceCache& cache) { return parse_stb_image(path); });
    register_extension(".bmp", [this](const auto& path, ResourceCache& cache) { return parse_stb_image(path); });
    register_extension(".tga", [this](const auto& path, ResourceCache& cache) { return parse_stb_image(path); });

    register_extension(".exr", [this](const auto& path, ResourceCache& cache) { return parse_exr(path); });
    // register_extension(".hdr", [this](const auto& path, ResourceCache& cache) { return parse_exr(path); });

    register_exporter(".png", [this](const IResource& resource, const std::filesystem::path& path) { return export_png(resource, path); });
    register_exporter(".jpg", [this](const IResource& resource, const std::filesystem::path& path) { return export_jpg(resource, path); });
    register_exporter(".jpeg", [this](const IResource& resource, const std::filesystem::path& path) { return export_jpg(resource, path); });
    register_exporter(".bmp", [this](const IResource& resource, const std::filesystem::path& path) { return export_bmp(resource, path); });
    register_exporter(".tga", [this](const IResource& resource, const std::filesystem::path& path) { return export_tga(resource, path); });
    register_exporter(".exr", [this](const IResource& resource, const std::filesystem::path& path) { return export_exr(resource, path); });
}

std::shared_ptr<IResource> ImageParser::parse_stb_image(const std::filesystem::path& path, const ImageImportOptions& options) {
    auto ptr = std::make_shared<Image>(path);

    int width, height, original_channels;
    // 强制加载为 4 通道 (RGBA)，支持任意通道数的源图像（包括灰度图）
    std::unique_ptr<unsigned char[], decltype(&stbi_image_free)> data(
        stbi_load(path.string().c_str(), &width, &height, &original_channels, 4),
        stbi_image_free);

    if (!data) {
        throw std::runtime_error("Failed to load image: " + path.string());
    }

    ptr->set_data(data.get(), width, height, 4);
    if (options.compress) {
        ptr->compress(options.format);
    }

    return ptr;
}

std::shared_ptr<IResource> ImageParser::parse_exr(const std::filesystem::path& path) {
    auto img = std::make_shared<Image>(path);

    float* raw = nullptr;
    int width = 0, height = 0;
    const char* err = nullptr;

    int ret = LoadEXR(&raw, &width, &height, path.string().c_str(), &err);
    if (ret != TINYEXR_SUCCESS) {
        std::string msg = err ? err : "Unknown error";
        if (err) FreeEXRErrorMessage(err);
        throw std::runtime_error("Failed to load EXR: " + msg);
    }

    std::unique_ptr<float, void (*)(void*)> data(raw, [](void* p) { if (p) free(p); });

    if (width <= 0 || height <= 0) {
        throw std::runtime_error("Invalid EXR dimensions");
    }

    const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixel_count > (SIZE_MAX / 4)) {
        throw std::runtime_error("EXR image too large");
    }

    img->set_float_data(data.get(), width, height, 4);
    return img;
}

bool ImageParser::export_png(const IResource& resource, const std::filesystem::path& path) {
    CFW_LOG_DEBUG("Path : {}", path.string());
    auto handle = ResourceManager::get_instance().acquire_read<Image>(resource.get_uid());
    if (!handle) {
        CFW_LOG_ERROR("[ImageParser::export_png] Failed to acquire read access to resource: {}", resource.get_uid());
        return false;
    }

    auto width = handle->get_width();
    auto height = handle->get_height();
    auto channels = handle->get_channels();
    auto data = handle->get_data();
    int result = stbi_write_png(path.string().c_str(), width, height, channels, data, width * channels);

    if (result == 0) {
        CFW_LOG_ERROR("[ImageParser::export_png] Failed to write PNG image to: {}", path.string());
        return false;
    }

    return true;
}
bool ImageParser::export_jpg(const IResource& resource, const std::filesystem::path& path) {
    CFW_LOG_DEBUG("Path : {}", path.string());
    auto handle = ResourceManager::get_instance().acquire_read<Image>(resource.get_uid());
    if (!handle) {
        CFW_LOG_ERROR("[ImageParser::export_jpg] Failed to acquire read access to resource: {}", resource.get_uid());
        return false;
    }

    auto width = handle->get_width();
    auto height = handle->get_height();
    auto channels = handle->get_channels();
    auto data = handle->get_data();
    int result = stbi_write_jpg(path.string().c_str(), width, height, channels, data, 90);

    if (result == 0) {
        CFW_LOG_ERROR("[ImageParser::export_jpg] Failed to write JPG image to: {}", path.string());
        return false;
    }

    return true;
}
bool ImageParser::export_bmp(const IResource& resource, const std::filesystem::path& path) {
    CFW_LOG_DEBUG("Path : {}", path.string());
    auto handle = ResourceManager::get_instance().acquire_read<Image>(resource.get_uid());
    if (!handle) {
        CFW_LOG_ERROR("[ImageParser::export_bmp] Failed to acquire read access to resource: {}", resource.get_uid());
        return false;
    }

    auto width = handle->get_width();
    auto height = handle->get_height();
    auto channels = handle->get_channels();
    auto data = handle->get_data();
    int result = stbi_write_bmp(path.string().c_str(), width, height, channels, data);

    if (result == 0) {
        CFW_LOG_ERROR("[ImageParser::export_bmp] Failed to write BMP image to: {}", path.string());
        return false;
    }

    return true;
}
bool ImageParser::export_tga(const IResource& resource, const std::filesystem::path& path) {
    CFW_LOG_DEBUG("Path : {}", path.string());
    auto handle = ResourceManager::get_instance().acquire_read<Image>(resource.get_uid());
    if (!handle) {
        CFW_LOG_ERROR("[ImageParser::export_tga] Failed to acquire read access to resource: {}", resource.get_uid());
        return false;
    }

    auto width = handle->get_width();
    auto height = handle->get_height();
    auto channels = handle->get_channels();
    auto data = handle->get_data();
    int result = stbi_write_tga(path.string().c_str(), width, height, channels, data);

    if (result == 0) {
        CFW_LOG_ERROR("[ImageParser::export_tga] Failed to write TGA image to: {}", path.string());
        return false;
    }

    return true;
}

bool ImageParser::export_exr(const IResource& resource, const std::filesystem::path& path) {
    CFW_LOG_DEBUG("Path : {}", path.string());
    auto handle = ResourceManager::get_instance().acquire_read<Image>(resource.get_uid());
    if (!handle) {
        CFW_LOG_ERROR("[ImageParser::export_exr] Failed to acquire read access to resource: {}", resource.get_uid());
        return false;
    }

    auto width = handle->get_width();
    auto height = handle->get_height();
    auto channels = handle->get_channels();
    auto data = handle->get_float_data();
    auto result = SaveEXR(data, width, height, channels, 1, path.string().c_str(), nullptr);

    if (result != TINYEXR_SUCCESS) {
        CFW_LOG_ERROR("[ImageParser::export_exr] Failed to write EXR image to: {}", path.string());
        return false;
    }

    return true;
}

std::shared_ptr<IResource> ImageParser::parse_from_memory(std::span<const std::byte> data,
                                                          const std::filesystem::path& virtual_path,
                                                          const ImageImportOptions& options) {
    if (data.empty()) {
        return nullptr;
    }

    int width, height, original_channels;
    // 强制加载为 4 通道 (RGBA)，支持任意通道数的源图像（包括灰度图）
    std::unique_ptr<unsigned char[], decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(data.data()),
            static_cast<int>(data.size()),
            &width, &height, &original_channels, 4),
        stbi_image_free);

    if (!pixels) {
        std::cerr << "[ImageParser] Failed to parse image from memory: " << stbi_failure_reason() << std::endl;
        return nullptr;
    }

    auto image = std::make_shared<Image>(virtual_path);
    image->set_data(pixels.get(), width, height, 4);
    if (options.compress) {
        image->compress(options.format);
    }

    return image;
}

std::shared_ptr<IResource> ImageParser::parse_usd_asset(const std::string& resolved_path, const ImageImportOptions& options) {
    if (resolved_path.empty()) {
        return nullptr;
    }

    // 直接通过文件系统加载（tinyusdz 已将包内资源解出或使用文件路径）
    if (!std::filesystem::exists(resolved_path)) {
        std::cerr << "[ImageParser] File not found: " << resolved_path << std::endl;
        return nullptr;
    }

    return parse_stb_image(resolved_path, options);
}

std::uint64_t get_or_create_default_white_texture() {
    static std::uint64_t default_texture_id = 0;
    static bool initialized = false;

    if (initialized) {
        return default_texture_id;
    }

    auto image = std::make_shared<Image>(std::filesystem::path("__default_white_texture__"));

    static unsigned char white_pixel[4] = {255, 255, 255, 255};
    image->set_data(white_pixel, 1, 1, 4);

    image->compress(ImageImportOptions{}.format);

    default_texture_id = image->get_uid();
    ResourceManager::get_instance().add_resource(default_texture_id, image);

    initialized = true;
    return default_texture_id;
}

}  // namespace Corona::Resource
