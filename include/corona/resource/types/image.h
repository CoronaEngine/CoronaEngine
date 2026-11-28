#pragma once

#include <span>
#include <vector>

#include "corona/resource/resource.h"

namespace Corona::Resource {

struct CompressedData {
    std::vector<unsigned char> data;
    enum class Format {
        BC1,      // DXT1: 8 bytes/block, 无Alpha, PC/Xbox
        BC3,      // DXT5: 16 bytes/block, 完整Alpha, PC/Xbox
        ASTC_4x4  // ASTC 4x4: 16 bytes/block, 高质量, 移动端优先
    } format;
    int mip_levels;
};

struct ImageImportOptions {
    CompressedData::Format format = CompressedData::Format::BC3;
    // CompressedData::Format format = CompressedData::Format::ASTC_4x4;
    bool compress = true;
};

class Image : public IResource {
   public:
    explicit Image(const std::filesystem::path& path);
    ~Image() override = default;

    void compress(CompressedData::Format format);
    [[nodiscard]] bool is_compressed() const;
    [[nodiscard]] const CompressedData& get_compressed_data() const;

    void set_data(unsigned char* data, int width, int height, int channels);
    void set_float_data(float* data, int width, int height, int channels);

    [[nodiscard]] unsigned char* get_data() const;
    [[nodiscard]] float* get_float_data() const;
    [[nodiscard]] int get_width() const;
    [[nodiscard]] int get_height() const;
    [[nodiscard]] int get_channels() const;

   private:
    std::vector<unsigned char> pixel_data_{};
    std::vector<float> float_pixel_data_{};
    int width_{0}, height_{0}, channels_{0};

    std::optional<CompressedData> compressed_data_;
};

class ImageParser : public IParser {
   public:
    ImageParser();
    ~ImageParser() override = default;

    /**
     * @brief 从内存数据解析图像
     *
     * @param data 图像数据
     * @param virtual_path 虚拟路径（用于生成资源ID）
     * @param options 导入选项
     * @return std::shared_ptr<IResource> 图像资源
     */
    std::shared_ptr<IResource> parse_from_memory(std::span<const std::byte> data,
                                                 const std::filesystem::path& virtual_path,
                                                 const ImageImportOptions& options = {});

    /**
     * @brief 从 USD 资产路径加载图像（支持 USDZ 内嵌资源）
     *
     * @param resolved_path USD 解析后的路径（可能包含包内路径如 "archive.usdz[0/texture.jpg]"）
     * @param options 导入选项
     * @return std::shared_ptr<IResource> 图像资源，失败返回 nullptr
     */
    std::shared_ptr<IResource> parse_usd_asset(const std::string& resolved_path,
                                               const ImageImportOptions& options = {});

   protected:
    std::shared_ptr<IResource> parse_stb_image(const std::filesystem::path& path,
                                               const ImageImportOptions& options = {});
    std::shared_ptr<IResource> parse_exr(const std::filesystem::path& path);
    std::shared_ptr<IResource> parse_float_image(const std::filesystem::path& path);

    bool export_png(const IResource& resource, const std::filesystem::path& path);
    bool export_jpg(const IResource& resource, const std::filesystem::path& path);
    bool export_bmp(const IResource& resource, const std::filesystem::path& path);
    bool export_tga(const IResource& resource, const std::filesystem::path& path);
    bool export_exr(const IResource& resource, const std::filesystem::path& path);
};

/**
 * @brief 获取或创建默认的1x1白色纹理
 *
 * 当模型没有纹理时使用此默认纹理。
 * 纹理仅在首次调用时创建，后续调用返回缓存的纹理ID。
 *
 * @return std::uint64_t 默认白色纹理的资源ID
 */
std::uint64_t get_or_create_default_white_texture();

}  // namespace Corona::Resource
