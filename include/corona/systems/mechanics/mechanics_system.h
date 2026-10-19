#pragma once

#include <corona/events/mechanics_system_events.h>
#include <corona/events/scene_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>

#include <memory>
#include <array>
#include <span>
#include <vector>
#include <functional>
#include <mutex>
#include <chrono>
#include <unordered_map>

namespace Corona::Resource { struct IkChain; struct SkeletonData; }

namespace Corona::Systems {

// 前向声明，避免每帧通过 ISystemContext::get_system() 获取
class GeometrySystem;

/**
 * @brief 力学系统 (Mechanics System)
 *
 * 负责物理模拟、刚体动力学、碰撞检测和响应。
 * 运行在独立线程，以 60 FPS 更新物理状态。
 */
class MechanicsSystem : public Kernel::SystemBase {
   public:
    MechanicsSystem();

    ~MechanicsSystem() override;

    // ========================================
    // ISystem 接口实现
    // ========================================

    std::string_view get_name() const override {
        return "Mechanics";
    }

    int get_priority() const override {
        return 75;  // 中高优先级，在几何系统之后
    }

    /**
     * @brief 初始化力学系统
     * @param ctx 系统上下文
     * @return 初始化成功返回 true
     */
    bool initialize(Kernel::ISystemContext* ctx) override;

    /**
     * @brief 每帧更新物理
     *
     * 在独立线程中调用，执行物理模拟
     */
    void update() override;

    /**
     * @brief 关闭力学系统
     *
     * 清理所有物理资源
     */
    void stop() override;

    void shutdown() override;

    struct MeshMoveCommand {
        std::uintptr_t actor{}, camera{};
        std::array<float, 3> velocity{};
        float dt{};
        bool jump{};
        float facing{};
        bool turn{};
    };
    struct MeshMoveEvent {
        std::string status{"unavailable"};
        std::array<float,3> position{}, normal{};
        bool grounded{}, ceiling{}, collided{};
        std::uintptr_t other_actor{};
        int triangle{-1}, other_triangle{-1};
        std::array<float,3> rotation{};
    };
    // Nonblocking input. Completion runs after releasing scene/storage locks;
    // callers marshal it onto their UI/event thread. No synchronous query API.
    void submit_mesh_move(MeshMoveCommand command, std::function<void(MeshMoveEvent)> completion);

    // ========================================
    // IK 链管理
    // ========================================

    /// 注册一条由地面探测驱动的 FootPlant 链；运行权重从零开始，命中地面后渐入。
    ///
    /// @param geom_handle   蒙皮角色的 GeometryDevice 句柄。
    /// @param bone_name     末端骨骼名称，须与 SkeletonData::nodes[x].name 完全匹配（大小写敏感）。
    ///                      仅接受非根叶子节点（parent >= 0 且 children 为空）。
    /// @param chain_length  参与 CCD 求解的关节数（含末端，沿 parent 上溯）。
    ///                      按模型层级设置，使链覆盖所需父关节；合法范围 [2,64]。
    /// @param damping       CCD 每步旋转压制系数 [0,1]，建议 0.85~0.92，防落地时抖动。
    /// @param weight_decay  脚离地后 weight 每秒衰减量，0.8 ≈ 1.25s 平滑归零。
    /// @return 成功返回 true；名称不存在、不唯一、不是非根叶子，或模型未完成 import，返回 false。
    ///
    /// 若该骨骼已有 FootPlant 链则更新参数。
    /// 必须在对应模型 gpu_build_state == Ready 之后调用。
    bool register_foot_plant_chain(std::uintptr_t geom_handle,
                                   std::string_view bone_name,
                                   int chain_length = 3,
                                   float damping = 0.88f,
                                   float weight_decay = 0.8f);

    /// 注销脚踩地 IK 链。链正在激活中时立即移除，当帧蒙皮使用原动画姿态。
    void unregister_foot_plant_chain(std::uintptr_t geom_handle,
                                     std::string_view bone_name);

   private:
    // 力学系统私有成员
    void update_physics(float fixed_dt);
    void process_mesh_moves();
    struct PendingMeshMove {
        MeshMoveCommand command;
        std::function<void(MeshMoveEvent)> completion;
        std::chrono::steady_clock::time_point submitted;
    };
    struct MeshMoverState {
        float vertical_velocity{};
        bool grounded{};
        std::array<float,3> last_position{};
        std::uintptr_t geometry{}, scene{};
    };
    std::mutex mesh_move_mutex_;
    std::vector<PendingMeshMove> mesh_moves_;
    std::unordered_map<std::uintptr_t,MeshMoverState> mesh_movers_;

    /// 骨骼动画 GPU 蒙皮。CPU 推进 anim_time、求解 FK/IK 并生成 final 骨骼矩阵；
    /// Vulkan/Helicon EDSL 蒙皮并归约 AABB，通过 GPU 拷贝更新所有已驻留 LOD 的
    /// 顶点缓冲（LOD1..N 经 GeometrySystem::get_skinning_lod_targets 借出）。
    /// 碰撞直接消费 GPU 顶点；仅活跃 Vision 相机请求 LOD0 回读。动态 AABB 写回 GeometryDevice。
    /// 在固定步进前等待该批完成，使物理消费同帧数据；GPU 错误向系统错误处理器传播。
    /// @param dt 本真实帧经过时间（秒），由 update() 测量并传入；首帧传 0。
    void update_skinned_geometry(float dt);
    void refresh_foot_ground_cache();
    // Inputs stay alive and chain vectors must not move until the whole scene's
    // ground batch has completed. Every probe uses the same frozen support pose.
    struct FootPose {
        std::uintptr_t geometry{};
        const Resource::SkeletonData* skeleton{};
        const std::vector<std::array<float, 16>>* locals{};
        std::vector<Resource::IkChain>* chains{};
    };
    void update_foot_targets(std::span<const FootPose> poses, float dt);
    void update_foot_targets(std::uintptr_t geom_handle,
                             const Resource::SkeletonData& skeleton,
                             const std::vector<std::array<float, 16>>& locals,
                             std::vector<Resource::IkChain>& chains, float dt);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Systems
