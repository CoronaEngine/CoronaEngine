#pragma once

#include <horizon.h>

#include <corona/events/geometry_system_events.h>
#include <corona/events/scene_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>
#include <corona/math/frustum.h>
#include <corona/memory/gpu_mem_ledger.h>
#include <corona/spatial/aabb.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
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
    bool collect_stats             = true;

    bool enable_distance_culling  = true;   // 是否启用距离剔除
    float unload_distance         = 80.0f;  // 超过此距离且不可见时触发淘汰
    float preload_distance        = 50.0f;  // 进入此距离时触发预加载

    // 八叉树距离驱动 LOD 级空间淘汰
    bool  enable_lod_spatial_eviction = false;  // 默认关闭（需足够大的 preload→unload gap）
    float lod_evict_distance = 0.0f;             // 0 = 自动: (preload+effective_unload)/2
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
// 资源内存账本（P0：mesh / texture 的 CPU + GPU 计量）
// ========================================

/**
 * @brief 单个内存池（VRAM 或 RAM）的用量 + 预算视图
 *
 * 只"计量与计算预算"，不触发淘汰。need_free_bytes 供后续淘汰步骤消费。
 */
struct MemoryPoolReport {
    std::size_t mesh_bytes      = 0;  ///< mesh 占用
    std::size_t texture_bytes   = 0;  ///< texture 占用
    std::size_t used_bytes      = 0;  ///< mesh + texture
    std::size_t budget_bytes    = 0;  ///< 预算上限，0 = 不限制
    std::size_t high_bytes      = 0;  ///< 高水位（触发淘汰阈值）
    std::size_t low_bytes       = 0;  ///< 低水位（淘汰目标）
    std::size_t over_bytes      = 0;  ///< max(0, used - high)
    std::size_t need_free_bytes = 0;  ///< used > high ? used - low : 0（降到低水位需释放量）
    bool        pressured       = false;  ///< over_bytes > 0
};

/**
 * @brief mesh/texture 的 CPU(RAM) 与 GPU(VRAM) 内存账本快照
 */
struct MemoryReport {
    MemoryPoolReport vram;  ///< GPU：mesh/texture 缓冲（geometry 自有，精确）
    MemoryPoolReport ram;   ///< CPU：Scene/Image 资源（按 rid 去重，对账存活）
    std::size_t      vram_mesh_peak    = 0;
    std::size_t      vram_texture_peak = 0;
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
    float  screen_threshold = 1.0f;  // 屏幕占比阈值：低于此值时切换到此级别（legacy 回退用）
    // 模型空间几何误差（meshopt result_error × simplifyScale）。运行时 × actor_scale 得
    // 世界单位误差，再除以相机到 mesh 世界 AABB 最近点距离得角误差，与相机角预算 ε 比较选级。
    // LOD0 恒为 0（无简化误差）。这是 screen-space-error 选级的核心量，取代 screen_threshold。
    float  geometric_error  = 0.0f;
    bool   ready            = false; // GPU 缓冲是否已创建完毕（创建前不能用于渲染）
    std::uint32_t vertex_count = 0;  // 该级别顶点数（调试/诊断用）
    std::uint32_t index_count  = 0;  // 该级别索引数（调试/诊断用）
    std::uint32_t max_index    = 0;  // 该级别最大索引（调试/诊断用）

    // 按需驻留（Step 3a）：本级在 Scene::data.meshes[mesh].lod_levels 中的源下标。
    // upload 会跳过空 LOD 级，故缓存级序与源级序不一一对应；reconcile 重建某级时
    // 用此下标回到 Scene CPU 取对应顶点/索引数据。LOD0 恒为 -1（其源是 mesh 本体）。
    int source_lod_index = -1;

    // GPU 显存记账令牌（P0）：LOD1..N 各自的顶点/索引缓冲字节。
    // LOD0 复用 mesh_dev 的缓冲（非新分配）→ 该令牌留空计 0，避免重复计量。
    Corona::Memory::GpuMemToken mesh_mem;

