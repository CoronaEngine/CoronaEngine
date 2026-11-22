#include <horizon/core/logging.h>
#include <corona/systems/script/python_api.h>
#include <corona/systems/script/script_system.h>
#include <windows.h>

#include <chrono>
#include <iostream>

namespace Corona::Systems {

ScriptSystem::ScriptSystem()
    : python_api_(std::make_unique<Script::Python::PythonAPI>()) {
    // 脚本系统不再由 SystemBase 计时：update() 进入 Python 自己的循环并一直阻塞到退出。
}

ScriptSystem::~ScriptSystem() = default;

bool ScriptSystem::initialize(Kernel::ISystemContext* ctx) {
    CFW_LOG_NOTICE("event=script.system.initializing");

    // JS/Python 业务能力走 C++ Editor API；ScriptSystem 不再订阅 UI 事件。
    if (ctx->event_bus() == nullptr) {
        CFW_LOG_WARNING("event=script.system.event_bus_missing");
    }

#ifdef CORONA_ENABLE_PYTHON_API
    if (!python_api_->initializeInterpreter()) {
        CFW_LOG_ERROR("event=script.system.python_init_failed");
        return false;
    }
#endif

    return true;
}

void ScriptSystem::stop() {
    if (python_api_) {
        python_api_->request_stop();
    }
    constexpr auto kShutdownTimeout = std::chrono::seconds(3);
    if (stop_for(kShutdownTimeout)) {
        return;
    }

    CFW_LOG_ERROR(
        "ScriptSystem: Python worker did not stop within {} seconds; terminating process",
        kShutdownTimeout.count());
    const auto diagnostics = python_api_->shutdown_diagnostics();
    std::cerr << diagnostics << std::endl;
    ::OutputDebugStringA((diagnostics + "\n").c_str());
    CFW_LOG_CRITICAL("{}", diagnostics);
    CFW_LOG_FLUSH();
    std::cerr.flush();
    ::TerminateProcess(::GetCurrentProcess(), 1);
}

void ScriptSystem::update() {
#ifdef CORONA_ENABLE_PYTHON_API
    python_api_->enter_runtime();
#endif
}

void ScriptSystem::shutdown() {
    CFW_LOG_NOTICE("event=script.system.shutting_down");

    // 主动关闭 Python 解释器，避免在析构时阻塞
    python_api_->shutdown();
}

}  // namespace Corona::Systems
