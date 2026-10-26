#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace Corona::Kernel {

/**
 * @brief 帧图节点 id
 *
 * 由 FrameGraph::add_node() 分配，从 0 开始单调递增，等于节点的插入序。
 * 插入序同时决定拓扑排序平局时的稳定顺序，因此执行序在同样的输入下永远一致。
 */
using FrameNodeId = std::uint32_t;

/// 无效节点 id
inline constexpr FrameNodeId kInvalidFrameNodeId = static_cast<FrameNodeId>(-1);

/**
 * @brief 一次 tick 的上下文
 */
struct FrameTick {
    std::uint64_t tick = 0;      ///< 单调递增的帧号，从 0 开始
    double delta_seconds = 0.0;  ///< 与上一 tick 的墙钟间隔（秒）
};

/**
 * @brief 节点体返回的状态
 */
enum class FrameNodeStatus : std::uint8_t {
    ran,   ///< 正常执行并产出了本帧结果
    idle,  ///< 执行了但本帧无事可做（例如队列为空），不算错误
};

/**
 * @brief 帧图节点描述
 *
 * 语义约定（三条，务必区分）：
 * 1. `dependencies` 表示"必须先执行"，不表示"必须产出新数据"。上游被速率门控
 *    跳过时下游仍然执行（见下条），这是 M-β 模型：慢链路按自己的节拍跑，快链路
 *    不因为上游本 tick 没跑就停下。
 * 2. `period_ticks` / `phase` 是**速率门控**：只有 `tick % period_ticks ==
 *    phase % period_ticks` 时才执行本节点，**不影响下游**。默认 period_ticks = 1
 *    即每 tick 都执行。
 * 3. `enabled = false` 是**子树关闭**：本节点以及所有（直接或间接）依赖它的节点
 *    本 tick 都不执行。用于整体停掉一条链路，而不是降频。
 */
struct FrameNodeDesc {
    std::string name;                       ///< 稳定名称：日志、dump、依赖引用都用它
    std::vector<FrameNodeId> dependencies;  ///< 必须先执行的节点 id
    std::uint32_t period_ticks = 1;         ///< 速率门控周期（1 = 每 tick 都跑）
    std::uint32_t phase = 0;                ///< 速率门控相位，必须 < period_ticks
    bool enabled = true;                    ///< 关闭时连同下游子树一起不执行
    std::function<FrameNodeStatus(const FrameTick&)> body;  ///< 节点体；为空按 idle 处理
};

/**
 * @brief 单节点累计统计
 *
 * 全部为单调累计值：average_ms 是历次执行的算术平均，max_ms 是历史最大值。
 * 统计只在 run_tick() 所在的线程写入，读取方必须自己保证不发生并发写。
 */
struct FrameNodeStats {
    std::uint64_t invocations = 0;    ///< 真正执行次数（含 idle）
    std::uint64_t idle_invocations = 0;  ///< 其中返回 idle 的次数
    std::uint64_t gated_out = 0;      ///< 被速率门控跳过的次数
    std::uint64_t disabled_out = 0;   ///< 因自身或上游关闭而跳过的次数
    std::uint64_t failures = 0;       ///< 抛异常的累计次数
    std::uint32_t consecutive_failures = 0;  ///< 连续失败次数：成功执行一次即清零
    double last_ms = 0.0;             ///< 最近一次执行耗时（毫秒）
    double average_ms = 0.0;          ///< 平均执行耗时（毫秒）
    double max_ms = 0.0;              ///< 最大执行耗时（毫秒）
};

/**
 * @brief 一次 run_tick() 的结果
 */
struct FrameTickReport {
    bool completed = true;                    ///< 全部节点跑完为 true；节点抛异常或被看门狗中止为 false
    FrameNodeId failed_node = kInvalidFrameNodeId;  ///< 抛异常的节点（completed=false 时有效）
    FrameNodeId stopped_on = kInvalidFrameNodeId;   ///< 中止时停在哪个节点（含被中止钩子要求停止）
    std::string error;                        ///< 异常文本（completed=false 时有效）
    std::uint64_t executed = 0;               ///< 真正执行的节点数
    std::uint64_t idle = 0;                   ///< 其中返回 idle 的节点数
    std::uint64_t gated = 0;                  ///< 被速率门控跳过的节点数
    std::uint64_t disabled = 0;               ///< 被子树关闭跳过的节点数
    double total_ms = 0.0;                    ///< 本 tick 所有节点耗时之和（毫秒）
};

/**
 * @brief 单节点的统计快照（带名字与 id，便于外部直接打印）
 */
struct FrameNodeSnapshot {
    FrameNodeId id{kInvalidFrameNodeId};
    std::string_view name;  ///< 指向图内节点名，图的生命周期内有效
    FrameNodeStats stats;
};

/**
 * @brief 整图统计快照
 *
 * 拷贝式快照：调用方拿到之后可以慢慢读，不受 tick 线程影响。
 */
struct FrameGraphStats {
    std::vector<FrameNodeSnapshot> nodes;
};

