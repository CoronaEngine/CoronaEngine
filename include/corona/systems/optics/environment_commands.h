#pragma once

/// Environment 的跨线程交互边界（P4：在 tick 边界阶段统一应用）。
///
/// 为什么 Environment 不放在各自系统的 update() 里：
/// - 它**跨两个消费者**：`sun_* / sky_intensity / floor_grid_enabled` 归 Optics，
///   `gravity / floor_y / floor_restitution / fixed_dt` 归 Mechanics；
/// - Mechanics 在 DAG 里排在 Optics 之前，若在 Optics 里应用，Mechanics 本 tick
///   就会晚一帧看到 `gravity / fixed_dt` 变更 —— 那是会改变模拟行为的隐性回归。
/// 因此统一交给 `SystemsThread` 的 tick 边界阶段（所有节点之前）应用。
///
/// `authored_lighting`（shared_ptr<json>）是结构负载，不进这条通道。

#include <corona/kernel/command/command_buffer.h>
#include <corona/kernel/command/snapshot_channel.h>
#include <corona/shared_data_hub.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Corona {

enum class EnvironmentField : std::uint32_t {
    None = 0,
    SunDirection = 1u << 0,
    SunIntensity = 1u << 1,
    SkyIntensity = 1u << 2,
    FloorGrid = 1u << 3,
    Gravity = 1u << 4,
    FloorY = 1u << 5,
    FloorRestitution = 1u << 6,
    FixedDt = 1u << 7,
    /// 全局曝光。网络同步的 environment 条目带这个字段，命令通道必须能承载它，
    /// 否则远程曝光只能靠直写槽位（已禁止）。
    Exposure = 1u << 8,
    /// 作者给定的整份光照配置（shared_ptr<json> 结构负载）。
    AuthoredLighting = 1u << 9,
    /// 太阳颜色。只有 AuthoredLighting 派生时会改它。
    SunColor = 1u << 10,
};

constexpr EnvironmentField operator|(EnvironmentField lhs, EnvironmentField rhs) {
    return static_cast<EnvironmentField>(static_cast<std::uint32_t>(lhs) |
                                         static_cast<std::uint32_t>(rhs));
}

constexpr bool has_environment_field(EnvironmentField fields, EnvironmentField field) {
    return (static_cast<std::uint32_t>(fields) & static_cast<std::uint32_t>(field)) != 0;
}

struct EnvironmentUpdateCommand {
    std::uintptr_t environment_handle{};
    EnvironmentField fields{EnvironmentField::None};
    ktm::fvec3 sun_direction{0.0f, 1.0f, 0.0f};
    float sun_intensity{10.0f};
    float sky_intensity{20.0f};
    std::uint32_t floor_grid_enabled{1};
    ktm::fvec3 gravity{0.0f, -9.8f, 0.0f};
    float floor_y{0.0f};
    float floor_restitution{0.6f};
    float fixed_dt{1.0f / 60.0f};
    float exposure{1.0f};
    /// 仅 AuthoredLighting 位有效；nullptr 表示解绑。
    std::shared_ptr<const nlohmann::json> authored_lighting;
    std::size_t authored_lighting_revision{0};
    ktm::fvec3 sun_color{1.0f, 0.949f, 0.853f};
};

struct EnvironmentUpdateKey {
    static std::uintptr_t get(const EnvironmentUpdateCommand& command) noexcept {
        return command.environment_handle;
    }

