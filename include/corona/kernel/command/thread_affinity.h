#pragma once

/// 单写者纪律的运行时护栏。
///
/// DAG 的存储是"单线程写"的：任何外部线程拿到哪怕只读引用都是设计错误
/// （见 docs/dag-command-vs-event.md 第 5 节）。编译期靠头文件边界，
/// 运行期靠这个断言在**开发期立刻喊出来**，而不是等到线上出现撕裂读。
///
/// 有意做成 header-only 且不依赖 windows.h：线程标识用
/// std::hash<std::thread::id>，与平台无关，也不把 Horizon 日志拉进内核头。
///
/// 用法：
/// @code
///   // 拥有者线程（DAG tick 开始时）
///   SharedDataHub::instance().bind_owner_thread();
///   // 任何可能越界的地方
///   hub.assert_owner_thread("CameraDevice write");
/// @endcode

#include <horizon/core/logging.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string_view>
#include <thread>

namespace Corona::Kernel {

/// 当前线程的标识（进程内唯一即可，无需与 OS 线程 id 对应）
[[nodiscard]] inline std::uint32_t current_thread_token() noexcept {
    return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

/**
 * @brief 记录"拥有者线程"，并允许任意线程检查自己是不是它
 */
class ThreadAffinity {
   public:
    /// 把当前线程登记为拥有者（重复调用以最后一次为准）
    void bind_to_current_thread() noexcept {
        owner_.store(current_thread_token(), std::memory_order_release);
    }

    [[nodiscard]] bool bound() const noexcept {
        return owner_.load(std::memory_order_acquire) != 0;
    }

    [[nodiscard]] bool is_owner_thread() const noexcept {
        const auto owner = owner_.load(std::memory_order_acquire);
        return owner != 0 && owner == current_thread_token();
    }

    /**
     * @brief 断言当前线程是拥有者；不是则记一条 ERROR 日志
     * @param what 出错位置的可读描述，例如 "CameraDevice::position write"
     * @return 当前线程是否为拥有者
     */
    bool assert_owner_thread(std::string_view what) const {
        if (!bound() || is_owner_thread()) {
            return true;
        }
        CFW_LOG_ERROR("[ThreadAffinity] 非拥有者线程越过单写者边界: {}", what);
        violations_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// 越界次数（测试与诊断用）
    [[nodiscard]] std::uint64_t violations() const noexcept {
        return violations_.load(std::memory_order_relaxed);
    }

    void reset() noexcept {
        owner_.store(0, std::memory_order_release);
        violations_.store(0, std::memory_order_relaxed);
    }

   private:
    std::atomic<std::uint32_t> owner_{0};
    mutable std::atomic<std::uint64_t> violations_{0};
};

}  // namespace Corona::Kernel
