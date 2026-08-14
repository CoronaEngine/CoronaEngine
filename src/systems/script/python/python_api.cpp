#define PY_SSIZE_T_CLEAN
#include <corona/events/script_system_events.h>
#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/systems/script/python_api.h>
#include <corona/systems/script/engine_scripts.h>
#include <corona/systems/script/python/python_error_handler.h>
#include <corona/systems/script/python/python_path_config.h>
#include <nanobind/stl/string.h>
#include <nlohmann/json.hpp>
#include <windows.h>

#include <array>
#include <iostream>
#include <sstream>
#include <ranges>
#include <regex>
#include <set>
#include <filesystem>
#include <unordered_map>
#include <vector>


extern "C" PyObject* PyInit_CoronaEngine();

namespace Corona::Script::Python {

const std::string codePath = PathCfg::engine_root();

PythonAPI::PythonAPI() {
    lifecycle_snapshot_.state = PythonLifecycleState::Created;
    install_active_python_runtime_coordinator(&runtime_coordinator_);
}

PythonAPI::~PythonAPI() {
    if (active_python_runtime_coordinator() == &runtime_coordinator_) {
        install_active_python_runtime_coordinator(nullptr);
    }
    if (runtime_coordinator_.state() != PythonRuntimeState::Stopped) {
        shutdown();
    }
    detach_python_objects_without_decref();
}

void PythonAPI::begin_shutdown() {
    if (shutting_down_.load()) {
        return;
    }
    if (backend_initialized_.load() && lifecycle_.state() == PythonLifecycleState::Running) {
        PythonRuntimeRequest request;
        request.kind = PythonRuntimeRequestKind::LifecycleControl;
        request.source = "ScriptSystem::stop";
        request.function = "shutdown";
        const auto response = runtime_coordinator_.submit_and_wait(
            std::move(request), std::chrono::milliseconds(2500));
        if (response.status != PythonRuntimeResponseStatus::Success) {
            CFW_LOG_ERROR("PythonAPI: cooperative shutdown request failed: {}", response.error);
        }
    }
    shutting_down_.store(true);
    runtime_coordinator_.begin_quiescing();
    lifecycle_.request_stop();
    std::lock_guard lock(lifecycle_mtx_);
    lifecycle_snapshot_.state = lifecycle_.state();
    lifecycle_snapshot_.shutting_down = true;
    lifecycle_snapshot_.phase = "shutdown_requested";
    CFW_LOG_NOTICE("PythonAPI: lifecycle transition -> StopRequested");
}

PythonLifecycleSnapshot PythonAPI::lifecycle_snapshot() const {
    std::lock_guard lock(lifecycle_mtx_);
    auto snapshot = lifecycle_snapshot_;
    snapshot.state = lifecycle_.state();
    snapshot.shutting_down = shutting_down_.load();
    return snapshot;
}

std::string PythonAPI::shutdown_diagnostics() const {
    const auto runtime = runtime_coordinator_.snapshot();
    std::lock_guard lock(lifecycle_mtx_);
    std::ostringstream out;
    out << "python.lifecycle.shutdown_timeout"
        << " lifecycle_state=" << static_cast<int>(lifecycle_.state())
        << " lifecycle_phase=" << lifecycle_snapshot_.phase
        << " coordinator_state=" << static_cast<int>(runtime.state)
        << " queued=" << runtime.queued_count
        << " pending=" << runtime.pending_count
        << " consumer_thread=" << runtime.consumer_thread_token
        << " execution_phase=" << runtime.execution_phase;
    if (runtime.current_request) {
        out << " request_id=" << runtime.current_request->request_id
            << " request_kind=" << static_cast<int>(runtime.current_request->kind)
            << " request_source=" << runtime.current_request->source
            << " request_module=" << runtime.current_request->module
            << " request_function=" << runtime.current_request->function;
    } else {
        out << " request_id=none";
    }
    if (!last_python_shutdown_snapshot_json_.empty()) {
        out << " python_snapshot=" << last_python_shutdown_snapshot_json_;
    } else {
        out << " python_snapshot=unavailable";
    }
    return out.str();
}

void PythonAPI::shutdown() {
    begin_shutdown();

    CFW_LOG_INFO("PythonAPI: Shutting down Python runtime ownership...");
    lifecycle_.transition(PythonLifecycleState::Stopping);
    runtime_coordinator_.begin_python_stopping();

    const auto detached_count = detach_python_objects_without_decref();
    if (detached_count != 0) {
        CFW_LOG_ERROR(
            "PythonAPI: {} Python references survived cooperative shutdown; detached without DECREF "
            "because the ScriptSystem Python thread is no longer available",
            detached_count);
    }

    PyConfig_Clear(&config);

    lifecycle_.transition(PythonLifecycleState::Stopped);
    runtime_coordinator_.stop();
    {
        std::lock_guard lock(lifecycle_mtx_);
        lifecycle_snapshot_.state = lifecycle_.state();
        lifecycle_snapshot_.phase = "shutdown_complete";
        lifecycle_snapshot_.shutting_down = true;
    }

    CFW_LOG_INFO("PythonAPI: Python shutdown complete");
}

std::size_t PythonAPI::detach_python_objects_without_decref() {
    std::size_t detached_count = 0;
    auto detach = [&detached_count](nanobind::object& object) {
        if (!object.is_valid()) return;
        const auto released = object.release();
        if (released.is_valid()) ++detached_count;
    };
    detach(pStartFunc);
    detach(messageFunc);
    detach(pEditor);
    detach(pModule);
    detach(pFunc);
    return detached_count;
}

int64_t PythonAPI::nowMsec() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string PythonAPI::wstr2str(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), out.data(), len, nullptr, nullptr);
    return out;
}

