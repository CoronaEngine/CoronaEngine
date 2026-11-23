#include <corona/systems/network/sync_boundary.h>

#include <corona/ecs/access.h>
#include <corona/ecs/components.h>
#include <corona/ecs/structural_commands.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/optics/optics_commands.h>

#include <horizon/core/logging.h>

#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Corona::Network {
namespace {

/// 远程 optics 包承载整份材质快照，因此应用时覆盖全部 16 个标量/可见性位。
constexpr auto kRemoteOpticsFields =
    Corona::OpticsField::Visible | Corona::OpticsField::EnableLighting |
    Corona::OpticsField::Metallic | Corona::OpticsField::Roughness |
    Corona::OpticsField::Subsurface | Corona::OpticsField::Specular |
    Corona::OpticsField::SpecularTint | Corona::OpticsField::Anisotropic |
    Corona::OpticsField::Sheen | Corona::OpticsField::SheenTint |
    Corona::OpticsField::Clearcoat | Corona::OpticsField::ClearcoatGloss |
    Corona::OpticsField::Ambient | Corona::OpticsField::Diffuse |
    Corona::OpticsField::SpecularColor | Corona::OpticsField::Shininess;

/// 入站队列上限（与 `Kernel::CommandBuffer` 的 4096 同一先例）。
///
/// 队列按 `(storage, entity_seq)` 合并，所以正常情况下规模不超过"本端有多少个可被
/// 远端写入的实体"；4096 是防御"对端刷同一个/不同 key"的安全阀，溢出丢最旧并计数，
/// 绝不把溢出推回 `pending_editor_operations`（那条队列没有上限）。
constexpr std::size_t kMaxQueuedRemoteWrites = 4096;

/// `Ecs::seq_of(handle)` 的反向映射：对外序号就是实体索引，所以句柄 = seq + 1。
///
/// 迁移前网络线程靠 `rebuild_entity_maps()` 把 `seq → handle` 建表，而那张表的值
/// 恒等于 `seq + 1`（`Ecs::seq_of` 的定义就是 `handle - 1`）。直接算，网络线程
/// 就再也不需要遍历 registry 来解析句柄了。
[[nodiscard]] Corona::EntityId handle_for_seq(std::uint64_t entity_seq) noexcept {
    return static_cast<Corona::EntityId>(entity_seq + 1);
}

/// 一次出站帧请求的结算槽 + 结果槽。
///
/// 结算本身复用结构命令的 `StructuralRequest`（存活判据、闩锁、停机结算都在那里），
/// 因此 `request_sync_frame()` 的外部行为与 `request_create_*` 完全同一套规则。
struct FrameExchange {
    explicit FrameExchange(std::uint64_t id) : slot(id) {}

