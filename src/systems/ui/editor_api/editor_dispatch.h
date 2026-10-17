#pragma once

/// 编辑器状态派发：单一 owner 线程（CEF UI 线程）
///
/// 背景：编辑器场景状态（`native_editor_state()` 及其 `NativeEditorScene`）被三类线程
/// 触碰 —— CEF UI 线程（JS 请求）、引擎主线程（协作同步）、Python/Script 运行时线程
/// （`_invoke_cpp_editor_api`）。它们此前的唯一"同步"是运气：状态里没有一把锁，handler
/// 也按单线程写（缓存、表、`unique_ptr` 成员）。并发进来就是堆损坏。
///
/// 方案 A：让**所有** handler 只在 owner 线程执行。
///   - owner 线程上的调用（JS）：就地执行（该线程本来串行）；
///   - 其它线程：把执行打包 post 到 owner 线程并**有界**等待（超时返回错误/撤销，不无限
///     等待，避免把 Python 线程或引擎主线程也拖死）。Python（`_invoke_cpp_editor_api`）和
///     引擎主线程的协作同步（`tick_collaborative_editor_runtime`）都走这条路；
///   - CEF 还没起来 / headless（从没见过 owner 线程）：没有 owner 可投递，就地执行，
///     但**加 `editor_state_mutex`**，让多个外来线程之间仍然串行。
///
/// 唯一的环路（必须小心）：owner 线程的 handler 会同步等 Python 运行时
/// （`invoke_python_script_service` → `submit_and_wait`），而 Python 消费线程此刻可能正在
/// 调编辑器 API。若它阻塞等 owner 线程就是互等。因此 owner 在等待期间用
/// `EditorWaitingForPythonScope` 打标，外来线程命中标记时**就地执行**。
///
/// 就地执行与 owner 线程恢复之间不能有窗口，所以交接是一个三段协议：
///   owner:  置标记(A) → 等 Python(B) → 抢 `editor_state_mutex`(C) → 清标记(D) → 放锁(E)
///   外来:   抢锁(1) → 复查标记(2) → 就地执行(3) → 放锁(4)
///   - 外来线程在 (2) 复查看不到标记，说明 owner 已经/即将恢复，于是改走 marshal（(3)
///     绝不与 owner 并发）；
///   - owner 在 (C) 抢锁，会**等所有在跑的就地执行结束**，因此 (E) 之后 owner 独占状态。
/// (C) 与 (1) 的先后由锁保证，标记只在持锁范围内变化，所以不存在漏判。
///
/// 还有一条不变量，专门喂给"会重试"的调用方（协作同步每帧重试、Python 脚本重试）：
/// **派发返回"没执行"就等价于那个闭包绝不会再被执行**。清不掉的投递（消息循环已经收了）
/// 会在执行前发现调用方已放弃而直接返回；反过来，owner 线程如果已经开始执行，即使已经
/// 超时也要等它跑完把真实结果带回来。否则调用方会"报告没执行 → 重试"，同一个 actor 被
/// 创建两次。
///
/// 本头不依赖 CEF，便于单元测试；CEF 的实现（`CefCurrentlyOn`／`CefPostTask`）由
/// `cef_editor_api.cpp` 在首次使用时安装。
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