bool PythonAPI::initializeInterpreterLocked() {
    if (interpreter_initialized_.load(std::memory_order_acquire)) {
        if (lifecycle_.state() == PythonLifecycleState::Created) {
            lifecycle_.transition(PythonLifecycleState::InterpreterInitializing);
            lifecycle_.transition(PythonLifecycleState::InterpreterReady);
        }
        return true;
    }

    if (!lifecycle_.transition(PythonLifecycleState::InterpreterInitializing)) {
        return lifecycle_.state() == PythonLifecycleState::InterpreterReady;
    }
    CFW_LOG_INFO("PythonAPI: Initializing Python interpreter...");

    PyImport_AppendInittab("CoronaEngine", &PyInit_CoronaEngine);

    PyConfig_InitPythonConfig(&config);
    auto check_status = [&](PyStatus status, const char* step) {
        if (!PyStatus_Exception(status)) {
            return true;
        }
        lifecycle_.transition(PythonLifecycleState::Failed);
        {
            std::lock_guard lock(lifecycle_mtx_);
            lifecycle_snapshot_.state = lifecycle_.state();
            lifecycle_snapshot_.phase = step;
            lifecycle_snapshot_.error = status.err_msg ? status.err_msg : "unknown Python error";
        }
        CFW_LOG_CRITICAL("PythonAPI: {} failed: {}", step, status.err_msg ? status.err_msg : "unknown Python error");
        PyConfig_Clear(&config);
        config = PyConfig{};
        return false;
    };

    // Do not inherit Python settings from the developer machine. The runtime
    // is portable and all paths are resolved beside corona_engine.exe.
    config.use_environment = 0;
    config.module_search_paths_set = 1;

    const auto bundled_home = PathCfg::python_home_dir();
    const auto bundled_stdlib = PathCfg::python_stdlib_zip();
    const auto bundled_dlls = PathCfg::python_dll_dir();
    const auto bundled_lib = PathCfg::python_lib_dir();
    const auto bundled_site_packages = PathCfg::site_packages_dir();
    const auto bundled_editor = PathCfg::runtime_backend_abs();
    const auto bundled_root = PathCfg::engine_root();

    const auto bundled_stdlib_path = std::filesystem::path(bundled_stdlib);
    const auto bundled_dlls_path = std::filesystem::path(bundled_dlls);
    const auto bundled_lib_path = std::filesystem::path(bundled_lib);
    const auto bundled_editor_main_path = std::filesystem::path(bundled_editor) / "main.py";
    if ((!std::filesystem::exists(bundled_stdlib_path) &&
         !std::filesystem::exists(bundled_lib_path)) ||
        !std::filesystem::exists(bundled_dlls_path) ||
        !std::filesystem::exists(bundled_editor_main_path)) {
        const std::string message =
            "Bundled Python runtime is incomplete. Expected python stdlib zip or Lib directory, " +
            bundled_dlls + ", and " + bundled_editor_main_path.generic_string();
        lifecycle_.transition(PythonLifecycleState::Failed);
        {
            std::lock_guard lock(lifecycle_mtx_);
            lifecycle_snapshot_.state = lifecycle_.state();
            lifecycle_snapshot_.phase = "bundled_runtime_validation";
            lifecycle_snapshot_.error = message;
        }
        CFW_LOG_CRITICAL("PythonAPI: {}", message);
        PyConfig_Clear(&config);
        config = PyConfig{};
        return false;
    }
    if (!check_status(PyConfig_SetString(&config, &config.home,
                                         str2wstr(PathCfg::python_home_dir()).c_str()),
                      "set bundled Python home")) {
        return false;
    }
    std::vector<std::string> module_paths{
        bundled_editor,
        bundled_root,
        bundled_dlls,
        bundled_lib,
        bundled_site_packages,
    };
    if (std::filesystem::exists(bundled_stdlib_path)) {
        module_paths.insert(module_paths.begin() + 1, bundled_stdlib);
    }
    for (const auto& path : module_paths) {
        if (!check_status(PyWideStringList_Append(&config.module_search_paths, str2wstr(path).c_str()),
                          "append bundled Python module path")) {
            return false;
        }
    }

    if (!check_status(Py_InitializeFromConfig(&config), "initialize Python interpreter")) {
        return false;
    }
    main_thread_state_ = PyEval_SaveThread();
    interpreter_initialized_.store(true, std::memory_order_release);
    lifecycle_.transition(PythonLifecycleState::InterpreterReady);
    {
        std::lock_guard lock(lifecycle_mtx_);
        lifecycle_snapshot_.state = lifecycle_.state();
        lifecycle_snapshot_.phase = "interpreter_ready";
    }
    CFW_LOG_INFO("PythonAPI: Python interpreter initialized successfully");

    return true;
}

