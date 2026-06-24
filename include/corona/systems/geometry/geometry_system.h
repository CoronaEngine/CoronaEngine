#pragma once

#include <horizon.h>

#include <corona/events/geometry_system_events.h>
#include <corona/events/scene_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>
#include <corona/math/frustum.h>
#include <corona/spatial/aabb.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace Corona::Systems {

/**
 * @brief 物体加载状态枚举
 */
enum class ActorLoadState : uint8_t {
    Loaded,     // 已加载，可正常渲染和物理模拟
    Loading,    // 正在异步加载中
    Unloading,  // 正在异步卸载中
    Unloaded    // 已卸载，数据不在内存中
};

/**
 * @brief 单场景的可见性策略
 */
struct SceneVisibilityConfig {
    /// 连续不可见超过该帧数时，触发 ActorEvictRequestedEvent。
    /// 0 表示永不 evict（默认，避免在 LRU 接入前误触）。
    int  invisible_frames_to_evict = 0;
    bool collect_stats             = true;

    bool enable_distance_culling  = false; // 是否启用距离卸载
    float unload_distance         = 0.0f; // 超过此距离且不可见时触发卸载
    float preload_distance        = 0.0f; // 进入此距离时触发预加载
};

/**
 * @brief 单场景统计信息（供 UI / 日志读取）
 */
struct SceneStats {
    std::size_t actor_total       = 0;
    std::size_t actor_visible     = 0;  // 上一帧所有相机视锥的并集
    std::size_t actor_offline     = 0;  // 已被 LRU 卸载（M3 起）
    std::size_t octree_entries    = 0;
    double      last_rebuild_ms   = 0.0;
    double      last_query_ms     = 0.0;

    //距离卸载统计
    std::size_t actor_loaded      = 0;
    std::size_t actor_loading     = 0;
    std::size_t actor_unloading   = 0;
    std::size_t actor_unloaded    = 0;
};

// ========================================
// 动态减面 (Mesh Simplification) 相关类型
// ========================================
//
// 网格简化由资源导入管线完成（使用 meshoptimizer 库，参见
// modules/corona_resource 中的 parse_common.h）。
// 导入时已生成多级 LOD 数据（MeshData::lod_levels），
// 本系统负责：
//   1. 将导入时生成的 CPU 端 LOD 数据上传为 GPU 缓冲（upload_lod_from_scene_data）
//   2. 提供线程安全的 LOD 查询接口供渲染线程使用
//   3. 根据屏幕占比自动选择合适的 LOD 级别
//
// 数据流向：
//   导入时 meshoptimizer → MeshData::lod_levels [CPU]
//   → upload_lod_from_scene_data() → LODMeshBuffers [GPU] → 存入 lod_cache
//   → 渲染时查询 get_lod_buffers() → 替换原始缓冲 → 提交 GPU 绘制

/**
 * @brief 单个 LOD 级别的 GPU 缓冲集合
 *
 */
struct LODMeshBuffers {
    Horizon::HardwareBuffer vertex_buffer;    // GPU 顶点缓冲（Vertex Shader 读取）
    Horizon::HardwareBuffer index_buffer;     // GPU 索引缓冲（组装三角形）
    Horizon::HardwareBuffer vertex_storage;   // GPU 顶点 StorageBuffer（Compute Shader 用）
    Horizon::HardwareBuffer index_storage;    // GPU 索引 StorageBuffer（Compute Shader 用）
    float  error            = 0.0f;  // 该级别的几何误差（QEM 计算得出，用于调试）
    float  screen_threshold = 1.0f;  // 屏幕占比阈值：低于此值时切换到此级别
    bool   ready            = false; // GPU 缓冲是否已创建完毕（创建前不能用于渲染）
};

/**
 * @brief 动态减面全局配置
 *
 * 控制 LOD 系统的行为。通过 set_simplification_config() 设置。
 * 注意：具体的简化参数（target_ratios、max_errors 等）由导入时的
 * LODGenerationOptions 控制（参见 corona/resource/types/scene.h）。
 */
struct MeshSimplificationConfig {
    bool enabled      = false;  // 总开关：false 时整个 LOD 系统不工作
    int  max_lod_levels = 4;    // 最大 LOD 级别数（含 LOD 0 原始精度）
    bool auto_on_load = false;  // 模型加载后是否自动将导入时的 LOD 数据上传 GPU
};