namespace Corona::Systems::UI::Detail {

/// 一次派发应该走哪条路。
enum class EditorDispatchRoute {
    Inline,         ///< 已在 owner 线程：就地执行（该线程天然串行，不需要锁）
    Marshal,        ///< 别的线程、owner 可用：post 到 owner 线程并有界等待
    InlineBusy,     ///< owner 正阻塞等 Python：就地执行（持锁），避免互等环路
    InlineNoOwner,  ///< 还没见过 owner 线程（CEF 未起 / headless）：就地执行（持锁）
};

/// 纯决策函数（单元测试直接覆盖）。
[[nodiscard]] constexpr EditorDispatchRoute decide_editor_dispatch_route(
    bool on_owner_thread,
    bool owner_waiting_for_python,
    bool owner_thread_seen) noexcept {
    if (on_owner_thread) return EditorDispatchRoute::Inline;
    if (owner_waiting_for_python) return EditorDispatchRoute::InlineBusy;
    if (!owner_thread_seen) return EditorDispatchRoute::InlineNoOwner;
    return EditorDispatchRoute::Marshal;
}

/// 派发所需的平台钩子。引擎里由 CEF 实现；测试可替换。
struct EditorDispatchHooks {
    /// 当前线程是否是 owner（CEF UI）线程
    std::function<bool()> is_owner_thread;
    /// 把任务 post 到 owner 线程；返回 false 表示 post 失败
    std::function<bool(std::function<void()>)> post_to_owner;
};

inline std::mutex& editor_dispatch_hooks_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline EditorDispatchHooks& editor_dispatch_hooks() {
    static EditorDispatchHooks hooks;
    return hooks;
}

inline void set_editor_dispatch_hooks(EditorDispatchHooks hooks) {
    std::lock_guard<std::mutex> lock(editor_dispatch_hooks_mutex());
    editor_dispatch_hooks() = std::move(hooks);
}

inline void clear_editor_dispatch_hooks() {
    set_editor_dispatch_hooks(EditorDispatchHooks{});
}

inline EditorDispatchHooks copy_editor_dispatch_hooks() {
    std::lock_guard<std::mutex> lock(editor_dispatch_hooks_mutex());
    return editor_dispatch_hooks();
}

/// 串行化"非 owner 线程就地执行 handler"。owner 线程从不持有它（它自己就是单线程）；
/// 用 recursive 是为了容忍同一线程的嵌套就地执行。
inline std::recursive_mutex& editor_state_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}
/// 是否见过 owner 线程（第一次在 owner 线程上派发时置位）。用来区分"CEF 还没起来"与
/// "CEF 起来了但此刻 post 不进去"——前者没有 owner 可等，就地执行是安全的。
inline std::atomic<bool>& editor_owner_thread_seen_flag() {
    static std::atomic<bool> seen{false};
    return seen;
}

[[nodiscard]] inline bool editor_owner_thread_seen() {
    return editor_owner_thread_seen_flag().load(std::memory_order_acquire);
}

/// 仅供测试：把"见过 owner 线程"复位（生产代码里它一旦置位就不该回退）。
inline void reset_editor_owner_thread_seen_for_test() {
    editor_owner_thread_seen_flag().store(false, std::memory_order_release);
}

/// 当前线程是否是 owner（编辑器状态）线程。没有安装钩子时返回 false（于是派发会退化成
/// marshal 或就地执行，见 `decide_editor_dispatch_route`）。
///
/// 首次置位时要和"正在就地执行的外来线程"握手：置位发生在 `editor_state_mutex` 之内，
/// 而 `InlineNoOwner` 分支的执行也在该锁之内，所以要么外来线程先跑完、要么 owner 先置位
/// 之后外来线程改走 marshal，两种顺序都不会让 owner 和外来线程同时碰状态。
[[nodiscard]] inline bool on_editor_owner_thread() {
    auto hooks = copy_editor_dispatch_hooks();
    if (!hooks.is_owner_thread || !hooks.is_owner_thread()) return false;
    if (!editor_owner_thread_seen()) {
        std::lock_guard<std::recursive_mutex> handshake(editor_state_mutex());
        editor_owner_thread_seen_flag().store(true, std::memory_order_release);
    }
    return true;
}

/// owner 线程此刻阻塞在"等 Python 运行时"的嵌套层数（见文件头说明的环路）。
inline std::atomic<int>& editor_python_wait_depth() {
    static std::atomic<int> depth{0};
    return depth;
}

[[nodiscard]] inline bool editor_thread_waiting_for_python() {
    return editor_python_wait_depth().load(std::memory_order_acquire) > 0;
}