bool PythonAPI::initializeInterpreter() {
    std::lock_guard init_lock(initMtx);
    return initializeInterpreterLocked();
}

bool PythonAPI::ensureInitialized() {
    if (backend_initialized_.load()) {
        return true;
    }

    std::lock_guard init_lock(initMtx);
    if (backend_initialized_.load()) {
        return true;
    }
    if (!initializeInterpreterLocked()) {
        lifecycle_.transition(PythonLifecycleState::Failed);
        return false;
    }

    if (!lifecycle_.transition(PythonLifecycleState::BackendInitializing)) {
        return lifecycle_.state() == PythonLifecycleState::Running;
    }

    {
        nanobind::gil_scoped_acquire gil;
        try {
            // nanobind::module_ main_mod = nanobind::module_::import_("cpp_client");

            // nanobind::object init_func = nanobind::getattr(main_mod, "initialize");
            // init_func();

            // nanobind::object run_attr = nanobind::getattr(main_mod, "run");
            // nanobind::object putq_attr = nanobind::getattr(main_mod, "put_queue");

            // if (!nanobind::callable::check_(run_attr)) {
            //     CFW_LOG_ERROR("PythonAPI: 'run' attribute is not callable");
            //     return false;
            // }

            // pModule = std::move(main_mod);
            // pFunc = std::move(run_attr);
            // messageFunc = std::move(putq_attr);

            nanobind::module_ entrance = nanobind::module_::import_("main");
            nanobind::object editor = nanobind::getattr(entrance, "editor");
            nanobind::object start_attr = nanobind::getattr(entrance, "run");
            nanobind::object log_attr = nanobind::getattr(editor, "show_log_on_js");
            if (nanobind::hasattr(editor, "initialize_runtime")) {
                nanobind::object init_runtime = nanobind::getattr(editor, "initialize_runtime");
                init_runtime();
            }
            pStartFunc = std::move(start_attr);
            messageFunc = std::move(log_attr);
            pEditor = editor;
            pStartFunc();
            if (nanobind::hasattr(editor, "start_runtime")) {
                nanobind::object start_runtime = nanobind::getattr(editor, "start_runtime");
                start_runtime();
            }
            if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
                event_bus->publish<Events::ScriptFinishStartEvent>({});
            }
            backend_initialized_.store(true);
            lifecycle_.transition(PythonLifecycleState::Running);
            {
                std::lock_guard lock(lifecycle_mtx_);
                lifecycle_snapshot_.state = lifecycle_.state();
                lifecycle_snapshot_.phase = "running";
            }
            CFW_LOG_INFO("PythonAPI: Python backend initialized successfully");
        } catch (const nanobind::python_error& e) {
            lifecycle_.transition(PythonLifecycleState::Failed);
            log_python_error(e);
            // pModule.reset();
            // pFunc.reset();
            // messageFunc.reset();
            pStartFunc.reset();
            messageFunc.reset();
            return false;
        }
    }
    return true;
}

