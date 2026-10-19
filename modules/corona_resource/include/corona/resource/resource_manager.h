#pragma once
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

#include <future>

#include "parser_registry.h"
#include "resource.h"
#include "resource_cache.h"

namespace Corona::Resource {

struct AnimationSource;
using AnimationSources = std::vector<std::shared_ptr<const AnimationSource>>;

/**
 * @brief 资源管理器类（单例模式）
 *
 * 负责资源的加载、卸载、缓存、导入、导出以及读写操作。
 * 支持同步和异步操作，并使用 TBB 进行并发任务管理。
 */
class ResourceManager final {
   public:
    /**
     * @brief 获取单例实例
     *
     * @return ResourceManager& 资源管理器实例的引用
     */
    static ResourceManager& get_instance();

    /**
     * @brief 删除拷贝构造函数
     */
    ResourceManager(const ResourceManager&) = delete;

    /**
     * @brief 删除拷贝赋值操作符
     */
    ResourceManager& operator=(const ResourceManager&) = delete;

    /**
     * @brief 注册资源解析器
     *
     * @tparam Parser 解析器类型
     * @tparam Args 构造函数参数类型
     * @param args 传递给 Parser 构造函数的参数
     * @return true 注册成功
     * @return false 注册失败
     */
    template <typename Parser, typename... Args>
    bool register_parser(Args&&... args);

    /**
     * @brief 注册已创建的解析器实例
     * @param parser 解析器实例
     * @return true 注册成功
     * @return false 注册失败
     */
    bool register_parser(std::shared_ptr<IParser> parser);

    /**
     * @brief 同步导入资源
     *
     * @param path 资源路径
     * @return TResourceID 资源ID，如果失败返回 IResource::INVALID_UID
     */
    TResourceID import_sync(const std::filesystem::path& path);

    /**
     * @brief 异步导入资源
     *
     * @param path 资源路径
     * @return std::future<TResourceID> 包含资源ID的future对象
     */
    std::future<TResourceID> import_async(const std::filesystem::path& path);

    // Separate typed animation cache; uses the same managed worker lifetime.
    std::shared_future<AnimationSources> import_animations_async(std::vector<std::filesystem::path> paths);

    /**
     * @brief 同步导出资源
     *
     * @param rid 资源ID
     * @param path 导出路径
     * @return true 导出成功
     * @return false 导出失败
     */
    bool export_sync(TResourceID rid, const std::filesystem::path& path);

    /**
     * @brief 异步导出资源
     *
     * @param rid 资源ID
     * @param path 导出路径
     * @return std::future<bool> 包含导出结果的future对象
     */
    std::future<bool> export_async(TResourceID rid, const std::filesystem::path& path);

    /**
     * @brief 获取资源读取句柄
     *
     * 尝试获取资源的共享锁。如果资源未就绪，返回无效句柄。
     *
     * @param rid 资源ID
     * @return ResourceReadHandle<T> 资源读取句柄
     */
    template <typename T = IResource>
    ReadHandle<T> acquire_read(TResourceID rid) {
        return resource_cache_.acquire_read<T>(rid);
    }

    /**
     * @brief 尽力而为的读取句柄：不等待被占用的条目锁
     *
     * 供绘制/输入线程等"必须保持响应"的调用者使用；拿不到就当作本次没有数据。
     */
    template <typename T = IResource>
    ReadHandle<T> try_acquire_read_nowait(TResourceID rid) {
        return resource_cache_.try_acquire_read_nowait<T>(rid);
    }

    /**
     * @brief 获取资源写入句柄
     *
     * 尝试获取资源的独占锁。如果资源未就绪，返回无效句柄。
     *
     * @param rid 资源ID
     * @return ResourceWriteHandle<T> 资源写入句柄
     */
    template <typename T = IResource>
    WriteHandle<T> acquire_write(TResourceID rid) {
        return resource_cache_.acquire_write<T>(rid);
    }

