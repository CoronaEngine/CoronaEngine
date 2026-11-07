#pragma once

/// Optics 的跨线程交互边界（P3：写走命令，读走快照）。
///
/// 只覆盖**标量**材质/可见性参数：`material_overrides` / `principled` 是
/// shared_ptr 换绑，属于结构变更，走 P4 的结构命令通道。
///
/// 通道放在领域自己的头里（理由见 mechanics_commands.h）：不进 shared_data_hub.h，
/// 避免每加一个命令类型就让全仓 600 个 TU 重编。

#include <corona/kernel/command/command_buffer.h>
#include <corona/systems/optics/material_source.h>
#include <corona/kernel/command/snapshot_channel.h>
#include <corona/shared_data_hub.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Corona {

enum class OpticsField : std::uint32_t {
    None = 0,
    Visible = 1u << 0,
    EnableLighting = 1u << 1,
    Metallic = 1u << 2,
    Roughness = 1u << 3,
    Subsurface = 1u << 4,
    Specular = 1u << 5,
    SpecularTint = 1u << 6,
    Anisotropic = 1u << 7,
    Sheen = 1u << 8,
    SheenTint = 1u << 9,
    Clearcoat = 1u << 10,
    ClearcoatGloss = 1u << 11,
    Ambient = 1u << 12,
    Diffuse = 1u << 13,
    SpecularColor = 1u << 14,
    Shininess = 1u << 15,
    /// 非标量材质载荷（CEF 编辑器写入整份材质配置）
    MaterialOverrides = 1u << 16,
    Principled = 1u << 17,
};

constexpr OpticsField operator|(OpticsField lhs, OpticsField rhs) {
    return static_cast<OpticsField>(static_cast<std::uint32_t>(lhs) |
                                    static_cast<std::uint32_t>(rhs));
}

constexpr bool has_optics_field(OpticsField fields, OpticsField field) {
    return (static_cast<std::uint32_t>(fields) & static_cast<std::uint32_t>(field)) != 0;
}

struct OpticsUpdateCommand {
    std::uintptr_t optics_handle{};
    /// 非 0 时作为守卫：只有 optics 设备的 geometry_handle 匹配才应用。
    /// 编辑器按 profile 找到 optics 设备后，会用它确认"这份材质属于当前几何"。
    std::uintptr_t geometry_handle{};
    std::shared_ptr<const Materials::Overrides> material_overrides;
    std::shared_ptr<const Materials::Principled> principled;
    OpticsField fields{OpticsField::None};
    bool visible{true};
    bool enable_lighting{true};
    float metallic{0.0f};
    float roughness{0.5f};
    float subsurface{0.0f};
    float specular{0.5f};
    float specular_tint{0.0f};
    float anisotropic{0.0f};
    float sheen{0.0f};
    float sheen_tint{0.5f};
    float clearcoat{0.0f};
    float clearcoat_gloss{1.0f};
    ktm::fvec3 ambient{0.2f, 0.2f, 0.2f};
    ktm::fvec3 diffuse{0.8f, 0.8f, 0.8f};
    ktm::fvec3 specular_color{1.0f, 1.0f, 1.0f};
    float shininess{32.0f};
};

struct OpticsUpdateKey {
    static std::uintptr_t get(const OpticsUpdateCommand& command) noexcept {
        return command.optics_handle;
    }
};

/// 对外投影：标量材质参数 + 可见性（材质句柄不进来）
struct OpticsSnapshotRow {
    std::uintptr_t handle{0};
    bool visible{true};
    bool enable_lighting{true};
    float metallic{0.0f};
    float roughness{0.5f};
    float subsurface{0.0f};
    float specular{0.5f};
    float specular_tint{0.0f};
    float anisotropic{0.0f};
    float sheen{0.0f};
    float sheen_tint{0.5f};
    float clearcoat{0.0f};
    float clearcoat_gloss{1.0f};
    ktm::fvec3 ambient{0.2f, 0.2f, 0.2f};
    ktm::fvec3 diffuse{0.8f, 0.8f, 0.8f};
    ktm::fvec3 specular_color{1.0f, 1.0f, 1.0f};
    float shininess{32.0f};
};

struct OpticsSnapshot {
    std::uint64_t tick{0};
    std::vector<OpticsSnapshotRow> rows;
};

class OpticsCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<OpticsUpdateCommand, OpticsUpdateKey,
                                         Kernel::CoalescePolicy::LatestWins>;

    static OpticsCommandChannel& instance() {
        static OpticsCommandChannel channel;
        return channel;
    }

    [[nodiscard]] Buffer& commands() { return commands_; }
    [[nodiscard]] Kernel::SnapshotChannel<OpticsSnapshot>& snapshots() { return snapshots_; }

   private:
    Buffer commands_;
    Kernel::SnapshotChannel<OpticsSnapshot> snapshots_;
};

inline OpticsSnapshotRow make_optics_row(std::uintptr_t handle, const OpticsDevice& device) {
    OpticsSnapshotRow row;
    row.handle = handle;
    row.visible = device.visible;
    row.enable_lighting = device.bEnableLighting;
    row.metallic = device.metallic;
    row.roughness = device.roughness;
    row.subsurface = device.subsurface;
    row.specular = device.specular;
    row.specular_tint = device.specularTint;
    row.anisotropic = device.anisotropic;
    row.sheen = device.sheen;
    row.sheen_tint = device.sheenTint;
    row.clearcoat = device.clearcoat;
    row.clearcoat_gloss = device.clearcoatGloss;
    row.ambient = device.ambient;
    row.diffuse = device.diffuse;
    row.specular_color = device.specular_color;
    row.shininess = device.shininess;
    return row;
}

/// 拥有者线程：从槽位重建整份快照（Optics 的写入完成之后调用）
inline void publish_optics_snapshot_from_storage() {
    auto& storage = SharedDataHub::instance().optics_storage();
    OpticsSnapshot snapshot;
    snapshot.tick = OpticsCommandChannel::instance().snapshots().revision() + 1;
    for (const auto& device : std::as_const(storage)) {
        const auto handle = reinterpret_cast<std::uintptr_t>(static_cast<const void*>(&device));
        if (handle == 0) {
            continue;
        }
        snapshot.rows.push_back(make_optics_row(handle, device));
    }
    OpticsCommandChannel::instance().snapshots().publish(std::move(snapshot));
}

[[nodiscard]] inline std::optional<OpticsSnapshotRow> optics_row(std::uintptr_t handle) {
    if (handle == 0) {
        return std::nullopt;
    }
    auto snapshot = OpticsCommandChannel::instance().snapshots().read();
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
[[nodiscard]] inline std::optional<OpticsSnapshotRow> optics_view(std::uintptr_t handle) {
    if (handle == 0) {
        return std::nullopt;
    }
    std::optional<OpticsSnapshotRow> view = optics_row(handle);
    if (const auto pending = OpticsCommandChannel::instance().commands().peek(handle)) {
        if (!view.has_value()) {
            view.emplace();
            view->handle = handle;
        }
        const auto& f = pending->fields;
        if (has_optics_field(f, OpticsField::Visible)) view->visible = pending->visible;
        if (has_optics_field(f, OpticsField::EnableLighting)) view->enable_lighting = pending->enable_lighting;
        if (has_optics_field(f, OpticsField::Metallic)) view->metallic = pending->metallic;
        if (has_optics_field(f, OpticsField::Roughness)) view->roughness = pending->roughness;
        if (has_optics_field(f, OpticsField::Subsurface)) view->subsurface = pending->subsurface;
        if (has_optics_field(f, OpticsField::Specular)) view->specular = pending->specular;
        if (has_optics_field(f, OpticsField::SpecularTint)) view->specular_tint = pending->specular_tint;
        if (has_optics_field(f, OpticsField::Anisotropic)) view->anisotropic = pending->anisotropic;
        if (has_optics_field(f, OpticsField::Sheen)) view->sheen = pending->sheen;
        if (has_optics_field(f, OpticsField::SheenTint)) view->sheen_tint = pending->sheen_tint;
        if (has_optics_field(f, OpticsField::Clearcoat)) view->clearcoat = pending->clearcoat;
        if (has_optics_field(f, OpticsField::ClearcoatGloss)) view->clearcoat_gloss = pending->clearcoat_gloss;
        if (has_optics_field(f, OpticsField::Ambient)) view->ambient = pending->ambient;
        if (has_optics_field(f, OpticsField::Diffuse)) view->diffuse = pending->diffuse;
        if (has_optics_field(f, OpticsField::SpecularColor)) view->specular_color = pending->specular_color;
        if (has_optics_field(f, OpticsField::Shininess)) view->shininess = pending->shininess;
    }
    return view;
}

}  // namespace Corona