    // 释放冷却（方案 A）：本级最后一次被 reconcile 判定为"需求级 D"的帧号。
    // reconcile 每帧给当前 D 级刷新此值；其余已就绪级仅在连续 idle 帧数超过
    // kLodReleaseCooldownFrames 后才释放。这样物体停在 LOD 阈值边界微动时，
    // D 在相邻级反复横跳不会每帧重建/释放 GPU 缓冲（消除 GPU churn）。
    // 0 = 从未被需求（LOD0 恒驻，不参与冷却释放）。
    std::uint64_t last_demand_frame = 0;
};

// LOD（动态减面）现由 GeometrySystem 内部自行决策，无外部配置面：
// - 是否生成：由导入层 LODGenerationOptions 决定（见 corona/resource/types/scene.h）
// - 是否上传 GPU / 选哪一级：每帧在 update() 中自动完成
// 最大级别数等内部参数见 geometry_system.cpp 中的 kMaxLodLevels 常量。

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
    // LRU 协作（M3 生产化）
    // ========================================
    //
    // ActorEvictRequestedEvent 发布后，GeometrySystem 自动：
    //   1. 创建 ActorStreamingRecord（scene/actor + model_path + transform + handles + flags）
    //   2. 存入 ActorCache（两级 LRU：内存 64MB + 磁盘 256MB）
    //   3. 标记 actor 为 offline，状态置为 Unloaded
    //
    // ActorRestoreRequestedEvent 发布后，GeometrySystem 自动：
    //   1. 从 ActorCache 获取 ActorStreamingRecord（或回退到磁盘 model_path）
    //   2. 调用 ResourceManager::import_async 重新导入
    //   3. 导入完成后重建 GPU 资源，标记为 online
    //
    // 磁盘目录默认：{cwd}/cache/actors/，可通过 set_cache_directory() 修改

    /// 设置 LRU ActorCache 磁盘目录（需在首次 evict 前调用）
    void set_cache_directory(std::filesystem::path dir);

    /// 设置资源内存预算（MB），0 = 不限制。
    /// 当 ResourceManager 估算内存用量超过预算时，GeometrySystem 每帧末尾
    /// 触发 evict_until_under_budget，优先淘汰最久未访问（cold）的资源。
    void set_resource_memory_budget_mb(std::size_t mb);

    /// 设置 GPU 显存（VRAM）预算（MB），0 = 不限制（默认）。
    /// P0 仅用于计算 over/need_free 报告，不触发淘汰。
    void set_vram_budget_mb(std::size_t mb);

    /// mesh/texture 的 CPU(RAM) + GPU(VRAM) 内存账本快照（线程安全）。
    [[nodiscard]] MemoryReport memory_report() const;

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

    /// 根据屏幕占比选择 LOD 等级（0 = 原始网格）。
    /// 旧屏占比路径：仍保留供无几何误差数据的资源 fallback。
    static int select_lod_level(float                     screen_ratio,
                                const std::vector<float>& thresholds);

    // ---- 屏幕空间误差选级（统一相机/GI 的球形角预算模型）----
    //
    // 核心恒等：屏幕像素误差 = 角误差 × 焦距(px)。投影/FOV/分辨率全部塌缩进单个
    // 标量角预算 epsilon = pixel_budget · 2·tan(fov/2) / height_px。于是选级判据
    //   world_error / d ≤ epsilon
    // 是纯球形量（只看距离、与方向无关）→ 相机背后物体（GI 需要）同样有定义。
    //
    // d 取「相机到 mesh 世界 AABB 最近点」的距离：保留各向异性（扁平/细长物体不被
    // 外接球高估），且不受轴心(pivot)偏移影响（消除环绕跳级）。相机进入 AABB → d→0
    // → 角误差→∞ → 强制最高精度 LOD0，天然正确，无需 d=max(d,r) 补丁。

    /// 相机到一个世界空间 AABB 的最近点欧氏距离（点在盒内时为 0）。
    static float distance_point_to_aabb(const ktm::fvec3& p,
                                        const ktm::fvec3& aabb_min,
                                        const ktm::fvec3& aabb_max);

    /// 相机角预算 epsilon：把像素预算换算成「每弧度多少世界误差可接受」的角阈值。
    /// height_px 为相机渲染高度（像素），fov_deg 为垂直 FOV（度）。
    static float compute_angular_epsilon(float pixel_budget,
                                         float fov_deg,
                                         float height_px);

    /// 根据 VRAM 占比动态缩放像素预算：压力越大 → 预算越宽松 → 倾向粗 LOD → 显存自然回落。
    /// @param vram_ratio  used_bytes / budget_bytes，范围 [0,1]；budget=0 时返回默认值。
    /// @return 缩放后的像素预算（≥ 默认值 1.5px）。
    static float compute_pixel_budget_from_pressure(float vram_ratio);

    /// 屏幕空间误差选级：给定到 mesh 最近点距离 d、各级世界误差（world_errors[i] =
    /// geometric_error[i]·actor_scale，下标与 levels 对齐，level 0 误差为 0），以及相机角
    /// 预算 epsilon，返回「角误差仍 ≤ epsilon 的最粗一级」。0 = LOD0（最高精度）。
    static int select_lod_by_error(float                     distance_to_aabb,
                                   const std::vector<float>& world_errors,
                                   float                     epsilon);

    // ========================================
    // 动态减面 (Mesh Simplification) API
    // ========================================
    //
    // LOD 由 GeometrySystem 内部自动管理，无对外配置开关：
    //
    //   【自动上传】
    //   模型导入时 meshoptimizer 已生成了 LOD 数据（存在 MeshData::lod_levels）。
    //   引擎在 update() 中每帧调用 upload_lod_from_scene_data() 将其上传 GPU，
    //   无 LOD 数据的 mesh 自动跳过。无需任何外部调用。
    //
    //   【渲染时查询】
    //   渲染线程调用 select_render_buffers()，由 GeometrySystem 内部完成
    //   屏幕占比计算 + LOD 选级 + 降级兜底，直接返回可用的渲染缓冲。

    /// 渲染用的一组 GPU 缓冲（顶点/索引 + 对应 StorageBuffer）
    /// select_render_buffers 的入参（fallback）与返回值均为此类型。
    struct RenderMeshBuffers {
        Horizon::HardwareBuffer vertex;
        Horizon::HardwareBuffer index;
        Horizon::HardwareBuffer vertex_storage;
        Horizon::HardwareBuffer index_storage;
        std::uint32_t vertex_count = 0;
        std::uint32_t index_count = 0;
        std::uint32_t max_index = 0;
    };

    /// 单个 mesh 的当前 GPU 状态快照（LOD 已由 GeometrySystem 内部解析）。
    ///
    /// 所有句柄均为引用计数值副本，调用方在 query_mesh_slots 返回后无需持有任何锁。
    /// 适用于所有需要访问 mesh GPU 资源的系统（光学、力学、Vision 等）。
    ///
    /// valid=false 仅在 Actor 首次加载尚未完成时成立（mesh_handles 为空），
    /// 是唯一合法的跳过原因。对于所有已加载的 Actor，valid=true 且
    /// geo.vertex / geo.index 一定非空。
    struct MeshSlot {
        uint32_t               mesh_index    = 0;
        uint32_t               instance_index = 0;
        uint32_t               object_id = 0;
        ktm::fmat4x4           instance_transform{ktm::fmat4x4::from_eye()};
        RenderMeshBuffers      geo;              ///< LOD 解析后的几何缓冲
        Horizon::HardwareImage texture;          ///< 贴图句柄（null = 无贴图）
        std::array<float, 4>   material_color = {1.f, 1.f, 1.f, 1.f}; ///< 材质颜色 RGBA
        bool                   valid = false;    ///< false = 首次加载中，唯一合法的跳过原因
        bool                   texture_ready = false;
        std::uint32_t          vertex_count = 0;
        std::uint32_t          index_count = 0;
        std::uint32_t          max_index = 0;
    };

    struct ShadowLodQuery {
        bool enabled = false;
        float world_units_per_texel = 0.0f;
    };

    /// Minimal immutable shadow-pass snapshot. Shadow rendering does not sample
    /// the material texture, so keeping those handles out of the hot path avoids
    /// four cascades worth of unnecessary ref-counted copies.
    struct ShadowMeshSlot {
        uint32_t          mesh_index = 0;
        uint32_t          instance_index = 0;
        uint32_t          object_id = 0;
        ktm::fmat4x4      instance_transform{ktm::fmat4x4::from_eye()};
        RenderMeshBuffers geo;
        bool              valid = false;
        std::uint32_t     vertex_count = 0;
        std::uint32_t     index_count = 0;
        std::uint32_t     max_index = 0;
    };

    /// 查询一个 geometry 的所有 MeshSlot（常驻路由，无相机上下文）。
    ///
    /// 适用：V-buffer 收集、Vision BVH 构建、MechanicsSystem 蒙皮、无相机的通用遍历。
    /// 内部取 geometry 写锁（texture storeDescriptor 需非 const 访问）；
    /// 调用方不得持有同一 geometry_handle 的锁。
    /// @return 每个 mesh_index 对应一个 MeshSlot；geometry 无效或首次加载中时返回空 vector。
    [[nodiscard]] std::vector<MeshSlot> query_mesh_slots(
        std::uintptr_t geometry_handle) const;

    /// 查询一个 geometry 的所有 MeshSlot（LOD 选级路由，带相机上下文）。
    ///
    /// 适用：主渲染 pass、阴影 pass、需要 committed_demand 精度的所有路径。
    [[nodiscard]] std::vector<MeshSlot> query_mesh_slots(
        std::uintptr_t    geometry_handle,
        const ktm::fvec3& camera_pos,
        float             camera_fov_deg,
        const ktm::fvec3& world_center,
        float             bounding_radius,
        const ktm::fmat4x4* actor_transform = nullptr) const;

    /// Per-instance LOD selection: uses actor × instance transform to derive the
    /// instance world AABB and chooses the coarsest ready level within the
    /// screen-space error budget.
    [[nodiscard]] RenderMeshBuffers select_render_buffers_for_instance(
        std::uintptr_t geometry_handle,
        uint32_t mesh_index,
        const ktm::fvec3& camera_pos,
        float camera_fov_deg,
        const ktm::fmat4x4& actor_transform,
        const ktm::fmat4x4& instance_transform,
        const RenderMeshBuffers& fallback) const;

    /// Shadow-pass routing: selects the coarsest ready level whose geometric
    /// error fits within one shadow texel, never falling back to a coarser
    /// level than the requested target.
    [[nodiscard]] std::vector<MeshSlot> query_shadow_mesh_slots(
        std::uintptr_t geometry_handle,
        float world_units_per_texel,
        float max_abs_scale,
        std::uint64_t frame = 0) const;

    /// Batch shadow routing for all cascades. Acquires Geometry Storage and the
    /// LOD cache once, aggregates demand once per mesh, and returns one slot
    /// vector per input cascade. Disabled cascades return an empty vector.
    [[nodiscard]] std::vector<std::vector<ShadowMeshSlot>> query_shadow_mesh_slots_batch(
        std::uintptr_t geometry_handle,
        std::span<const ShadowLodQuery> cascades,
        float max_abs_scale,
        std::uint64_t frame = 0) const;

    void request_shadow_lod(std::uintptr_t geometry_handle, uint32_t mesh_index,
                            float world_units_per_texel, float max_abs_scale,
                            std::uint64_t frame) const;

    /// 一站式渲染缓冲选择（渲染线程调用，线程安全）。
    ///
    /// 内部流程：compute_screen_ratio() → 选 LOD 级别 → 降级到已就绪级别，
    /// 命中则返回该级 LOD 缓冲；无 LOD 数据 / 未就绪 / 缓冲无效时，原样返回
    /// fallback。**保证返回值始终可直接用于渲染**，调用方无需判空或降级。
    ///
    /// @param geometry_handle GeometryDevice 句柄
    /// @param mesh_index      子网格索引
    /// @param camera_pos      相机世界坐标
    /// @param camera_fov_deg  相机垂直 FOV（度）
    /// @param world_center    物体包围球世界中心
    /// @param bounding_radius 物体包围球半径
    /// @param fallback        无 LOD 时使用的原始缓冲（通常为 mesh 的 LOD0）
    [[nodiscard]] RenderMeshBuffers select_render_buffers(
        std::uintptr_t          geometry_handle,
        uint32_t                mesh_index,
        const ktm::fvec3&       camera_pos,
        float                   camera_fov_deg,
        const ktm::fvec3&       world_center,
        float                   bounding_radius,
        const RenderMeshBuffers& fallback) const;

    [[nodiscard]] RenderMeshBuffers select_shadow_render_buffers(
        std::uintptr_t geometry_handle,
        uint32_t mesh_index,
        float world_units_per_texel,
        float max_abs_scale,
        const RenderMeshBuffers& fallback) const;

    /// 驻留路由（渲染线程调用，线程安全，**不做屏幕占比选级**）。
    ///
    /// 与 select_render_buffers 的区别：本方法不需要相机参数，不按屏幕占比选级，
    /// 只负责"返回当前最高精度的**已常驻**级缓冲"。用于无相机上下文的渲染路径
    /// （如 V-buffer 可见性收集 collect_actor_instances_for_visibility、actor 拾取），
    /// 这些路径此前直接读 MeshDevice 缓冲、不接 LOD。
    ///
    /// 设计目的：让"几何缓冲读取统一经 GeometrySystem 路由"，从而支持后续逐级
    /// LOD 淘汰——当 LOD0 被显存压力释放后，本方法自动改返回次高的已常驻级，
    /// 渲染路径无需感知。
    ///
    /// 行为：
    ///   - 无 LOD 缓存条目（如 from_image 程序化几何）→ 原样返回 fallback。
    ///   - 有缓存：从 LOD0 向高级别扫描，返回首个 ready 且缓冲有效的级别。
    ///     （今天 LOD0 恒常驻 → 返回 LOD0 = fallback，行为与改造前完全一致。）
    ///   - 全级皆不常驻 → 返回 fallback（其缓冲可能为空，调用方据空判断跳过）。
    ///
    /// StorageBuffer 缺失时沿用 fallback 的，避免 compute 路径拿到空句柄。
    ///
    /// @param geometry_handle GeometryDevice 句柄
    /// @param mesh_index      子网格索引
    /// @param fallback        调用方持 geom 槽锁时从 MeshDevice 读出的 LOD0 候选缓冲
    [[nodiscard]] RenderMeshBuffers resident_render_buffers(
        std::uintptr_t           geometry_handle,
        uint32_t                 mesh_index,
        const RenderMeshBuffers& fallback) const;

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

    /// 一站式 LOD 缓冲获取：自动选级 + 返回 GPU 缓冲（单次加锁）
    ///
    /// @deprecated 渲染热路径请改用 select_render_buffers()。本方法返回裸
    /// const LODMeshBuffers*（指向 lod_cache 内部），调用方在 shared_lock 释放后
    /// 解引用；一旦逐级 LOD 淘汰使缓存频繁增删，该指针会悬垂。select_render_buffers
    /// 在持锁期间即拷出 HardwareBuffer 句柄（值语义、refcount 安全），无此问题。
    /// 保留此接口仅为兼容潜在外部调用，渲染路径已不再使用。
    ///
    /// @return 指向 LODMeshBuffers 的指针，或 nullptr 表示该 mesh 无 LOD 数据
    [[nodiscard]] const LODMeshBuffers* resolve_lod_buffers(
        std::uintptr_t geometry_handle,
        uint32_t       mesh_index,
        float          screen_ratio) const;

    /// 蒙皮专用：一次性拷出某 mesh 所有 LOD 级别的 (vertex, vertex_storage) 句柄对。
    ///
    /// 供 MechanicsSystem 在物理线程把蒙皮后顶点 write_bytes 回所有 LOD 级别的 GPU
    /// 缓冲。下标 0 = LOD0（= MeshDevice 缓冲），1..N = 各级简化。单次 shared_lock
    /// 拷贝；HardwareBuffer 为引用计数句柄，拷出后即便 Geometry 线程后续 evict 缓存，
    /// 调用方持有的拷贝仍保活底层 buffer。无 LOD 缓存条目时返回空 vector（调用方回退
    /// 到只写 LOD0，即 GeometryDevice.mesh_handles 里的缓冲）。
    ///
    /// @param geometry_handle GeometryDevice 句柄
    /// @param mesh_index      子网格索引
    /// @return 各 LOD 级别的 (vertexBuffer, vertexStorageBuffer) 句柄对，含 LOD0
    [[nodiscard]] std::vector<std::pair<Horizon::HardwareBuffer, Horizon::HardwareBuffer>>
    get_skinning_targets(std::uintptr_t geometry_handle, uint32_t mesh_index) const;

    // ========================================
    // 统计
    // ========================================
    [[nodiscard]] SceneStats stats(std::uintptr_t scene) const;

   private:
    void on_load_finished(const Events::ActorLoadFinishedEvent& event);
    void on_unload_finished(const Events::ActorUnloadFinishedEvent& event);
    void on_load_requested(const Events::ActorLoadRequestedEvent& event);
    void on_unload_requested(const Events::ActorUnloadRequestedEvent& event);
    void on_evict_requested(const Events::ActorEvictRequestedEvent& event);
    void on_restore_requested(const Events::ActorRestoreRequestedEvent& event);
    void process_async_tasks();  // 处理完成的异步资源任务

    /// 扫描 PendingImport 的 GeometryDevice，发起异步 import；轮询已完成的 import
    /// 任务，将解析出的 model_id 写入其 ModelResource 槽并转入 PendingBuild。
    /// import 完成后回填引用该 geometry 的 MechanicsDevice 的 AABB（八叉树每帧自愈）。
    /// 这是"导入异步化"的承接点：Python ctor 仅记录 model_path 并标记 PendingImport，
    /// 磁盘 IO / assimp 解析全部移到 GeometrySystem 线程，不阻塞前端（CEF UI 线程）。
    void process_pending_geometry_imports();

    /// 扫描所有 GeometryDevice，为标记 PendingBuild 且 model_id 已就绪者构建 GPU
    /// 资源（mesh_handles），构建后置回 Ready 并失效其 LOD 缓存。
    /// 这是"初始加载异步化"的承接点：Python ctor 仅记录 model_id 并标记 PendingBuild，
    /// 实际 GPU 构建延迟到此处（GeometrySystem 线程）完成，不阻塞前端。
    /// 当前默认无人产出 PendingBuild（所有路径仍同步构建为 Ready），本扫描为空跑。
    void process_pending_geometry_builds();

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

    /// 遍历所有已加载的 Scene 资源，建立 LOD 缓存条目（元数据）。
    /// LOD0 立即就绪（复用 mesh_dev 缓冲）；LOD1..N 仅登记元数据
    /// （screen_threshold / 计数 / source_lod_index），ready=false、不建 GPU 缓冲、不建 BVH。
    /// 实际 GPU 缓冲由 reconcile_lod_residency() 按需构建（Step 3a：按需驻留）。
    /// 每帧调用但只对新模型建条目（已有缓存的跳过）。
    void upload_lod_from_scene_data();

    /// 按需 LOD 驻留协调（Step 3a，每帧 update 调用）。
    /// 遍历所有有 LOD 缓存的 geometry，按相机屏占比算出"需求级 D"（多相机取最高精度），
    /// 确保 D 已构建 GPU 缓冲（缺则从 Scene CPU 即时建），并释放其余 LOD1..N 级（回收显存）。
    /// LOD0 始终保留作降级兜底（其释放属 Step 3b，本步不做）。
    /// 渲染线程 select_render_buffers 用同一屏占比公式 → 与本决策一致；
    /// 偶发不一致时渲染自动降级到 LOD0，不致黑屏。
    void reconcile_lod_residency();

    /// 轮询已完成的异步 LOD 构建任务（方案 C），将 GPU 缓冲回写进 lod_cache。
    /// 在 update() 中 reconcile_lod_residency() 之前调用：让本帧完成的级即刻可见。
    /// 回写前做 ABA 重校验（model_id + residency_epoch + 级存在 + 未就绪），
    /// 失败则丢弃（结果 RAII 自动释放 GPU）。仅几何线程访问在途表，无需加锁。
    void process_pending_lod_builds();

    /// 空间距离驱动的 LOD 级淘汰：释放指定 actor 所有 geometry 的 LOD1..N GPU 缓冲，
    /// 仅保留 LOD0，并设 lod_spatially_evicted 标记。仅几何线程 update() 中调用。
    void evict_lods_for_actor(std::uintptr_t actor);

    /// 维护 mesh/texture 的 CPU 资源账本（P0）：登记新出现 model_id 的 Scene
    /// (mesh CPU) 与其 Image 纹理 (texture CPU)，按 rid 去重；并对 ResourceManager
    /// 的存活集合做对账，删除已被驱逐的 rid。低频调用（~1Hz）即可，CPU 用量变化缓慢。
    void update_cpu_resource_ledger();

    /// CPU LOD 驻留协调（RAM 3层窗口管理）。
    /// 遍历所有 model_id，按各实例 committed_demand 的中位数确定 CPU 驻留窗口
    /// {lod_levels[0], lod_levels[demand_median-1], lod_levels[N-1]}，清空窗口外级的
    /// vertices/indices/bone_weights。每 kCpuWindowEvalInterval 帧调用一次。
    void reconcile_cpu_residency();

    // ========================================
    // LOD 级 LRU — 延迟 Cap 机制
    // ========================================
    // 当 VRAM 超过 soft 水位（75% 预算）时，按距离排序对远处 geometry 施加 LOD 下限，
    // 迫使其选更粗 LOD 以释放显存。远者优先降级——保留近处精细度，牺牲远处细节。

    /// 单条 LOD 预算候选项：reconcile 每 (geom,mesh) 收集一条，供 enforce_lod_budget
    /// 在帧末尾统一按距离排序、按需施加 LOD 下限（cap）。
    struct LodBudgetEntry {
        std::uint64_t lod_key;        // make_lod_key(geometry_handle, mesh_index)
        ktm::fvec3    world_center;   // mesh 世界 AABB 中心，用于距相机距离排序
        int           current_demand; // 本帧自然需求（滞回后、cap 前），非 cap 后值
        int           level_count;    // 该 mesh 的 LOD 总级数（= lod_cache 条目中 levels.size()）
    };

    /// 在 reconcile_lod_residency 末尾调用：扫描所有 entry，按 VRAM 压力对远处 mesh
    /// 施加 demand 下限（lod_budget_caps），下一帧 reconcile 生效。
    /// @param camera_positions 所有相机世界坐标（用于距离排序）
    /// @param entries          本帧收集的所有 LodBudgetEntry
    void enforce_lod_budget(
        const std::vector<ktm::fvec3>& camera_positions,
        const std::vector<LodBudgetEntry>& entries);

    /// 计算 mesh/texture 的 VRAM/RAM 用量 + 预算视图（线程安全，内部加锁）。
    [[nodiscard]] MemoryReport compute_memory_report() const;

    /// 估算单个 actor 占用的可淘汰 GPU 字节（mesh+texture 缓冲 + 其 LOD 缓存）与
    /// CPU 字节（其 Scene mesh 资源，按 model_id 查 cpu_ledger）。用于按需淘汰定额。
    void estimate_actor_memory(std::uintptr_t actor,
                               std::size_t& out_gpu_bytes,
                               std::size_t& out_cpu_bytes) const;

    /// 满载淘汰：当 VRAM/RAM 用量达到高水位（默认 90%）时，按"最冷"（不可见帧多、
    /// 距相机远）顺序淘汰 Loaded actor，发 ActorEvictRequestedEvent（复用快照+释放+级联
    /// 通路），按 need_free 定额停止，降到低水位（默认 80%）。每隔若干帧评估一次。
    void evict_under_memory_pressure();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Systems
