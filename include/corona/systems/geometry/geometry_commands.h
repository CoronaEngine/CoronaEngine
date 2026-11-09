#pragma once

/// GeometryDevice 的跨线程交互边界。
///
/// `GeometryStorage` 的写者有两个 DAG 系统（Geometry 建网格、Mechanics 写蒙皮
/// 包围盒/碰撞模型 id），因此外部线程（CEF 编辑器、脚本）对它的写入必须走
/// 命令，由 Geometry 在自己的 update 里应用。
///
/// 覆盖范围：
///   - NativeLocalCorrection：导入坐标修正（标量，LatestWins）
///   - IkChains：骨架 IK 链配置（结构负载，Fifo —— 顺序与 revision 都有意义）
///   - ModelNodeState / ModelNodeOverrides：逐节点编辑（结构负载，Fifo）
///
/// 这些命令**不带返回值**。调用方需要同步校验的部分（索引越界、expected_revision
/// 冲突）在入队前用只读锁完成，入队成功即视为接受；应用阶段的失败只记日志，
/// 因为此时已经无法把失败回传给调用者。

#include <corona/kernel/command/command_buffer.h>
#include <corona/resource/types/scene.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/geometry/model_node_geometry.h>
#include <horizon/core/logging.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace Corona {

enum class GeometryField : std::uint32_t {
    None = 0,
    NativeLocalCorrection = 1u << 0,
    IkChains = 1u << 1,
    /// 整份覆盖列表替换（`set_model_node_overrides`）
    ModelNodeOverrides = 1u << 2,
    /// 单个节点编辑（`set_model_node_state`），配 `node_index` + `node_state`
    ModelNodeState = 1u << 3,
};

constexpr GeometryField operator|(GeometryField lhs, GeometryField rhs) {
    return static_cast<GeometryField>(static_cast<std::uint32_t>(lhs) |
                                      static_cast<std::uint32_t>(rhs));
}

constexpr bool has_geometry_field(GeometryField fields, GeometryField field) {
    return (static_cast<std::uint32_t>(fields) & static_cast<std::uint32_t>(field)) != 0;
}

struct GeometryCommand {
    EntityId geometry_handle{};
    GeometryField fields{GeometryField::None};

    // NativeLocalCorrection
    ktm::fvec3 correction_offset{};
    float correction_scale{1.0f};

    // IkChains
    std::vector<Resource::IkChain> ik_chains;

    // ModelNodeOverrides
    std::vector<API::ModelNodeOverride> model_node_overrides;

    // ModelNodeState
    std::uint32_t node_index{0};
    API::ModelNodeState node_state;
};

struct GeometryCommandKey {
    static std::uintptr_t get(const GeometryCommand& command) noexcept {
        return command.geometry_handle;
    }
};

class GeometryCommandChannel {
   public:
    /// Fifo：IK 与逐节点编辑的顺序有意义，不能按 key 合并。
    using Buffer = Kernel::CommandBuffer<GeometryCommand, GeometryCommandKey,
                                         Kernel::CoalescePolicy::Fifo>;

    static GeometryCommandChannel& instance() {
        static GeometryCommandChannel channel;
        return channel;
    }

    [[nodiscard]] Buffer& commands() { return commands_; }

   private:
    Buffer commands_;
};

/// 单节点编辑后刷新该节点的 override 投影。
/// 与 `Geometry::set_model_node_state()` 里原先内联的逻辑逐字对应。
inline void refresh_node_override(GeometryDevice& geometry, std::uint32_t index) {
    if (index >= geometry.model_nodes.size()) {
        return;
    }
    const auto& node = geometry.model_nodes[index];
    API::ModelNodeOverride override_entry;
    override_entry.node_index = index;
    override_entry.node_path = node.state.node_path;
    if (node.state.position != node.base.position) override_entry.position = node.state.position;
    if (node.state.rotation != node.base.rotation) override_entry.rotation = node.state.rotation;
    if (node.state.scale != node.base.scale) override_entry.scale = node.state.scale;
    if (!node.state.visible) override_entry.visible = false;

    auto& overrides = geometry.model_node_overrides;
    overrides.erase(std::remove_if(overrides.begin(), overrides.end(),
                                   [index](const auto& entry) { return entry.node_index == index; }),
                    overrides.end());
    if (override_entry.position || override_entry.rotation ||
        override_entry.scale || override_entry.visible) {
        overrides.push_back(std::move(override_entry));
    }
}

/// 应用入站几何命令（GeometrySystem::update 开头，拥有者线程）。
inline bool apply_pending_geometry_commands() {
    auto commands = GeometryCommandChannel::instance().commands().drain();
    if (commands.empty()) {
        return false;
    }

    auto& storage = SharedDataHub::instance().geometry_storage();
    for (auto& command : commands) {
        auto geometry = storage.try_acquire_write(command.geometry_handle);
        if (!geometry) {
            continue;  // 几何已销毁：命令随对象作废
        }

        if (has_geometry_field(command.fields, GeometryField::NativeLocalCorrection)) {
            geometry->native_local_correction_offset = command.correction_offset;
            geometry->native_local_correction_scale = command.correction_scale;
        }

        if (has_geometry_field(command.fields, GeometryField::IkChains)) {
            geometry->ik_chains = std::move(command.ik_chains);
            ++geometry->ik_chains_revision;
        }

        if (has_geometry_field(command.fields, GeometryField::ModelNodeOverrides)) {
            // 快照当前状态用于失败回滚（原先由调用方持有写锁时内联完成）。
            auto previous_nodes = geometry->model_nodes;
            auto previous_overrides = geometry->model_node_overrides;
            auto previous_instances = geometry->mesh_instances;
            auto previous_bounds = geometry->model_node_bounds;
            auto previous_revision = geometry->model_node_revision;

            geometry->model_node_overrides = std::move(command.model_node_overrides);
            for (auto& node : geometry->model_nodes) {
                node.state.position = node.base.position;
                node.state.rotation = node.base.rotation;
                node.state.scale = node.base.scale;
                node.state.visible = true;
                node.dirty = true;
            }
            for (std::uint32_t i = 0; i < geometry->model_nodes.size(); ++i) {
                ModelNodes::update(geometry->model_nodes, i);
            }
            if (!geometry->model_nodes.empty() && !ModelNodes::apply_overrides(*geometry)) {
                // 与旧的同步实现一致：整体回滚，live 状态保持上一份。
                geometry->model_nodes = std::move(previous_nodes);
                geometry->model_node_overrides = std::move(previous_overrides);
                geometry->mesh_instances = std::move(previous_instances);
                geometry->model_node_bounds = previous_bounds;
                geometry->model_node_revision = previous_revision;
                CFW_LOG_WARNING("[GeometryCommands] node overrides rejected (stale index/path); rolled back");
            }
        }

        if (has_geometry_field(command.fields, GeometryField::ModelNodeState)) {
            const auto index = command.node_index;
            if (index < geometry->model_nodes.size()) {
                const auto previous_state = geometry->model_nodes[index].state;
                if (!ModelNodes::set(geometry->model_nodes, index, command.node_state) ||
                    !ModelNodes::update_instances(*geometry)) {
                    ModelNodes::set(geometry->model_nodes, index, previous_state);
                    ModelNodes::update_instances(*geometry);
                    CFW_LOG_WARNING("[GeometryCommands] node state rejected at index {}", index);
                } else {
                    refresh_node_override(*geometry, index);
                }
            }
        }
    }
    return true;
}

}  // namespace Corona