/// 仅供诊断/测试：直接把标记置为"在等/不在等"。
inline void set_editor_waiting_for_python(bool waiting) {
    editor_python_wait_depth().store(waiting ? 1 : 0, std::memory_order_release);
}


/// RAII：标记"owner 线程正在同步等待 Python 运行时"，并在退出时完成文件头描述的交接。
///
/// 只在 owner 线程上生效：别的线程等 Python 时 owner 仍在正常跑 handler，没有环路，
/// 也没有"就地执行"的许可可以发。
class EditorWaitingForPythonScope {
   public:
    EditorWaitingForPythonScope() : engaged_(on_editor_owner_thread()) {
        if (!engaged_) return;
        editor_python_wait_depth().fetch_add(1, std::memory_order_acq_rel);
    }

    ~EditorWaitingForPythonScope() {
        if (!engaged_) return;
        // 抢锁 = 等在跑的就地执行全部结束；清标记放在持锁范围内，与外来线程的"抢锁后
        // 复查标记"配合，保证恢复之后不会再有新的就地执行与 owner 并发。
        {
            std::lock_guard<std::recursive_mutex> drain(editor_state_mutex());
            if (editor_python_wait_depth().fetch_sub(1, std::memory_order_acq_rel) <= 1) {
                editor_python_wait_depth().store(0, std::memory_order_release);
            }
        }
    }

    EditorWaitingForPythonScope(const EditorWaitingForPythonScope&) = delete;
    EditorWaitingForPythonScope& operator=(const EditorWaitingForPythonScope&) = delete;

    [[nodiscard]] bool engaged() const noexcept { return engaged_; }

   private:
    bool engaged_ = false;
};

/// 在 owner 线程上执行 `body` 的结果。
enum class EditorOwnerRun {
    Ran,         ///< 确实执行了（可能在时限之后才开始，见 `call_on_editor_owner_thread`）
    Timeout,     ///< 投递成功但没在时限内开始 → 已撤销，**不会**再执行
    PostFailed,  ///< 无法投递（CEF 消息循环已停止 / 钩子缺失）→ 不会执行
};

/// 一次派发的执行报告。
struct EditorDispatchReport {
    EditorOwnerRun run = EditorOwnerRun::Ran;
    /// body 是否跑在**调用线程**上而不是 owner 线程上（InlineBusy / InlineNoOwner）
    bool ran_inline_on_caller = false;
};

/// `call_on_editor_owner_thread` 的跨线程单元格。
template <typename R>
struct EditorOwnerCallCell {
    std::mutex mutex;
    std::condition_variable cv;
    bool claimed = false;    ///< 调用方已放弃：任务不得执行
    bool running = false;    ///< owner 线程已开始执行
    bool finished = false;
    std::exception_ptr error;
    std::optional<R> result;
};

