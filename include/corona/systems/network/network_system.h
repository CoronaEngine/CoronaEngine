#pragma once

#include <corona/events/network_system_events.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/kernel/event/i_event_stream.h>
#include <corona/kernel/system/system_base.h>

#include <corona/systems/network/protocol.h>
#include <corona/systems/network/lanchat_history_store.h>
#include <corona/systems/network/lanchat_state.h>
#include <corona/systems/network/network_identity.h>
#include <corona/systems/network/peer_manager.h>
#include <corona/systems/network/sync_engine.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Corona::Systems {

/**
 * @brief 网络系统 — 基于 ENet 可靠 UDP 的局域网多人协同编辑。
 *
 * 采用手动 IP 连接模型：房主启动监听，客户端输入房主 IP 加入。
 * 每个实例仍保留 ENet host 用于收发同步数据。
 * 采用 Last-Write-Wins (LWW) 冲突解决策略。
 *
 * 架构：
 *   PeerManager — ENet host + full-mesh peer 管理
 *   SyncEngine  — SharedDataHub 脏轮询 + LWW 合并
 *
 * 优先级 55：在 ScriptSystem(60) 之后、UiSystem(40) 之前。
 */
class NetworkSystem : public Kernel::SystemBase {
public:
    /// 网络会话状态
    enum class SessionState : uint8_t {
        Idle = 0,     ///< 未启动
        Starting,     ///< 正在启动
        Active,       ///< 已启动，可收发同步
        Error         ///< 启动失败
    };

    /// 当前实例在协同会话中的身份。
    enum class SessionRole : uint8_t {
        None = 0,      ///< 未加入任何会话
        Host,          ///< 房主：创建房间并等待客户端输入 IP 加入
        Client         ///< 客户端：通过房主 IP 主动加入
    };

    struct LanChatRoomEvent {
        std::string event;
        std::string room_id;
    };

    struct LanChatSyncEvent {
        std::string event;
        std::string room_id;
        std::string payload_json;
    };

    NetworkSystem();
    ~NetworkSystem() override;

    // ========================================
    // ISystem 接口
    // ========================================

    std::string_view get_name() const override { return "Network"; }
    int get_priority() const override { return 55; }

    bool initialize(Kernel::ISystemContext* ctx) override;
    void update() override;
    void shutdown() override;

    // ========================================
    // 公共接口
    // ========================================

    /**
     * @brief 启动协同会话（仅 ENet host；不再自动广播发现）。
     * @param instance_name 本实例名（用于 UI 显示，最多 31 字符）
     * @param project_id    保留字段，兼容旧调用；手动 IP 连接不依赖它
     * @param port          UDP 端口（默认 kDefaultPort = 27960）
     * @param role          会话身份（房主或客户端）
     * @return true 成功
     */
    bool start_session(const std::string& instance_name, uint64_t project_id,
                       uint16_t port = Network::kDefaultPort,
                       SessionRole role = SessionRole::Host);

    /// 停止会话，断开所有 peer，关闭 host。
    void stop_session();

    /// 当前会话状态。
    [[nodiscard]] SessionState session_state() const;

    /// 当前会话身份。
    [[nodiscard]] SessionRole session_role() const;

    /// 当前会话身份字符串，用于 UI/日志：none、host、client。
    [[nodiscard]] std::string_view session_role_name() const;

    /// 客户端记录的房主地址；房主或未连接时为空。
    [[nodiscard]] const std::string& host_address() const;

    /// 客户端记录的房主端口；房主或未连接时为 0。
    [[nodiscard]] uint16_t host_port() const;

    /// 当前 ENet 会话监听端口；未启动时为 0。
    [[nodiscard]] uint16_t session_port() const;

    /// 已连接的 peer 数量。
    [[nodiscard]] size_t peer_count() const;

    /// 本机稳定 peer id，用于 LANChat 消息 sender_id/去重。
    [[nodiscard]] std::string local_peer_id() const;

    /// 手动连接到指定 IP 的房主或 peer。
    /// 要求会话已启动。force=true 跳过 ID 排序，由主动方发起连接。
    bool connect_to_peer(const std::string& ip, uint16_t port,
                         const std::string& peer_name);

    /// LANChat 事件回调。参数是可直接传给 window.__coronaEmit 的 JSON 对象字符串。
    void set_lanchat_event_callback(std::function<void(const std::string&)> callback);

