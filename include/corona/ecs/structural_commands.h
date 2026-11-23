#pragma once

/// ECS **结构命令**通道：registry 的跨线程结构变更边界（P1 / spec §5.1）。
///
/// `entt::registry` 没有任何同步（见 `ecs/world.h` 的线程约定）：它必须只被一个
/// 线程触碰。但 `create_*` / `destroy_*` 的调用方是编辑器 UI / 脚本线程，而 DAG
/// 线程同时在遍历同一 registry —— 这是"两个写者 + 一个读者"的未同步状态。
///
/// 因此结构变更不再由调用方直接写 registry：
///
///   create_* 不创建实体：组装"初始组件值"入队 → 同步等待 DAG 在 tick 边界
///   创建 → 拿回 `EntityId`。句柄到手时实体已经存在，没有"先无效后生效"的中间态。
///   destroy_* 同契约但**不等待**：销毁幂等，调用方不需要同步答案；等待只会把一次
///   卡死按实体数放大（见下方"销毁"一节）。
///
/// 为什么是 **Fifo**：创建顺序有语义（actor 先于它的 profile、场景先于成员），
/// 不能合并；与 `scene_commands.h` / `mechanics_structural_commands.h` 同一选择。
/// 为什么应用在 **tick 边界**：所有节点遍历之前落地，本 tick 内实体集合不变化。
///
/// ---- 四条不变量（缺一条就是回归）----
/// 1. **结算**：每个入队请求都拿到结果（句柄或失败）。DAG 未运行 / 正在停止 /
///    边界真的卡死，调用方都**有界地**拿到失败，绝不无限等待。本仓库 Python 桥的
///    `submit_and_wait` 没有截止时间，一次漏结算就永久挂住一个线程 —— 这里不允许重演。
/// 2. **重入**：调用方本身就是 DAG 线程（系统内部创建）时**直接应用**，
///    入队后等自己会自死锁。
/// 3. **存活，不是墙钟**（Task 1c，spec §5.1）：tick 边界每完成一次就发布一次
///    **心跳**（`publish_tick_boundary()`），等待方只在两种情况下失败：
///    DAG 已停止 / 正在关停；或**连续** `kNoProgressBudget`（默认 10s，可配置）内
///    没有任何边界完成。**单个慢 tick 不失败**：项目加载期 Optics 的
///    `render_dispatch` 会占住 DAG 3–4s（GPU 管线编译 + 128MiB shadow 分配，
///    实测 PhaseWatchdog 3013ms / max_tick 3989ms），旧的 17ms 墙钟预算把这种
///    "慢但活着"的边界判成超时，直接丢掉了 actor 实体。
///    旧的两帧预算降级为**诊断阈值**：超过它只写一条 WARNING（保留 PhaseWatchdog
///    给的可见性），绝不失败请求。
/// 4. **停机**：DAG 停止时把队列里未消费的请求全部以失败结算
///    （与 Python 桥 `transition_and_cancel` 同一原则）。
/// 5. **不推进就快速失败**（`DAG_UNRESPONSIVE` 闩锁）：一次无进展超时之后，后续
///    请求立即失败，而不是各等一个预算 —— 否则装载失败的回滚路径会把一次卡死放大成
///    "实体数 × 预算"的阻塞。下一次 tick 边界完成（或 DAG 重新启动）自动解锁。
/// 6. **销毁不等待**：销毁是幂等的，入队即返回；装载回滚因此不会按实体数堆叠预算。
///    销毁走**自己的、不设固定容量的队列**（共享 Fifo 的 4096 上限会在 ~512 个 actor
///    的场景 teardown 时丢命令，把实体变成孤儿），在同一个边界内、创建之后应用。
///
/// ---- 装配前（`notify_owner_expected()` 之前）----
/// 引擎装配阶段（系统线程还没启动）与单线程测试夹具里**不存在第二个写者**，
/// 也没有任何线程会来 drain 队列。这段时间的请求由调用线程直接应用（并记
/// `stats.pre_assembly`），否则 UI 初始化建 surface 图像这类装配期结构写会超时失败。
/// 装配方一旦宣布"拥有者即将上线"（DAG 装配 / 降级路径装配完成 / DAG 线程启动），
/// 之后所有请求一律经由队列结算 —— 见 `owner_expected()`。

#include <corona/ecs/access.h>
#include <corona/ecs/components.h>
#include <corona/kernel/command/command_buffer.h>
#include <corona/kernel/command/thread_affinity.h>
#include <corona/shared_data_hub.h>

#include <horizon/core/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Corona::Ecs {

/// 两帧诊断阈值的下限：两帧 @120Hz ≈ 17ms。
///
/// Task 1b 之前这是**超时**默认值。现在它只是"这条等待该报一次警告了"的界：
/// 超过它写 WARNING，请求继续等（spec §5.1，Task 1c）。
inline constexpr std::int64_t kMinSlowWaitDiagnosticMs = 17;

/// 无进展预算默认值：**10s**（spec §5.1）。
///
/// 判据不是"等了多久"，而是"多久没有 tick 边界完成"。10s 远大于项目加载期实测的
/// 最长单 tick（3989ms），所以慢 tick 一定能等到；同时又能让真正卡死的边界在
/// 可接受的时间内把调用方结算掉。
inline constexpr std::int64_t kDefaultNoProgressBudgetMs = 10'000;

/// 显式覆盖的上限：再长也不该把一个 UI / 脚本线程挂死在一个卡住的 DAG 上。
inline constexpr std::int64_t kMaxNoProgressBudgetMs = 120'000;

/// 等待方检查存活的轮询粒度。
///
/// 心跳前进**不会**通知条件变量（心跳发布在 tick 边界上，而等待方各自有自己的
/// condition_variable），所以等待按切片轮询：成功仍然由拥有者在结算时立即唤醒，
/// 这个切片只决定"发现心跳没前进"的粒度。25ms 对检测 10s 预算的卡死完全够用，
/// 对正常路径（结算唤醒）没有任何成本。
inline constexpr std::int64_t kLivenessPollMs = 25;

/// 装配方告知真实帧周期之前使用的保守假设（120Hz）。
inline constexpr std::int64_t kDefaultFramePeriodUs = 8333;

/// 环境覆盖（毫秒）；0 表示未覆盖。
///
/// `CORONA_ECS_STRUCTURAL_NO_PROGRESS_MS` 是新的名字（语义 = 无进展预算）；
/// 兼容旧名 `CORONA_ECS_STRUCTURAL_TIMEOUT_MS`，新名优先。
[[nodiscard]] inline std::int64_t structural_no_progress_override_ms() noexcept {
    for (const char* name : {"CORONA_ECS_STRUCTURAL_NO_PROGRESS_MS",
                             "CORONA_ECS_STRUCTURAL_TIMEOUT_MS"}) {
        if (const char* env = std::getenv(name)) {
            const long parsed = std::strtol(env, nullptr, 10);
            if (parsed > 0 && parsed <= kMaxNoProgressBudgetMs) {
                return parsed;
            }
        }
    }
    return 0;
}

/// **诊断阈值** = 两帧（帧周期向上取整到毫秒），下限 `kMinSlowWaitDiagnosticMs`。
///
/// 帧周期由装配方用 `set_tick_period()` 告知：DAG 模式取配置的 tick
/// （`CORONA_SYSTEMS_DAG_TICK`，30..480Hz），降级模式取 Geometry 的更新周期。
/// 它只决定"多久算慢"（写 WARNING），**不再决定失败**。
[[nodiscard]] inline std::int64_t two_frame_diagnostic_ms(std::int64_t frame_period_us) noexcept {
    const std::int64_t two_frames_ms = (frame_period_us * 2 + 999) / 1000;
    return std::max(two_frames_ms, kMinSlowWaitDiagnosticMs);
}