    /**
     * @brief 有界等待地尝试获取资源写入句柄（不会无限阻塞）
     *
     * 见 ResourceCache::try_acquire_write_for。适合"可延后"的后台任务：
     * 拿不到锁就本周期跳过，而不是把调用线程（常常是某个系统的帧循环）冻住。
     *
     * @param rid 资源ID
     * @param timeout 最长等待时间
     * @return ResourceWriteHandle<T> 句柄；超时返回无效句柄
     */
    template <typename T = IResource>
    WriteHandle<T> try_acquire_write_for(
        TResourceID rid, std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        return resource_cache_.try_acquire_write_for<T>(rid, timeout);
    }

    /**
     * @brief 尽力而为的写入句柄：不等待被占用的条目锁
     *
     * 周期性的整理任务（LOD/RAM 驻留窗口）用它，避免被一个慢读者把系统线程
     * 永久钉死在该锁上；拿不到就跳过本轮。
     */
    template <typename T = IResource>
    WriteHandle<T> try_acquire_write_nowait(TResourceID rid) {
        return resource_cache_.try_acquire_write_nowait<T>(rid);
    }

    /**
     * @brief 移除资源缓存
     *
     * @param rid 资源ID
     * @return true 移除成功
     * @return false 移除失败（资源不存在）
     */
    bool remove_cache(TResourceID rid);

    /**
     * @brief 异步移除资源缓存
     * @param rid 资源ID
     * @return 包含移除结果的future对象
     */
    std::future<bool> remove_cache_async(TResourceID rid);

    /**
     * @brief 添加已加载的资源到缓存
     *
     * 将外部加载的资源添加到管理器缓存中。
     *
     * @param rid 资源ID
     * @param resource 资源指针
     * @return true 添加成功
     * @return false 资源已存在
     */
    bool add_resource(TResourceID rid, std::shared_ptr<IResource> resource,
                      std::size_t estimated_bytes = 0);

    // ========================================
    // 资源管理 (pin / touch / budget / evict)
    // ========================================

    /// 标记资源为不可淘汰
    bool pin(TResourceID rid);

    /// 取消不可淘汰标记
    bool unpin(TResourceID rid);

    /// 仅更新访问时间（比 acquire_read 更轻量）
    bool touch(TResourceID rid);

    /// 查询单个资源的只读信息
    std::optional<ResourceEntryInfo> entry_info(TResourceID rid) const;

    /// 列出所有资源的只读信息
    std::vector<ResourceEntryInfo> list_entries() const;

    /// 设置内存预算上限（字节），0 = 不限制
    void set_memory_budget(std::size_t bytes);

    /// 当前估算内存使用量（字节）
    std::size_t used_memory_bytes() const;

    /// 内存预算上限（字节）
    std::size_t memory_budget() const;

    /// 尝试淘汰一个指定资源（跳过 pinned 和 ref_count > 0 的项）
    EvictResult try_evict(TResourceID rid);

    /// 持续淘汰最旧的未 pin 资源，直到内存使用量低于预算
    /// @return 最后一次淘汰操作的结果
    EvictResult evict_until_under_budget();

   private:
    /**
     * @brief 私有构造函数
     */
    ResourceManager();

    /**
     * @brief 析构函数
     *
     * 取消所有异步任务并清理资源。
     */
    ~ResourceManager();

    /**
     * @brief 内部加载逻辑
     *
     * 处理资源的加载、缓存查找和并发控制。
     *
     * @param path 资源路径
     * @return TResourceID 资源ID
     */
    TResourceID load_internal(const std::filesystem::path& path);

   private:
    // 模型/场景导入通常同时触发多个大任务。将导入任务限制在小型 arena
    // 内，避免一次打开场景时占满所有 worker 并阻塞渲染线程。
    tbb::task_arena import_arena_{2};
    tbb::task_group async_tasks_{};  ///< TBB任务组，用于管理异步任务

    ParserRegistry parser_registry_;
    ResourceCache resource_cache_;
};

template <typename Parser, typename... Args>
bool ResourceManager::register_parser(Args&&... args) {
    return parser_registry_.register_parser<Parser>(std::forward<Args>(args)...);
}

inline bool ResourceManager::register_parser(std::shared_ptr<IParser> parser) {
    return parser_registry_.register_parser(std::move(parser));
}
}  // namespace Corona::Resource
