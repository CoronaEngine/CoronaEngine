#pragma once

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "resource.h"

namespace Corona::Resource {

/**
 * @brief 解析器注册表
 *
 * 管理所有注册的资源解析器，提供按扩展名或内容查找解析器的功能。
 * 它是线程安全的。
 */
class ParserRegistry {
   public:
    ParserRegistry() = default;
    ~ParserRegistry() = default;

    /**
     * @brief 注册一个新的解析器类型
     *
     * @tparam Parser 解析器类，必须继承自 IParser
     * @tparam Args 构造函数参数类型
     * @param args 传递给 Parser 构造函数的参数
     * @return true 注册成功
     * @return false 注册失败（例如解析器已存在）
     */
    template <typename Parser, typename... Args>
    bool register_parser(Args&&... args) {
        return register_parser_impl(std::make_shared<Parser>(std::forward<Args>(args)...));
    }

    /**
     * @brief 注册一个已创建的解析器实例
     * @param parser 解析器实例
     * @return true 注册成功
     * @return false 注册失败
     */
    bool register_parser(std::shared_ptr<IParser> parser) {
        return register_parser_impl(std::move(parser));
    }

    /**
     * @brief 查找适合指定路径的解析器
     *
     * 首先尝试通过文件扩展名查找，如果失败则遍历所有解析器尝试解析。
     *
     * @param path 资源路径
     * @return std::shared_ptr<IParser> 找到的解析器，如果未找到则返回 nullptr
     */
    std::shared_ptr<IParser> find_parser(const std::filesystem::path& path);

    /**
     * @brief 查找适合导出到指定路径的解析器
     *
     * 根据文件扩展名查找支持该格式导出的解析器。
     *
     * @param path 导出路径
     * @return std::shared_ptr<IParser> 找到的解析器，如果未找到则返回 nullptr
     */
    std::shared_ptr<IParser> find_export_parser(const std::filesystem::path& path);

    /**
     * @brief 清空所有注册的解析器
     */
    void clear();

   private:
    /**
     * @brief 注册解析器实现的内部方法
     *
     * @param parser 解析器实例
     * @return true 注册成功
     * @return false 注册失败
     */
    bool register_parser_impl(std::shared_ptr<IParser> parser);

    // 读多写少：注册发生在启动期，查找发生在导入线程。锁只保护三张表的
    // 结构，不覆盖 is_supported() 之类的文件探测（那里先拷贝候选再放锁）。
    mutable std::shared_mutex mutex_;
    std::vector<std::shared_ptr<IParser>> parsers_;
    std::unordered_map<std::string, std::shared_ptr<IParser>> parser_registry_;
    std::unordered_map<std::string, std::shared_ptr<IParser>> export_parser_registry_;
};

}  // namespace Corona::Resource