void PythonAPI::invokeEntry(bool isReload) {
    // 如果正在关闭，不执行
    if (shutting_down_.load()) {
        return;
    }

    if (!messageFunc.is_valid()) {
        return;
    }
    runtime_coordinator_.set_execution_phase("gil_wait:editor_update");
    nanobind::gil_scoped_acquire gil;
    runtime_coordinator_.set_execution_phase("editor_update");

    try {
        //(void)pFunc(isReload ? 1 : 0);
        messageFunc();
        runtime_coordinator_.set_execution_phase("idle");
    } catch (const nanobind::python_error& e) {
        runtime_coordinator_.set_execution_phase("editor_update_error");
        log_python_error(e);
    }
}

void PythonAPI::sendMessage(const std::string& message) const {
    // if (!messageFunc.is_valid()) {
    //     return;
    // }
    // nanobind::gil_scoped_acquire gil;

    // try {
    //     (void)messageFunc(message.c_str());
    // } catch (const nanobind::python_error& e) {
    //     log_python_error(e);
    // }
}

void PythonAPI::runPythonScript() {
    const auto frame_start = std::chrono::steady_clock::now();
    if (!runtime_coordinator_.bind_consumer_thread()) {
        CFW_LOG_CRITICAL("PythonAPI: runPythonScript called from a non-owner thread");
        return;
    }
    // 如果正在关闭，不执行任何 Python 代码
    if (shutting_down_.load() || lifecycle_.state() == PythonLifecycleState::StopRequested ||
        lifecycle_.state() == PythonLifecycleState::Stopping ||
        lifecycle_.state() == PythonLifecycleState::Stopped) {
        return;
    }

    if (!ensureInitialized()) {
        CFW_LOG_ERROR("PythonAPI: Python initialization failed");
        return;
    }

    process_runtime_requests();
    if (shutting_down_.load()) {
        return;
    }

    // 再次检查是否正在关闭
    if (shutting_down_.load()) {
        return;
    }

    invokeEntry(false);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - frame_start).count();
    const auto now = nowMsec();
    auto last = last_overrun_log_ms_.load(std::memory_order_relaxed);
    if (elapsed >= 50 && now - last >= 5000 &&
        last_overrun_log_ms_.compare_exchange_strong(last, now)) {
        CFW_LOG_WARNING("PythonAPI: python.lifecycle.overrun phase=update elapsed_ms={} state={}",
                        elapsed, static_cast<int>(lifecycle_.state()));
    }
}

void PythonAPI::process_runtime_requests() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
    for (std::size_t processed = 0; processed < 32; ++processed) {
        auto request = runtime_coordinator_.wait_pop(std::chrono::milliseconds(0));
        if (!request) {
            return;
        }
        runtime_coordinator_.set_execution_phase("coordinator_request:" + request->source + ":" + request->function);
        auto response = execute_runtime_request(*request);
        runtime_coordinator_.complete(request->request_id, std::move(response));
        runtime_coordinator_.set_execution_phase("idle");
        if (std::chrono::steady_clock::now() >= deadline) {
            return;
        }
    }
}

