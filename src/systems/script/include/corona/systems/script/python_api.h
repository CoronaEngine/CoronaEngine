#pragma once

#include <Python.h>
#include <corona/systems/script/python_lifecycle.h>
#include <corona/systems/script/python_runtime_coordinator.h>
#include <nanobind/nanobind.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace Corona::Script::Python {

struct PythonLifecycleSnapshot {
    PythonLifecycleState state = PythonLifecycleState::Created;
    std::string phase;
    int64_t phase_elapsed_ms = 0;
    bool shutting_down = false;
    std::string error;
};

struct PythonAPI {
    PythonAPI();

    ~PythonAPI();

    /**
     * @brief 主动关闭 Python 解释器
     *
     * 在 shutdown 中调用，避免在析构时阻塞
     */
    void shutdown();

    /**
     * @brief 请求宿主循环停机
     *
     * 只置标志：Python 自己的 main.loop() 看到它后跳出循环，并在 finally 里执行
     * shutdown_runtime()。不跨线程提交任何请求。
     */
    void request_stop();

    /**
     * @brief 初始化解释器与 Python 后端
     *
     * 解析 main.run / main.loop 两个入口并调用 run()。任一入口缺失或 run() 抛异常
     * 都是硬失败：lifecycle_ → Failed，引擎拒绝启动。
     */
    bool initializeInterpreter();

    /**
     * @brief 第一次调用进入 main.loop() 并阻塞到它退出；之后的调用立即返回
     *
     * 循环退出（正常或异常）后把协调器推进终态并结算所有挂起 ticket，然后
     * lifecycle_ → Stopped。main.loop() 抛异常只降级，不请求引擎关机。
     */
    void enter_runtime();

    /**
     * @brief 检查 Python 是否正在关闭
     */
    bool is_shutting_down() const { return shutting_down_.load(); }
    PythonLifecycleState lifecycle_state() const { return lifecycle_.state(); }
    PythonLifecycleSnapshot lifecycle_snapshot() const;
    std::string shutdown_diagnostics() const;
    bool checkpoint() const { return !shutting_down_.load(); }
    PythonRuntimeCoordinator& runtime_coordinator() { return runtime_coordinator_; }

    nanobind::object pStartFunc;  // callable 'main.run'
    nanobind::object loopFunc;    // callable 'main.loop'

   private:
    static const std::string codePath;

    mutable std::mutex initMtx;
    std::atomic<bool> shutting_down_{false};  // 标记是否正在关闭
    std::atomic<bool> interpreter_initialized_{false};
    std::atomic<bool> backend_initialized_{false};
    std::atomic<bool> entered_{false};  // main.loop() 是否已经进入过一次
    PythonLifecycle lifecycle_;
    PythonRuntimeCoordinator runtime_coordinator_;
    mutable std::mutex lifecycle_mtx_;
    PythonLifecycleSnapshot lifecycle_snapshot_;
    std::string last_python_shutdown_snapshot_json_;

    PyConfig config{};  // 值初始化
    PyThreadState* main_thread_state_ = nullptr;

    bool initializeInterpreterLocked();
    std::size_t detach_python_objects_without_decref();
    static std::wstring str2wstr(const std::string& str);
    static std::string wstr2str(const std::wstring& wstr);
};
}  // namespace Corona::Script::Python
