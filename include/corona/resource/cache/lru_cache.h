#pragma once

/// @file lru_cache.h
/// @brief 生产级两级 LRU 缓存（内存 + 磁盘）— ResourceResidency 基础设施
///
/// 从早期的 lru_cache_example 示例提升并重构：
/// - 命名空间收口到 Corona::Cache
/// - 新增淘汰回调 (evict callback)，通知上层释放 GPU 资源
/// - 磁盘目录策略强制：{root}/{key}.cache
/// - 线程安全：CacheManager 级别 mutex 保护所有操作

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Corona::Cache {

// ============================================================================
// CacheRecord — 单个缓存项
// ============================================================================

struct CacheRecord {
    std::string                            key;
    std::vector<char>                      data;
    size_t                                 data_size = 0;
    std::chrono::system_clock::time_point  last_access;
    bool                                   pinned = false;

    CacheRecord();
    CacheRecord(std::string k, const char* d, size_t sz);
    ~CacheRecord() = default;

    // 只允许移动（避免意外拷贝大数据）
    CacheRecord(const CacheRecord&) = delete;
    CacheRecord& operator=(const CacheRecord&) = delete;
    CacheRecord(CacheRecord&&) noexcept = default;
    CacheRecord& operator=(CacheRecord&&) noexcept = default;
};

// ============================================================================
// EvictStatus / EvictResult — 淘汰操作的结构化结果
// ============================================================================

enum class EvictStatus {
    Success,        // 成功淘汰一项
    CacheEmpty,     // 缓存为空，无项可淘汰
    AllPinned,      // 所有项都被 pinned，跳过后无候选项
    DiskFull,       // 磁盘容量不足（仅 DiskCache）
    DiskWriteError  // 磁盘写入失败（仅 DiskCache）
};

struct EvictResult {
    EvictStatus status = EvictStatus::Success;
    std::string key;                           // 被淘汰项的 key（Success 时有效）
    size_t      bytes_freed = 0;               // 释放的字节数
    std::optional<CacheRecord> item;             // 被淘汰的数据（仅 MemoryCache::evict_lru 填充）

    [[nodiscard]] bool ok() const { return status == EvictStatus::Success; }
};

// ============================================================================
// MemoryCache — 内存 LRU
// ============================================================================

class MemoryCache {
public:
    /// @param capacity_bytes 内存容量上限（字节）
    explicit MemoryCache(size_t capacity_bytes);
    ~MemoryCache();

    MemoryCache(const MemoryCache&) = delete;
    MemoryCache& operator=(const MemoryCache&) = delete;

    /// 插入/更新项。若 key 已存在则更新并提升到 LRU 头部。
    /// @return false 表示容量不足以容纳新项
    bool put(const std::string& key, const char* data, size_t size);

    /// 获取项（提升到 LRU 头部）。返回拷贝，避免引用在后续 evict 中悬空。
    std::optional<CacheRecord> get(const std::string& key);

    /// 仅更新访问时间并提升到 LRU 头部，不返回数据。
    /// 比 get() 更轻量，适合调用方每帧"续期"活跃资源。
    bool touch(const std::string& key);

    /// 标记为不可淘汰（LRU evict 时跳过）
    bool pin(const std::string& key);

    /// 取消不可淘汰标记
    bool unpin(const std::string& key);

    /// 移除项
    void erase(const std::string& key);

    /// 淘汰 LRU 尾部第一个非 pinned 项。
    /// @return EvictResult{Success, key, bytes_freed, item} 或 {CacheEmpty} / {AllPinned}
    EvictResult evict_lru();

    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] size_t used_bytes() const { return used_; }
    [[nodiscard]] size_t capacity_bytes() const { return capacity_; }
    [[nodiscard]] size_t item_count() const { return map_.size(); }

private:
    size_t capacity_;
    size_t used_ = 0;
    std::list<CacheRecord> list_;
    std::unordered_map<std::string, std::list<CacheRecord>::iterator> map_;
};

// ============================================================================
// DiskCache — 磁盘 LRU
// ============================================================================

class DiskCache {
public:
    /// @param capacity_bytes 磁盘容量上限
    /// @param directory      缓存目录（会自动创建）
    DiskCache(size_t capacity_bytes, std::filesystem::path directory);
    ~DiskCache();

    DiskCache(const DiskCache&) = delete;
    DiskCache& operator=(const DiskCache&) = delete;