/**
 * @brief 帧图：在**调用线程上内联执行**的 DAG 调度器
 *
 * 设计约束（决定了它为什么不直接套用 taskflow / TBB flow_graph）：
 * - 节点一律在调用 run_tick() 的线程上就地执行，不把节点甩到内部 worker 线程。
 *   引擎里系统数据依赖通过 SharedDataHub 的锁暴露，跨线程执行会让"谁在持锁"
 *   重新变得不可预测；先要的是**确定的执行序与边界**，不是吞吐。
 * - 依赖是节点级而不是系统级：一次 tick 里可以拆出十几个节点，便于打点、限速、
 *   单独关停某条链路。
 * - 稳定执行序：拓扑排序平局按插入序决定，同样的图永远得到同样的序。
 * - 异常不跨出 run_tick()：节点抛出的异常被捕获并记录在 FrameTickReport 里，
 *   由调用方决定是停机还是记录后继续。这避免了 SystemBase::thread_loop() 里
 *   "任一次 update 抛异常就 publish EngineShutdownEvent" 那种一刀切行为。
 *
 * 线程模型：FrameGraph 自身不加锁。run_tick() 与所有写操作必须来自同一个线程；
 * 跨线程读取统计请通过 SystemsThread 提供的快照接口。
 *
 * 使用示例：
 * @code
 * FrameGraph graph;
 * auto skin = graph.add_node({
 *     .name = "Mechanics.skin",
 *     .body = [&](const FrameTick& tick) { mechanics.update_skinning(tick.delta_seconds);
 *                                          return FrameNodeStatus::ran; },
 * });
 * graph.add_node({
 *     .name = "Mechanics.step",
 *     .dependencies = {skin},
 *     .body = [&](const FrameTick& tick) { mechanics.step(tick.delta_seconds);
 *                                          return FrameNodeStatus::ran; },
 * });
 *
 * std::string error;
 * if (!graph.compile(&error)) { CFW_LOG_ERROR("frame graph: {}", error); }
 * FrameTick tick{0, 1.0 / 120.0};
 * const auto report = graph.run_tick(tick);
 * @endcode
 */
class FrameGraph {
   public:
    FrameGraph() = default;
    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;
    FrameGraph(FrameGraph&&) noexcept = default;
    FrameGraph& operator=(FrameGraph&&) noexcept = default;

    // ========================================
    // 构图
    // ========================================

    /**
     * @brief 添加节点
     * @param desc 节点描述，name 必须非空且全局唯一
     * @return 新节点 id
     * @throws std::invalid_argument name 为空、重名，或 phase >= period_ticks
     *
     * 添加节点会让已编译的执行序失效（需要重新 compile()）。
     */
    FrameNodeId add_node(FrameNodeDesc desc);

    /**
     * @brief 按名称查找节点
     * @return 找到返回节点 id，否则 kInvalidFrameNodeId
     */
    [[nodiscard]] FrameNodeId find_node(std::string_view name) const;

    /**
     * @brief 节点数量
     */
    [[nodiscard]] std::size_t node_count() const;

    /**
     * @brief 节点名称；id 无效时返回空字符串
     */
    [[nodiscard]] std::string_view node_name(FrameNodeId id) const;

    /**
     * @brief 替换节点的依赖列表
     * @return 节点存在返回 true
     *
     * 允许在 add_node() 之后再补边，因此依赖环是**可以被构造出来**的 ——
     * compile() 会拒绝它（见 "依赖环"）。这一条也是"节点必须先声明对端"之外
     * 唯一的连边方式，按名称装配图时靠的就是它。
     */
    bool set_dependencies(FrameNodeId id, std::vector<FrameNodeId> dependencies);

    // ========================================
    // 编译
    // ========================================

    /**
     * @brief 校验依赖并生成稳定拓扑序
     * @param error 失败时写入人类可读的原因（悬空依赖、自依赖、环）
     * @return 成功返回 true
     */
    bool compile(std::string* error = nullptr);

    /**
     * @brief 是否已经有可执行的拓扑序
     */
    [[nodiscard]] bool compiled() const { return compiled_; }

    /**
     * @brief 拓扑序（编译成功后可读）
     */
    [[nodiscard]] const std::vector<FrameNodeId>& execution_order() const;

    // ========================================
    // 执行
    // ========================================

    /**
     * @brief 执行一个 tick
     * @return 本 tick 的执行报告
     *
     * 未编译时直接返回 completed = false 的报告（error = "frame graph is not compiled"）。
     */
    FrameTickReport run_tick(const FrameTick& tick);

    /**
     * @brief 节点边界钩子：每个"执行过"的节点返回后调用一次
     *
     * 留给自己插入事件 drain、Tracy 标记、看门狗心跳。钩子不得抛异常。
     */
    void set_boundary_hook(std::function<void(FrameNodeId, const FrameTick&)> hook);

    // ========================================
    // 运行时开关
    // ========================================

    /**
     * @brief 打开/关闭节点的子树（见 FrameNodeDesc::enabled 的语义）
     * @return 节点存在返回 true
     */
    bool set_enabled(FrameNodeId id, bool enabled);

    /**
     * @brief 改速率门控
     * @return 节点存在且参数合法返回 true
     */
    bool set_rate(FrameNodeId id, std::uint32_t period_ticks, std::uint32_t phase = 0);

    // ========================================
    // 诊断
    // ========================================

    /**
     * @brief 节点统计快照；id 无效时返回默认值
     */
    [[nodiscard]] FrameNodeStats stats(FrameNodeId id) const;

    /**
     * @brief 整图统计快照（逐节点拷贝）
     */
    [[nodiscard]] FrameGraphStats snapshot_stats() const;

    /**
     * @brief 清零所有统计
     */
    void reset_stats();

    /**
     * @brief 文本转储：执行序 + 逐节点计数与耗时，用于日志与测试断言
     */
    [[nodiscard]] std::string dump() const;

   private:
    struct Node {
        FrameNodeDesc desc;
        FrameNodeStats stats;
    };

    bool valid(FrameNodeId id) const { return id < nodes_.size(); }

    std::vector<Node> nodes_;
    std::vector<FrameNodeId> order_;
    std::vector<std::uint8_t> dependents_mark_;  ///< 每个节点的下游掩码（run_tick 复用）
    bool compiled_ = false;
    std::function<void(FrameNodeId, const FrameTick&)> boundary_hook_;
};

}  // namespace Corona::Kernel
