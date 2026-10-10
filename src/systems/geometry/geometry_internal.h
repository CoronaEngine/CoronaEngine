#pragma once

#include <corona/spatial/octree.h>
#include <corona/systems/geometry/actor_cache.h>
#include <corona/systems/geometry/geometry_mesh_builder.h>
#include <corona/systems/geometry/geometry_system.h>
#include <corona/resource/types/scene.h>
#include "shadow_lod_state.h"

#include <oneapi/tbb/task_group.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <ktm/ktm.h>

namespace Corona::Systems::GeometryInternal {

[[nodiscard]] inline ktm::fvec3 make_fvec3(float x, float y, float z) {
    ktm::fvec3 value;
    value[0] = x;
    value[1] = y;
    value[2] = z;
    return value;
}

[[nodiscard]] inline ktm::fvec4 make_fvec4(float x, float y, float z, float w) {
    ktm::fvec4 value;
    value[0] = x;
    value[1] = y;
    value[2] = z;
    value[3] = w;
    return value;
}

[[nodiscard]] inline ktm::fvec3 transform_local_point_to_world(const Corona::ModelTransform& transform,
                                                        const ktm::fvec3& local_point) {
    const ktm::fmat4x4 matrix = transform.compute_matrix();
    const ktm::fvec4 local_h = make_fvec4(local_point[0], local_point[1], local_point[2], 1.0f);
    const ktm::fvec4 world_h = matrix * local_h;
    return make_fvec3(world_h[0], world_h[1], world_h[2]);
}

inline void world_aabb_from_local_bounds(const Corona::ModelTransform& transform,
                                         const ktm::fvec3& local_min,
                                         const ktm::fvec3& local_max,
                                         Spatial::AABB& out_world_aabb) {
    const ktm::fvec3 corners[8] = {
        make_fvec3(local_min[0], local_min[1], local_min[2]),
        make_fvec3(local_max[0], local_min[1], local_min[2]),
        make_fvec3(local_min[0], local_max[1], local_min[2]),
        make_fvec3(local_max[0], local_max[1], local_min[2]),
        make_fvec3(local_min[0], local_min[1], local_max[2]),
        make_fvec3(local_max[0], local_min[1], local_max[2]),
        make_fvec3(local_min[0], local_max[1], local_max[2]),
        make_fvec3(local_max[0], local_max[1], local_max[2]),
    };

    const ktm::fvec3 first_corner = transform_local_point_to_world(transform, corners[0]);
    out_world_aabb.min = first_corner;
    out_world_aabb.max = first_corner;

    for (int i = 1; i < 8; ++i) {
        const ktm::fvec3 world_corner = transform_local_point_to_world(transform, corners[i]);
        out_world_aabb.min[0] = std::min(out_world_aabb.min[0], world_corner[0]);
        out_world_aabb.min[1] = std::min(out_world_aabb.min[1], world_corner[1]);
        out_world_aabb.min[2] = std::min(out_world_aabb.min[2], world_corner[2]);
        out_world_aabb.max[0] = std::max(out_world_aabb.max[0], world_corner[0]);
        out_world_aabb.max[1] = std::max(out_world_aabb.max[1], world_corner[1]);
        out_world_aabb.max[2] = std::max(out_world_aabb.max[2], world_corner[2]);
    }
}


}  // namespace Corona::Systems::GeometryInternal

