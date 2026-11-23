#pragma once

/// 网络同步的**拥有者线程边界**（P1 / Task 2）。
///
/// 背景：`Ecs::World`（entt::registry）没有任何同步，网络线程既不能读也不能写它。
/// 迁移前 `sync_engine.cpp` 直接在网络线程上遍历 `OpticsDevice` /
/// 按句柄取组件、并把远端状态直写进组件 —— 与 DAG 线程的遍历并发。`Material` /
/// `OpticsDevice` 还是 DAG 私有状态（spec §4.4），不能发布成对外数组，所以"序列化"
/// 本身必须发生在组件所在线程上。
///
/// 这条边界只做两件事，都跑在**拥有者线程**（DAG tick 边界；降级模式下是
/// Geometry 的更新线程）：
///
///   - **出站**：`request_sync_frame()` 把一条"取一帧同步状态"的请求挂到 tick 边界上。
///     拥有者线程遍历组件、编码出线上字节、解析 actor guid，然后把整帧交回等待方；
///     网络线程只负责 dirty 记账、套 LWW 版本、发只读字节。等待复用结构命令通道的
///     **存活性**结算（`StructuralCommandChannel::await`）：DAG 停机或连续无进展预算内
///     没有边界完成才失败，单个慢 tick 继续等，不设任何墙钟预算。
///     调用方必须保证它**不持有** UI 线程也要拿的锁（见 `NetworkSystem::update()`：
///     同步轮询在 `pending_mutex` 之外执行）。
///
///   - **入站**：`submit_remote_state()` 把解码后的远端值入队，边界上一次性应用到组件。
///     它**不阻塞**调用方 —— 网络线程每收到一条编辑操作就等一个 tick，会把一次卡死的
///     代价按载荷条数放大，也会让 ENet 收包落后于 DAG tick（结构命令的销毁走同一取舍：
///     结算不等于调用方阻塞）。但结算规则与结构命令一致：DAG 停机 / 已判定不推进时
///     **立即拒绝**（调用方保留待重试），队列里的命令由边界应用到组件，绝不悬挂。
///     队列有固定上限并按 `(storage, entity_seq)` 合并（同一实体保留最新值），
///     溢出丢最旧并计数；会话停止/引擎关停时整体清空（`reset_sync_boundary_state()`）。
///
/// 队列内部的锁只保护"入队/取走"，不保护组件：组件只在 `apply_pending_sync_boundary()`
/// 里、拥有者线程上被读写。

#include <corona/systems/network/protocol.h>
#include <corona/systems/network/sync_codec.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Corona::Network {

/// 拥有者线程解析 actor guid（网络身份注册表是独立加锁的，可跨线程调用）。
///
/// 仅在 `SyncFrameRequestOptions::identity_index` 为空（旧调用方 / 单线程夹具）时作为
/// 逐实体回退路径；生产路径用索引快照，避免在 tick 边界上 O(actors) 扫描。
using SyncEntityGuidLookup = std::function<std::string(StorageID, std::uint64_t)>;

/// 拥有者线程解析本地所有权；`nullopt` = 没有信息，`false` = 远端拥有（不发）。
using SyncEntityOwnershipLookup =
    std::function<std::optional<bool>(StorageID, std::uint64_t)>;

/// 拥有者线程的脏值探针：只读、不记账。
///
/// 拥有者线程先算哈希、再问"这一行脏吗"；不脏就跳过身份解析与字节编码。真正记账
/// （`set_snapshot`）仍由网络线程在发送时完成，因此一次被放弃的帧不会留下"已同步"
/// 的假记录。
using SyncDirtyProbe =
    std::function<bool(StorageID, std::uint64_t entity_seq, std::uint64_t hash)>;

/// 一帧出站状态里的一行：`value` 是拥有者线程编码好的线上字节。
struct SyncFrameRow {
    std::uint64_t entity_seq{0};
    /// 仅 transform / optics 有值（拥有者线程从身份索引快照解析）。
    std::string actor_guid;
    /// 脏值哈希（只用于内存里的脏值判定，不上线；见 `sync_codec.h` 的说明）。
    std::uint64_t hash{0};
    /// 线上值字节（36 / 84 / 20 字节）；网络线程原样拷进 EditorSyncOperation。
    std::vector<std::uint8_t> value;
};

/// 一帧出站同步状态（按组件种类分组，发送顺序与迁移前一致）。
struct SyncFrame {
    std::vector<SyncFrameRow> transforms;
    std::vector<SyncFrameRow> optics;
    std::vector<SyncFrameRow> environments;
};

/// 请求一帧时携带的上下文。
struct SyncFrameRequestOptions {
    /// 本帧的身份索引快照（网络线程每帧取一次，O(actors) 一次而不是每实体一次）。
    /// 为空时回退到下面两个逐实体回调（旧调用方 / 单线程夹具）。
    std::vector<SyncIdentityEntry> identity_index;
    SyncEntityGuidLookup guid_for_entity;
    SyncEntityOwnershipLookup ownership_for_entity;
    /// 脏值探针；为空表示"全部视为脏"（只有测试/装配前会走到）。
    SyncDirtyProbe is_dirty;
};

