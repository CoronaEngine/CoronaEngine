#pragma once

/// NetworkSystem 的跨线程交互边界。
///
/// CEF（编辑器线程）此前直接调用 `NetworkSystem` 的方法约 40 次。按语义拆开：
///   - **读**  → `NetworkStateChannel`（快照，每 tick 发布）
///   - **写**  → `NetworkCommandChannel`（Fifo，NetworkSystem::update 开头 drain）
///   - **请求/应答** → 仍留在同步 API（返回值要进 JSON 回包），见迁移文档 §2.1
///
/// 数据面 `peek_pending_actor_create` 一族不在这里：它本身就是 NetworkSystem
/// 内部 mutex 保护的"生产者 → 消费者"队列，不是直写共享状态。

#include <corona/kernel/command/command_buffer.h>
#include <corona/kernel/command/snapshot_channel.h>
#include <corona/systems/network/lanchat_state.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Corona::Network {

/// 发现到的一个对端（CEF 的网络面板直接渲染这些字段）
struct NetworkPeerRow {
    std::string ip;
    std::string name;
    std::string role;       ///< none / host / client
    std::string stable_id;
    std::uint16_t port{0};
    std::uint64_t project_id{0};
    std::uint64_t last_seen_ms{0};
};

/// 会话状态投影。只放宽需要被编辑器看到的字段。
struct NetworkStateSnapshot {
    bool active{false};
    std::string role;               ///< none / host / client
    std::size_t peer_count{0};
    std::string host_address;
    std::string local_peer_id;      ///< 本机在网络里的稳定 ID（LanChat 回包用）
    std::uint16_t host_port{0};
    std::uint16_t listen_port{0};
    std::vector<NetworkPeerRow> peers;
};

/// LanChat 面板/历史查询的结果投影。
///
/// 刻意**不做每 tick 发布**：history 可能有上千条消息，每帧深拷贝不可接受。
/// 改为"按需拉取"——CEF 投一条查询命令，网络线程填好后发布，调用方等回执后再读。
/// 单生产者单消费者 + 回执先行，所以覆盖式发布是安全的。
struct LanChatQuerySnapshot {
    std::vector<LanChatMember> members;
    std::vector<LanChatMessage> history;
    std::vector<LanChatAgent> agents;
    std::vector<LanChatHistoryRoomSummary> history_rooms;
};

class LanChatQueryChannel {
   public:
    static LanChatQueryChannel& instance() {
        static LanChatQueryChannel channel;
        return channel;
    }

    [[nodiscard]] Kernel::SnapshotChannel<LanChatQuerySnapshot>& snapshots() { return snapshots_; }

   private:
    Kernel::SnapshotChannel<LanChatQuerySnapshot> snapshots_;
};

class NetworkStateChannel {
   public:
    static NetworkStateChannel& instance() {
        static NetworkStateChannel channel;
        return channel;
    }

    [[nodiscard]] Kernel::SnapshotChannel<NetworkStateSnapshot>& snapshots() { return snapshots_; }

   private:
    Kernel::SnapshotChannel<NetworkStateSnapshot> snapshots_;
};

/// 只写控制命令：全部对应 `NetworkSystem` 中返回 void 的方法。
enum class NetworkCommandKind : std::uint8_t {
    StopSession,                    ///< —（void，收尾动作，顺序必须排在其它命令之后）
    SetSyncPaused,                  ///< flag
    SetProjectRoot,                 ///< name
    ClearDiscoveredPeers,           ///< —
    BroadcastActorTransformUpdate,  ///< guid + scene + transform + source_user_id + correlation_id
    BroadcastActorDelete,           ///< guid + scene + name
    BroadcastActorStateUpdate,      ///< guid + scene + json
    BroadcastActorCreate,           ///< guid + scene + name(model_path) + json + transform + params(deps) + blob(ActorCreatePacked)
    RequestActorSceneSnapshot,      ///< scene
    BroadcastActorSceneSnapshot,    ///< scene + json
    LanChatBroadcastIntent,         ///< user_id + name(tooltip) + position + json(status) + now_ms
    // ---- 需要回执的分支（request_id != 0 时应用点会 push 一条 NetworkCommandResult）----
    RegisterActorIdentity,          ///< guid + handle + flag(locally_owned)
    ClaimActorOwnership,            ///< guid
    LanChatStartRoom,               ///< scene(room_id) + name(nickname) + port
    LanChatStartLocalRoom,          ///< scene(room_id) + name(nickname)
    LanChatJoinRoom,                ///< guid(ip) + port + scene(room_id) + name(nickname)
    LanChatRestoreHistoryRoom,      ///< scene(room_id)
    // ---- 无回执 ----
    LanChatStopLocalRoom,           ///< —
    LanChatLeaveRoom,               ///< —
    SetLanChatEventCallback,        ///< callback
    // ---- 结果型：params 按注释顺序承载实参 ----
    StartSession,                   ///< params[0]=project_root（可空）
    StartDiscovery,                 ///< —
    LanChatSendMessage,             ///< text, kind, target_agent, source_user, correlation, metadata
    LanChatSendAgentReply,          ///< agent_id, agent_name, text, sender_type, kind, target, source_user, correlation, metadata
    LanChatSendSystemToHost,        ///< sender_id, sender_name, text, kind, correlation, metadata
    LanChatSendSystemToUser,        ///< target_user, sender_id, sender_name, text, kind, correlation, metadata
    LanChatRegisterAgent,           ///< agent_id, name, persona, owner_id
    LanChatRemoveAgent,             ///< agent_id
    LanChatLockObject,              ///< object_id, user_id, operation, now_ms(十进制串)
    LanChatUnlockObject,            ///< object_id, user_id
    // ---- 面板/历史查询：结果发布到 LanChatQueryChannel ----
    RefreshLanChatPanel,            ///< —
    LoadLanChatRoomHistory,         ///< params[0]=room_id params[1]=max_messages
    ConnectToPeer,                  ///< params[0]=ip params[1]=name, port 走 command.port
    LanChatLockedBy,                ///< params[0]=object_id params[1]=now_ms
    LanChatCheckPreviewCollision,   ///< params[0]=user_id params[1]=delta params[2]=now_ms, position 走 command.position
};