namespace Corona::Systems {

// ============================================================================
// LOD 磁盘缓存记录 — 与 Resource::LODLevel 字段一一对应
// ============================================================================
// 当 reconcile_cpu_residency 清空 CPU 窗口外的 LOD 级时，其顶点/索引/骨骼权重
// 数据被移入此结构体，随后序列化为二进制 blob 写入磁盘缓存。恢复时反序列化回填。
// ============================================================================

struct LodDiskRecord {
    std::vector<Resource::Vertex>       vertices;         // 顶点位置+法线+UV，32B/顶点 (packed)
    std::vector<std::uint16_t>          indices;          // 三角形索引，2B/索引
    std::vector<Resource::BoneWeights>  bone_weights;     // 蒙皮骨骼权重，32B/顶点（非蒙皮网格为空）
    float error             = 0.0f;  // meshopt 归一化几何误差
    float screen_threshold  = 0.0f;  // 屏幕占比切换阈值（legacy 回退用）
    float geometric_error   = 0.0f;  // 物体空间几何误差（LOD 选级主判据）
};

// ============================================================================
// 异步写入队列元素
// ============================================================================
// 几何线程在 reconcile_cpu_residency 中构造此任务并入队（std::move 移交数据所有权，
// 不拷贝），后台 worker 线程出队后序列化 + 写入 CacheManager。model_id/mesh_index/
// lod_level 仅用于日志，不参与缓存 key（key 由 make_lod_disk_key 单独生成）。
// ============================================================================

struct LodDiskWriteTask {
    std::string    key;           // 磁盘缓存 key（make_lod_disk_key 生成）
    LodDiskRecord  record;        // 序列化前的 LOD 数据（持有 vector 所有权）
    std::uint64_t  model_id   = 0;  // 仅日志
    std::uint32_t  mesh_index = 0;  // 仅日志
    int            lod_level  = 0;  // 仅日志
    int            retry_count = 0; // 写盘失败重试计数（>0 表示已重试）
};

/// 可移动的 LOD 需求位图（按级号置位）。
/// 渲染线程在 select_* 里 set()，几何线程 reconcile 每帧 take() 取走并清空。
/// LODCacheEntry 需要随 lod_cache insert_or_assign 移动，而 std::atomic 本身不可
/// 拷贝/移动，故包一层：拷贝/移动只搬运当前位值，不搬运原子对象身份。
struct AtomicLodMask {
    std::atomic<std::uint64_t> bits{0};

