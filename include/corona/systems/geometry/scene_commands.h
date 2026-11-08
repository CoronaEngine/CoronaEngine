#pragma once

/// SceneDevice 的跨线程交互边界。
///
/// SceneDevice 是"混合所有权"结构，必须拆开看：
///   - **外部拥有**：enabled / simulation_enabled / environment /
///     actor_handles / camera_handles / active_camera_handle
///     （编辑器、脚本、网络增删成员）
///   - **Geometry 拥有**：visible_actor_handles / min_world / max_world /
///     center_world（剔除与包围盒的产物）
///
/// 这条通道只承载外部拥有的那部分。此前 `Scene::add_actor()` 等 11 个 setter
/// 直接在调用者线程（CEF / 脚本 / 主线程）上 `acquire_write()` 写槽位，
/// 与 Geometry 的写形成无仲裁竞争。
///
/// 为什么是 **Fifo**：成员的增删顺序有意义（add 之后紧跟 remove 不能合并）。
/// 为什么应用在 **tick 边界**：Mechanics 在 DAG 里排在 Geometry 之前，
/// 若在 Geometry::update 里应用，Mechanics 会晚一帧看到成员变更。

#include <corona/kernel/command/command_buffer.h>
#include <corona/shared_data_hub.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace Corona {

/// SceneDevice 外部可写字段的操作种类
enum class SceneCommandKind : std::uint8_t {
    SetEnabled,            ///< flag -> enabled
    SetSimulationEnabled,  ///< flag -> simulation_enabled
    SetEnvironment,        ///< target -> environment（0 表示解绑）
    AddActor,              ///< target -> actor_handles.push_back
    RemoveActor,           ///< target -> actor_handles.erase
    ClearActors,           ///< actor_handles.clear()
    AddCamera,             ///< target -> camera_handles.push_back；active 为空时顺带选中
    RemoveCamera,          ///< target -> camera_handles.erase；若为 active 则改选队首
    ClearCameras,          ///< camera_handles.clear() + active = 0
    SetActiveCamera,       ///< target -> active_camera_handle
};

struct SceneCommand {
    std::uintptr_t scene_handle{};
    SceneCommandKind kind{SceneCommandKind::SetEnabled};
    /// actor / camera / environment 的槽位句柄；标量命令为 0。
    std::uintptr_t target{};
    /// 布尔型命令的取值。
    bool flag{true};
};

struct SceneCommandKey {
    static std::uintptr_t get(const SceneCommand& command) noexcept {
        return command.scene_handle;
    }
};

class SceneCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<SceneCommand, SceneCommandKey,
                                         Kernel::CoalescePolicy::Fifo>;

    static SceneCommandChannel& instance() {
        static SceneCommandChannel channel;
        return channel;
    }

    [[nodiscard]] Buffer& commands() { return commands_; }

   private:
    Buffer commands_;
};

/// 入队一条场景命令（任意线程）。
inline void push_scene_command(std::uintptr_t scene_handle, SceneCommandKind kind,
                               std::uintptr_t target = 0, bool flag = true) {
    if (scene_handle == 0) {
        return;
    }
    SceneCommand command{};
    command.scene_handle = scene_handle;
    command.kind = kind;
    command.target = target;
    command.flag = flag;
    SceneCommandChannel::instance().commands().push(command);
}

/// 应用入站场景命令（tick 边界阶段；DAG 降级路径由 GeometrySystem::update 兜底）。
/// 幂等：没有命令时直接返回 false，不拿任何锁。
inline bool apply_pending_scene_commands() {
    auto commands = SceneCommandChannel::instance().commands().drain();
    if (commands.empty()) {
        return false;
    }

    auto& storage = SharedDataHub::instance().scene_storage();
    for (const auto& command : commands) {
        auto scene = storage.try_acquire_write(command.scene_handle);
        if (!scene) {
            continue;  // 场景已销毁：命令随场景一起作废
        }
        switch (command.kind) {
            case SceneCommandKind::SetEnabled:
                scene->enabled = command.flag;
                break;
            case SceneCommandKind::SetSimulationEnabled:
                scene->simulation_enabled = command.flag;
                break;
            case SceneCommandKind::SetEnvironment:
                scene->environment = command.target;
                break;
            case SceneCommandKind::AddActor:
                scene->actor_handles.push_back(command.target);
                break;
            case SceneCommandKind::RemoveActor:
                std::erase(scene->actor_handles, command.target);
                break;
            case SceneCommandKind::ClearActors:
                scene->actor_handles.clear();
                break;
            case SceneCommandKind::AddCamera:
                scene->camera_handles.push_back(command.target);
                // 与旧的同步实现一致：第一台相机自动成为 active。
                if (scene->active_camera_handle == 0) {
                    scene->active_camera_handle = command.target;
                }
                break;
            case SceneCommandKind::RemoveCamera:
                std::erase(scene->camera_handles, command.target);
                if (scene->active_camera_handle == command.target) {
                    scene->active_camera_handle =
                        scene->camera_handles.empty() ? 0 : scene->camera_handles.front();
                }
                break;
            case SceneCommandKind::ClearCameras:
                scene->camera_handles.clear();
                scene->active_camera_handle = 0;
                break;
            case SceneCommandKind::SetActiveCamera:
                scene->active_camera_handle = command.target;
                break;
        }
    }
    return true;
}

}  // namespace Corona