/// 一条结构请求的**结算槽**：等待方（任意线程）与拥有者（DAG 线程）共享。
///
/// 关键点：拥有者在**持有槽锁**的状态下创建实体。等待方放弃也需要先拿到槽锁，
/// 因此不会出现"等待方已放弃、拥有者却刚创建出一个无人签收的实体"的孤儿。
///
/// 等待策略不在这里：见 `StructuralCommandChannel::await()` —— 它需要通道的
/// 心跳与停机状态，才能区分"慢 tick"与"卡死"。
class StructuralRequest {
   public:
    struct Result {
        EntityId handle{0};  ///< 0 表示失败
        std::string error;   ///< 失败原因（成功时为空）
        [[nodiscard]] bool ok() const noexcept { return handle != 0; }
    };

    explicit StructuralRequest(std::uint64_t id) noexcept : id_(id) {}

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }

    /// 等待方（任意线程）：等一个切片。返回 true = 已结算（成功或失败）。
    ///
    /// 切片只是"多久回头看一眼心跳"，不是截止时间：谁来唤醒、为什么超时由
    /// `await()` 决定。
    bool wait_slice(std::chrono::milliseconds slice) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, slice, [this] { return settled_; });
    }

    /// 已结算的结果副本（未结算时是默认构造的失败值）。
    [[nodiscard]] Result result() const {
        std::lock_guard lock(mutex_);
        return result_;
    }

    /// 等待方放弃（边界卡死 / DAG 停机）：标记已放弃，拥有者之后取到它也会跳过 ——
    /// 不会创建无人签收的实体。已结算时原样返回已结算的结果。
    [[nodiscard]] Result abandon(std::string reason) {
        {
            std::lock_guard lock(mutex_);
            if (settled_) {
                return result_;
            }
            settled_ = true;
            abandoned_ = true;
            result_.handle = 0;
            result_.error = std::move(reason);
        }
        cv_.notify_all();
        return result();
    }

    /// 拥有者线程：持锁应用并结算。请求已被放弃 / 已结算时返回 false（跳过应用）。
    template <typename Apply>
    bool apply(Apply&& apply) {
        std::unique_lock lock(mutex_);
        if (settled_) {
            // 已被等待方放弃（超时）或被停机结算：跳过，绝不创建无人签收的实体。
            return false;
        }
        try {
            result_.handle = apply();
            if (result_.handle == 0) {
                result_.error = "structural command returned an invalid handle";
            }
        } catch (const std::exception& error) {
            result_.handle = 0;
            result_.error = std::string{"exception while applying the structural command: "} +
                            error.what();
        } catch (...) {
            result_.handle = 0;
            result_.error = "unknown exception while applying the structural command";
        }
        settled_ = true;
        lock.unlock();
        cv_.notify_all();
        return true;
    }

    /// 停机结算（任意线程）。返回本次是否真的结算了一个在途请求。
    bool settle_failure(std::string reason) {
        {
            std::lock_guard lock(mutex_);
            if (settled_) {
                return false;
            }
            settled_ = true;
            result_.handle = 0;
            result_.error = std::move(reason);
        }
        cv_.notify_all();
        return true;
    }

    /// 停机式失败结算并取回结果（调用方线程自己用，不计入"等待方放弃"）。
    [[nodiscard]] Result settle_failure_result(std::string reason) {
        settle_failure(std::move(reason));
        return result();
    }

    [[nodiscard]] bool abandoned() const {
        std::lock_guard lock(mutex_);
        return abandoned_;
    }

   private:
    std::uint64_t id_{0};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool settled_{false};
    bool abandoned_{false};
    Result result_{};
};

// ============================================================================
// 每类结构变更携带的初始组件值
// ============================================================================
//
// 命令里装的是**值**，不是裸指针：调用方线程写、DAG 线程读，中间的交接由队列的
// 互斥量负责。payload 默认构造出来的值与 `World::create<T>()` 的默认组件值一致，
// 所以"默认值包装"（`SharedDataHub::create_*`）和"显式 payload"不会有两套事实。

/// `create_model_resource`
struct CreateModelResourcePayload {
    ModelResource value{};
};

/// `create_transform`
struct CreateTransformPayload {
    ModelTransform value{};
};

/// `create_geometry`
struct CreateGeometryPayload {
    GeometryDevice geometry;
    ModelSource model_source;
};

/// `create_mechanics`
struct CreateMechanicsPayload {
    MechanicsDevice mechanics;
    Bounds bounds;
};

/// `create_optics`
struct CreateOpticsPayload {
    OpticsDevice optics;
    Visibility visibility;
};

/// `create_acoustics`
struct CreateAcousticsPayload {
    AcousticsDevice acoustics;
};

/// `create_profile_component`
struct CreateProfilePayload {
    ProfileDevice profile;
};

/// `create_actor_component`
struct CreateActorPayload {
    ActorDevice actor;
    StreamingPolicy streaming;
    ModelPath model_path;
    Profiles profiles;
    FollowCamera follow_camera;
};

/// `create_camera`
struct CreateCameraPayload {
    CameraDevice camera;
};

/// `create_environment`
struct CreateEnvironmentPayload {
    EnvironmentDevice environment;
};

/// `create_scene`
struct CreateScenePayload {
    SceneDevice scene;
};

/// `create_image`：没有初始值 payload。
/// `ImageDevice` 装的是 GPU 句柄与收据，内容一律在**每图写租约**（`image_write`）
/// 下写入；把它拷进命令只会让两张图短暂共享同一批 GPU 句柄。
struct CreateImagePayload {};

/// `destroy_*`：销毁只需要句柄。`release_image_lock` 表示同时摘掉该图的写租约表项
/// （`destroy_image` 的语义；其它种类只是 `Ecs::destroy`）。
struct DestroyEntityPayload {
    EntityId handle{0};
    bool release_image_lock{false};
};

using StructuralPayload = std::variant<
    CreateModelResourcePayload, CreateTransformPayload, CreateGeometryPayload,
    CreateMechanicsPayload, CreateOpticsPayload, CreateAcousticsPayload,
    CreateProfilePayload, CreateActorPayload, CreateCameraPayload,
    CreateEnvironmentPayload, CreateScenePayload, CreateImagePayload,
    DestroyEntityPayload>;

struct StructuralCommand {
    std::shared_ptr<StructuralRequest> request;  ///< 结算槽（唯一，key 即序号）
    StructuralPayload payload;                   ///< 初始组件值 / 待销毁句柄
    std::string_view what{"structural command"};  ///< 诊断用操作名（字面量，静态生命期）
};

struct StructuralCommandKey {
    static std::uintptr_t get(const StructuralCommand& command) noexcept {
        return command.request ? static_cast<std::uintptr_t>(command.request->id()) : 0;
    }
};

// ============================================================================
// 拥有者线程：创建 / 销毁原语（不产生"裸实体"）
// ============================================================================
//
// 每个组件种类只有一个权威创建原语，初始值一次性写入。`SharedDataHub::create_*`
// 是"默认值 payload"的薄包装（单线程测试夹具与装配期用），tick 边界的应用器
// 直接调用这些函数。