    bool lanchat_start_room(const std::string& room_id,
                            const std::string& nickname,
                            uint16_t port);
    bool lanchat_start_local_room(const std::string& room_id,
                                  const std::string& nickname);
    void lanchat_stop_local_room();
    bool lanchat_join_room(const std::string& ip,
                           uint16_t port,
                           const std::string& room_id,
                           const std::string& nickname);
    void lanchat_leave_room();
    Network::LanChatMessageResult lanchat_send_message(const std::string& text);
    Network::LanChatMessageResult lanchat_send_message_ex(
        const std::string& text,
        const std::string& message_kind,
        const std::string& target_agent_id = {},
        const std::string& source_user_id = {},
        const std::string& correlation_id = {},
        const std::string& metadata_json = {});
    Network::LanChatMessageResult lanchat_send_agent_reply(const std::string& agent_id,
                                                           const std::string& agent_name,
                                                           const std::string& text);
    Network::LanChatMessageResult lanchat_send_agent_reply_ex(
        const std::string& agent_id,
        const std::string& agent_name,
        const std::string& text,
        const std::string& sender_type,
        const std::string& message_kind,
        const std::string& target_agent_id = {},
        const std::string& source_user_id = {},
        const std::string& correlation_id = {},
        const std::string& metadata_json = {});
    Network::LanChatMessageResult lanchat_send_system_message_to_host_ex(
        const std::string& sender_id,
        const std::string& sender_name,
        const std::string& text,
        const std::string& message_kind = {},
        const std::string& correlation_id = {},
        const std::string& metadata_json = {});
    Network::LanChatMessageResult lanchat_send_system_message_to_user_ex(
        const std::string& target_user_id,
        const std::string& sender_id,
        const std::string& sender_name,
        const std::string& text,
        const std::string& message_kind = {},
        const std::string& correlation_id = {},
        const std::string& metadata_json = {});
    Network::LanChatResult lanchat_register_agent(const std::string& agent_id,
                                                  const std::string& name,
                                                  const std::string& persona,
                                                  const std::string& owner_id = {});
    Network::LanChatResult lanchat_remove_agent(const std::string& agent_id);
    [[nodiscard]] const std::vector<Network::LanChatMember>& lanchat_members() const;
    [[nodiscard]] const std::vector<Network::LanChatMessage>& lanchat_history() const;
    bool lanchat_restore_history_room(const std::string& room_id);
    [[nodiscard]] std::vector<Network::LanChatHistoryRoomSummary> lanchat_history_rooms() const;
    [[nodiscard]] std::vector<Network::LanChatMessage> lanchat_load_history_room(
        const std::string& room_id,
        size_t max_messages = 5000) const;
    [[nodiscard]] std::vector<Network::LanChatAgent> lanchat_load_history_agents(
        const std::string& room_id) const;
    [[nodiscard]] const std::vector<Network::LanChatAgent>& lanchat_agents() const;
    [[nodiscard]] std::optional<Network::LanChatAgentTrigger> lanchat_pop_agent_trigger();
    [[nodiscard]] std::optional<Network::LanChatMessage> lanchat_pop_coordinator_sync_message();
    [[nodiscard]] std::optional<LanChatRoomEvent> lanchat_pop_room_event();
    [[nodiscard]] std::optional<LanChatSyncEvent> lanchat_pop_sync_event();

    Network::LanChatResult lanchat_lock_object(const std::string& object_id,
                                               const std::string& user_id,
                                               const std::string& operation,
                                               uint64_t now_ms);
    Network::LanChatResult lanchat_unlock_object(const std::string& object_id,
                                                 const std::string& user_id);
    [[nodiscard]] std::string lanchat_locked_by(const std::string& object_id,
                                                uint64_t now_ms);
    void lanchat_broadcast_intent(const std::string& user_id,
                                  const std::string& tooltip,
                                  const std::array<float, 3>& position,
                                  const std::string& status,
                                  uint64_t now_ms);
    [[nodiscard]] std::string lanchat_check_preview_collision(
        const std::string& user_id,
        const std::array<float, 3>& position,
        float delta,
        uint64_t now_ms);

    /**
     * @brief 向所有已连接的 peer 广播 Actor 创建事件。
     * @param actor_guid 稳定 Actor 网络 ID
     * @param scene_name 场景路径（如 "Scene/场景1.scene"）
     * @param model_path 模型相对路径（如 "Resource/ball.obj"）
     * @param transform  9 个 float: position(3) + rotation(3) + scale(3)
     * @param optics_packed 打包的 OpticsPacked 结构 (72 字节)
     * @param optics_size   optics_packed 的大小
     * @param actor_json    完整 Actor 数据 JSON，用于保留别名和基础状态
     */
    void broadcast_actor_create(const std::string& actor_guid,
                                const std::string& scene_name,
                                const std::string& model_path,
                                const std::vector<std::string>& dependency_paths,
                                const float* transform,
                                const void* optics_packed, size_t optics_size,
                                const std::string& actor_json = {});

    /// Broadcast a demo-grade Actor transform delta. Transform is 9 floats:
    /// position(3) + rotation(3, radians) + scale(3).
    void broadcast_actor_transform_update(const std::string& actor_guid,
                                          const std::string& scene_name,
                                          const float* transform,
                                          const std::string& source_user_id = {},
                                          const std::string& correlation_id = {});

    /// Broadcast an Actor deletion by stable network identity.
    void broadcast_actor_delete(const std::string& actor_guid,
                                const std::string& scene_name,
                                const std::string& actor_name);

    void request_actor_scene_snapshot(const std::string& scene_name);

    void broadcast_actor_scene_snapshot(const std::string& scene_name,
                                        const std::string& snapshot_json);

