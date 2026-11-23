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
///
///   - **入站**：`submit_remote_state()` 把解码后的远端值入队，边界上一次性应用到组件。
///     它**不阻塞**调用方 —— 网络线程每收到一条编辑操作就等一个 tick，会把一次卡死的
///     代价按载荷条数放大，也会让 ENet 收包落后于 DAG tick（结构命令的销毁走同一取舍：
///     结算不等于调用方阻塞）。但结算规则与结构命令一致：DAG 停机 / 已判定不推进时
///     **立即拒绝**（调用方保留待重试），队列里的命令由边界应用到组件，绝不悬挂。
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
using SyncEntityGuidLookup = std::function<std::string(StorageID, std::uint64_t)>;

/// 拥有者线程解析本地所有权；`nullopt` = 没有信息，`false` = 远端拥有（不发）。
using SyncEntityOwnershipLookup =
    std::function<std::optional<bool>(StorageID, std::uint64_t)>;

/// 一帧出站状态里的一行：`value` 是拥有者线程编码好的线上字节。
struct SyncFrameRow {
    std::uint64_t entity_seq{0};
    /// 仅 transform / optics 有值（拥有者线程用身份注册表解析）。
    std::string actor_guid;
    /// 与迁移前 `hash_mt` / `hash_opt` / `hash_env` 逐字节一致的脏值哈希。
    std::string hash;
    /// 线上值字节（36 / 84 / 20 字节）；网络线程原样拷进 EditorSyncOperation。
    std::vector<std::uint8_t> value;
};

/// 一帧出站同步状态（按组件种类分组，发送顺序与迁移前一致）。
struct SyncFrame {
    std::vector<SyncFrameRow> transforms;
    std::vector<SyncFrameRow> optics;
    std::vector<SyncFrameRow> environments;
};

/// 请求一帧时需要拥有者线程执行的身份解析（来自 `SyncEngine` 的回调）。
struct SyncFrameRequestOptions {
    SyncEntityGuidLookup guid_for_entity;
    SyncEntityOwnershipLookup ownership_for_entity;
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
    std::uint64_t frames_abandoned{0};   ///< 等待方放弃后拥有者跳过的帧请求数
    std::uint64_t remote_writes_submitted{0};  ///< 入队/直接应用的远端状态条数
    std::uint64_t remote_writes_applied{0};    ///< 边界上成功应用到组件的条数
    std::uint64_t remote_writes_dropped{0};    ///< 目标实体不存在而丢弃的条数
    std::uint64_t remote_writes_rejected{0};   ///< 停机/闩锁导致立即拒绝的条数
};

/// 网络线程（任意非拥有者线程）：请求一帧当前同步状态。
///
/// 返回 `nullopt` 表示 DAG 已停机 / 已判定不推进（已打开 `DAG_UNRESPONSIVE` 闩锁）：
/// 本 tick 不序列化、不发送，下一 tick 再试。绝不从调用线程读 registry。
[[nodiscard]] std::optional<SyncFrame> request_sync_frame(
    SyncFrameRequestOptions options,
    std::string_view what = "network_sync_frame");

/// 网络线程：提交一条入站远端状态，由下一个 tick 边界应用到组件。
///
/// 返回 false = 立即拒绝（DAG 停机 / 已判定不推进）；调用方应保留这条操作待重试
/// （与迁移前的 `pending_editor_operations` 保持同一语义）。入队成功返回 true ——
/// 应用是异步的，目标实体不存在时该条会被丢弃并计入 `remote_writes_dropped`。
[[nodiscard]] bool submit_remote_state(RemoteStateWrite write);

/// 拥有者线程：tick 边界收口。先应用入站远端状态，再服务出站帧请求（同一边界内，
/// 帧看到的是本 tick 已应用的远端值）。
///
/// 由 `Corona::Ecs::StructuralCommandChannel::apply_pending()` 注册的边界任务调用，
/// 因此在 DAG 模式（engine.cpp 的边界钩子）与 `CORONA_SYSTEMS_DAG=0` 降级模式
/// （GeometrySystem 的兜底 drain）下都在同一个拥有者线程上执行。
void apply_pending_sync_boundary();

// ---- 诊断 ----

[[nodiscard]] SyncBoundaryStats sync_boundary_stats();
void reset_sync_boundary_stats();

}  // namespace Corona::Network
