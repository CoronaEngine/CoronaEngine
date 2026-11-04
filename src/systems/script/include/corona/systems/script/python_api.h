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

    void begin_shutdown();
    void request_stop() { begin_shutdown(); }

    bool initializeInterpreter();
    void runPythonScript();
    void sendMessage(const std::string& message) const;

    /**
     * @brief 检查 Python 是否正在关闭
     */
    bool is_shutting_down() const { return shutting_down_.load(); }
    PythonLifecycleState lifecycle_state() const { return lifecycle_.state(); }
    PythonLifecycleSnapshot lifecycle_snapshot() const;
    std::string shutdown_diagnostics() const;
    bool checkpoint() const { return !shutting_down_.load(); }
    PythonRuntimeCoordinator& runtime_coordinator() { return runtime_coordinator_; }

    nanobind::object pStartFunc;  // callable 'start'

   private:
    static const std::string codePath;

    mutable std::mutex initMtx;
    std::atomic<bool> shutting_down_{false};  // 标记是否正在关闭
    std::atomic<bool> interpreter_initialized_{false};
    std::atomic<bool> backend_initialized_{false};
    std::atomic<int64_t> last_overrun_log_ms_{0};
    PythonLifecycle lifecycle_;
    PythonRuntimeCoordinator runtime_coordinator_;
    mutable std::mutex lifecycle_mtx_;
    PythonLifecycleSnapshot lifecycle_snapshot_;
    std::string last_python_shutdown_snapshot_json_;

    nanobind::object pModule;      // module 'main'
    nanobind::object pEditor;      // CoronaEditor lifecycle owner
    nanobind::object pFunc;        // callable 'run'
    nanobind::object runtimeUpdateFunc;  // CoronaEditor.update_runtime

    std::vector<std::string> moduleList;
    std::vector<std::string> callableList;

    PyConfig config{};  // 值初始化
    PyThreadState* main_thread_state_ = nullptr;

    bool ensureInitialized();
    bool initializeInterpreterLocked();
    void invokeEntry(bool isReload);
    void process_runtime_requests();
    PythonRuntimeResponse execute_runtime_request(const PythonRuntimeRequest& request);
    std::size_t detach_python_objects_without_decref();
    static int64_t nowMsec();
    static std::wstring str2wstr(const std::string& str);
    static std::string wstr2str(const std::wstring& wstr);
};
}  // namespace Corona::Script::Python
