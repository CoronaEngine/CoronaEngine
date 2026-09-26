#pragma once

/// @file actor_cache.h
/// @brief ActorCache — 类型化 Actor 流式缓存，封装两级 LRU CacheManager
///
/// ActorStreamingRecord 包含重建 actor 所需的完整信息集：
/// - 身份：scene / actor handle
/// - model_path（资源文件路径，也是 ResourceManager 的 key）
/// - transform（position / euler_rotation / scale，ModelTransform 值类型）
/// - profile_handles / geometry_handles（用于恢复时校验和诊断）
/// - resource_ids（geometry → model_resource → model_id 链，用于恢复时匹配）
/// - 运行时标志：body_type / optics_visible / follow_camera / pinned
/// - priority（流式调度优先级，预留）
///
/// 注意：mesh / texture 等重资源由 ResourceManager 自行管理；
///       ActorStreamingRecord 只存资源路径和句柄引用，不重复序列化几何数据。

#include <corona/resource/cache/lru_cache.h>
#include <corona/shared_data_hub.h>  // ModelTransform

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Corona::Cache {

// ============================================================================
// ActorStreamingRecord — 重建 actor 所需的完整状态
// ============================================================================

struct ActorStreamingRecord {
    std::uintptr_t              scene{};             // 所属 scene handle
    std::uintptr_t              actor{};             // actor handle（与 ActorCache key 一致）
    std::filesystem::path       model_path;          // 模型文件路径（ResourceManager key）
    std::vector<std::uintptr_t> profile_handles;     // ActorDevice::profile_handles 全量拷贝
    std::vector<std::uintptr_t> geometry_handles;    // 每个 profile 的 geometry_handle
    std::vector<std::uint64_t>  resource_ids;        // geometry → model_resource → model_id
    ModelTransform              transform;           // world position / euler rotation / scale
    BodyType                    body_type{BodyType::Dynamic};
    bool                        optics_visible{true};
    bool                        follow_camera{false};
    bool                        pinned{false};
    float                       priority{0.0f};

    /// JSON 序列化
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static std::optional<ActorStreamingRecord> from_json(const std::string& json);
};

// ============================================================================
// ActorCache — 类型化 actor 缓存
// ============================================================================

class ActorCache {
public:
    /// @param mem_capacity  内存缓存容量（字节），默认 64 MB
    /// @param disk_capacity 磁盘缓存容量（字节），0 = 仅内存
    /// @param disk_dir      磁盘缓存目录
    ActorCache(size_t mem_capacity, size_t disk_capacity,
               std::filesystem::path disk_dir);

    ~ActorCache() = default;

    ActorCache(const ActorCache&) = delete;
    ActorCache& operator=(const ActorCache&) = delete;

    // ---- 核心 API ----

    /// 存入 actor 流式记录
    bool put(std::uintptr_t actor_handle, const ActorStreamingRecord& record);

    /// 获取 actor 流式记录
    std::optional<ActorStreamingRecord> get(std::uintptr_t actor_handle);

    /// 移除 actor 缓存项
    void remove(std::uintptr_t actor_handle);

    /// 检查 actor 是否在缓存中
    [[nodiscard]] bool contains(std::uintptr_t actor_handle);

    // ---- 容量查询 ----
    [[nodiscard]] size_t memory_used() const;
    [[nodiscard]] size_t memory_capacity() const;
    [[nodiscard]] size_t disk_used();
    [[nodiscard]] size_t disk_capacity() const;

    // ---- 淘汰回调 ----
    /// 设置淘汰回调：当 actor 流式记录被从 LRU 中淘汰时调用
    void set_evict_callback(std::function<void(std::uintptr_t, const std::string&)> cb);

private:
    CacheManager cache_;

    [[nodiscard]] static std::string make_key(std::uintptr_t handle);
    [[nodiscard]] static std::uintptr_t parse_key(const std::string& key);
};

}  // namespace Corona::Cache