    /// 同 key 合并时只覆盖本条命令声明要改的字段。
    /// 光照面板（UI 线程）与网络同步（Network 线程）会同时向这条队列推部分
    /// 字段命令；没有 merge，后到的事件会把对方尚未应用的字段一起冲掉。
    static void merge(EnvironmentUpdateCommand& pending,
                      const EnvironmentUpdateCommand& next) noexcept {
        if (has_environment_field(next.fields, EnvironmentField::SunDirection)) pending.sun_direction = next.sun_direction;
        if (has_environment_field(next.fields, EnvironmentField::SunIntensity)) pending.sun_intensity = next.sun_intensity;
        if (has_environment_field(next.fields, EnvironmentField::SkyIntensity)) pending.sky_intensity = next.sky_intensity;
        if (has_environment_field(next.fields, EnvironmentField::FloorGrid)) pending.floor_grid_enabled = next.floor_grid_enabled;
        if (has_environment_field(next.fields, EnvironmentField::Gravity)) pending.gravity = next.gravity;
        if (has_environment_field(next.fields, EnvironmentField::FloorY)) pending.floor_y = next.floor_y;
        if (has_environment_field(next.fields, EnvironmentField::FloorRestitution)) pending.floor_restitution = next.floor_restitution;
        if (has_environment_field(next.fields, EnvironmentField::FixedDt)) pending.fixed_dt = next.fixed_dt;
        if (has_environment_field(next.fields, EnvironmentField::Exposure)) pending.exposure = next.exposure;
        if (has_environment_field(next.fields, EnvironmentField::AuthoredLighting)) {
            pending.authored_lighting = next.authored_lighting;
            pending.authored_lighting_revision = next.authored_lighting_revision;
        }
        if (has_environment_field(next.fields, EnvironmentField::SunColor)) pending.sun_color = next.sun_color;
        pending.fields = pending.fields | next.fields;
    }
};

struct EnvironmentSnapshotRow {
    std::uintptr_t handle{0};
    ktm::fvec3 sun_direction{0.0f, 1.0f, 0.0f};
    float sun_intensity{10.0f};
    float sky_intensity{20.0f};
    std::uint32_t floor_grid_enabled{1};
    ktm::fvec3 gravity{0.0f, -9.8f, 0.0f};
    float floor_y{0.0f};
    float floor_restitution{0.6f};
    float fixed_dt{1.0f / 60.0f};
    float exposure{1.0f};
    /// 只投影标量；authored_lighting 的 json 负载不进快照（太重且无外部读者）。
    std::size_t authored_lighting_revision{0};
    ktm::fvec3 sun_color{1.0f, 0.949f, 0.853f};
};

struct EnvironmentSnapshot {
    std::uint64_t tick{0};
    std::vector<EnvironmentSnapshotRow> rows;
};

class EnvironmentCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<EnvironmentUpdateCommand, EnvironmentUpdateKey,
                                         Kernel::CoalescePolicy::LatestWins>;

    static EnvironmentCommandChannel& instance() {
        static EnvironmentCommandChannel channel;
        return channel;
    }

    [[nodiscard]] Buffer& commands() { return commands_; }
    [[nodiscard]] Kernel::SnapshotChannel<EnvironmentSnapshot>& snapshots() { return snapshots_; }

   private:
    Buffer commands_;
    Kernel::SnapshotChannel<EnvironmentSnapshot> snapshots_;
};

inline EnvironmentSnapshotRow make_environment_row(std::uintptr_t handle,
                                                  const EnvironmentDevice& device) {
    EnvironmentSnapshotRow row;
    row.handle = handle;
    row.sun_direction = device.sun_position;
    row.sun_intensity = device.sun_intensity;
    row.sky_intensity = device.sky_intensity;
    row.floor_grid_enabled = device.floor_grid_enabled;
    row.gravity = device.gravity;
    row.floor_y = device.floor_y;
    row.floor_restitution = device.floor_restitution;
    row.fixed_dt = device.fixed_dt;
    row.exposure = device.exposure;
    row.authored_lighting_revision = device.authored_lighting_revision;
    row.sun_color = device.sun_color;
    return row;
}

/// 从槽位重建整份快照（只应在 tick 边界、应用完命令之后调用）
inline void publish_environment_snapshot_from_storage() {
    auto& storage = SharedDataHub::instance().environment_storage();
    EnvironmentSnapshot snapshot;
    snapshot.tick = EnvironmentCommandChannel::instance().snapshots().revision() + 1;
    for (const auto& device : std::as_const(storage)) {
        const auto handle = reinterpret_cast<std::uintptr_t>(static_cast<const void*>(&device));
        if (handle == 0) {
            continue;
        }
        snapshot.rows.push_back(make_environment_row(handle, device));
    }
    EnvironmentCommandChannel::instance().snapshots().publish(std::move(snapshot));
}

