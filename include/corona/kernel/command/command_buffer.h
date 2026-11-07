#pragma once

/// 入站命令队列：**单消费者**（拥有数据的 DAG 系统），多生产者（CEF / 脚本 / 网络 / 主线程）。
///
/// 设计要点（见 docs/dag-command-vs-event.md 第 2.1 节）：
/// - 连续量用 LatestWins：按 key 合并，只保留最新一条（相机拖拽、滑条、gizmo）。
/// - 离散量用 Fifo：按序全量投递，队列满时拒绝并计数（创建、销毁、显隐）。
/// - 出队顺序由单调 sequence 决定，与生产者线程的调度顺序无关 —— 因此可复现。
/// - peek() 给"读己之写"提供待提交视图，替代过去散落在各处的 requested_* 镜像 map。
///
/// 线程模型：内部一把 std::mutex。push()/peek()/size() 可在任意线程调用，
/// drain() 只允许消费线程调用（由调用方保证，P0 先靠纪律，后续可用
/// ThreadAffinity 断言收紧）。
///
/// 用法：
/// @code
///   struct CameraMoveCommand { std::uintptr_t camera_handle; ... };
///   struct CameraMoveKey {
///       static std::uintptr_t get(const CameraMoveCommand& c) noexcept { return c.camera_handle; }
///   };
///   using CameraMoveBuffer =
///       Kernel::CommandBuffer<CameraMoveCommand, CameraMoveKey, Kernel::CoalescePolicy::LatestWins>;
/// @endcode

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Corona::Kernel {

/// 合并策略
enum class CoalescePolicy : std::uint8_t {
    Fifo,        ///< 离散命令：按序全量投递，队列满时拒绝新命令
    LatestWins,  ///< 连续命令：同一个 key 只保留最新一条，队列满时挤掉最旧的 key
};

/// 合并键提取器（默认：整条队列共用一个 key，只适用于全局唯一状态）
template <typename Command>
struct CommandKey {
    static std::uintptr_t get(const Command&) noexcept { return 0; }
};

/**
 * @brief 有界、线程安全、单消费者的入站命令队列
 *
 * @tparam Command 命令值类型（按值传递，不含裸指针）
 * @tparam KeyOf   合并键提取器，需提供 static std::uintptr_t get(const Command&)
 * @tparam Policy  合并策略，见 CoalescePolicy
 */
template <typename Command,
          typename KeyOf = CommandKey<Command>,
          CoalescePolicy Policy = CoalescePolicy::Fifo>
class CommandBuffer {
   public:
    /// 计数器快照（读一次是一致的一组值）
    struct Stats {
        std::uint64_t pushed{0};     ///< 成功入队次数
        std::uint64_t drained{0};    ///< 成功出队的命令数
        std::uint64_t coalesced{0};  ///< 被更新的同 key 命令覆盖掉的旧命令数
        std::uint64_t dropped{0};    ///< 因容量上限被丢弃 / 拒绝的命令数
    };

    explicit CommandBuffer(std::size_t capacity = kDefaultCapacity)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    // ---------------------------------------------------------------- 生产者

    /**
     * @brief 入队一条命令（任意线程）
     * @return Fifo：队列满时返回 false（命令被拒绝）；LatestWins：总是返回 true
     *         （必要时挤掉最旧的 key，计入 Stats::dropped）
     */
    bool push(Command command) {
        const std::uintptr_t key = KeyOf::get(command);
        std::lock_guard<std::mutex> lock(mutex_);

        Entry entry{next_sequence_++, std::move(command)};

        if constexpr (Policy == CoalescePolicy::LatestWins) {
            auto it = latest_.find(key);
            if (it != latest_.end()) {
                it->second = std::move(entry);
                ++stats_.pushed;
                ++stats_.coalesced;
                return true;
            }
            if (latest_.size() >= capacity_) {
                evict_oldest_key_locked();
            }
            latest_.emplace(key, std::move(entry));
            ++stats_.pushed;
            return true;
        } else {
            if (fifo_.size() >= capacity_) {
                ++stats_.dropped;
                return false;
            }
            fifo_.push_back(std::move(entry));
            ++stats_.pushed;
            return true;
        }
    }

    // ---------------------------------------------------------------- 消费者

    /// 取出当前所有命令并清空队列（消费线程）
    [[nodiscard]] std::vector<Command> drain() {
        std::vector<Command> out;
        drain_into(out);
        return out;
    }

    /// 取出到调用方持有的容器（避免每 tick 新分配）
    void drain_into(std::vector<Command>& out) {
        out.clear();
        std::lock_guard<std::mutex> lock(mutex_);

        if constexpr (Policy == CoalescePolicy::LatestWins) {
            scratch_.clear();
            scratch_.reserve(latest_.size());
            for (auto& entry : latest_) {
                scratch_.push_back(std::move(entry.second));
            }
            latest_.clear();
            std::sort(scratch_.begin(), scratch_.end(), [](const Entry& lhs, const Entry& rhs) {
                return lhs.sequence < rhs.sequence;
            });
            out.reserve(scratch_.size());
            for (auto& entry : scratch_) {
                out.push_back(std::move(entry.command));
            }
        } else {
            out.reserve(fifo_.size());
            for (auto& entry : fifo_) {
                out.push_back(std::move(entry.command));
            }
            fifo_.clear();
        }

        stats_.drained += out.size();
        scratch_.clear();
    }

    // ---------------------------------------------------------------- 查询

    /**
     * @brief 读取尚未被消费的最新命令（任意线程）
     *
     * 这就是"读己之写"的待提交视图：写命令的线程可以在下一 tick 消费前
     * 读到自己刚提交的值，而不必等待 DAG 应用。
     */
    [[nodiscard]] std::optional<Command> peek(std::uintptr_t key) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if constexpr (Policy == CoalescePolicy::LatestWins) {
            auto it = latest_.find(key);
            if (it != latest_.end()) {
                return it->second.command;
            }
            return std::nullopt;
        } else {
            for (const auto& entry : fifo_) {
                if (KeyOf::get(entry.command) == key) {
                    return entry.command;
                }
            }
            return std::nullopt;
        }
    }

    /// 丢弃某个 key 的未消费命令（槽位销毁时调用，避免向死实体写）
    bool erase(std::uintptr_t key) {
        std::lock_guard<std::mutex> lock(mutex_);
        if constexpr (Policy == CoalescePolicy::LatestWins) {
            return latest_.erase(key) > 0;
        } else {
            const auto before = fifo_.size();
            fifo_.erase(std::remove_if(fifo_.begin(), fifo_.end(),
                                       [key](const Entry& entry) {
                                           return KeyOf::get(entry.command) == key;
                                       }),
                        fifo_.end());
            return fifo_.size() != before;
        }
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if constexpr (Policy == CoalescePolicy::LatestWins) {
            return latest_.size();
        } else {
            return fifo_.size();
        }
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_.clear();
        fifo_.clear();
        scratch_.clear();
    }

    static constexpr std::size_t kDefaultCapacity = 4096;

   private:
    struct Entry {
        std::uint64_t sequence{0};
        Command command{};
    };

    void evict_oldest_key_locked() {
        auto oldest = latest_.begin();
        for (auto it = latest_.begin(); it != latest_.end(); ++it) {
            if (it->second.sequence < oldest->second.sequence) {
                oldest = it;
            }
        }
        if (oldest != latest_.end()) {
            latest_.erase(oldest);
            ++stats_.dropped;
        }
    }

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::uint64_t next_sequence_{0};
    std::deque<Entry> fifo_;                                         ///< Fifo 专用
    std::unordered_map<std::uintptr_t, Entry> latest_;               ///< LatestWins 专用
    mutable std::vector<Entry> scratch_;                             ///< drain 复用，避免每 tick 分配
    Stats stats_{};
};

}  // namespace Corona::Kernel