/**
 * @brief 几何系统 (Geometry System)
 *
 * 负责几何数据管理、空间变换、包围盒计算，并承载场景八叉树空间索引服务
 * （原 SceneSystem 职责已并入此处）：
 * - 每帧重建场景八叉树；
 * - 提供线程安全的 AABB / 球 / 视锥 / 碰撞对查询；
 * - 维护 Actor 加载状态机与距离预加载/卸载；
 * - 维护 actor 可见性热度并发出 LRU evict/restore 事件。
 * - 管理运行时 LOD 切换（基于导入时 meshoptimizer 生成的简化数据）。
 *
 * 运行在独立线程，以 60 FPS 更新几何状态。
 *
 * 优先级 85：晚于 transform 写入者，早于 MechanicsSystem(75)，确保物理宽相
 * query_pairs() 在同帧读取到已重建的八叉树。
 */
class GeometrySystem : public Kernel::SystemBase {
   public:
    GeometrySystem();
    ~GeometrySystem() override;

    // ========================================
    // ISystem 接口实现
    // ========================================

    std::string_view get_name() const override {
        return "Geometry";
    }

    int get_priority() const override {
        return 85;  // 高优先级，早于 MechanicsSystem(75)，保证八叉树同帧就绪
    }

    /**
     * @brief 初始化几何系统
     * @param ctx 系统上下文
     * @return 初始化成功返回 true
     */
    bool initialize(Kernel::ISystemContext* ctx) override;

    /**
     * @brief 每帧更新几何
     *
     * 在独立线程中调用，更新几何变换、重建八叉树并维护加载状态。
     */
    void update() override;

    /**
     * @brief 关闭几何系统
     *
     * 清理所有几何资源与异步任务。
     */
    void shutdown() override;

    // ========================================
    // 配置
    // ========================================
    void set_visibility_config(std::uintptr_t scene, SceneVisibilityConfig cfg);

    /// 距离卸载配置接口
    void set_distance_config(std::uintptr_t scene, float unload_dist, float preload_dist, bool enable = true);

    // ========================================
    // 空间查询（线程安全）
    // ========================================
    [[nodiscard]] std::vector<std::uintptr_t> query_aabb(
        std::uintptr_t scene, const Spatial::AABB& box) const;

    [[nodiscard]] std::vector<std::uintptr_t> query_sphere(
        std::uintptr_t scene, const ktm::fvec3& center, float radius) const;

    [[nodiscard]] std::vector<std::uintptr_t> query_frustum(
        std::uintptr_t scene, const Math::Frustum& frustum) const;

    /// 物理宽相用：返回 (handle_a, handle_b)，a < b。
    [[nodiscard]] std::vector<std::pair<std::uintptr_t, std::uintptr_t>> query_pairs(
        std::uintptr_t scene) const;

    /// 便捷：内部从 CameraDevice 构造 frustum 后查询
    [[nodiscard]] std::vector<std::uintptr_t> query_visible_for_camera(
        std::uintptr_t scene, std::uintptr_t camera) const;

    // ========================================
    // LRU 协作（M3 起启用，当前为占位）
    // ========================================
    [[nodiscard]] bool is_actor_offline(std::uintptr_t actor) const;
    void               mark_actor_restored(std::uintptr_t actor);

    /// 加载状态查询接口
    [[nodiscard]] ActorLoadState get_actor_load_state(std::uintptr_t actor, std::uintptr_t scene) const;

    // ========================================
    // LOD 工具
    // ========================================
    /// 计算物体包围球在屏幕上的占比（0~1）
    static float compute_screen_ratio(const ktm::fvec3& camera_pos,
                                      float              camera_fov_deg,
                                      const ktm::fvec3& world_center,
                                      float              bounding_radius);

    /// 根据屏幕占比选择 LOD 等级（0 = 原始网格）
    static int select_lod_level(float                     screen_ratio,
                                const std::vector<float>& thresholds);

    // ========================================
    // 动态减面 (Mesh Simplification) API
    // ========================================
    //
    // 以下方法构成了 LOD 系统的对外接口。使用流程：
    //
    //   【初始化阶段】
    //   set_simplification_config(cfg)  ← 配置 LOD 开关和最大级别数
    //
    //   【自动上传】
    //   模型导入时 meshoptimizer 已生成了 LOD 数据（存在 MeshData::lod_levels）。
    //   引擎会在 update() 中自动调用 upload_lod_from_scene_data() 将其上传 GPU。
    //   无需手动调用任何方法。
    //
    //   【渲染时查询】
    //   get_lod_buffers(geom, mesh_idx, lod) ← 获取指定级别的 GPU 缓冲句柄
    //   resolve_lod_level(geom, mesh_idx, ...) ← 根据屏幕占比自动选择 LOD 级别