/// 一条入站远端状态：网络线程已解码/校验，拥有者线程按 `storage` 应用。
struct RemoteStateWrite {
    StorageID storage{StorageID::ST_MODEL_TRANSFORM};
    /// 目标实体序号（`Ecs::seq_of(handle)` 的反向值）；environment 忽略它（取第一个
    /// EnvironmentDevice，与迁移前的"任意环境槽位"语义一致）。
    std::uint64_t entity_seq{0};
    TransformState transform;
    OpticsPacked optics;
    EnvironmentState environment;
};

/// 诊断计数（进程内累计；`reset_sync_boundary_stats()` 供测试与测量用）。
struct SyncBoundaryStats {
    std::uint64_t frames_requested{0};   ///< 网络线程发起的帧请求数
    std::uint64_t frames_built{0};       ///< 拥有者线程在边界上构建并结算的帧数
    std::uint64_t frames_inline{0};      ///< 装配前/单线程夹具里直接构建的帧数
    std::uint64_t frames_rejected{0};    ///< 停机/闩锁导致立即失败的帧请求数
    std::uint64_t frames_failed{0};      ///< 已入队但等待失败（停机结算）的帧请求数
    std::uint64_t frames_abandoned{0};   ///< 等待方放弃（无进展预算/闩锁）的帧请求数
    std::uint64_t frames_skipped{0};     ///< 拥有者跳过已结算（放弃/停机）帧请求数
    std::uint64_t remote_writes_submitted{0};  ///< 入队/直接应用的远端状态条数
    std::uint64_t remote_writes_applied{0};    ///< 边界上成功应用到组件的条数
    std::uint64_t remote_writes_dropped{0};    ///< 目标实体不存在而丢弃的条数
    std::uint64_t remote_writes_rejected{0};   ///< 停机/闩锁导致立即拒绝的条数
    std::uint64_t remote_writes_coalesced{0};  ///< 同 key 被新值覆盖（未入队）的条数
    std::uint64_t remote_writes_evicted{0};    ///< 队列达到上限时丢最旧的条数
    std::uint64_t remote_writes_pending{0};    ///< 当前排队的远端状态条数（快照值）
};

/// 网络线程（任意非拥有者线程）：请求一帧当前同步状态。
///
/// 返回 `nullopt` 表示 DAG 已停机 / 已判定不推进（已打开 `DAG_UNRESPONSIVE` 闩锁）：
/// 本 tick 不序列化、不发送，下一 tick 再试。绝不从调用线程读 registry。
/// 调用方不得持有 UI 线程也要获取的锁（本函数可能等待一个 tick 边界）。
[[nodiscard]] std::optional<SyncFrame> request_sync_frame(
    SyncFrameRequestOptions options,
    std::string_view what = "network_sync_frame");

/// 网络线程：提交一条入站远端状态，由下一个 tick 边界应用到组件。
///
/// 返回 false = 立即拒绝（DAG 停机 / 已判定不推进）；调用方应保留这条操作待重试
/// （与迁移前的 `pending_editor_operations` 保持同一语义）。入队成功返回 true ——
/// 应用是异步的，目标实体不存在时该条会被丢弃并计入 `remote_writes_dropped`。
/// 同一 `(storage, entity_seq)` 的连续写入只保留最新一条（计入
/// `remote_writes_coalesced`）；队列达到 4096 条时丢最旧（计入
/// `remote_writes_evicted`），不把溢出推给 `pending_editor_operations`。
[[nodiscard]] bool submit_remote_state(RemoteStateWrite write);

/// 拥有者线程：tick 边界收口。先应用入站远端状态，再服务出站帧请求（同一边界内，
/// 帧看到的是本 tick 已应用的远端值）。
///
/// 由 `Corona::Ecs::StructuralCommandChannel::apply_pending()` 注册的边界任务调用，
/// 因此在 DAG 模式（engine.cpp 的边界钩子）与 `CORONA_SYSTEMS_DAG=0` 降级模式
/// （GeometrySystem 的兜底 drain）下都在同一个拥有者线程上执行。
void apply_pending_sync_boundary();

/// 会话停止 / 引擎关停：清空尚未应用的入站队列与未服务的帧请求。
///
/// 只应由网络线程（`SyncEngine::shutdown()`）或拥有者线程已停止之后调用。清空是
/// **必须**的：下一个会话的实体序号可能复用，旧会话的远端写入不能落到新会话上。
void reset_sync_boundary_state();

// ---- 诊断 ----

[[nodiscard]] SyncBoundaryStats sync_boundary_stats();
void reset_sync_boundary_stats();

}  // namespace Corona::Network