/// 应用入站 Environment 命令（tick 边界阶段，所有节点之前）。
/// 应用完立即发布快照：外部读到的是"本 tick 开始时的最终环境"。
inline bool apply_pending_environment_updates() {
    auto& channel = EnvironmentCommandChannel::instance();
    auto commands = channel.commands().drain();
    bool applied = false;

    if (!commands.empty()) {
        applied = true;
        auto& storage = SharedDataHub::instance().environment_storage();
        for (const auto& command : commands) {
            auto device = storage.try_acquire_write(command.environment_handle);
            if (!device) {
                continue;
            }
            const auto fields = command.fields;
            if (has_environment_field(fields, EnvironmentField::SunDirection)) {
                device->sun_position = command.sun_direction;
            }
            if (has_environment_field(fields, EnvironmentField::SunIntensity)) {
                device->sun_intensity = command.sun_intensity;
            }
            if (has_environment_field(fields, EnvironmentField::SkyIntensity)) {
                device->sky_intensity = command.sky_intensity;
            }
            if (has_environment_field(fields, EnvironmentField::FloorGrid)) {
                device->floor_grid_enabled = command.floor_grid_enabled;
            }
            if (has_environment_field(fields, EnvironmentField::Gravity)) {
                device->gravity = command.gravity;
            }
            if (has_environment_field(fields, EnvironmentField::FloorY)) {
                device->floor_y = command.floor_y;
            }
            if (has_environment_field(fields, EnvironmentField::FloorRestitution)) {
                device->floor_restitution = command.floor_restitution;
            }
            if (has_environment_field(fields, EnvironmentField::FixedDt)) {
                device->fixed_dt = command.fixed_dt;
            }
            if (has_environment_field(fields, EnvironmentField::Exposure)) {
                device->exposure = command.exposure;
            }
            if (has_environment_field(fields, EnvironmentField::AuthoredLighting)) {
                device->authored_lighting = command.authored_lighting;
                device->authored_lighting_revision = command.authored_lighting_revision;
            }
            if (has_environment_field(fields, EnvironmentField::SunColor)) {
                device->sun_color = command.sun_color;
            }
        }
    }

    // 幂等：没有命令、且快照已存在时不重复重建（DAG 模式下 Optics 的兜底调用
    // 因此几乎没有开销）。
    if (applied || !channel.snapshots().published()) {
        publish_environment_snapshot_from_storage();
    }
    return applied;
}

[[nodiscard]] inline std::optional<EnvironmentSnapshotRow> environment_row(std::uintptr_t handle) {
    if (handle == 0) {
        return std::nullopt;
    }
    auto snapshot = EnvironmentCommandChannel::instance().snapshots().read();
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

/// 有效值 = 快照（已提交）+ 待处理命令（pending）
[[nodiscard]] inline std::optional<EnvironmentSnapshotRow> environment_view(std::uintptr_t handle) {
    if (handle == 0) {
        return std::nullopt;
    }
    std::optional<EnvironmentSnapshotRow> view = environment_row(handle);
    if (const auto pending = EnvironmentCommandChannel::instance().commands().peek(handle)) {
        if (!view.has_value()) {
            view.emplace();
            view->handle = handle;
        }
        const auto fields = pending->fields;
        if (has_environment_field(fields, EnvironmentField::SunDirection)) view->sun_direction = pending->sun_direction;
        if (has_environment_field(fields, EnvironmentField::SunIntensity)) view->sun_intensity = pending->sun_intensity;
        if (has_environment_field(fields, EnvironmentField::SkyIntensity)) view->sky_intensity = pending->sky_intensity;
        if (has_environment_field(fields, EnvironmentField::FloorGrid)) view->floor_grid_enabled = pending->floor_grid_enabled;
        if (has_environment_field(fields, EnvironmentField::Gravity)) view->gravity = pending->gravity;
        if (has_environment_field(fields, EnvironmentField::FloorY)) view->floor_y = pending->floor_y;
        if (has_environment_field(fields, EnvironmentField::FloorRestitution)) view->floor_restitution = pending->floor_restitution;
        if (has_environment_field(fields, EnvironmentField::FixedDt)) view->fixed_dt = pending->fixed_dt;
        if (has_environment_field(fields, EnvironmentField::Exposure)) view->exposure = pending->exposure;
        if (has_environment_field(fields, EnvironmentField::AuthoredLighting)) {
            view->authored_lighting_revision = pending->authored_lighting_revision;
        }
        if (has_environment_field(fields, EnvironmentField::SunColor)) view->sun_color = pending->sun_color;
    }
    return view;
}

}  // namespace Corona
