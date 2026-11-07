#pragma once

/// Mechanics 的**结构命令**通道（P4）。
///
/// 与 mechanics_commands.h 的区别：
/// - 那条是"值命令"（标量参数，LatestWins 合并）；
/// - 这条是"结构命令"：`std::function` 无法随值命令传递，而且注册顺序有意义，
///   所以用 Fifo（**不可合并**）。
///
/// 应用时机：tick 边界阶段（所有节点之前），这样 Mechanics 本 tick 一开始就
/// 看到新的回调；DAG 降级路径下由 MechanicsSystem::update() 兜底调用（幂等）。

#include <corona/kernel/command/command_buffer.h>
#include <corona/shared_data_hub.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace Corona {

/// 碰撞回调用到的签名（与 MechanicsDevice 内部签名一致）
using MechanicsCollisionCallback =
    std::function<void(std::uintptr_t, bool, const std::array<float, 3>&,
                       const std::array<float, 3>&)>;

struct MechanicsCallbackBinding {
    enum class Kind : std::uint8_t { Collision, OnMove };

    std::uintptr_t mechanics_handle{};
    Kind kind{Kind::OnMove};
    MechanicsCollisionCallback collision;
    std::function<void()> on_move;
};

struct MechanicsCallbackKey {
    static std::uintptr_t get(const MechanicsCallbackBinding& command) noexcept {
        return command.mechanics_handle;
    }
};

class MechanicsStructuralChannel {
   public:
    // 结构命令不可合并：create 后紧跟 destroy 的顺序必须保持
    using Buffer = Kernel::CommandBuffer<MechanicsCallbackBinding, MechanicsCallbackKey,
                                         Kernel::CoalescePolicy::Fifo>;

    static MechanicsStructuralChannel& instance() {
        static MechanicsStructuralChannel channel;
        return channel;
    }

    [[nodiscard]] Buffer& commands() { return commands_; }

   private:
    Buffer commands_;
};

/// 应用入站结构命令（tick 边界阶段 / 降级兜底）。返回是否真的应用了命令。
inline bool apply_pending_mechanics_structural_commands() {
    auto& channel = MechanicsStructuralChannel::instance();
    auto commands = channel.commands().drain();
    if (commands.empty()) {
        return false;
    }

    auto& storage = SharedDataHub::instance().mechanics_storage();
    for (auto& command : commands) {
        auto device = storage.try_acquire_write(command.mechanics_handle);
        if (!device) {
            continue;
        }
        switch (command.kind) {
            case MechanicsCallbackBinding::Kind::Collision:
                device->collision_callback = std::move(command.collision);
                break;
            case MechanicsCallbackBinding::Kind::OnMove:
                device->on_move_callback = std::move(command.on_move);
                break;
        }
    }
    return true;
}

}  // namespace Corona