    /// 插入/更新项
    bool put(CacheRecord item);

    /// 获取项（从磁盘读回并提升到 LRU 头部）
    std::optional<CacheRecord> get(const std::string& key);

    /// 仅更新访问时间并提升到 LRU 头部（不读磁盘）
    bool touch(const std::string& key);

    /// 标记为不可淘汰
    bool pin(const std::string& key);

    /// 取消不可淘汰标记
    bool unpin(const std::string& key);

    /// 查询是否被 pin
    [[nodiscard]] bool is_pinned(const std::string& key) const;

    /// 删除项（从磁盘删除文件 + 从 LRU 链表移除）
    void erase(const std::string& key);

    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] size_t used_bytes();
    [[nodiscard]] size_t capacity_bytes() const { return capacity_; }
    [[nodiscard]] size_t item_count() const { return map_.size(); }

private:
    size_t capacity_;
    size_t used_ = 0;  // 增量维护的磁盘总字节，恒等于 sum(map_[*].data_size)。
                       // 替代热路径上的 calc_directory_size() 全目录 stat 扫描。
    std::filesystem::path dir_;
    mutable std::mutex mtx_;  // 仅保护 list_ + map_ + 单次文件操作；CacheManager 的锁独立
    std::list<std::string> list_;
    // 映射中的 CacheRecord 只存元数据（不含实际 data），磁盘是真实数据源
    std::unordered_map<std::string, std::pair<CacheRecord, std::list<std::string>::iterator>> map_;

    /// 将 key 转为安全的文件路径
    [[nodiscard]] std::filesystem::path safe_path(const std::string& key) const;

    /// 从磁盘读取文件
    std::optional<CacheRecord> read_file(const std::string& key) const;

    /// 写入文件到磁盘
    bool write_file(const CacheRecord& item) const;

    /// 淘汰最旧的文件以腾出空间。调用方需持有 mtx_。
    /// @param skip_key 跳过此 key（避免刚插入的项被立即淘汰）
    EvictResult evict_one(const std::string& skip_key = "");
};

// ============================================================================
// CacheManager — 两级 LRU 管理器
// ============================================================================

/// 淘汰回调类型：当 MemoryCache 满时将项刷到磁盘，或 DiskCache 满时淘汰最旧项
/// @param key   被淘汰项的 key
/// @param data  被淘汰项的数据（仅在从内存刷到磁盘时非空）
using EvictCallback = std::function<void(const std::string& key, const std::vector<char>& data)>;

class CacheManager {
public:
    /// @param mem_capacity  内存缓存容量（字节）
    /// @param disk_capacity 磁盘缓存容量（字节），0 = 仅内存
    /// @param disk_dir      磁盘缓存目录，仅 disk_capacity > 0 时需要
    CacheManager(size_t mem_capacity, size_t disk_capacity, std::filesystem::path disk_dir);

    ~CacheManager() = default;

    CacheManager(const CacheManager&) = delete;
    CacheManager& operator=(const CacheManager&) = delete;

    /// 存入一项。优先放内存；内存满则刷盘。
    bool put(const std::string& key, const char* data, size_t size);

    /// 获取一项。先查内存后查磁盘；命中磁盘后提升到内存。
    std::optional<CacheRecord> get(const std::string& key);

    /// 删除一项（内存 + 磁盘）
    void erase(const std::string& key);

    /// 检查 key 是否存在
    [[nodiscard]] bool contains(const std::string& key);

    // ---- pin / unpin / touch ----
    /// 仅更新访问时间（内存或磁盘），不返回数据
    bool touch(const std::string& key);

    /// 标记为不可淘汰（内存 + 磁盘）
    bool pin(const std::string& key);

    /// 取消不可淘汰标记（内存 + 磁盘）
    bool unpin(const std::string& key);

    // ---- 容量查询 ----
    [[nodiscard]] size_t memory_used() const;
    [[nodiscard]] size_t memory_capacity() const;
    [[nodiscard]] size_t disk_used();
    [[nodiscard]] size_t disk_capacity() const;

    // ---- 淘汰回调 ----
    /// 设置淘汰回调：当内存刷盘或磁盘淘汰时被调用
    void set_evict_callback(EvictCallback cb);

private:
    MemoryCache mem_;
    std::optional<DiskCache> disk_;
    mutable std::mutex mtx_;
    EvictCallback evict_cb_;

};


}  // namespace Corona::Cache
