#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "resource_cache.h"

namespace Corona::Resource {
using TResourceID = std::uint64_t;

/**
 * @brief 资源基类接口
 *
 * 所有具体资源类型都应继承自此类。
 */
class IResource {
   public:
    /**
     * @brief 根据路径生成唯一的资源UID
     *
     * @param path 资源路径
     * @return std::uint64_t 生成的UID
     */
    static std::uint64_t generate_uid(const std::filesystem::path& path);

   public:
    static constexpr TResourceID INVALID_UID = 0;  ///< 无效UID常量

    /**
     * @brief 构造函数
     *
     * @param path 资源路径
     */
    explicit IResource(const std::filesystem::path& path = "");
    virtual ~IResource() = default;

    /**
     * @brief 检查资源是否有效
     *
     * @return true 路径不为空
     * @return false 资源无效
     */
    [[nodiscard]] bool valid() const;

    /**
     * @brief 获取资源UID
     *
     * @return std::uint64_t 资源UID
     */
    std::uint64_t get_uid() const;

   private:
    friend class ResourceManager;
    std::uint64_t uid;
};

/**
 * @brief 资源解析器接口
 *
 * 负责将文件解析为资源对象，或将资源对象导出为文件。
 */
class IParser {
   public:
    /// 导入处理函数类型
    using ImportHandler = std::function<std::shared_ptr<IResource>(const std::filesystem::path&, ResourceCache&)>;
    /// 导出处理函数类型
    using ExportHandler = std::function<bool(const IResource&, const std::filesystem::path&)>;

    IParser() = default;
    virtual ~IParser() = default;

    /**
     * @brief 从指定路径导入资源
     *
     * @param path 文件路径
     * @return std::shared_ptr<IResource> 创建的资源对象，失败返回nullptr
     */
    [[nodiscard]] std::shared_ptr<IResource> import_from(const std::filesystem::path& path, ResourceCache& cache);

    /**
     * @brief 将资源导出到指定路径
     *
     * @param resource 要导出的资源
     * @param path 目标路径
     * @return true 导出成功
     * @return false 导出失败
     */
    [[nodiscard]] bool export_to(const IResource& resource, const std::filesystem::path& path);

    /**
     * @brief 检查导入路径是否支持
     *
     * @param path 文件路径
     * @return bool 是否支持
     */
    [[nodiscard]] bool is_supported(const std::filesystem::path& path) const;

    /**
     * @brief 检查导出路径是否支持
     *
     * @param path 文件路径
     * @return bool 是否支持
     */
    [[nodiscard]] bool is_export_supported(const std::filesystem::path& path) const;

    /**
     * @brief 获取支持的导入扩展名列表
     *
     * @return const std::unordered_map<std::string, ImportHandler>& 支持的扩展名列表
     */
    [[nodiscard]] std::unordered_map<std::string, ImportHandler> get_supported_extensions() const { return supported_extensions_; }

    /**
     * @brief 获取支持的导出扩展名列表
     *
     * @return const std::unordered_map<std::string, ExportHandler>& 支持的导出扩展名列表
     */
    [[nodiscard]] std::unordered_map<std::string, ExportHandler> get_supported_export_extensions() const { return supported_export_extensions_; }

   protected:
    /**
     * @brief 注册支持的导入扩展名
     *
     * @param extension 扩展名（如 ".obj"）
     * @param handler 处理函数，用于创建资源对象
     */
    void register_extension(const std::string& extension, ImportHandler handler);

    /**
     * @brief 注册支持的导出扩展名
     *
     * @param extension 扩展名（如 ".png"）
     * @param handler 处理函数，用于导出资源
     */
    void register_exporter(const std::string& extension, ExportHandler handler);

   protected:
    std::unordered_map<std::string, ImportHandler> supported_extensions_;         ///< 支持的导入扩展名列表
    std::unordered_map<std::string, ExportHandler> supported_export_extensions_;  ///< 支持的导出扩展名列表
};

}  // namespace Corona::Resource