    void broadcast_actor_state_update(const std::string& actor_guid,
                                      const std::string& scene_name,
                                      const std::string& actor_json);

    /// 检查是否有待完成的文件传输（需要在 update 中处理）
    [[nodiscard]] bool has_pending_transfers() const;

    /// 暂停或恢复数据同步（Actor 创建期间暂停以避免 seq_id 碰撞）
    void set_sync_paused(bool paused);

    /// 消费一个待创建的 Actor 数据。返回 true 表示有数据被消费。
    bool pop_pending_actor_create(std::string& actor_guid,
                                  std::string& scene_name, std::string& model_path,
                                  void* actor_packed_out, size_t packed_size,
                                  std::string* actor_json_out = nullptr);

    bool peek_pending_actor_create(std::string& actor_guid,
                                   std::string& scene_name, std::string& model_path,
                                   void* actor_packed_out, size_t packed_size,
                                   std::string* actor_json_out = nullptr) const;
    bool ack_pending_actor_create(const std::string& actor_guid);

    bool pop_pending_actor_transform_update(std::string& actor_guid,
                                            std::string& scene_name,
                                            float* transform_out,
                                            size_t transform_count,
                                            std::string& source_user_id,
                                            std::string& correlation_id);
    bool peek_pending_actor_transform_update(std::string& actor_guid,
                                             std::string& scene_name,
                                             float* transform_out,
                                             size_t transform_count,
                                             std::string& source_user_id,
                                             std::string& correlation_id) const;
    bool ack_pending_actor_transform_update(const std::string& actor_guid);

    bool pop_pending_actor_delete(std::string& actor_guid,
                                  std::string& scene_name,
                                  std::string& actor_name);
    bool peek_pending_actor_delete(std::string& actor_guid,
                                   std::string& scene_name,
                                   std::string& actor_name) const;
    bool ack_pending_actor_delete(const std::string& actor_guid);

    bool pop_pending_actor_scene_snapshot_request(std::string& scene_name);

    bool pop_pending_actor_scene_snapshot(std::string& scene_name,
                                          std::string& snapshot_json);

    bool pop_pending_actor_state_update(std::string& actor_guid,
                                        std::string& scene_name,
                                        std::string& actor_json);
    bool peek_pending_actor_state_update(std::string& actor_guid,
                                         std::string& scene_name,
                                         std::string& actor_json) const;
    bool ack_pending_actor_state_update(const std::string& actor_guid);

    /**
     * @brief 注册稳定 Actor 网络 ID 到本地 SharedDataHub handle 映射。
     *
     * Actor 由 Python/编辑器创建完成后调用。后续同步可以通过 actor_guid
     * 找到本机对应的 Actor/Profile/Geometry/Transform handle，避免跨端 seq_id
     * 不一致导致的协同抖动。
     */
    bool register_actor_identity(const std::string& actor_guid,
                                 std::uintptr_t actor_handle,
                                 bool locally_owned = true);

    /// 声明本端接管指定 Actor 的 transform 发送权，并通知 peer 停止发送它。
    bool claim_actor_ownership(const std::string& actor_guid);

    /// 查询已注册的 Actor 网络身份快照。
    [[nodiscard]] std::optional<Network::ActorNetworkIdentity> resolve_actor_identity(
        const std::string& actor_guid) const;

    /**
     * @brief 设置当前项目的绝对路径（用于文件传输的目标目录）。
     * 接收到的模型文件将写入 active_project_path + model_path。
     * @param project_root 项目根目录的绝对路径
     */
    void set_project_root(const std::string& project_root);

    /**
     * @brief 处理收到的 FILE_CHUNK。由 on_custom_message 调用。
     * 所有 chunk 收齐后自动写入文件。
     */
    void handle_file_chunk(const std::string& sender_peer_id,
                           const uint8_t* data, size_t len);

    /**
     * @brief 响应 FILE_REQUEST，发送请求的文件。
     */
    void handle_file_request(const std::string& sender_peer_id,
                             const uint8_t* data, size_t len);

private:
    // ========================================
    // 内部回调
    // ========================================

    void on_peer_connected(const Network::PeerManager::PeerInfo& info);
    void on_peer_disconnected(const Network::PeerManager::PeerInfo& info);
    void on_data_received(const std::string& peer_id, const uint8_t* data, size_t len);
    void on_custom_message(const std::string& sender_peer_id, const uint8_t* data, size_t len);
    bool is_connected_host_peer(const Network::PeerManager::PeerInfo& info) const;
    bool is_message_from_connected_host(const std::string& sender_peer_id) const;
    bool send_to_connected_host_peer(const std::vector<uint8_t>& packet);
    void notify_lanchat_room_closed();
    void enqueue_lanchat_sync_event(const std::string& event,
                                    const std::string& payload_json);
    void clear_lanchat_room_state();
    void persist_lanchat_message(const Network::LanChatMessage& message);
    void persist_lanchat_agents(const std::string& room_id);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Corona::Systems
