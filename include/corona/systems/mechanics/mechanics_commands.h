#pragma once

/// Mechanics 的跨线程交互边界（P3：写走命令，读走快照）。
///
/// 与相机/变换不同，这一层**不放进 SharedDataHub**：`shared_data_hub.h` 被全仓
/// 广泛包含，任何新增命令类型都会让所有 TU 重新编译。把每个领域的入站通道放在
/// 它自己的头里，blast radius 就只有"拥有者系统 + 生产方"这几个 TU。
/// 这也是后续 Optics/Acoustics/Environment 批次化时要沿用的形状。
///
/// 用法：
/// @code
///   // 生产方（CEF / 脚本 / EngineAPI）
///   MechanicsUpdateCommand cmd{};
///   cmd.mechanics_handle = handle;
///   cmd.fields = MechanicsField::Mass;
///   cmd.mass = 2.5f;
///   MechanicsCommandChannel::instance().commands().push(cmd);
///
///   // 拥有者（MechanicsSystem::update 开头）
///   apply_pending_mechanics_updates();
///
///   // 读者（任意线程）
///   if (auto view = mechanics_view(handle)) { view->mass ... }
/// @endcode

#include <corona/kernel/command/command_buffer.h>
#include <corona/kernel/command/snapshot_channel.h>
#include <corona/shared_data_hub.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Corona {

enum class MechanicsField : std::uint32_t {
    None = 0,
    Mass = 1u << 0,
    Restitution = 1u << 1,
    Damping = 1u << 2,
    BodyType = 1u << 3,
    LinearLock = 1u << 4,
    AngularLock = 1u << 5,
    All = Mass | Restitution | Damping | BodyType | LinearLock | AngularLock,
};

constexpr MechanicsField operator|(MechanicsField lhs, MechanicsField rhs) {
    return static_cast<MechanicsField>(static_cast<std::uint32_t>(lhs) |
                                       static_cast<std::uint32_t>(rhs));
}

constexpr bool has_mechanics_field(MechanicsField fields, MechanicsField field) {
    return (static_cast<std::uint32_t>(fields) & static_cast<std::uint32_t>(field)) != 0;
}

struct MechanicsUpdateCommand {
    std::uintptr_t mechanics_handle{};
    MechanicsField fields{MechanicsField::None};
    float mass{1.0f};
    float restitution{0.8f};
    float damping{0.99f};
    BodyType body_type{BodyType::Dynamic};
    std::uint8_t linear_lock_mask{0};
    std::uint8_t angular_lock_mask{0};
};

struct MechanicsUpdateKey {
    static std::uintptr_t get(const MechanicsUpdateCommand& command) noexcept {
        return command.mechanics_handle;
    }
};

/// 对外的力学参数投影（不含回调与几何句柄）
struct MechanicsSnapshotRow {
    std::uintptr_t handle{0};
    float mass{1.0f};
    float restitution{0.8f};
    float damping{0.99f};
    BodyType body_type{BodyType::Dynamic};
    std::uint8_t linear_lock_mask{0};
    std::uint8_t angular_lock_mask{0};
};

struct MechanicsSnapshot {
    std::uint64_t tick{0};
    std::vector<MechanicsSnapshotRow> rows;
};

/// 单消费者命令队列 + 单发布者快照通道
class MechanicsCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<MechanicsUpdateCommand, MechanicsUpdateKey,
                                         Kernel::CoalescePolicy::LatestWins>;

    static MechanicsCommandChannel& instance() {
        static MechanicsCommandChannel channel;
        return channel;
    }

    [[nodiscard]] Buffer& commands() { return commands_; }
    [[nodiscard]] Kernel::SnapshotChannel<MechanicsSnapshot>& snapshots() { return snapshots_; }

   private:
    Buffer commands_;
    Kernel::SnapshotChannel<MechanicsSnapshot> snapshots_;
};

inline MechanicsSnapshotRow make_mechanics_row(std::uintptr_t handle, const MechanicsDevice& device) {
    MechanicsSnapshotRow row;
    row.handle = handle;
    row.mass = device.mass;
    row.restitution = device.restitution;
    row.damping = device.damping;
    row.body_type = device.body_type;
    row.linear_lock_mask = device.linear_lock_mask;
    row.angular_lock_mask = device.angular_lock_mask;
    return row;
}

/// 拥有者线程：从槽位重建整份快照（Mechanics 的写入已经完成之后调用）
inline void publish_mechanics_snapshot_from_storage() {
    auto& storage = SharedDataHub::instance().mechanics_storage();
    MechanicsSnapshot snapshot;
    snapshot.tick = MechanicsCommandChannel::instance().snapshots().revision() + 1;
    for (const auto& device : std::as_const(storage)) {
        const auto handle = reinterpret_cast<std::uintptr_t>(static_cast<const void*>(&device));
        if (handle == 0) {
            continue;
        }
        snapshot.rows.push_back(make_mechanics_row(handle, device));
    }
    MechanicsCommandChannel::instance().snapshots().publish(std::move(snapshot));
}

[[nodiscard]] inline std::optional<MechanicsSnapshotRow> mechanics_row(std::uintptr_t handle) {
    if (handle == 0) {
        return std::nullopt;
    }
    auto snapshot = MechanicsCommandChannel::instance().snapshots().read();
    if (!snapshot) {
        return std::nullopt;
    }
    for (const auto& row : snapshot->rows) {
        if (row.handle == handle) {
            return row;
        }
    }
    return std::nullopt;
}

/// 有效值 = 快照（已提交）+ 待处理命令（pending），用于 get_* 的读己之写
[[nodiscard]] inline std::optional<MechanicsSnapshotRow> mechanics_view(std::uintptr_t handle) {
    if (handle == 0) {
        return std::nullopt;
    }
    std::optional<MechanicsSnapshotRow> view = mechanics_row(handle);
    if (const auto pending = MechanicsCommandChannel::instance().commands().peek(handle)) {
        if (!view.has_value()) {
            view.emplace();
            view->handle = handle;
        }
        if (has_mechanics_field(pending->fields, MechanicsField::Mass)) {
            view->mass = pending->mass;
        }
        if (has_mechanics_field(pending->fields, MechanicsField::Restitution)) {
            view->restitution = pending->restitution;
        }
        if (has_mechanics_field(pending->fields, MechanicsField::Damping)) {
            view->damping = pending->damping;
        }
        if (has_mechanics_field(pending->fields, MechanicsField::BodyType)) {
            view->body_type = pending->body_type;
        }
        if (has_mechanics_field(pending->fields, MechanicsField::LinearLock)) {
            view->linear_lock_mask = pending->linear_lock_mask;
        }
        if (has_mechanics_field(pending->fields, MechanicsField::AngularLock)) {
            view->angular_lock_mask = pending->angular_lock_mask;
        }
    }
    return view;
}

}  // namespace Corona