    /// 设置减面全局配置
    /// 修改会立即生效（已有缓存不受影响）
    void set_simplification_config(const MeshSimplificationConfig& cfg);

    /// 获取当前减面配置（只读）
    [[nodiscard]] const MeshSimplificationConfig& get_simplification_config() const;

    /// 查询指定 LOD 级别的 GPU 缓冲（渲染线程调用，线程安全）
    ///
    /// @param geometry_handle GeometryDevice 句柄
    /// @param mesh_index      子网格索引
    /// @param lod_level       LOD 级别（0=原始精度，1..N=各级简化）
    /// @return 指向 LODMeshBuffers 的指针，或 nullptr 表示该级别不存在
    ///
    /// 降级策略：如果请求的级别尚未就绪（ready=false），自动返回 LOD 0。
    /// 调用者无需处理未就绪的情况。
    [[nodiscard]] const LODMeshBuffers* get_lod_buffers(
        std::uintptr_t geometry_handle,
        uint32_t       mesh_index,
        int            lod_level) const;

    /// 查询某个 mesh 已就绪的 LOD 级别数
    /// @return 0 表示该 mesh 还未上传任何 LOD 数据
    [[nodiscard]] int get_lod_count(std::uintptr_t geometry_handle,
                                    uint32_t       mesh_index) const;

    /// 一站式 LOD 级别选择：给定屏幕占比，返回应使用的 LOD 级别
    ///
    /// 内部流程：
    ///   1. 从 lod_cache 获取该 mesh 的各 LOD 级别阈值
    ///   2. 调用 select_lod_level(screen_ratio, thresholds) 选择级别
    ///   3. 如果选中的级别未就绪，自动降级到最近的已就绪级别
    ///
    /// @param geometry_handle GeometryDevice 句柄
    /// @param mesh_index      子网格索引
    /// @param screen_ratio    物体在屏幕上的占比（0~1），由 compute_screen_ratio() 算得
    /// @return 应使用的 LOD 级别（0=原始，1..N=各级简化）
    [[nodiscard]] int resolve_lod_level(std::uintptr_t geometry_handle,
                                        uint32_t       mesh_index,
                                        float          screen_ratio) const;

    /// 一站式 LOD 缓冲获取：自动选级 + 返回 GPU 缓冲（渲染线程调用，单次加锁）
    ///
    /// 等价于 resolve_lod_level() + get_lod_buffers()，但只获取一次锁。
    /// 渲染热路径上应优先使用此方法。
    ///
    /// @return 指向 LODMeshBuffers 的指针，或 nullptr 表示该 mesh 无 LOD 数据
    [[nodiscard]] const LODMeshBuffers* resolve_lod_buffers(
        std::uintptr_t geometry_handle,
        uint32_t       mesh_index,
        float          screen_ratio) const;

    // ========================================
    // 统计
    // ========================================
    [[nodiscard]] SceneStats stats(std::uintptr_t scene) const;

   private:
    void on_load_completed(const Events::ActorLoadCompletedEvent& event);
    void on_unload_completed(const Events::ActorUnloadCompletedEvent& event);
    void on_load_requested(const Events::ActorLoadRequestedEvent& event);
    void on_unload_requested(const Events::ActorUnloadRequestedEvent& event);
    void process_async_tasks();  // 处理完成的异步资源任务

    /// 卸载完成时释放 actor 关联的 GPU 资源（HardwareBuffer / HardwareImage），
    /// 并清理对应的 LOD 缓存条目。不释放 SharedDataHub 存储槽位本身——
    /// 槽位归 Python API 层 Geometry 对象所有，由其析构函数回收。
    void release_actor_gpu_resources(std::uintptr_t actor);

    /// 重新加载完成时重建 actor 关联的 GPU 资源（HardwareBuffer / HardwareImage），
    /// 从已导入的 Scene 资源中重新创建 mesh_handles 并恢复 model_resource_handle。
    /// 同时清理 LOD 缓存以保证下一帧 update() 重新上传 LOD 数据。
    void rebuild_actor_gpu_resources(std::uintptr_t actor, std::uint64_t rid);

    // ========================================
    // 动态减面内部管线（在 update() 中每帧调用，外部不可见）
    // ========================================
    //
    // 模型导入时 meshoptimizer 已生成 LOD 数据（MeshData::lod_levels），
    // 这里只负责将其上传为 GPU 缓冲。

    /// 遍历所有已加载的 Scene 资源，
    /// 将其 MeshData::lod_levels（导入时 meshoptimizer 生成的LOD数据）上传到 GPU。
    /// 每帧调用但只对新模型生效（已有缓存的跳过）。
    void upload_lod_from_scene_data();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Systems
