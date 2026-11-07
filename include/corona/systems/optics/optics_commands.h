#pragma once

/// Optics 的跨线程交互边界（P3：写走命令，读走快照）。
///
/// 标量、可见性与不可变材质载荷按字段合并，由 Optics 消费线程统一应用。
/// `material_overrides` / `principled` 的换绑携带 geometry_handle 归属守卫。
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
    /// 非 0 时作为材质守卫：只有 optics 设备的 geometry_handle 匹配才应用材质。
    /// 标量与可见性更新不受此守卫影响。
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

    static void merge(OpticsUpdateCommand& pending, const OpticsUpdateCommand& next) noexcept {
        if (has_optics_field(next.fields, OpticsField::Visible)) pending.visible = next.visible;
        if (has_optics_field(next.fields, OpticsField::EnableLighting)) pending.enable_lighting = next.enable_lighting;
        if (has_optics_field(next.fields, OpticsField::Metallic)) pending.metallic = next.metallic;
        if (has_optics_field(next.fields, OpticsField::Roughness)) pending.roughness = next.roughness;
        if (has_optics_field(next.fields, OpticsField::Subsurface)) pending.subsurface = next.subsurface;
        if (has_optics_field(next.fields, OpticsField::Specular)) pending.specular = next.specular;
        if (has_optics_field(next.fields, OpticsField::SpecularTint)) pending.specular_tint = next.specular_tint;
        if (has_optics_field(next.fields, OpticsField::Anisotropic)) pending.anisotropic = next.anisotropic;
        if (has_optics_field(next.fields, OpticsField::Sheen)) pending.sheen = next.sheen;
        if (has_optics_field(next.fields, OpticsField::SheenTint)) pending.sheen_tint = next.sheen_tint;
        if (has_optics_field(next.fields, OpticsField::Clearcoat)) pending.clearcoat = next.clearcoat;
        if (has_optics_field(next.fields, OpticsField::ClearcoatGloss)) pending.clearcoat_gloss = next.clearcoat_gloss;
        if (has_optics_field(next.fields, OpticsField::Ambient)) pending.ambient = next.ambient;
        if (has_optics_field(next.fields, OpticsField::Diffuse)) pending.diffuse = next.diffuse;
        if (has_optics_field(next.fields, OpticsField::SpecularColor)) pending.specular_color = next.specular_color;
        if (has_optics_field(next.fields, OpticsField::Shininess)) pending.shininess = next.shininess;

        constexpr auto material_fields = OpticsField::MaterialOverrides | OpticsField::Principled;
        if (has_optics_field(next.fields, material_fields)) {
            // Both material payloads share one geometry guard. A new owner must
            // not relabel a retained payload from the previous geometry.
            if (pending.geometry_handle != next.geometry_handle) {
                pending.fields = static_cast<OpticsField>(
                    static_cast<std::uint32_t>(pending.fields) &
                    ~static_cast<std::uint32_t>(material_fields));
                pending.material_overrides.reset();
                pending.principled.reset();
            }
            pending.geometry_handle = next.geometry_handle;
            // A flagged null payload explicitly clears the existing material.
            if (has_optics_field(next.fields, OpticsField::MaterialOverrides))
                pending.material_overrides = next.material_overrides;
            if (has_optics_field(next.fields, OpticsField::Principled))
                pending.principled = next.principled;
        }
        pending.fields = pending.fields | next.fields;
    }
};

/// Apply a drained update while the caller owns the device's write access.
inline void apply_optics_update(OpticsDevice& device, const OpticsUpdateCommand& command) {
    const auto fields = command.fields;
    if (has_optics_field(fields, OpticsField::Visible)) device.visible = command.visible;
    if (has_optics_field(fields, OpticsField::EnableLighting)) device.bEnableLighting = command.enable_lighting;
    if (has_optics_field(fields, OpticsField::Metallic)) device.metallic = command.metallic;
    if (has_optics_field(fields, OpticsField::Roughness)) device.roughness = command.roughness;
    if (has_optics_field(fields, OpticsField::Subsurface)) device.subsurface = command.subsurface;
    if (has_optics_field(fields, OpticsField::Specular)) device.specular = command.specular;
    if (has_optics_field(fields, OpticsField::SpecularTint)) device.specularTint = command.specular_tint;
    if (has_optics_field(fields, OpticsField::Anisotropic)) device.anisotropic = command.anisotropic;
    if (has_optics_field(fields, OpticsField::Sheen)) device.sheen = command.sheen;
    if (has_optics_field(fields, OpticsField::SheenTint)) device.sheenTint = command.sheen_tint;
    if (has_optics_field(fields, OpticsField::Clearcoat)) device.clearcoat = command.clearcoat;
    if (has_optics_field(fields, OpticsField::ClearcoatGloss)) device.clearcoatGloss = command.clearcoat_gloss;
    if (has_optics_field(fields, OpticsField::Ambient)) device.ambient = command.ambient;
    if (has_optics_field(fields, OpticsField::Diffuse)) device.diffuse = command.diffuse;
    if (has_optics_field(fields, OpticsField::SpecularColor)) device.specular_color = command.specular_color;
    if (has_optics_field(fields, OpticsField::Shininess)) device.shininess = command.shininess;
    if (command.geometry_handle != 0 && device.geometry_handle != command.geometry_handle) return;
    if (has_optics_field(fields, OpticsField::MaterialOverrides)) device.material_overrides = command.material_overrides;
    if (has_optics_field(fields, OpticsField::Principled)) device.principled = command.principled;
}

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