[[nodiscard]] inline EntityId create_model_resource_entity(CreateModelResourcePayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<ModelResource>();
    world.get<ModelResource>(entity) = payload.value;
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_transform_entity(CreateTransformPayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<ModelTransform>();
    world.get<ModelTransform>(entity) = std::move(payload.value);
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_geometry_entity(CreateGeometryPayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<GeometryDevice, ModelSource>();
    world.get<GeometryDevice>(entity) = std::move(payload.geometry);
    world.get<ModelSource>(entity) = std::move(payload.model_source);
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_mechanics_entity(CreateMechanicsPayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<MechanicsDevice, Bounds>();
    world.get<MechanicsDevice>(entity) = std::move(payload.mechanics);
    world.get<Bounds>(entity) = payload.bounds;
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_optics_entity(CreateOpticsPayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<OpticsDevice, Visibility>();
    world.get<OpticsDevice>(entity) = std::move(payload.optics);
    world.get<Visibility>(entity) = payload.visibility;
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_acoustics_entity(CreateAcousticsPayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<AcousticsDevice>();
    world.get<AcousticsDevice>(entity) = std::move(payload.acoustics);
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_profile_entity(CreateProfilePayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<ProfileDevice>();
    world.get<ProfileDevice>(entity) = payload.profile;
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_actor_entity(CreateActorPayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<ActorDevice, StreamingPolicy, ModelPath, Profiles,
                                        FollowCamera>();
    world.get<ActorDevice>(entity) = std::move(payload.actor);
    world.get<StreamingPolicy>(entity) = payload.streaming;
    world.get<ModelPath>(entity) = std::move(payload.model_path);
    world.get<Profiles>(entity) = std::move(payload.profiles);
    world.get<FollowCamera>(entity) = payload.follow_camera;
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_camera_entity(CreateCameraPayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<CameraDevice>();
    world.get<CameraDevice>(entity) = std::move(payload.camera);
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_environment_entity(CreateEnvironmentPayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<EnvironmentDevice>();
    world.get<EnvironmentDevice>(entity) = std::move(payload.environment);
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_scene_entity(CreateScenePayload payload) {
    auto& world = World::instance();
    const Entity entity = world.create<SceneDevice>();
    world.get<SceneDevice>(entity) = std::move(payload.scene);
    return to_handle(entity);
}

[[nodiscard]] inline EntityId create_image_entity(CreateImagePayload) {
    const Entity entity = World::instance().create<ImageDevice>();
    return to_handle(entity);
}

/// 销毁图像实体：连同它的每图写租约表项一起摘掉（租约持锁期间锁对象本身必须存活）。
inline void destroy_image_entity(EntityId handle) {
    if (handle == 0) {
        return;
    }
    destroy(handle);
    SharedDataHub::instance().forget_image_lock(handle);
}

inline void destroy_entity_owner(DestroyEntityPayload payload) {
    if (payload.handle == 0) {
        return;
    }
    if (payload.release_image_lock) {
        destroy_image_entity(payload.handle);
        return;
    }
    destroy(payload.handle);
}

// ---------------------------------------------------------------------------
// payload → 应用（统一返回 EntityId：0 表示失败；销毁返回被销毁的句柄）
// ---------------------------------------------------------------------------

[[nodiscard]] inline EntityId apply_structural_payload(CreateModelResourcePayload& payload) {
    return create_model_resource_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateTransformPayload& payload) {
    return create_transform_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateGeometryPayload& payload) {
    return create_geometry_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateMechanicsPayload& payload) {
    return create_mechanics_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateOpticsPayload& payload) {
    return create_optics_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateAcousticsPayload& payload) {
    return create_acoustics_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateProfilePayload& payload) {
    return create_profile_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateActorPayload& payload) {
    return create_actor_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateCameraPayload& payload) {
    return create_camera_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateEnvironmentPayload& payload) {
    return create_environment_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateScenePayload& payload) {
    return create_scene_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(CreateImagePayload& payload) {
    return create_image_entity(std::move(payload));
}
[[nodiscard]] inline EntityId apply_structural_payload(DestroyEntityPayload& payload) {
    const EntityId handle = payload.handle;
    if (handle == 0) {
        return 0;
    }
    destroy_entity_owner(std::move(payload));
    return handle;
}

/// 分发：variant 的哪一个分支就决定创建/销毁哪一类实体（没有第二条"kind"事实）。
[[nodiscard]] inline EntityId apply_structural_payload(StructuralPayload& payload) {
    return std::visit([](auto& value) -> EntityId { return apply_structural_payload(value); },
                      payload);
}

// ============================================================================
// 通道
// ============================================================================

/// 注册在 tick 边界上的**额外拥有者任务**（无参函数指针）。
///
/// 结构命令只负责"本 tick 存在哪些实体"。同一个单写者边界还要承载不属于结构变更、
/// 但同样只能由拥有者线程执行的收口工作 —— 目前是网络同步的入站写入与出站序列化
/// （`Corona::Network::apply_pending_sync_boundary`）。
///
/// 注册发生在装配期（系统线程启动之前），任务按注册顺序执行。用函数指针而不是
/// `std::function`：任务的生命期必须覆盖进程，注册方不该在运行时持有/释放闭包。
/// 两个边界应用器都经由 `apply_pending()`，所以 DAG 模式（engine.cpp 的边界钩子）
/// 与 `CORONA_SYSTEMS_DAG=0` 降级模式（GeometrySystem 的兜底 drain）自动都执行它。
using TickBoundaryTask = void (*)();

/// 通道本身：队列 + 拥有者线程标识 + 生命周期状态 + 诊断计数。
class StructuralCommandChannel {
   public:
    using Buffer = Kernel::CommandBuffer<StructuralCommand, StructuralCommandKey,
                                         Kernel::CoalescePolicy::Fifo>;

    /// 诊断计数（进程内累计；`reset_stats()` 供测试用）
    struct Stats {
        std::uint64_t requested{0};       ///< 入队并**等待结算**的请求数（不含 deferred destroys）
        std::uint64_t inline_applied{0};  ///< 重入规则下直接应用的请求数
        std::uint64_t pre_assembly{0};    ///< 装配前（无拥有者存在）直接应用的请求数
        std::uint64_t applied{0};         ///< 由 DAG 应用并成功结算的**等待中**请求数
        std::uint64_t failed{0};          ///< 等待方拿到失败的请求数（每条只记一次）
        std::uint64_t rejected{0};        ///< 未入队就拒绝的请求数（停机/队列满）
        std::uint64_t timeouts{0};        ///< 无进展预算耗尽而失败的请求数（failed 的子集）
        std::uint64_t unresponsive_rejections{0}; ///< 闩锁打开后立即失败的请求数（failed 的子集）
        std::uint64_t slow_waits{0};      ///< 超过两帧诊断阈值的等待数（每条请求最多记一次）
        std::uint64_t abandoned{0};       ///< 等待方放弃后拥有者跳过的命令数
        std::uint64_t deferred_destroys{0}; ///< 入队的"不等待结算"销毁命令数
        std::uint64_t applied_destroys{0};  ///< 已由边界应用的"不等待结算"销毁命令数
        std::uint64_t settled_on_stop{0}; ///< 停机时结算的**仍有等待方**的请求数（不计入 failed）
    };

    /// 闩锁打开后所有结构请求携带的拒绝原因（日志里可直接搜 "DAG_UNRESPONSIVE"）。
    static constexpr std::string_view kUnresponsiveReason =
        "DAG_UNRESPONSIVE: no tick boundary completed for the no-progress budget; the systems "
        "DAG is not advancing (waiting again is pointless until a boundary completes)";

    /// "闩锁未打开"的哨兵值（心跳从 0 开始单调递增，用不到 max）。
    static constexpr std::uint64_t kNoUnresponsiveBeat =
        std::numeric_limits<std::uint64_t>::max();

    static StructuralCommandChannel& instance() {
        static StructuralCommandChannel channel;
        return channel;
    }

    StructuralCommandChannel(const StructuralCommandChannel&) = delete;
    StructuralCommandChannel& operator=(const StructuralCommandChannel&) = delete;

    [[nodiscard]] Buffer& commands() noexcept { return commands_; }

    /// registry 的拥有者线程（DAG 线程）。重入判定与越界断言都用它。
    [[nodiscard]] Kernel::ThreadAffinity& affinity() noexcept { return affinity_; }

    [[nodiscard]] std::uint64_t next_request_id() noexcept {
        return request_ids_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    /// 无进展预算：连续这么久**没有任何 tick 边界完成**，等待方就以失败结算。
    /// 显式覆盖（env / `set_no_progress_budget()`）优先，否则用默认 10s。
    [[nodiscard]] std::chrono::milliseconds no_progress_budget() const noexcept {
        const auto override_ms = no_progress_override_ms_.load(std::memory_order_relaxed);
        return std::chrono::milliseconds{override_ms > 0 ? override_ms
                                                         : kDefaultNoProgressBudgetMs};
    }

    /// 诊断阈值 = 两帧（真实帧周期，下限 17ms）：超过它只写 WARNING，不失败。
    [[nodiscard]] std::chrono::milliseconds slow_wait_diagnostic_threshold() const noexcept {
        return std::chrono::milliseconds{
            two_frame_diagnostic_ms(frame_period_us_.load(std::memory_order_relaxed))};
    }

    /// 装配方告知真实帧周期（DAG tick / 降级模式下 Geometry 的更新周期），
    /// 两帧**诊断阈值**据此推导；无进展预算与 env 覆盖不受影响。
    void set_tick_period(std::chrono::microseconds period) noexcept {
        const auto count = std::clamp<std::int64_t>(period.count(), 1, 1'000'000);
        frame_period_us_.store(count, std::memory_order_relaxed);
    }

    /// 显式覆盖无进展预算（测试与需要固定预算的集成方）；env 覆盖在构造时已生效。
    void set_no_progress_budget(std::chrono::milliseconds value) noexcept {
        const auto count = std::clamp<std::int64_t>(value.count(), 1, kMaxNoProgressBudgetMs);
        no_progress_override_ms_.store(count, std::memory_order_relaxed);
    }

    /// 清除显式覆盖，回到默认 10s（测试与运行时重配用）。
    void clear_no_progress_budget_override() noexcept {
        no_progress_override_ms_.store(0, std::memory_order_relaxed);
    }

    /// 是否由 env / `set_no_progress_budget()` 显式指定（诊断用）。
    [[nodiscard]] bool no_progress_budget_is_override() const noexcept {
        return no_progress_override_ms_.load(std::memory_order_relaxed) > 0;
    }

    // ------------------------------------------------------------ 心跳

    /// **tick 边界心跳**：每完成一次边界 +1（单调，不回绕到 0 之前够用几百年）。
    ///
    /// 两个边界应用器都必须调用它：`apply_pending()`（DAG 边界钩子与降级兜底都走
    /// 这里）在 drain 之前发一次。**即使队列为空也要发** —— 等待方要的是
    /// "边界还在推进"这个事实，而不是"队列被消费过"。
    void publish_tick_boundary() {
        heartbeat_.fetch_add(1, std::memory_order_release);
        // 边界完成 = DAG 又开始推进：先让**以心跳序号为准**的闩锁失效（见 `unresponsive()`），
        // 再清掉记账值并记一条恢复日志。清除只是卫生：判断本身比较心跳序号，
        // 所以这里不存在"清了又被旧闩锁挡住"的窗口。
        const auto stamp = unresponsive_beat_.load(std::memory_order_acquire);
        if (stamp != kNoUnresponsiveBeat) {
            unresponsive_beat_.store(kNoUnresponsiveBeat, std::memory_order_release);
            CFW_LOG_NOTICE(
                "[Ecs/StructuralCommand] the systems DAG is advancing again (heartbeat #{}); "
                "structural requests are accepted again",
                static_cast<unsigned long long>(heartbeat_.load(std::memory_order_relaxed)));
        }
    }

    /// 当前心跳序号。等待方用它判断"上次记录之后有没有边界完成"。
    [[nodiscard]] std::uint64_t heartbeat() const noexcept {
        return heartbeat_.load(std::memory_order_acquire);
    }

    // ------------------------------------------------------------ 闩锁

    /// DAG 是否被判定为"不推进"（DAG_UNRESPONSIVE）。
    ///
    /// 一次无进展超时之后，后续结构请求**立即**失败而不是各等一个预算：否则
    /// 一次卡死 × N 个实体（装载失败的回滚路径会销毁每个已装载 actor 的 ~8 个实体）
    /// 会放大成 N × 预算 的阻塞，把 CEF/主线程冻住几分钟到几小时。
    ///
    /// 实现是**以心跳序号为戳**，不是布尔量：`unresponsive()` 只在"戳上记录的心跳
    /// == 当前心跳"时成立，也就是"从判定无进展那一刻起，一次边界也没有完成"。
    /// 只要边界前进一次，闩锁自动失效 —— 不需要谁去清它，也就不存在"边界刚好完成、
    /// 清除还没执行"的卡死窗口（Task 1c review item 2）。
    [[nodiscard]] bool unresponsive() const noexcept {
        const auto stamp = unresponsive_beat_.load(std::memory_order_acquire);
        return stamp != kNoUnresponsiveBeat &&
               stamp == heartbeat_.load(std::memory_order_acquire);
    }

    /// 等待方发现无进展时打开闩锁（由 `await()` 调用）。
    void mark_unresponsive() noexcept {
        unresponsive_beat_.store(heartbeat(), std::memory_order_release);
    }

    // ------------------------------------------------------------ 生命周期

    /// DAG 线程即将开始 tick：重新接受请求，并确认"拥有者即将上线"。
    void on_dag_started() noexcept {
        owner_expected_.store(true, std::memory_order_release);
        stopped_.store(false, std::memory_order_release);
        // 新的一次 DAG 运行：旧的"不推进"结论作废（否则重启后的第一条请求就被拒）。
        unresponsive_beat_.store(kNoUnresponsiveBeat, std::memory_order_release);
    }

    /// 装配方宣布"拥有者即将上线"（DAG 装配完成 / 降级路径装配完成）。
    /// 从此不再有"装配前直接应用"的例外，所有结构变更都排队结算。
    void notify_owner_expected() noexcept {
        owner_expected_.store(true, std::memory_order_release);
    }

    /// 是否已有装配方承诺"会有线程来 drain 队列"。
    /// false = 装配前（系统线程还不存在，调用方是唯一写者）或单线程夹具。
    [[nodiscard]] bool owner_expected() const noexcept {
        return owner_expected_.load(std::memory_order_acquire);
    }

    /// DAG 线程即将停止：先关入口，再把未消费的请求全部以失败结算。
    /// 与 Python 桥 `transition_and_cancel` 同一原则：停机不能让任何请求悬空。
    std::size_t on_dag_stopping(std::string_view reason) {
        stopped_.store(true, std::memory_order_release);
        const std::size_t settled = fail_pending(reason);
        CFW_LOG_INFO("[Ecs/StructuralCommand] DAG stopping: settled {} pending structural request(s) as failure",
                     settled);
        return settled;
    }

    [[nodiscard]] bool stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

    /// 装配方是否安装了 DAG tick 边界钩子。安装后 `GeometrySystem::update()` 的
    /// 降级兜底不再 drain —— 保证"实体集合在一个 tick 内不变"。
    [[nodiscard]] bool tick_boundary_installed() const noexcept {
        return boundary_installed_.load(std::memory_order_acquire);
    }

    void notify_tick_boundary_installed() noexcept {
        boundary_installed_.store(true, std::memory_order_release);
    }

    // ------------------------------------------------------------ 额外边界任务

    /// 注册一条额外的拥有者任务。重复注册同一函数指针返回 false（幂等）。
    bool add_boundary_task(TickBoundaryTask task) {
        if (task == nullptr) {
            return false;
        }
        std::lock_guard lock(boundary_tasks_mutex_);
        if (std::find(boundary_tasks_.begin(), boundary_tasks_.end(), task) !=
            boundary_tasks_.end()) {
            return false;
        }
        boundary_tasks_.push_back(task);
        return true;
    }

    /// 拥有者线程：按注册顺序执行额外边界任务。单个任务抛异常不阻断其它任务。
    void run_boundary_tasks() {
        std::vector<TickBoundaryTask> tasks;
        {
            std::lock_guard lock(boundary_tasks_mutex_);
            tasks = boundary_tasks_;
        }
        for (const auto task : tasks) {
            try {
                task();
            } catch (const std::exception& error) {
                CFW_LOG_ERROR("[Ecs/StructuralCommand] tick boundary task failed: {}",
                              error.what());
            } catch (...) {
                CFW_LOG_ERROR("[Ecs/StructuralCommand] tick boundary task failed: "
                              "unknown exception");
            }
        }
    }

    /// 已注册的额外边界任务数（诊断/测试用）。
    [[nodiscard]] std::size_t boundary_task_count() const {
        std::lock_guard lock(boundary_tasks_mutex_);
        return boundary_tasks_.size();
    }

    // ------------------------------------------------------------ 应用

    /// 应用入站结构命令（tick 边界阶段 / DAG 降级兜底）。返回是否真的应用了命令。
    bool apply_pending() {
        // 心跳在 drain **之前**发布，而且**队列为空也发**：等待方要的是"边界在推进"
        // 这个事实，不是"队列被消费过"。两个边界应用器（DAG 钩子 / 降级兜底）都
        // 经由这里，因此它们都自动满足"必须 beat"的契约。
        publish_tick_boundary();

        bool applied_any = false;
        auto commands = commands_.drain();
        if (!commands.empty()) {
            applied_any = true;
            for (auto& command : commands) {
                auto request = command.request;
                if (!request) {
                    continue;
                }
                try {
                    const bool applied = request->apply(
                        [&command]() { return apply_structural_payload(command.payload); });
                    if (!applied && request->abandoned()) {
                        // 等待方已放弃（卡死 / 停机）：命令被跳过，不创建无人签收的实体。
                        // 成功与失败都由等待方结算时各记一次，这里不重复计数。
                        record_abandoned();
                    }
                } catch (const std::exception& error) {
                    // settle_failure 唤醒等待方，由它记一次 failed。
                    request->settle_failure(std::string{"structural command failed: "} + error.what());
                    CFW_LOG_ERROR("[Ecs/StructuralCommand] {} failed: {}", command.what, error.what());
                } catch (...) {
                    request->settle_failure("structural command failed: unknown exception");
                    CFW_LOG_ERROR("[Ecs/StructuralCommand] {} failed: unknown exception", command.what);
                }
            }
        }

        // 延迟销毁：同一个边界内、在**创建之后**应用（绝不销毁刚创建的东西），
        // 队列内部保持入队顺序。它们没有等待方，所以直接应用、单独计数。
        auto destroys = drain_deferred_destroys();
        for (auto& destroy : destroys) {
            try {
                StructuralPayload payload{std::move(destroy.payload)};
                apply_structural_payload(payload);
                record_applied_destroy();
            } catch (const std::exception& error) {
                CFW_LOG_ERROR("[Ecs/StructuralCommand] {} #{} failed: {}", destroy.what,
                              static_cast<unsigned long long>(destroy.handle), error.what());
            } catch (...) {
                CFW_LOG_ERROR("[Ecs/StructuralCommand] {} #{} failed: unknown exception",
                              destroy.what,
                              static_cast<unsigned long long>(destroy.handle));
            }
        }

        // 额外拥有者任务（网络同步的入站写入 / 出站序列化）：在实体集合已经定型
        // （创建 + 延迟销毁都应用完）之后执行，读到的就是本 tick 的真实状态。
        run_boundary_tasks();
        return applied_any || !destroys.empty();
    }

    /// 把所有未消费的等待请求以失败结算（停机）。返回结算的请求数。
    ///
    /// 延迟销毁没有等待方，不能计入这个数（否则 `settled_on_stop` 会虚高）；
    /// 它们随 world 一起消失，只记一条带数量的诊断。
    std::size_t fail_pending(std::string_view reason) {
        auto commands = commands_.drain();
        std::size_t settled = 0;
        for (auto& command : commands) {
            if (!command.request) {
                continue;
            }
            if (command.request->settle_failure(std::string(reason))) {
                ++settled;
            }
        }
        if (settled != 0) {
            // 只记"停机时结算了多少条"；失败本身由各等待方各记一次。
            std::lock_guard lock(stats_mutex_);
            stats_.settled_on_stop += settled;
        }
        const auto dropped_destroys = drop_deferred_destroys();
        if (dropped_destroys != 0) {
            CFW_LOG_WARNING(
                "[Ecs/StructuralCommand] dropped {} deferred destroy(s): {}; the entities die "
                "with the world",
                dropped_destroys, reason);
        }
        return settled;
    }

    // ------------------------------------------------------------ 延迟销毁（fire-and-forget）

    /// 一条延迟销毁：payload + 诊断名 + 句柄（"DAG 不恢复"时点名用）。
    struct DeferredDestroy {
        DestroyEntityPayload payload;
        std::string_view what{"destroy_entity"};
        EntityId handle{0};
    };

    /// 入队一条延迟销毁，**不设固定容量**。
    ///
    /// 为什么不用 `commands_` 的 Fifo：它有固定上限（`CommandBuffer::kDefaultCapacity`
    /// = 4096），而 `scene.reset()` 会在一次 UI 调用里为每个已装载 actor 的 ~8 个实体
    /// 各压一条销毁 —— 512 个 actor 就溢出，被拒绝的销毁在**活着的 DAG** 上也会把
    /// 实体和 GPU 资源变成找不到的孤儿（Task 1c review item 1，fire-and-forget 之前
    /// 不可能发生）。
    ///
    /// 这条队列的规模由"刚刚卸载的场景里有多少实体"决定（O(live entities)，每个句柄
    /// 至多入队一次），不由生产者速率决定，所以不再需要"满了就丢"的安全阀；
    /// 同一个句柄即使重复入队，销毁也是幂等的。
    bool enqueue_deferred_destroy(DestroyEntityPayload payload, std::string_view what) {
        {
            std::lock_guard lock(destroy_mutex_);
            const EntityId handle = payload.handle;
            destroy_queue_.push_back(DeferredDestroy{payload, what, handle});
        }
        record_deferred_destroy();
        return true;
    }

    /// 尚未应用的延迟销毁数量（诊断/测试用）。
    [[nodiscard]] std::size_t pending_destroy_count() const {
        std::lock_guard lock(destroy_mutex_);
        return destroy_queue_.size();
    }

    /// 拥有者线程：取走当前所有延迟销毁（在创建之后应用）。
    [[nodiscard]] std::vector<DeferredDestroy> drain_deferred_destroys() {
        std::vector<DeferredDestroy> out;
        std::lock_guard lock(destroy_mutex_);
        out.reserve(destroy_queue_.size());
        for (const auto& entry : destroy_queue_) {
            out.push_back(entry);
        }
        destroy_queue_.clear();
        return out;
    }

    /// 停机：丢弃所有未应用的延迟销毁（world 马上拆掉）。返回丢弃数量。
    std::size_t drop_deferred_destroys() {
        std::lock_guard lock(destroy_mutex_);
        const auto count = destroy_queue_.size();
        destroy_queue_.clear();
        return count;
    }

    // ------------------------------------------------------------ 等待（存活判据）

    /// `await()` 的结算结果：结果本身 + 失败属于哪一类（决定计数与日志级别）。
    struct WaitOutcome {
        StructuralRequest::Result result;  ///< 结算槽里的结果
        bool abandoned{false};             ///< 等待方放弃（命令会被拥有者跳过）
        bool no_progress{false};           ///< 无进展预算耗尽（唯一的"真卡死"，计入 timeouts）
        bool unresponsive{false};          ///< 被闩锁立即拒绝（计入 unresponsive_rejections）
    };

    /// 按**存活性**等待一条请求结算（spec §5.1，Task 1c）。调用方线程执行。
    ///
    /// 失败只有两种原因：
    /// 1. DAG 已经停止 / 正在关停（停机判定先于重入判定，见 `submit_structural_command`）；
    /// 2. 连续 `no_progress_budget()` 内**没有任何 tick 边界完成**（心跳不前进）。
    ///
    /// **单个慢 tick 不失败**：项目加载期 Optics 的 `render_dispatch` 会占住 DAG
    /// 3–4s，这段时间心跳确实不前进，但预算（默认 10s）远大于它，所以等待方继续等，
    /// 边界恢复后的第一帧就拿到句柄。心跳每前进一次就把"无进展"计时器清零。
    ///
    /// 旧的两帧预算降级为诊断阈值：第一次越过它写一条 WARNING（保留 PhaseWatchdog
    /// 给的可见性），然后继续等。
    ///
    /// 无进展超时会打开 `DAG_UNRESPONSIVE` 闩锁：同一次卡死里后续的请求（以及装载
    /// 失败后的回滚销毁）不再各等一个预算，而是立即失败/立即返回。
    [[nodiscard]] WaitOutcome await(StructuralRequest& request, std::string_view what) {
        const auto budget = no_progress_budget();
        const auto diagnostic = slow_wait_diagnostic_threshold();
        const auto slice = std::min<std::chrono::milliseconds>(
            std::chrono::milliseconds{kLivenessPollMs}, budget);
        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        auto last_progress = started;
        auto observed = heartbeat();
        std::uint64_t boundaries_seen = 0;
        bool warned = false;

        // 已经判定不推进：请求早已入队，直接放弃，不再等第二个预算。
        if (unresponsive()) {
            WaitOutcome outcome{};
            outcome.result = request.abandon(std::string{kUnresponsiveReason});
            outcome.abandoned = true;
            outcome.unresponsive = true;
            return outcome;
        }

        for (;;) {
            if (request.wait_slice(slice)) {
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - started);
                if (waited >= diagnostic && !warned) {
                    record_slow_wait();
                    CFW_LOG_WARNING(
                        "[Ecs/StructuralCommand] {} took {}ms to reach a tick boundary "
                        "(two-frame diagnostic threshold {}ms); {} boundary/boundaries "
                        "completed while waiting, heartbeat #{} - a slow tick, not a stall",
                        what, waited.count(), diagnostic.count(), boundaries_seen,
                        static_cast<unsigned long long>(observed));
                }
                WaitOutcome outcome{};
                outcome.result = request.result();
                return outcome;
            }

            const auto now = Clock::now();
            if (stopped()) {
                // 停机结算：**不是**无进展超时（不计 timeouts），也不是等待方放弃
                // （命令已被 settle_failure 标记，拥有者不会应用它）。
                WaitOutcome outcome{};
                outcome.result = request.settle_failure_result(
                    "the systems DAG stopped while the request was still pending");
                return outcome;
            }

            // 心跳前进 = 边界完成 = 进展。先记账，再看闩锁：闩锁以心跳序号为准，
            // 如果心跳刚刚前进（边界刚好完成），这条闩锁就是陈旧的，不能据此拒绝。
            const auto current = heartbeat();
            const bool advanced = current != observed;
            if (advanced) {
                boundaries_seen += current - observed;  // 真心跳数，不是轮询次数
                observed = current;
                last_progress = now;
            }

            if (unresponsive()) {
                if (advanced) {
                    continue;  // 陈旧闩锁：边界刚完成，继续等而不是误拒一次
                }
                // 另一个等待方先一步把闩锁打开了：别在这里再耗一个预算。
                WaitOutcome outcome{};
                outcome.result = request.abandon(std::string{kUnresponsiveReason});
                outcome.abandoned = true;
                outcome.unresponsive = true;
                return outcome;
            }

            const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_progress);
            if (idle >= budget) {
                // 慢 vs 卡死：到这里就是"连续一个预算没有任何边界完成" = 卡死。
                if (!warned) {
                    record_slow_wait();  // 每条请求最多记一次，不重复计数
                }
                mark_unresponsive();
                std::string reason = "no tick boundary completed for ";
                reason += std::to_string(idle.count());
                reason += "ms (no-progress budget ";
                reason += std::to_string(budget.count());
                reason += "ms, last heartbeat #";
                reason += std::to_string(static_cast<unsigned long long>(observed));
                reason += "): the systems DAG is not advancing; ";
                reason += kUnresponsiveReason;
                CFW_LOG_ERROR(
                    "[Ecs/StructuralCommand] {}: further structural requests fail immediately "
                    "until a tick boundary completes again",
                    reason);
                WaitOutcome outcome{};
                outcome.result = request.abandon(std::move(reason));
                outcome.abandoned = true;
                outcome.no_progress = true;
                return outcome;
            }

            if (!warned && std::chrono::duration_cast<std::chrono::milliseconds>(now - started) >=
                               diagnostic) {
                warned = true;
                record_slow_wait();
                CFW_LOG_WARNING(
                    "[Ecs/StructuralCommand] {} has been waiting {}ms for a tick boundary "
                    "(two-frame diagnostic threshold {}ms, no-progress budget {}ms); {} "
                    "boundary/boundaries completed while waiting, heartbeat #{} - still "
                    "waiting, a slow tick keeps waiting",
                    what,
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count(),
                    diagnostic.count(), budget.count(), boundaries_seen,
                    static_cast<unsigned long long>(observed));
            }
        }
    }

    // ------------------------------------------------------------ 诊断

    [[nodiscard]] Stats stats() const {
        std::lock_guard lock(stats_mutex_);
        return stats_;
    }

    void reset_stats() {
        std::lock_guard lock(stats_mutex_);
        stats_ = Stats{};
    }

    void record_requested() { note([](Stats& s) { ++s.requested; }); }
    void record_inline_applied() { note([](Stats& s) { ++s.inline_applied; }); }
    void record_pre_assembly() { note([](Stats& s) { ++s.pre_assembly; }); }
    void record_rejected() { note([](Stats& s) { ++s.rejected; }); }
    void record_applied() { note([](Stats& s) { ++s.applied; }); }
    void record_failed() { note([](Stats& s) { ++s.failed; }); }
    void record_timeout() { note([](Stats& s) { ++s.timeouts; ++s.failed; }); }
    void record_unresponsive_rejection() { note([](Stats& s) { ++s.unresponsive_rejections; ++s.failed; }); }
    void record_slow_wait() { note([](Stats& s) { ++s.slow_waits; }); }
    void record_abandoned() { note([](Stats& s) { ++s.abandoned; }); }
    void record_deferred_destroy() { note([](Stats& s) { ++s.deferred_destroys; }); }
    void record_applied_destroy() { note([](Stats& s) { ++s.applied_destroys; }); }

   private:
    StructuralCommandChannel() = default;

    template <typename Mutate>
    void note(Mutate&& mutate) {
        std::lock_guard lock(stats_mutex_);
        mutate(stats_);
    }

    Buffer commands_;
    Kernel::ThreadAffinity affinity_;
    std::atomic<bool> stopped_{false};            ///< 停机后拒绝新请求
    /// 闩锁的时间戳 = "判定无进展时的心跳序号"（不是布尔量，见 `unresponsive()`）
    std::atomic<std::uint64_t> unresponsive_beat_{kNoUnresponsiveBeat};
    std::atomic<bool> owner_expected_{false};     ///< 已有装配方承诺"会有人 drain"
    std::atomic<bool> boundary_installed_{false}; ///< DAG tick 边界钩子已安装
    std::atomic<std::uint64_t> request_ids_{0};
    std::atomic<std::uint64_t> heartbeat_{0};     ///< tick 边界心跳（单调递增）
    std::atomic<std::int64_t> frame_period_us_{kDefaultFramePeriodUs};

    std::atomic<std::int64_t> no_progress_override_ms_{structural_no_progress_override_ms()};

    /// 延迟销毁队列：自己的锁、**不设固定容量**（规模 = 被卸载场景的实体数）。
    mutable std::mutex destroy_mutex_;
    std::deque<DeferredDestroy> destroy_queue_;

    /// 额外边界任务（网络同步收口）。注册在装配期，执行在拥有者线程。
    mutable std::mutex boundary_tasks_mutex_;
    std::vector<TickBoundaryTask> boundary_tasks_;

    mutable std::mutex stats_mutex_;
    Stats stats_{};
};

/// 应用入站结构命令（tick 边界，先于 scene 命令）。绑定/刷新 registry 的拥有者线程。
inline bool apply_pending_structural_commands() {
    auto& channel = StructuralCommandChannel::instance();
    channel.affinity().bind_to_current_thread();
    return channel.apply_pending();
}

/// 注册一条额外的 tick 边界任务（装配期调用；拥有者线程执行）。
/// 重复注册同一函数指针返回 false。
[[nodiscard]] inline bool add_tick_boundary_task(TickBoundaryTask task) {
    return StructuralCommandChannel::instance().add_boundary_task(task);
}

/// DAG 降级路径（`CORONA_SYSTEMS_DAG=0`）的兜底 drain：只在装配方**没有**安装
/// tick 边界钩子时生效。DAG 模式下返回 false 且不消费队列，因此实体集合在
/// 一个 tick 内不会因为 GeometrySystem::update() 而改变。
inline bool apply_pending_structural_commands_legacy_fallback() {
    if (StructuralCommandChannel::instance().tick_boundary_installed()) {
        return false;
    }
    return apply_pending_structural_commands();
}

/// DAG 线程启动（由 `SystemsThread` 的 start 钩子调用，接线在 engine.cpp）。
inline void on_dag_thread_started() {
    StructuralCommandChannel::instance().on_dag_started();
}

/// DAG 线程停止（由 `SystemsThread` 的 stop 钩子调用，接线在 engine.cpp）。
inline void on_dag_thread_stopping() {
    StructuralCommandChannel::instance().on_dag_stopping("systems DAG thread is stopping");
}

// ============================================================================
// 请求入口（任意调用线程）
// ============================================================================

/// 提交一条结构命令并按"同步应答"契约结算。
///
/// - 停机后即使调用方是 DAG 线程也不允许再写 registry：引擎正在关停，新建实体会
///   落在一个马上要被拆掉的世界上。所以停机判定在重入判定之前。
/// - 调用方就是 DAG 线程 → 直接应用（重入规则）；
/// - 装配前（还没有拥有者） → 调用方直接应用（此时不存在第二个写者）；
/// - 否则入队并按**存活性**等待（`await()`）：DAG 停机 / 连续无进展预算内没有边界
///   完成才失败；单个慢 tick 会继续等，然后成功。
///
/// `loud` 决定失败时的日志级别：创建是 ERROR（调用方拿到的是无效句柄），销毁是
/// WARNING（对象本来就在退出路径上，停机期拒绝销毁是预期行为）。
[[nodiscard]] inline EntityId submit_structural_command(StructuralPayload payload,
                                                       std::string_view what,
                                                       bool loud = true) {
    auto& channel = StructuralCommandChannel::instance();

    if (channel.stopped()) {
        channel.record_rejected();
        if (loud) {
            CFW_LOG_ERROR(
                "[Ecs/StructuralCommand] {} rejected: the systems DAG is not accepting "
                "structural commands (stopped)",
                what);
        } else {
            CFW_LOG_WARNING(
                "[Ecs/StructuralCommand] {} rejected: the systems DAG is not accepting "
                "structural commands (stopped)",
                what);
        }
        return 0;
    }

    // 重入：系统在 DAG 线程内部创建/销毁实体。入队后等自己 = 自死锁。
    if (channel.affinity().is_owner_thread()) {
        channel.record_inline_applied();
        return apply_structural_payload(payload);
    }

    // 装配前：装配方还没宣布拥有者存在（系统线程尚未装配、或单线程夹具）。
    // 此时调用线程就是唯一的写者，直接应用；记 pre_assembly 以便诊断里一眼看出
    // "这条结构变更没有经过 tick 边界"。
    if (!channel.owner_expected()) {
        channel.record_pre_assembly();
        return apply_structural_payload(payload);
    }

    // DAG_UNRESPONSIVE 闩锁：已经确认边界不推进（一次无进展超时），后面的请求
    // **立即**失败，不再各等一个预算 —— 否则装载失败后的回滚会按实体数 × 预算放大。
    // 下一次 tick 边界完成会自动解锁。注意位置：停机 / 重入 / 装配前判定都在它前面，
    // 因为拥有者线程自己还在跑就说明 DAG 活着。
    if (channel.unresponsive()) {
        channel.record_unresponsive_rejection();
        if (loud) {
            CFW_LOG_ERROR("[Ecs/StructuralCommand] {} rejected immediately: {}", what,
                          StructuralCommandChannel::kUnresponsiveReason);
        } else {
            CFW_LOG_WARNING("[Ecs/StructuralCommand] {} rejected immediately: {}", what,
                            StructuralCommandChannel::kUnresponsiveReason);
        }
        return 0;
    }

    auto request = std::make_shared<StructuralRequest>(channel.next_request_id());
    StructuralCommand command{};
    command.request = request;
    command.payload = std::move(payload);
    command.what = what;
    if (!channel.commands().push(std::move(command))) {
        channel.record_rejected();
        if (loud) {
            CFW_LOG_ERROR(
                "[Ecs/StructuralCommand] {} rejected: structural command queue is full "
                "({} pending); the DAG is not draining it",
                what, channel.commands().size());
        } else {
            CFW_LOG_WARNING(
                "[Ecs/StructuralCommand] {} rejected: structural command queue is full "
                "({} pending); the DAG is not draining it",
                what, channel.commands().size());
        }
        return 0;
    }
    channel.record_requested();

    const StructuralCommandChannel::WaitOutcome outcome = channel.await(*request, what);
    const StructuralRequest::Result& result = outcome.result;
    if (!result.ok()) {
        // 失败只在这里记一次，并且按**原因**分开计数：
        // - 无进展超时 → timeouts（真卡死）；
        // - 闩锁立即拒绝 → unresponsive_rejections；
        // - 停机 / 命令本身失败 → failed（停机不算超时）。
        if (outcome.no_progress) {
            channel.record_timeout();
        } else if (outcome.unresponsive) {
            channel.record_unresponsive_rejection();
        } else {
            channel.record_failed();
        }
        if (outcome.abandoned) {
            if (loud) {
                CFW_LOG_ERROR(
                    "[Ecs/StructuralCommand] {} abandoned: {}; owner thread {}; the late "
                    "command is skipped, nothing is applied",
                    what, result.error,
                    channel.affinity().bound()
                        ? "is bound but did not complete a tick boundary"
                        : "has never drained this channel (DAG not running?)");
            } else {
                CFW_LOG_WARNING(
                    "[Ecs/StructuralCommand] {} abandoned: {}; the late command is skipped, "
                    "nothing is applied",
                    what, result.error);
            }
        } else if (loud) {
            CFW_LOG_ERROR("[Ecs/StructuralCommand] {} failed: {}", what, result.error);
        } else {
            CFW_LOG_WARNING("[Ecs/StructuralCommand] {} failed: {}", what, result.error);
        }
        return 0;
    }
    channel.record_applied();
    return result.handle;
}

// ---- 创建（返回有效句柄；0 = 失败，调用方按失败处理）----

[[nodiscard]] inline EntityId request_create_model_resource(CreateModelResourcePayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_model_resource");
}
[[nodiscard]] inline EntityId request_create_transform(CreateTransformPayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_transform");
}
[[nodiscard]] inline EntityId request_create_geometry(CreateGeometryPayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_geometry");
}
[[nodiscard]] inline EntityId request_create_mechanics(CreateMechanicsPayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_mechanics");
}
[[nodiscard]] inline EntityId request_create_optics(CreateOpticsPayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_optics");
}
[[nodiscard]] inline EntityId request_create_acoustics(CreateAcousticsPayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_acoustics");
}
[[nodiscard]] inline EntityId request_create_profile(CreateProfilePayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_profile_component");
}
[[nodiscard]] inline EntityId request_create_actor(CreateActorPayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_actor_component");
}
[[nodiscard]] inline EntityId request_create_camera(CreateCameraPayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_camera");
}
[[nodiscard]] inline EntityId request_create_environment(CreateEnvironmentPayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_environment");
}
[[nodiscard]] inline EntityId request_create_scene(CreateScenePayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_scene");
}
[[nodiscard]] inline EntityId request_create_image(CreateImagePayload payload) {
    return submit_structural_command(StructuralPayload{std::move(payload)}, "create_image");
}

// ---- 销毁：**不等待结算**（fire-and-forget），走自己的无界队列 ----
//
// 销毁是幂等的，调用方不需要"实体已经没了"这个同步答案。等待却会把一次卡死的代价
// 按实体数放大：装载失败回滚一个 actor 要销毁 ~8 个实体（geometry/transform/
// model_resource/optics/mechanics/acoustics/profile/actor），每个等满一个无进展预算
// 就是 8 × 10s；N 个 actor 就是 ~80N 秒的 CEF/主线程冻结。
//
// 所以销毁只负责把命令交出去：
// - 拥有者线程 / 装配前：直接应用（与创建同一规则）；
// - 停机：world 马上被拆掉，不销毁，记诊断并返回 false（不阻塞）；
// - 否则入队到**延迟销毁队列**并立即返回。那条队列没有固定容量 —— 一次
//   `scene.reset()` 的销毁数量由场景里有多少实体决定，用共享 Fifo 的 4096 上限会
//   在 ~512 个 actor 时开始丢命令，把活着的 DAG 上的实体变成孤儿（Task 1c review
//   item 1）。DAG 恢复后下一次边界会应用它们；如果 DAG 再也不恢复，它们随进程退出
//   一起消失，日志里点名句柄（见 `kUnresponsiveReason`）。

/// 提交一条不等待结算的结构命令（目前只有销毁用它）。
[[nodiscard]] inline bool submit_structural_command_without_waiting(StructuralPayload payload,
                                                                   std::string_view what,
                                                                   EntityId handle) {
    auto& channel = StructuralCommandChannel::instance();

    if (channel.stopped()) {
        channel.record_rejected();
        CFW_LOG_WARNING(
            "[Ecs/StructuralCommand] {} #{} rejected: the systems DAG is not accepting "
            "structural commands (stopped); the entity dies with the world",
            what, handle);
        return false;
    }

    if (channel.affinity().is_owner_thread() || !channel.owner_expected()) {
        channel.record_inline_applied();
        apply_structural_payload(payload);
        return true;
    }

    // 独立队列、无固定上限：不丢命令。只在闩锁打开时点一句"如果 DAG 不恢复"，
    // 其余情况静默入队（正常 teardown 会一次压很多条，逐条打日志只会淹没有用信息）。
    const bool accepted = channel.enqueue_deferred_destroy(
        std::get<DestroyEntityPayload>(payload), what);
    if (!accepted) {
        channel.record_rejected();
        CFW_LOG_WARNING(
            "[Ecs/StructuralCommand] {} #{} rejected: the deferred destroy queue refused the "
            "command; the entity is orphaned until the world is torn down",
            what, handle);
        return false;
    }
    if (channel.unresponsive()) {
        CFW_LOG_WARNING(
            "[Ecs/StructuralCommand] {} #{} queued without waiting: {}; the entity is "
            "orphaned until the world is torn down",
            what, handle, StructuralCommandChannel::kUnresponsiveReason);
    }
    return true;
}

[[nodiscard]] inline bool request_destroy_entity(EntityId handle,
                                                 std::string_view what = "destroy_entity") {
    if (handle == 0) {
        return true;
    }
    return submit_structural_command_without_waiting(
        StructuralPayload{DestroyEntityPayload{handle, false}}, what, handle);
}

[[nodiscard]] inline bool request_destroy_image(EntityId handle) {
    if (handle == 0) {
        return true;
    }
    return submit_structural_command_without_waiting(
        StructuralPayload{DestroyEntityPayload{handle, true}}, "destroy_image", handle);
}

}  // namespace Corona::Ecs
