#pragma once

/// NetworkSystem 的跨线程交互边界。
///
/// CEF（编辑器线程）此前直接调用 `NetworkSystem` 的方法约 40 次。按语义拆开：
///   - **读**  → `NetworkStateChannel`（快照，由 `NetworkSystem::update` 发布）
///   - **写**  → `NetworkCommandChannel`（Fifo，`NetworkSystem::update` 开头 drain）
///   - **请求/应答** → 命令 + `NetworkReplyQueue`（有界同步等待）
///
/// 数据面 `peek_pending_actor_*` / `ack_pending_actor_*` 一族不在这里：它本身就是
/// NetworkSystem 内部 mutex 保护的"生产者 → 消费者"队列，不是直写共享状态。
///
/// 注：旧的内置聊天子系统已在远端 `c22e74de refactor(network)!` 中移除，
/// 本文件不再承载该子系统的任何命令。

#include <corona/kernel/command/command_buffer.h>
#include <corona/kernel/command/snapshot_channel.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
    std::string local_peer_id;      ///< 本机在网络里的稳定 ID
    std::uint16_t host_port{0};
    std::uint16_t listen_port{0};
    std::vector<NetworkPeerRow> peers;
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
    // ---- 需要回执的分支（request_id != 0 时应用点会 push 一条 NetworkCommandResult）----
    RegisterActorIdentity,          ///< guid + handle + flag(locally_owned)
    ClaimActorOwnership,            ///< guid
    StartSession,                   ///< params: instance_name, project_id, port, role
    StartDiscovery,                 ///< params: instance_name, project_id
    ConnectToPeer,                  ///< params: ip, name; port 走 command.port
    // ---- 协作对象锁（聊天子系统移除后的通用替代 API）----
    BroadcastIntent,                ///< params: user_id, tooltip, status, now_ms; position 走 command.position
    LockObject,                     ///< params: object_id, user_id, operation, now_ms —— 需要回执
    UnlockObject,                   ///< params: object_id, user_id —— 需要回执
    LockedBy,                       ///< params: object_id, now_ms —— 回执 text
    CheckPreviewCollision,          ///< params: user_id, delta, now_ms; position 走 command.position —— 回执 text
};

struct NetworkCommand {
    NetworkCommandKind kind{NetworkCommandKind::SetSyncPaused};
    bool flag{false};

    std::string guid;            ///< actor_guid
    std::string scene;           ///< scene_name
    std::string name;            ///< actor_name / project_root / instance_name / model_path
    std::string json;            ///< actor_json / snapshot_json
    std::string source_user_id;
    std::string correlation_id;

    std::array<float, 9> transform{};
    std::array<float, 3> position{};   ///< broadcast_intent / check_preview_collision
    std::uint64_t now_ms{0};
    std::uint64_t handle{0};        ///< register_actor_identity 的 actor_handle
    std::uint16_t port{0};          ///< start_session / connect_to_peer

    /// 0 = 不需要回执（fire-and-forget）。非 0 时应用点会回一条
    /// `NetworkCommandResult{request_id, ok}`。
    std::uint64_t request_id{0};

    /// 结果型命令的形参（语义见 NetworkCommandKind 上的注释）。
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
    /// 标量字符串结果（目前未被使用，保留给后续只返回一个字符串的查询）。
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
