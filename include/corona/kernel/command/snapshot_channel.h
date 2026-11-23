#pragma once

/// 只读快照通道：**单发布者**（拥有数据的 DAG 系统，每 tick 一次），
/// 多读者（CEF / 脚本 / 网络 / 主线程，任意时刻）。
///
/// 设计要点（见 docs/dag-command-vs-event.md 第 2.2 节）：
/// - 读者拿到的是一份不可变值（shared_ptr<const T>），生命周期与 DAG 解耦：
///   DAG 继续发布新快照不会让读者手里的旧快照失效。
/// - 读者永远拿到"某个完整 tick 的状态"，不会读到半更新（不是逐字段撕裂的读）。
/// - 没有队列、没有丢失语义、没有回执：读一次就是此刻的最新值。
///
/// 线程模型：内部一把 std::mutex，临界区只做一次 shared_ptr 拷贝，
/// 与 DAG 的槽锁相比不再持有任何存储锁。发布者由调用方保证单线程。

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace Corona::Kernel {

/**
 * @brief 单发布者 / 多读者的快照通道
 *
 * @tparam Snapshot 快照值类型（按值发布，内部持有 const 副本）
 */
template <typename Snapshot>
class SnapshotChannel {
   public:
    SnapshotChannel() = default;
    SnapshotChannel(const SnapshotChannel&) = delete;
    SnapshotChannel& operator=(const SnapshotChannel&) = delete;

    /**
     * @brief 发布一份新快照（拥有者线程，每 tick 一次）
     * @return 新快照的 revision（从 1 开始单调递增）
     */
    std::uint64_t publish(Snapshot snapshot) {
        auto shared = std::make_shared<const Snapshot>(std::move(snapshot));
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = std::move(shared);
        return ++revision_;
    }

    /// 读取当前快照（任意线程）；尚未发布过时返回 nullptr
    [[nodiscard]] std::shared_ptr<const Snapshot> read() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }

    /// 读取当前快照并带出 revision
    [[nodiscard]] std::shared_ptr<const Snapshot> read(std::uint64_t* out_revision) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (out_revision != nullptr) {
            *out_revision = revision_;
        }
        return current_;
    }

    /// 当前 revision（未发布过为 0）
    [[nodiscard]] std::uint64_t revision() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return revision_;
    }

    [[nodiscard]] bool published() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_ != nullptr;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        current_.reset();
        revision_ = 0;
    }

   private:
    mutable std::mutex mutex_;
    std::shared_ptr<const Snapshot> current_;
    std::uint64_t revision_{0};
};

}  // namespace Corona::Kernel