struct NetworkCommand {
    NetworkCommandKind kind{NetworkCommandKind::SetSyncPaused};
    bool flag{false};

    std::string guid;            ///< actor_guid / object_id
    std::string scene;           ///< scene_name
    std::string name;            ///< actor_name / tooltip / project_root
    std::string json;            ///< actor_json / snapshot_json / status
    std::string user_id;
    std::string source_user_id;
    std::string correlation_id;

    std::array<float, 9> transform{};
    std::array<float, 3> position{};
    std::uint64_t now_ms{0};
    std::uint64_t handle{0};        ///< register_actor_identity 的 actor_handle
    std::uint16_t port{0};          ///< lanchat_start_room / lanchat_join_room

    /// 0 = 不需要回执（fire-and-forget）。非 0 时应用点会回一条
    /// `NetworkCommandResult{request_id, ok}`。
    std::uint64_t request_id{0};

    std::function<void(const LanChatEvent&)> lanchat_event_callback;

    /// 结果型命令的形参（语义见 NetworkCommandKind 上的注释）。用变长列表
    /// 是为了不改动那些 6~9 个字符串形参的既有签名。
    std::vector<std::string> params;

    /// 二进制负载（`broadcast_actor_create` 的 `ActorCreatePacked`）。
    /// 直接存字节而不是存指针：命令是异步应用的，传指针会悬空。
    std::vector<std::uint8_t> blob;
};

struct NetworkCommandKey {
    static std::uintptr_t get(const NetworkCommand&) noexcept { return 0; }
};

class NetworkCommandChannel {
   public:
    /// Fifo：控制命令的顺序有意义（start → register → broadcast 不能重排）。
    using Buffer = Kernel::CommandBuffer<NetworkCommand, NetworkCommandKey,
                                         Kernel::CoalescePolicy::Fifo>;

    static NetworkCommandChannel& instance() {
        static NetworkCommandChannel channel;
        return channel;
    }

    [[nodiscard]] Buffer& commands() { return commands_; }

   private:
    Buffer commands_;
};

/// 入队一条控制命令（任意线程）。
inline void push_network_command(NetworkCommand command) {
    NetworkCommandChannel::instance().commands().push(std::move(command));
}

/// 命令回执。只有需要知道结果的调用方（编辑器 native API）才会等它。
struct NetworkCommandResult {
    std::uint64_t request_id{0};
    bool ok{false};
    std::string error;  ///< 失败原因（可空）
    /// 消息类命令（send_*）额外带回被接受的消息本体。
    bool has_message{false};
    LanChatMessage message;
    /// 标量字符串结果（lanchat_locked_by / check_preview_collision）。
    std::string text;
};

/// 有界同步等待的默认上限。NetworkSystem 每 tick 处理一次命令，
/// 60Hz 下典型延迟 < 20ms；150ms 足够覆盖一次卡顿又不会让编辑器明显顿住。
inline constexpr std::chrono::milliseconds kNetworkReplyTimeout{150};

class NetworkReplyQueue {
   public:
    static NetworkReplyQueue& instance() {
        static NetworkReplyQueue queue;
        return queue;
    }

    void push(NetworkCommandResult result) {
        {
            std::lock_guard lock(mutex_);
            pending_.push_back(std::move(result));
        }
        cv_.notify_all();
    }

    /// 等某个 request_id 的回执；超时返回 nullopt。
    std::optional<NetworkCommandResult> wait(std::uint64_t request_id,
                                             std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        const auto match = [request_id](const NetworkCommandResult& result) {
            return result.request_id == request_id;
        };
        if (!cv_.wait_for(lock, timeout, [&] {
                return std::any_of(pending_.begin(), pending_.end(), match);
            })) {
            return std::nullopt;
        }
        const auto it = std::find_if(pending_.begin(), pending_.end(), match);
        auto result = std::move(*it);
        pending_.erase(it);
        return result;
    }

    /// 超时后清掉迟到的回执，避免队列无限增长。
    void discard(std::uint64_t request_id) {
        std::lock_guard lock(mutex_);
        std::erase_if(pending_, [request_id](const NetworkCommandResult& result) {
            return result.request_id == request_id;
        });
    }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<NetworkCommandResult> pending_;
};

/// 投递一条需要回执的命令并等待结果。
/// 这是"跨线程直接调用"的替代品：命令走通道，结果走回执队列，
/// 调用方只阻塞在回执上（有超时），不再去摸 NetworkSystem 内部。
inline std::optional<NetworkCommandResult> submit_and_wait(
    NetworkCommand command, std::chrono::milliseconds timeout = kNetworkReplyTimeout) {
    static std::atomic<std::uint64_t> sequence{0};
    command.request_id = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto request_id = command.request_id;
    push_network_command(std::move(command));

    auto result = NetworkReplyQueue::instance().wait(request_id, timeout);
    if (!result) {
        NetworkReplyQueue::instance().discard(request_id);
    }
    return result;
}

}  // namespace Corona::Network