    AtomicLodMask() = default;
    AtomicLodMask(const AtomicLodMask&) : bits(0) {}
    AtomicLodMask& operator=(const AtomicLodMask&) {
        bits.store(0, std::memory_order_relaxed);
        return *this;
    }
    AtomicLodMask(AtomicLodMask&& other) noexcept
        : bits(other.bits.load(std::memory_order_relaxed)) {}
    AtomicLodMask& operator=(AtomicLodMask&& other) noexcept {
        bits.store(other.bits.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }

    void set(std::uint32_t level) {
        if (level >= 64u) return;
        bits.fetch_or(std::uint64_t{1} << level, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t take() {
        return bits.exchange(0, std::memory_order_relaxed);
    }
};

struct GeometrySystem::Impl {
    using Payload = std::uintptr_t;
    using OctreeEntry = Spatial::Octree<Payload>::Entry;

    struct SceneState {
        Spatial::Octree<Payload>                            tree;
        std::unordered_map<Payload,Spatial::AABB> actor_to_entry; //Actor到AABB映射
        SceneVisibilityConfig                               cfg;
        SceneStats                                          stats;
        mutable std::mutex                                  stats_mutex;
        std::unordered_map<Payload,ActorLoadState>          actor_load_states;

        std::unordered_map<Payload,std::future<std::uint64_t>> loading_tasks;
        std::unordered_map<Payload,std::future<bool>>       unloading_tasks;
        std::unordered_map<Payload,int>                     unload_retry_counts; //卸载重试次数
    };

    mutable std::shared_mutex                               mtx;
    std::unordered_map<std::uintptr_t /*scene*/, SceneState> scenes;
    std::unordered_map<Payload, bool>                       offline_actors;
    std::vector<Kernel::EventId>                            event_subscriptions;
    Kernel::ISystemContext*                                 ctx = nullptr;

    // ========================================
    // 动态减面（LOD）相关状态
    // ========================================
    struct LODCacheEntry {
        std::vector<LODMeshBuffers> levels;
        std::uint64_t model_id = 0;  // 用于检测模型变更（比地址指针可靠，不受 slot 复用影响）

        // 滞回状态（方案 B）：reconcile 每帧用滞回死区更新本值；render 直接读取，
        // 不再独立按屏占比选级。两者共用同一决策，保证 render 选中的级必然已驻留
        // （reconcile 已为 {LOD0, committed_demand} 建好 GPU 缓冲），不会选到未驻留级
        // 而被迫降级到 LOD0。0 = LOD0（最高精度）。
        int committed_demand = 0;

        // ---- swap 模型状态（替代旧冷却窗口）----
        // 稳态：swap_in_progress=false，prev_committed=-1，lod_cache 在 LOD1..N 中
        // 至多保有 committed_demand 这 1 套 GPU 缓冲（LOD0 永驻但只是 mesh_dev 的引用计数副本，
        // 不占额外显存）。
        // 切换中：demand 跳到新级但新级尚未 ready → swap_in_progress=true，
        // prev_committed 记录旧级（保活供 select_render_buffers 降级使用）。
        // 切换完成：process_pending_lod_builds 检测到新级 ready → 立即释放 prev_committed，
        // swap_in_progress=false，VRAM 回到稳态 1 套简化缓冲。
        int  prev_committed    = -1;    // ≥0 表示保活中的旧简化级；-1 表示无保活
        bool swap_in_progress  = false; // true = 正在等待新级 build 完成

        int shadow_committed_demand = -1;
        std::uint64_t shadow_last_request_frame = 0;
        int shadow_prev_committed = -1;
        bool shadow_swap_in_progress = false;

        // 条目身份版本（方案 C 异步构建 ABA 守卫）：upload 每次 (重)建本条目时自增。
        // 异步构建任务捕获发起时的 epoch；回写时比对，防止 evict→erase→重载同 model_id
        // 后旧任务误填新条目（model_id 相同骗过 model_id 校验的极端情形）。
        std::uint64_t residency_epoch = 0;

        // 包围半径（Fix 1）：= 0.5·diag(Scene 局部 AABB)，模型空间恒定，upload 时算一次缓存。
        // reconcile 每帧选级只需此标量，从而无需每帧每 geom 再 acquire_read<Scene> 重算 AABB
        // （消除 diag 标注的"scene_acquires 每帧每 geom"卡顿源）；仅真正 build 时才取 Scene。
        // [legacy] 旧屏占比选级用；screen-space-error 选级改用下方 per-mesh 局部 AABB。
        float bounding_radius = 1.0f;

        // per-mesh 局部空间 AABB（screen-space-error 选级用）。upload 时从 Scene 的
        // mesh.aabb_min/max 缓存（归一化模型空间，与 geometric_error 同空间）。
        // reconcile 每帧用 world_aabb_from_local_bounds(transform, ...) 经完整 R/S/T 变换得
        // mesh 世界 AABB → 算相机到最近点距离。这比"轴心 + 外接球"正确：
        //   (1) per-mesh 而非整场景，小 mesh 不被大场景半径误判；
        //   (2) 保留各向异性（扁平/细长物体不被外接球高估）；
        //   (3) 距离基于几何 AABB 而非轴心，相机环绕几何中心时距离恒定 → 不再无谓跳级。
        ktm::fvec3 local_aabb_min{0.0f, 0.0f, 0.0f};
        ktm::fvec3 local_aabb_max{0.0f, 0.0f, 0.0f};

        bool lod_spatially_evicted = false;  // 被空间淘汰强制 LOD0

        // ---- 共享实例资产的集合式驻留（GPU 只保留被需求的 LOD）----
        // 多实例资产（同一 MeshData 被多个 MeshInstanceData 引用）不能再按单个
        // committed_demand 选级：不同实例距离不同，各自需要的 LOD 也不同。
        // 渲染线程在 select_render_buffers_for_instance / select_shadow_render_buffers
        // 里把每个实例实际选中的级写进 frame_demand_mask；reconcile 每帧 take() 取走，
        // 于是驻留集合 = 需求集合 = 各实例选中级的并集：
        //   只构建并集里的级，只释放并集外的级（带逐级宽限帧，防边界抖动）。
        AtomicLodMask frame_demand_mask;                       // render 写，reconcile 取走
        std::uint64_t demanded_mask = 0;                       // 本帧并集快照（诊断）
        bool          shared_instances = false;                // true = 走集合式驻留
    };

    mutable std::shared_mutex          lod_cache_mutex;
    std::unordered_map<uint64_t, LODCacheEntry> lod_cache;

    // ========================================
    // LOD 异步构建（方案 C）
    // ========================================
    // reconcile 不再在几何线程同帧 make_geometry_buffer（×4，阻塞帧）；改为发起一个
    // TBB 任务在 worker 线程构建 GPU 缓冲（HardwareBuffer 构造走持久映射 memcpy+flush，
    // 经 Horizon ResourceManager 单 mutex 全序列化，任意线程安全），完成后由
    // process_pending_lod_builds() 在几何线程轮询回写。构建期间渲染端就近回退显示已驻级。
    //
    // GpuMemToken 不在 worker 构建（回避 ledger 线程安全问题）：worker 只产出缓冲 +
    // 字节数，令牌在回写时于几何线程构建。
    struct LODBuildResult {
        Horizon::HardwareBuffer vertex_buffer;
        Horizon::HardwareBuffer index_buffer;
        Horizon::HardwareBuffer vertex_storage;
        Horizon::HardwareBuffer index_storage;
        std::size_t             gpu_bytes = 0;
        bool ok = false;
    };

    // 每个 (geom,mesh) 至多一个在途任务（committed 是单值）。仅几何线程访问，无需加锁。
    enum class LodBuildPurpose : std::uint8_t { Main, Shadow };
    struct PendingLodBuild {
        std::uint64_t               model_id        = 0;  // ABA 守卫（防 slot 复用）
        std::uint64_t               residency_epoch = 0;  // ABA 守卫（防同 model_id 重建）
        int                         level           = 0;  // 目标缓存级下标
        std::future<LODBuildResult> future;
        LodBuildPurpose purpose = LodBuildPurpose::Main;
    };
    std::unordered_map<uint64_t /*lod_key*/, PendingLodBuild> pending_lod_builds;
    std::unordered_map<uint64_t /*lod_key*/, PendingLodBuild> pending_shadow_lod_builds;

    // TBB 任务组（复用全局 worker 池，不新建线程）；shutdown 时 wait() 排空。
    tbb::task_group lod_build_tasks;

    // 条目身份版本分配器（几何线程单调递增，0 保留为"无"）。
    std::uint64_t next_residency_epoch = 1;

    // 在途构建并发上限：约束 VRAM 尖峰与 worker 争用；超出则本帧不发起（仍 !ready，
    // 渲染就近回退），下帧 reconcile 再驱动。
    // Fix 2：4→8。相机移动时多 mesh 同时跨级会积压粗级 build，上限过低使其错峰完成、
    // 渲染回退窗口拉长 → 视觉上逐个 pop。提到 8 加快排空、缩短回退窗口（VRAM 尖峰可控：
    // 粗级数据量小，且 reconcile 仍每 (geom,mesh) 至多一个在途任务）。
    static constexpr size_t kMaxInflightLodBuilds = 8;

    // 诊断（与现有 diag_* 一同每秒输出）：发起次数 / 丢弃次数（ABA 校验失败或构建失败）。
    std::uint64_t diag_lod_build_launches = 0;
    std::uint64_t diag_lod_build_discards = 0;

    // ========================================
    // LOD 释放冷却（方案 A）
    // ========================================
    // reconcile_lod_residency 每帧自增的帧计数器，作为 LODMeshBuffers::last_demand_frame
    // 的时间基准。单调递增，永不回绕（uint64 @60fps 可跑约 97 亿年）。
    std::uint64_t lod_frame_counter = 0;

    // ===== 临时诊断计数器（定位每帧卡顿/LOD切换根因，定位后移除）=====
    // reconcile_lod_residency 一秒内的行为画像：是否在 churn。
    std::uint64_t diag_reconcile_mesh_visits = 0;  // 处理的 (geom,mesh) 次数
    std::uint64_t diag_lod_builds           = 0;   // GPU 缓冲构建次数（make_geometry_buffer×4 + BVH）
    std::uint64_t diag_lod_frees            = 0;   // 释放已就绪级次数
    std::uint64_t diag_scene_acquires       = 0;   // acquire_read<Scene> 次数（每帧每 geom）
    std::uint64_t diag_demand_changes       = 0;   // committed_demand 实际变更次数
    std::uint64_t diag_geometry_upload_queued = 0;
    std::uint64_t diag_geometry_upload_published = 0;
    std::uint64_t diag_geometry_upload_discarded = 0;

    // kLodReleaseCooldownFrames 已废弃（保留注释供历史参考）：
    // 旧机制：demand 切换后旧级保留 90 帧（≈1.5s@60fps），防止阈值边界横跳反复重建。
    // 新机制（swap 模型）：仅在新级 build 完成前保留旧级（通常 <5 帧），
    // 切换完成后立即释放，无额外等待；阈值横跳由滞回死区（kLodHysteresis）吸收。
    // static constexpr std::uint64_t kLodReleaseCooldownFrames = 90;  // 已废弃

    // LOD 选级滞回死区（方案 B）：升/降级用不对称阈值，避免物体停在阈值边界微动时
    // 需求级在相邻级反复横跳（视觉 pop）。当前驻留 L 级时：
    //   降精度(L→L+1)：screen_ratio 必须 < threshold[L]   * (1 - h)
    //   升精度(L→L-1)：screen_ratio 必须 > threshold[L-1] * (1 + h)
    // h=0.15 给出约 ±15% 的死区，正常运动平滑切换、边界抖动被吸收。
    static constexpr float kLodHysteresis = 0.15f;

    // 屏幕空间像素误差预算（screen-space error 选级）：某级简化误差投影到主相机
    // 屏幕后超过此像素数才弃用该级（选更精细级）。等价于"以相机为中心的角预算"
    //   ε = 2·budget_px·tan(fov/2)/height_px
    // 主相机用本预算 + 自身 fov/分辨率换算 ε；选级判据 geometric_error·scale / d ≤ ε，
    // 全方向有定义（相机背后物体同样选级），故未来 GI 观察者可复用同一路径。
    // 默认 1.5px：显存充裕时误差小于约 1.5 像素即视觉无感，可安全切粗级。
    // 显存承压时由 compute_pixel_budget_from_pressure() 动态放宽，趋粗 LOD 自然降显存。
    static constexpr float kLodDefaultPixelBudget = 1.5f;

    // 共享实例资产：某级最后一次被需求后，再保留这么多帧才允许释放。
    // 吸收"某帧实例被剔除/选级抖动"造成的短暂零需求，避免建-删-建。
    static constexpr std::uint64_t kSharedLodFreeGraceFrames = 5;

    // reconcile 每帧按显存压力算出的像素误差预算，供渲染端按实例选级复用，
    // 让显存压力同时作用于标量路径与共享实例集合路径（压力 → 选更粗 → 并集收窄）。
    std::atomic<float> last_lod_pixel_budget{ 1.5f };

    // ========================================
    // LRU ActorCache（M3 生产化）
    // ========================================
    // 两级 LRU 缓存（内存 + 磁盘），存储被 evict 的 actor 快照
    // 默认：64MB 内存 + 256MB 磁盘，目录可配置
    static constexpr size_t kDefaultMemCacheBytes  = 64 * 1024 * 1024;
    static constexpr size_t kDefaultDiskCacheBytes = 256 * 1024 * 1024;

    std::unique_ptr<Corona::Cache::ActorCache> actor_cache;
    std::filesystem::path                       actor_cache_dir;

    /// 初始化 ActorCache（延迟到首次 evict/restore 时）
    void ensure_actor_cache();

    // ========================================
    // LOD 磁盘缓存 — 异步写入
    // ========================================
    //
    // 当 reconcile_cpu_residency 清空 CPU 窗口外的 lod_levels 时，数据被移入
    // LodDiskWriteTask 并入队；后台 worker 线程序列化后通过 CacheManager 写入磁盘。
    // 窗口内恢复时从 CacheManager 回读并反序列化回填 lod_levels。
    //
    // 锁层次（严格顺序，禁止反向获取）：
    //   1. ResourceManager 内部锁（acquire_write<Scene> / acquire_read<Scene>）
    //   2. CacheManager::mtx_（lod_disk_cache->put / get / erase）
    //   3. lod_disk_write_mutex（队列锁）
    //
    // 淘汰回调仅做诊断日志，不得访问 ResourceManager、SharedDataHub 或任何需外部锁
    // 的资源。回调触发时对应的 Scene::lod_levels 已被 std::move 清空（RAM 已释放），
    // 磁盘副本被 LRU 淘汰后该级数据即不可恢复：GPU 侧无法再重建该级缓冲，
    // 渲染继续使用已驻留的级（通常 LOD0），直到模型重新导入。

    /// 两级 LRU 磁盘缓存（内存 128MB + 磁盘 512MB），直接使用 CacheManager
    std::unique_ptr<Corona::Cache::CacheManager> lod_disk_cache;

    // 全局默认 lod_evict_distance（reconcile 中读取，无 scene context）。
    // 初始化为与 SceneVisibilityConfig 默认值一致的计算结果：
    // (preload=50 + effective_unload=max(80, 50*1.2)=80) * 0.5 = 65.0
    // set_visibility_config / set_distance_config 调用时会同步更新。
    float lod_evict_distance = 65.0f;

    // 淘汰/恢复每帧限制
    static constexpr size_t kMaxLodSpatialEvictPerFrame  = 16;
    static constexpr size_t kMaxLodSpatialRestorePerFrame = 16;

    /// 保证 ensure_lod_disk_cache() 只初始化一次（线程安全）
    std::once_flag                                lod_disk_cache_once;

    // ---- 异步写入队列 ----

    /// 待写入队列容量上限，超出则本帧跳过 evict（数据保留在 lod_levels 下周期重试）
    static constexpr size_t kMaxPendingLodDiskWrites = 32;

    /// 写盘失败最大重试次数（仅瞬时错误如磁盘满可恢复；超大 blob 不重试）
    static constexpr int kMaxLodDiskWriteRetries = 3;

    /// 待写入任务双端队列：几何线程 push，worker 线程 pop
    std::deque<LodDiskWriteTask>    pending_lod_disk_writes;

    /// 保护 pending_lod_disk_writes 的互斥锁
    std::mutex                      lod_disk_write_mutex;

    /// 通知 worker 线程有新任务入队
    std::condition_variable         lod_disk_write_cv;

    /// worker 当前正在落盘的 key（受 lod_disk_write_mutex 保护，空 = 无在写任务）。
    /// restore 侧据此识别"任务已出队但 blob 尚未写完"的窗口，此时跳过磁盘回读，
    /// 避免命中同 key 的陈旧 blob；下个评估周期自然从磁盘命中。
    std::string                     lod_disk_write_inflight_key;

    /// 后台序列化+写盘线程（独立线程，避免阻塞几何帧循环）
    std::unique_ptr<std::thread>    lod_disk_worker;

    /// worker 线程运行标志（shutdown 时设为 false 通知退出）
    std::atomic<bool>               lod_disk_worker_running{false};

    /// 延迟初始化磁盘缓存 + 启动 worker 线程
    ///（reconcile_cpu_residency 入口、Scene 写锁之外调用；call_once 幂等）
    void ensure_lod_disk_cache();

    /// worker 线程主循环：等待任务 → 序列化 → 写盘
    void lod_disk_writer_loop();

    /// 处理单个写入任务：序列化 + 容量检查 + 写入 CacheManager
    /// @return true 写入成功；false 失败（超大项或 put 错误），调用方决定重试/丢弃
    bool write_one_lod_record(const LodDiskWriteTask& task);

    /// actor_handle → 最后一次快照时间（用于防抖）
    std::unordered_map<Payload, std::chrono::steady_clock::time_point> last_snapshot_time;

    /// actor_handle → 最近一次加载完成时间（避免新加载模型立即被不可见淘汰）
    std::unordered_map<Payload, std::chrono::steady_clock::time_point> last_load_finished_time;

    /// evict 后待释放 GPU 的 actor 集合（延迟到下一帧 update() 头部处理，
    /// 避免与 OpticsSystem 渲染线程产生 data race）
    std::unordered_set<Payload> pending_gpu_releases;

    /// 初始加载异步 import 任务：geometry_handle → (epoch, import future)。
    /// GeometrySystem 扫描 PendingImport 的 GeometryDevice，发起 import_async
    /// 并在此追踪；future 就绪后比对 epoch（防 slot 复用 ABA）、填 model_id、转 PendingBuild。
    struct PendingImportTask {
        std::uint64_t              epoch = 0;  // 与 GeometryDevice::import_epoch 比对
        std::future<std::uint64_t> future;
    };
    std::unordered_map<Payload, PendingImportTask> pending_import_tasks;

    // 完成 CPU import 后，在 worker 上异步创建 GPU mesh；几何线程只轮询并发布结果。
    struct PendingGeometryBuild {
        std::uint64_t model_id = 0;
        std::uint64_t epoch = 0;
        std::future<SceneGpuMeshData> future;
    };
    std::unordered_map<Payload, PendingGeometryBuild> pending_geometry_builds;
    tbb::task_group geometry_build_tasks;
    static constexpr std::size_t kMaxInflightGeometryBuilds = 1;
    std::uint64_t next_geometry_build_epoch = 1;

    /// import 任务 epoch 分配器（进程级单调递增，0 保留为"无任务"）。
    std::uint64_t next_import_epoch = 1;

    // ========================================
    // 资源内存预算（MB），0 = 不限制（默认）
    // ========================================
    // 注意：此前默认 512MB 会导致"巨物/大场景"每帧 used>budget → evict_until_under_budget
    // 每帧淘汰最旧资源 → 相机移动时被淘汰资源反复 reload（import+GPU 构建）→ 卡顿 +
    // LOD 缓存反复失效重建（快速 LOD 切换）。改为 0（不限制）= 关闭 B 轴资源预算淘汰，
    // 仅保留 evict_under_memory_pressure（VRAM/RAM 90% 水位安全网）。
    // 需要预算时由上层显式调用 set_resource_memory_budget_mb()。
    std::size_t resource_memory_budget_mb = 0;  // 0 = 不限制（默认）

    // ========================================
    // mesh/texture CPU 资源账本（P0：与 GPU 账本配对）
    // ========================================
    // 键 = ResourceManager rid（Scene 的 model_id 或 Image 的 texture_id），
    // 按 rid 去重（共享资源只记一次）。GPU 侧由 gpu_ledger() 单例 + RAII 令牌统计，
    // CPU 侧由本表登记 + 对 ResourceManager 存活集合对账维护。
    struct CpuResEntry {
        Corona::Memory::ResKind kind = Corona::Memory::ResKind::Mesh;
        std::size_t             bytes = 0;
    };
    mutable std::mutex                              cpu_ledger_mutex;
    std::unordered_map<std::uint64_t, CpuResEntry>  cpu_ledger;

    /// VRAM 预算（字节），0 = 不限制（默认）。P0 仅用于 over/need_free 计算，不淘汰。
    std::size_t vram_budget_bytes = 0;

    // ========================================
    // LOD 级 LRU — 延迟 Cap 机制
    // ========================================
    // 在 reconcile_lod_residency 末尾根据 VRAM 压力计算：对远处 geometry 的 demand
    // 施加下限（cap），迫使其选更粗 LOD，释放显存。cap 在下一帧 reconcile 中生效，
    // 一帧延迟有意为之——VRAM 压力变化缓慢（数百帧尺度），无需拆分主循环。
    //
    // Key = lod_key (make_lod_key 生成), Value = min_allowed_demand（下限；
    // demand 只能 ≥ 此值，即只能比此更粗）。空 map 表示无压力、无限制。
    std::unordered_map<uint64_t, int> lod_budget_caps;

    // 水位常量：soft(75%) 触发降级，hard(85%) 开启加速模式（每步 +2 级）。
    // 两个比例的分母均为 compute_memory_report().vram.budget_bytes（已做
    // min(物理VRAM, vram_budget_bytes) 封顶），不是裸 vram_budget_bytes。
    static constexpr float kLodBudgetSoftRatio = 0.75f;
    static constexpr float kLodBudgetHardRatio = 0.85f;

    // 单帧最多降级 mesh 数：防止 VRAM 突然承压时全场景同步 pop，
    // 将降级分散到多帧平滑过渡。
    static constexpr std::size_t kMaxDegradedPerFrame = 64;

    // 诊断计数器（在 update() ~1Hz 块重置并输出）
    std::uint64_t diag_lod_budget_checks   = 0;  // enforce_lod_budget 调用次数
    std::uint64_t diag_lod_budget_degraded = 0;  // 本秒内被强制降级的 entry 数
    std::uint64_t diag_lod_budget_entries  = 0;  // 本秒内收集的 candidate entries 数
    std::uint64_t diag_lod_budget_est_vram = 0;  // 估算的 LOD mesh 总 VRAM（字节）

    // ========================================
    // 满载淘汰水位（容量取自 SDL 系统内存 / Horizon 显存，非固定 MB）
    // ========================================
    float    evict_high_ratio = 0.90f;  // used ≥ high*capacity 时触发淘汰
    float    evict_low_ratio  = 0.80f;  // 淘汰目标：降到 low*capacity
    int      pressure_eval_interval = 15;  // 每隔多少帧评估一次压力（避免每帧查 VMA/系统）
    int      pressure_eval_counter  = 0;

    // ========================================
    // CPU LOD 驻留窗口（RAM 3层管理）
    // ========================================
    // 每 model_id 维护固定窗口 {lod_levels[0], lod_levels[demand_idx], lod_levels[coarsest_idx]}：
    //   finest_idx    = 0（lod_levels[0] = 最精细简化级，始终保留）
    //   demand_idx    = 所有实例 committed_demand 的中位数 - 1（对应 lod_levels[] 下标）
    //   coarsest_idx  = lod_levels.size()-1（最粗级，始终保留）
    // LOD0（MeshData::vertices/indices）永远保留，不在此管理。
    // 窗口外的 LODLevel::vertices/indices/bone_weights 被清空以节省 RAM。
    struct ModelCpuWindow {
        int finest_idx   = 0;  // 始终为 0
        int demand_idx   = 0;  // 中位数对应 lod_levels[] 下标（= median_committed_demand - 1，≥0）
        int coarsest_idx = 0;  // lod_levels.size()-1
    };
    mutable std::mutex cpu_window_mutex;
    std::unordered_map<uint64_t /*model_id*/, ModelCpuWindow> model_cpu_windows;

    // CPU 协调帧间隔计数器（避免每帧 acquire_write<Scene>，每2秒@60fps 评估一次）
    int cpu_window_eval_counter = 0;
    static constexpr int kCpuWindowEvalInterval = 120;

    [[nodiscard]] static uint64_t make_lod_key(std::uintptr_t geometry_handle,
                                               uint32_t       mesh_index) {
        return (static_cast<uint64_t>(geometry_handle) << 32) | mesh_index;
    }

    SceneState& get_or_create(std::uintptr_t scene) {
        auto [it, inserted] = scenes.try_emplace(scene);
        return it->second;
    }
};

}  // namespace Corona::Systems