    Corona::Ecs::StructuralRequest slot;
    /// 拥有者线程在持槽锁期间写入；等待方只在槽结算之后读。
    SyncFrame frame;
};

struct FrameRequestCommand {
    std::shared_ptr<FrameExchange> exchange;
    SyncFrameRequestOptions options;
    std::string what;
};

/// 入站写入的合并键：`storage` 高 8 位 + 实体序号。
///
/// 实体序号是 63 位（`Ecs::seq_of`），这里取低 56 位已经远超任何真实场景；高位放
/// storage 保证不同种类的写入不会互相覆盖。
[[nodiscard]] std::uint64_t write_key(const RemoteStateWrite& write) noexcept {
    return (static_cast<std::uint64_t>(write.storage) << 56) |
           (write.entity_seq & 0x00FFFFFFFFFFFFFFull);
}

[[nodiscard]] std::uint64_t identity_key(StorageID storage,
                                         std::uint64_t entity_seq) noexcept {
    return (static_cast<std::uint64_t>(storage) << 56) |
           (entity_seq & 0x00FFFFFFFFFFFFFFull);
}

// ---------------------------------------------------------------------------
// 拥有者线程：入站远端状态 → 组件
// ---------------------------------------------------------------------------

[[nodiscard]] bool apply_remote_state(const RemoteStateWrite& write) {
    switch (write.storage) {
    case StorageID::ST_MODEL_TRANSFORM: {
        auto* transform = Corona::Ecs::try_get<Corona::ModelTransform>(
            handle_for_seq(write.entity_seq));
        if (transform == nullptr) {
            return false;
        }
        transform->position = ktm::fvec3{write.transform.xform[0],
                                         write.transform.xform[1],
                                         write.transform.xform[2]};
        transform->euler_rotation = ktm::fvec3{write.transform.xform[3],
                                               write.transform.xform[4],
                                               write.transform.xform[5]};
        transform->scale = ktm::fvec3{write.transform.xform[6],
                                      write.transform.xform[7],
                                      write.transform.xform[8]};
        return true;
    }
    case StorageID::ST_OPTICS: {
        const auto handle = handle_for_seq(write.entity_seq);
        auto* device = Corona::Ecs::try_get<Corona::OpticsDevice>(handle);
        auto* visibility = Corona::Ecs::try_get<Corona::Ecs::Visibility>(handle);
        if (device == nullptr || visibility == nullptr) {
            return false;
        }
        Corona::OpticsUpdateCommand command{};
        command.optics_handle = handle;
        // 远程包是整份材质快照：16 个标量/可见性位全置；材质句柄不参与同步。
        command.fields = kRemoteOpticsFields;
        command.visible = write.optics.visible;
        command.enable_lighting = write.optics.bEnableLighting;
        command.metallic = write.optics.metallic;
        command.roughness = write.optics.roughness;
        command.subsurface = write.optics.subsurface;
        command.specular = write.optics.specular;
        command.specular_tint = write.optics.specularTint;
        command.anisotropic = write.optics.anisotropic;
        command.sheen = write.optics.sheen;
        command.sheen_tint = write.optics.sheenTint;
        command.clearcoat = write.optics.clearcoat;
        command.clearcoat_gloss = write.optics.clearcoatGloss;
        command.ambient = ktm::fvec3{write.optics.ambient[0], write.optics.ambient[1],
                                     write.optics.ambient[2]};
        command.diffuse = ktm::fvec3{write.optics.diffuse[0], write.optics.diffuse[1],
                                     write.optics.diffuse[2]};
        command.specular_color =
            ktm::fvec3{write.optics.specular_color[0], write.optics.specular_color[1],
                       write.optics.specular_color[2]};
        command.shininess = write.optics.shininess;
        Corona::apply_optics_update(*device, *visibility, command);
        return true;
    }
    case StorageID::ST_ENVIRONMENT: {
        // `__scene__` 的 environment 操作不带实体序号（迁移前同样取"任意一个"
        // EnvironmentDevice）；没有环境实体就丢弃，等下一次全量快照。
        for (const auto entity : Corona::Ecs::view<Corona::EnvironmentDevice>()) {
            auto& device = Corona::Ecs::get<Corona::EnvironmentDevice>(
                Corona::Ecs::to_handle(entity));
            device.sun_position = ktm::fvec3{write.environment.sun_position[0],
                                             write.environment.sun_position[1],
                                             write.environment.sun_position[2]};
            device.sun_intensity = write.environment.sun_intensity;
            device.exposure = write.environment.exposure;
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// 拥有者线程：组件 → 一帧出站状态（唯一的 registry 读取点）
// ---------------------------------------------------------------------------

/// 身份索引（每帧一次）：key = `storage << 56 | seq`，值 = 该行的 guid/所有权。
///
/// 网络线程在请求帧时把注册表快照放进 `options.identity_index`，这里只做 O(1) 查表；
/// 索引为空时退回逐实体回调（旧调用方 / 单线程夹具），那条路径的复杂度才是 O(actors)。
using IdentityIndex = std::unordered_map<std::uint64_t, const SyncIdentityEntry*>;

[[nodiscard]] IdentityIndex make_identity_index(const SyncFrameRequestOptions& options) {
    IdentityIndex index;
    index.reserve(options.identity_index.size());
    for (const auto& entry : options.identity_index) {
        index.emplace(identity_key(entry.storage, entry.entity_seq), &entry);
    }
    return index;
}

struct ResolvedIdentity {
    std::string actor_guid;
    bool locally_owned{true};
};

[[nodiscard]] std::optional<ResolvedIdentity> resolve_identity(
    const SyncFrameRequestOptions& options, const IdentityIndex& index, StorageID storage,
    std::uint64_t entity_seq) {
    if (!options.identity_index.empty()) {
        const auto it = index.find(identity_key(storage, entity_seq));
        if (it == index.end()) {
            return std::nullopt;
        }
        return ResolvedIdentity{it->second->actor_guid, it->second->locally_owned};
    }
    // 回退路径：逐实体回调（没有索引快照的旧调用方 / 单线程夹具）。
    if (!options.guid_for_entity) {
        return std::nullopt;
    }
    auto actor_guid = options.guid_for_entity(storage, entity_seq);
    if (actor_guid.empty()) {
        return std::nullopt;
    }
    bool locally_owned = true;
    if (options.ownership_for_entity) {
        const auto owned = options.ownership_for_entity(storage, entity_seq);
        if (owned) {
            locally_owned = *owned;
        }
    }
    return ResolvedIdentity{std::move(actor_guid), locally_owned};
}

[[nodiscard]] SyncFrame build_frame(const SyncFrameRequestOptions& options) {
    SyncFrame frame;
    // 身份索引表按需构建：干净帧（稳态下大多数帧）一行都不脏，就不该为它分配
    // 一张 (storage, seq) 表。dirty 行出现时才建一次，之后 O(1) 查表。
    IdentityIndex index;
    bool index_ready = false;
    auto resolve = [&options, &index, &index_ready](StorageID storage,
                                                    std::uint64_t entity_seq)
        -> std::optional<ResolvedIdentity> {
        if (!options.identity_index.empty() && !index_ready) {
            index = make_identity_index(options);
            index_ready = true;
        }
        return resolve_identity(options, index, storage, entity_seq);
    };

    // 顺序与迁移前一致：transform → optics → environment。每个实体先算哈希、问一次
    // "脏吗"（网络线程的 `last_synced`，只读探针），不脏就跳过身份解析与字节编码；
    // 脏了才查身份索引、检查所有权、编码线上字节。
    for (const auto entity : Corona::Ecs::view<Corona::ModelTransform>()) {
        const auto handle = Corona::Ecs::to_handle(entity);
        const auto entity_seq = static_cast<std::uint64_t>(Corona::Ecs::seq_of(handle));
        const auto* data = Corona::Ecs::try_get<Corona::ModelTransform>(handle);
        if (data == nullptr) {
            continue;
        }
        TransformState state{};
        state.xform[0] = data->position.x;
        state.xform[1] = data->position.y;
        state.xform[2] = data->position.z;
        state.xform[3] = data->euler_rotation.x;
        state.xform[4] = data->euler_rotation.y;
        state.xform[5] = data->euler_rotation.z;
        state.xform[6] = data->scale.x;
        state.xform[7] = data->scale.y;
        state.xform[8] = data->scale.z;
        const auto hash = hash_transform(state, entity_seq);
        if (options.is_dirty &&
            !options.is_dirty(StorageID::ST_MODEL_TRANSFORM, entity_seq, hash)) {
            continue;
        }
        auto identity = resolve(StorageID::ST_MODEL_TRANSFORM, entity_seq);
        if (!identity || !identity->locally_owned) {
            continue;
        }
        SyncFrameRow row;
        row.entity_seq = entity_seq;
        row.actor_guid = std::move(identity->actor_guid);
        row.hash = hash;
        row.value = encode_transform(state);
        frame.transforms.push_back(std::move(row));
    }

    for (const auto entity : Corona::Ecs::view<Corona::OpticsDevice>()) {
        const auto handle = Corona::Ecs::to_handle(entity);
        const auto entity_seq = static_cast<std::uint64_t>(Corona::Ecs::seq_of(handle));
        const auto* data = Corona::Ecs::try_get<Corona::OpticsDevice>(handle);
        if (data == nullptr) {
            continue;
        }
        const auto* visibility = Corona::Ecs::try_get<Corona::Ecs::Visibility>(handle);

        OpticsPacked packed{};
        // `OpticsPacked` 的默认值就是迁移前"没有 Visibility 时"的默认值。
        if (visibility != nullptr) {
            packed.visible = visibility->visible;
            packed.bEnableLighting = visibility->receives_lighting;
        }
        packed.metallic = data->metallic;
        packed.roughness = data->roughness;
        packed.subsurface = data->subsurface;
        packed.specular = data->specular;
        packed.specularTint = data->specularTint;
        packed.anisotropic = data->anisotropic;
        packed.sheen = data->sheen;
        packed.sheenTint = data->sheenTint;
        packed.clearcoat = data->clearcoat;
        packed.clearcoatGloss = data->clearcoatGloss;
        packed.ambient[0] = data->ambient.x;
        packed.ambient[1] = data->ambient.y;
        packed.ambient[2] = data->ambient.z;
        packed.diffuse[0] = data->diffuse.x;
        packed.diffuse[1] = data->diffuse.y;
        packed.diffuse[2] = data->diffuse.z;
        packed.specular_color[0] = data->specular_color.x;
        packed.specular_color[1] = data->specular_color.y;
        packed.specular_color[2] = data->specular_color.z;
        packed.shininess = data->shininess;

        const auto hash = hash_optics(packed, entity_seq);
        if (options.is_dirty &&
            !options.is_dirty(StorageID::ST_OPTICS, entity_seq, hash)) {
            continue;
        }
        auto identity = resolve(StorageID::ST_OPTICS, entity_seq);
        if (!identity || !identity->locally_owned) {
            continue;
        }
        SyncFrameRow row;
        row.entity_seq = entity_seq;
        row.actor_guid = std::move(identity->actor_guid);
        row.hash = hash;
        row.value = encode_optics(packed);
        frame.optics.push_back(std::move(row));
    }

    // 环境是全局状态（`__scene__`），迁移前也不看 actor guid / 所有权。
    for (const auto entity : Corona::Ecs::view<Corona::EnvironmentDevice>()) {
        const auto handle = Corona::Ecs::to_handle(entity);
        const auto entity_seq = static_cast<std::uint64_t>(Corona::Ecs::seq_of(handle));
        const auto* data = Corona::Ecs::try_get<Corona::EnvironmentDevice>(handle);
        if (data == nullptr) {
            continue;
        }
        EnvironmentState state{};
        state.sun_position[0] = data->sun_position.x;
        state.sun_position[1] = data->sun_position.y;
        state.sun_position[2] = data->sun_position.z;
        state.sun_intensity = data->sun_intensity;
        state.exposure = data->exposure;
        const auto hash = hash_environment(state, entity_seq);
        if (options.is_dirty &&
            !options.is_dirty(StorageID::ST_ENVIRONMENT, entity_seq, hash)) {
            continue;
        }
        SyncFrameRow row;
        row.entity_seq = entity_seq;
        row.hash = hash;
        row.value = encode_environment(state);
        frame.environments.push_back(std::move(row));
    }

    return frame;
}

// ---------------------------------------------------------------------------
// 通道
// ---------------------------------------------------------------------------

class SyncBoundaryChannel {
   public:
    static SyncBoundaryChannel& instance() {
        static SyncBoundaryChannel channel;
        return channel;
    }

    SyncBoundaryChannel(const SyncBoundaryChannel&) = delete;
    SyncBoundaryChannel& operator=(const SyncBoundaryChannel&) = delete;

    /// 网络线程：取一帧同步状态（等待拥有者线程在 tick 边界构建）。
    [[nodiscard]] std::optional<SyncFrame> request_frame(SyncFrameRequestOptions options,
                                                         std::string_view what) {
        auto& owner = Corona::Ecs::StructuralCommandChannel::instance();

        if (owner.stopped()) {
            record([](SyncBoundaryStats& stats) { ++stats.frames_rejected; });
            CFW_LOG_WARNING(
                "[Network/SyncBoundary] {} rejected: the systems DAG is not accepting "
                "owner-thread work (stopped); no sync frame is built this tick",
                what);
            return std::nullopt;
        }

        // 重入 / 装配前：没有第二个线程（DAG 系统内部、单线程夹具、系统线程启动之前），
        // 调用方就是唯一读者，直接构建。与 `submit_structural_command()` 同一规则。
        if (!owner.owner_expected() || owner.affinity().is_owner_thread()) {
            record([](SyncBoundaryStats& stats) { ++stats.frames_inline; });
            return build_frame(options);
        }

        // DAG_UNRESPONSIVE 闩锁：已经确认边界不推进，本 tick 不再等第二个预算。
        if (owner.unresponsive()) {
            record([](SyncBoundaryStats& stats) { ++stats.frames_rejected; });
            CFW_LOG_DEBUG("[Network/SyncBoundary] {} rejected immediately: {}", what,
                          Corona::Ecs::StructuralCommandChannel::kUnresponsiveReason);
            return std::nullopt;
        }

        auto exchange = std::make_shared<FrameExchange>(owner.next_request_id());
        {
            std::lock_guard lock(queue_mutex_);
            frames_.push_back(
                FrameRequestCommand{exchange, std::move(options), std::string{what}});
        }
        record([](SyncBoundaryStats& stats) { ++stats.frames_requested; });

        // 存活判据（无墙钟预算、单个慢 tick 继续等、DAG 停机/无进展才失败）与结构命令
        // 共用同一实现：`await()` 只在 settling 时被唤醒，否则按心跳切片检查。
        const auto outcome = owner.await(exchange->slot, what);
        if (!outcome.result.ok()) {
            if (outcome.abandoned) {
                record([](SyncBoundaryStats& stats) { ++stats.frames_abandoned; });
            } else {
                record([](SyncBoundaryStats& stats) { ++stats.frames_failed; });
            }
            if (outcome.abandoned) {
                CFW_LOG_WARNING(
                    "[Network/SyncBoundary] {} abandoned: {}; the late frame request is "
                    "skipped, no bytes are sent",
                    what, outcome.result.error);
            } else {
                CFW_LOG_WARNING("[Network/SyncBoundary] {} failed: {}", what,
                                outcome.result.error);
            }
            return std::nullopt;
        }
        return std::move(exchange->frame);
    }

    /// 网络线程：提交一条入站远端状态（不阻塞；应用发生在下一个 tick 边界）。
    [[nodiscard]] bool submit_remote_state(RemoteStateWrite write) {
        auto& owner = Corona::Ecs::StructuralCommandChannel::instance();

        if (owner.stopped()) {
            record([](SyncBoundaryStats& stats) { ++stats.remote_writes_rejected; });
            CFW_LOG_WARNING(
                "[Network/SyncBoundary] remote {} rejected: the systems DAG is not "
                "accepting owner-thread work (stopped); the operation stays pending",
                storage_label(write.storage));
            return false;
        }

        // 重入 / 装配前：调用方就是唯一的写者，直接应用（结果决定是否保留待重试）。
        if (!owner.owner_expected() || owner.affinity().is_owner_thread()) {
            record([](SyncBoundaryStats& stats) { ++stats.remote_writes_submitted; });
            if (apply_remote_state(write)) {
                record([](SyncBoundaryStats& stats) { ++stats.remote_writes_applied; });
                return true;
            }
            record([](SyncBoundaryStats& stats) { ++stats.remote_writes_dropped; });
            return false;
        }

        // 闩锁打开 = DAG 不推进：立即拒绝，让调用方保留待重试；恢复后的第一条边界
        // 就能应用它。逐个 pending 操作刷屏没有价值，这里只记 DEBUG。
        if (owner.unresponsive()) {
            record([](SyncBoundaryStats& stats) { ++stats.remote_writes_rejected; });
            CFW_LOG_DEBUG(
                "[Network/SyncBoundary] remote {} rejected immediately: {}",
                storage_label(write.storage),
                Corona::Ecs::StructuralCommandChannel::kUnresponsiveReason);
            return false;
        }

        const auto key = write_key(write);
        {
            std::lock_guard lock(queue_mutex_);
            // 停机/闩锁判定必须在**同一把锁**下重做：`reset()`（停机任务）也拿这把锁。
            // 否则"检查时还活着、入队时刚停机"的写入会活过清理，落到重启后的复用序号上。
            if (owner.stopped()) {
                record([](SyncBoundaryStats& stats) { ++stats.remote_writes_rejected; });
                return false;
            }
            if (owner.unresponsive()) {
                record([](SyncBoundaryStats& stats) { ++stats.remote_writes_rejected; });
                return false;
            }

            const auto existing = write_index_.find(key);
            if (existing != write_index_.end()) {
                // 同一 (storage, entity_seq) 保留最新值，并把它移到队尾：这样"丢弃最旧"
                // 指的是最旧的**写入**，而不是最旧的首次入队位置 —— 否则刚刷新过的 key
                // 可能因为位置靠前而被淘汰，位置靠后的旧值反而活下来。
                writes_.erase(existing->second);
                write_index_.erase(existing);
                record([](SyncBoundaryStats& stats) { ++stats.remote_writes_coalesced; });
            } else if (writes_.size() >= kMaxQueuedRemoteWrites) {
                // 丢最旧：对端刷得比边界消费得快时，保留最新的现场更有价值。
                write_index_.erase(write_key(writes_.front()));
                writes_.pop_front();
                record([](SyncBoundaryStats& stats) { ++stats.remote_writes_evicted; });
            }
            writes_.push_back(std::move(write));
            write_index_.emplace(key, std::prev(writes_.end()));
            // submitted 与队列变更在同一把锁下记账，避免"入队成功但计数丢失"的窗口。
            record([](SyncBoundaryStats& stats) { ++stats.remote_writes_submitted; });
        }
        return true;
    }

    /// 拥有者线程：tick 边界收口。
    void apply_pending() {
        if (!Corona::Ecs::StructuralCommandChannel::instance()
                 .affinity()
                 .assert_owner_thread("network sync boundary")) {
            return;
        }

        std::deque<FrameRequestCommand> frames;
        std::list<RemoteStateWrite> writes;
        {
            std::lock_guard lock(queue_mutex_);
            frames.swap(frames_);
            writes.swap(writes_);
            write_index_.clear();
        }

        // 入站先落地：同一边界构建的帧必须看到本 tick 已应用的远端值，否则会把
        // "远端刚发的旧值"当成新的本地脏值再广播回去。
        for (const auto& write : writes) {
            if (apply_remote_state(write)) {
                record([](SyncBoundaryStats& stats) { ++stats.remote_writes_applied; });
            } else {
                record([](SyncBoundaryStats& stats) { ++stats.remote_writes_dropped; });
                CFW_LOG_DEBUG(
                    "[Network/SyncBoundary] remote {} for entity #{} dropped: the target "
                    "entity/component no longer exists on this peer",
                    storage_label(write.storage),
                    static_cast<unsigned long long>(write.entity_seq));
            }
        }

        for (auto& command : frames) {
            if (!command.exchange) {
                continue;
            }
            const bool applied = command.exchange->slot.apply([&command]() -> Corona::EntityId {
                command.exchange->frame = build_frame(command.options);
                // 同步帧没有实体句柄：非 0 是"帧已构建"的结算标记（构建抛异常时
                // `StructuralRequest::apply()` 会以失败结算）。
                return 1;
            });
            if (applied) {
                record([](SyncBoundaryStats& stats) { ++stats.frames_built; });
            } else {
                // 等待方已放弃（无进展预算耗尽 / 闩锁）：跳过构建，不发送过期字节。
                record([](SyncBoundaryStats& stats) { ++stats.frames_skipped; });
            }
        }
    }

    /// 会话停止 / 引擎关停：清空未应用的入站队列与未服务的帧请求。
    void reset() {
        std::lock_guard lock(queue_mutex_);
        writes_.clear();
        write_index_.clear();
        frames_.clear();
    }

    [[nodiscard]] SyncBoundaryStats stats() const {
        // 锁序：queue -> stats，与 `submit_remote_state()` 的"持 queue 时记账"一致。
        // 反过来（stats -> queue）会与入队路径形成 ABBA 死锁。
        std::lock_guard queue_lock(queue_mutex_);
        std::lock_guard stats_lock(stats_mutex_);
        auto snapshot = stats_;
        snapshot.remote_writes_pending = writes_.size();
        return snapshot;
    }

    void reset_stats() {
        std::lock_guard lock(stats_mutex_);
        stats_ = SyncBoundaryStats{};
    }

   private:
    SyncBoundaryChannel() = default;

    [[nodiscard]] static std::string_view storage_label(StorageID storage) noexcept {
        switch (storage) {
        case StorageID::ST_MODEL_TRANSFORM:
            return "xform";
        case StorageID::ST_OPTICS:
            return "optics";
        case StorageID::ST_ENVIRONMENT:
            return "environment";
        default:
            return "state";
        }
    }

    template <typename Mutate>
    void record(Mutate&& mutate) {
        std::lock_guard lock(stats_mutex_);
        mutate(stats_);
    }

    mutable std::mutex queue_mutex_;
    std::deque<FrameRequestCommand> frames_;
    /// 入站队列用 `std::list` + 迭代器索引：合并时把旧节点摘掉、把最新值压到队尾，
    /// 这样"丢弃最旧"淘汰的是最旧的**写入**。`std::list` 的删除只让被删节点的迭代器
    /// 失效，其余节点的索引保持有效。
    std::list<RemoteStateWrite> writes_;
    std::unordered_map<std::uint64_t, std::list<RemoteStateWrite>::iterator> write_index_;

    mutable std::mutex stats_mutex_;
    SyncBoundaryStats stats_{};
};

}  // namespace

// ============================================================================
// 自由函数入口（拥有者边界任务 + 网络线程调用）
// ============================================================================

std::optional<SyncFrame> request_sync_frame(SyncFrameRequestOptions options,
                                            std::string_view what) {
    return SyncBoundaryChannel::instance().request_frame(std::move(options), what);
}

bool submit_remote_state(RemoteStateWrite write) {
    return SyncBoundaryChannel::instance().submit_remote_state(std::move(write));
}

void apply_pending_sync_boundary() {
    SyncBoundaryChannel::instance().apply_pending();
}

void reset_sync_boundary_state() {
    SyncBoundaryChannel::instance().reset();
}

SyncBoundaryStats sync_boundary_stats() {
    return SyncBoundaryChannel::instance().stats();
}

void reset_sync_boundary_stats() {
    SyncBoundaryChannel::instance().reset_stats();
}

}  // namespace Corona::Network