PythonRuntimeResponse PythonAPI::execute_runtime_request(const PythonRuntimeRequest& request) {
    if (request.cancelled()) {
        return PythonRuntimeResponse::timeout();
    }
    runtime_coordinator_.set_execution_phase("gil_wait:coordinator_request");
    nanobind::gil_scoped_acquire gil;
    runtime_coordinator_.set_execution_phase("coordinator_request");
    try {
        if (request.kind == PythonRuntimeRequestKind::LifecycleControl &&
            request.function == "shutdown") {
            std::string shutdown_snapshot_json = "{}";
            if (pEditor.is_valid()) {
                if (nanobind::hasattr(pEditor, "request_shutdown")) {
                    nanobind::getattr(pEditor, "request_shutdown")();
                }
                if (nanobind::hasattr(pEditor, "shutdown_runtime")) {
                    auto result = nanobind::getattr(pEditor, "shutdown_runtime")();
                    auto json = nanobind::module_::import_("json");
                    shutdown_snapshot_json = nanobind::cast<std::string>(
                        nanobind::getattr(json, "dumps")(result));
                }
            }
            EngineScripts::clear_python_callback_registry();
            pStartFunc.reset();
            messageFunc.reset();
            pEditor.reset();
            pModule.reset();
            pFunc.reset();
            backend_initialized_.store(false);
            shutting_down_.store(true);
            {
                std::lock_guard lock(lifecycle_mtx_);
                last_python_shutdown_snapshot_json_ = shutdown_snapshot_json;
            }
            return PythonRuntimeResponse::success(std::move(shutdown_snapshot_json));
        }

        if (request.kind == PythonRuntimeRequestKind::LifecycleControl &&
            request.function == "project_context_changed") {
            if (!pEditor.is_valid() || !nanobind::hasattr(pEditor, "update_project_context")) {
                return PythonRuntimeResponse::failure("python editor cannot update project context");
            }
            const auto payload = nlohmann::json::parse(request.payload_json, nullptr, false);
            const auto project_path = payload.is_object()
                ? payload.value("path", std::string{})
                : std::string{};
            if (project_path.empty()) {
                return PythonRuntimeResponse::failure("project context path is empty");
            }
            const bool updated = nanobind::cast<bool>(
                nanobind::getattr(pEditor, "update_project_context")(project_path.c_str()));
            return updated
                ? PythonRuntimeResponse::success()
                : PythonRuntimeResponse::failure("python project context update failed");
        }

        if (request.kind == PythonRuntimeRequestKind::ServiceCall) {
            if (request.cancelled()) {
                return PythonRuntimeResponse::timeout();
            }
            if (!pEditor.is_valid()) {
                return PythonRuntimeResponse::failure("python editor is unavailable");
            }
            auto dispatcher = nanobind::getattr(pEditor, "dispatch_script_request_from_cpp");
            auto result = dispatcher(request.payload_json.c_str());
            if (request.cancelled()) {
                return PythonRuntimeResponse::timeout();
            }
            return PythonRuntimeResponse::success(nanobind::cast<std::string>(result));
        }
        if (request.kind == PythonRuntimeRequestKind::Callback && request.handler) {
            if (request.cancelled()) {
                return PythonRuntimeResponse::timeout();
            }
            return request.handler(request);
        }
        return PythonRuntimeResponse::failure("unsupported python runtime request");
    } catch (const nanobind::python_error& error) {
        log_python_error(error);
        return PythonRuntimeResponse::failure(error.what());
    } catch (const std::exception& error) {
        return PythonRuntimeResponse::failure(error.what());
    }
}

std::wstring PythonAPI::str2wstr(const std::string& str) {
    if (str.empty()) {
        return {};
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), w.data(), wlen);
    return w;
}

}  // namespace Corona::Script::Python