/// 把 `fn` 投递到 owner 线程执行，并把返回值带回调用线程。
///
/// 关键不变量：**返回 `std::nullopt` 等价于 `fn` 绝不会被执行**（投递失败，或超时且
/// owner 线程还没开始 → 撤销）。反过来，如果 owner 线程已经开始执行，即使已经超时也要
/// 等它结束并把真实结果带回来 —— 否则调用方会"报告没执行"却让状态被改了一次，重试就是
/// 重复应用（协作创建 actor 会变成两个 actor）。
///
/// `*executed_as` 描述"执行了没有/为什么没执行"，与返回值一致：
/// `Ran` ⇒ 有值；`Timeout`/`PostFailed` ⇒ 空。
template <typename Fn>
[[nodiscard]] auto call_on_editor_owner_thread(Fn fn,
                                               std::chrono::milliseconds timeout,
                                               EditorOwnerRun* executed_as = nullptr)
    -> std::optional<std::invoke_result_t<Fn&>> {
    using R = std::invoke_result_t<Fn&>;

    auto hooks = copy_editor_dispatch_hooks();
    if (!hooks.post_to_owner) {
        if (executed_as) *executed_as = EditorOwnerRun::PostFailed;
        return std::nullopt;
    }

    auto cell = std::make_shared<EditorOwnerCallCell<R>>();
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    if (!hooks.post_to_owner([fn = std::move(fn), cell, done]() mutable {
            {
                std::lock_guard<std::mutex> lock(cell->mutex);
                if (cell->claimed) return;  // 调用方已经放弃：什么都不做
                cell->running = true;
            }
            std::optional<R> value;
            std::exception_ptr error;
            try {
                value.emplace(fn());
            } catch (...) {
                error = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(cell->mutex);
                cell->result = std::move(value);
                cell->error = error;
                cell->finished = true;
            }
            cell->cv.notify_all();
            try {
                done->set_value();
            } catch (...) {
                // promise 被重复/异常设置：忽略，调用方靠超时收尾。
            }
        })) {
        if (executed_as) *executed_as = EditorOwnerRun::PostFailed;
        return std::nullopt;
    }

    if (future.wait_for(timeout) == std::future_status::ready) {
        if (executed_as) *executed_as = EditorOwnerRun::Ran;
        if (cell->error) std::rethrow_exception(cell->error);
        return std::move(cell->result);
    }

    std::unique_lock<std::mutex> lock(cell->mutex);
    if (!cell->running) {
        cell->claimed = true;  // 撤销：fn 从此不会被执行
        if (executed_as) *executed_as = EditorOwnerRun::Timeout;
        return std::nullopt;
    }
    cell->cv.wait(lock, [&cell] { return cell->finished; });
    if (executed_as) *executed_as = EditorOwnerRun::Ran;
    if (cell->error) std::rethrow_exception(cell->error);
    return std::move(cell->result);
}

/// 按 `route` 在 owner 线程（或按交接协议就地）执行 `fn` 并带回返回值。
///
/// `*report` 记录走的是哪条路（用于日志："因为编辑器线程在等 Python，所以就地执行了"）。
template <typename Fn>
[[nodiscard]] auto call_editor_dispatch(Fn fn,
                                        EditorDispatchRoute route,
                                        std::chrono::milliseconds timeout,
                                        EditorDispatchReport* report = nullptr)
    -> std::optional<std::invoke_result_t<Fn&>> {
    EditorDispatchReport local;
    EditorDispatchReport& out = report ? *report : local;

    switch (route) {
        case EditorDispatchRoute::Inline:
            return std::optional{fn()};

        case EditorDispatchRoute::InlineBusy: {
            std::unique_lock<std::recursive_mutex> state_lock(editor_state_mutex());
            if (!editor_thread_waiting_for_python()) {
                // owner 已经恢复（或即将恢复）：改走 owner 线程，避免与它并发。
                state_lock.unlock();
                return call_on_editor_owner_thread(std::move(fn), timeout, &out.run);
            }
            out.ran_inline_on_caller = true;
            return std::optional{fn()};
        }

        case EditorDispatchRoute::InlineNoOwner: {
            std::unique_lock<std::recursive_mutex> state_lock(editor_state_mutex());
            if (editor_owner_thread_seen()) {
                // owner 线程刚刚出现（CEF 起来了）：交回给它，别在这里和它并发。
                state_lock.unlock();
                return call_on_editor_owner_thread(std::move(fn), timeout, &out.run);
            }
            out.ran_inline_on_caller = true;
            return std::optional{fn()};
        }

        case EditorDispatchRoute::Marshal:
        default:
            return call_on_editor_owner_thread(std::move(fn), timeout, &out.run);
    }
}

/// `void` 版本：只关心"执行了没有/怎么执行的"。
[[nodiscard]] inline EditorDispatchReport run_editor_api_dispatch(
    const std::function<void()>& body,
    EditorDispatchRoute route,
    std::chrono::milliseconds timeout) {
    EditorDispatchReport report;
    if (!body) return report;
    (void)call_editor_dispatch(
        [&body] { body(); return true; }, route, timeout, &report);
    return report;
}

}  // namespace Corona::Systems::UI::Detail
