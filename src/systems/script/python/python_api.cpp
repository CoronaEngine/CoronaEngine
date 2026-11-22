#define PY_SSIZE_T_CLEAN
#include <corona/events/script_system_events.h>
#include <horizon/core/logging.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/kernel/event/i_event_bus.h>
#include <corona/systems/script/python_api.h>
#include <corona/systems/script/engine_scripts.h>
#include <corona/systems/script/python/python_error_handler.h>
#include <corona/systems/script/python/python_path_config.h>
#include <corona/utils/path_utils.h>
#include <nanobind/stl/string.h>
#include <nlohmann/json.hpp>
#include "python_value.h"
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

void PythonAPI::request_stop() {
    if (shutting_down_.exchange(true)) {
        return;
    }
    // No request crosses threads: the owner of the loop polls this flag through
    // CoronaEngine.should_stop() and runs shutdown_runtime() on its way out.
    runtime_coordinator_.request_stop();
    // PythonLifecycle::request_stop() is idempotent and refuses to move a Failed or
    // Stopped runtime; a bare transition(StopRequested) would do neither. A recorded
    // failure phase/error (policies 1 and 2) must survive this call.
    const bool stop_requested = lifecycle_.request_stop();
    std::lock_guard lock(lifecycle_mtx_);
    lifecycle_snapshot_.state = lifecycle_.state();
    lifecycle_snapshot_.shutting_down = true;
    if (stop_requested && lifecycle_snapshot_.error.empty()) {
        lifecycle_snapshot_.phase = "shutdown_requested";
    }
    CFW_LOG_NOTICE("event=python.lifecycle.state state=stop_requested");
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
        << " lifecycle_error="
        << (lifecycle_snapshot_.error.empty() ? "none" : lifecycle_snapshot_.error)
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
    // The loop owns shutdown_runtime(). If it was never entered, ask it to stop
    // anyway so a script thread that starts later still exits immediately.
    if (!entered_.load(std::memory_order_acquire)) {
        request_stop();
    }

    CFW_LOG_INFO("event=python.shutdown.begin");
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
        // A recorded failure (hard-fail during init, or a dead/wedged host loop) is
        // the operator's only clue; shutdown must not replace it with
        // "shutdown_complete".
        if (lifecycle_snapshot_.error.empty()) {
            lifecycle_snapshot_.phase = "shutdown_complete";
        }
        lifecycle_snapshot_.shutting_down = true;
    }

    CFW_LOG_INFO("event=python.lifecycle.state state=stopped");
}

std::size_t PythonAPI::detach_python_objects_without_decref() {
    std::size_t detached_count = 0;
    auto detach = [&detached_count](nanobind::object& object) {
        if (!object.is_valid()) return;
        const auto released = object.release();
        if (released.is_valid()) ++detached_count;
    };
    detach(pStartFunc);
    detach(loopFunc);
    return detached_count;
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
    CFW_LOG_INFO("event=python.interpreter.initializing");

    {
        const auto packaged = PathCfg::inspect_deployed_runtime();
        const std::string configured_home = PathCfg::configured_python_home_dir();
        std::error_code fallback_ec;
        const bool fallback_available =
            !configured_home.empty() && std::filesystem::exists(Utils::utf8_to_path(configured_home), fallback_ec);
        const auto decision = PathCfg::decide_packaged_runtime_usage(
            packaged, PathCfg::packaged_runtime_required(), fallback_available);

        const auto fail_init = [&](const std::string& message, const char* phase) {
            lifecycle_.transition(PythonLifecycleState::Failed);
            {
                std::lock_guard lock(lifecycle_mtx_);
                lifecycle_snapshot_.state = lifecycle_.state();
                lifecycle_snapshot_.phase = phase;
                lifecycle_snapshot_.error = message;
            }
            CFW_LOG_CRITICAL("event=python.interpreter.bundled_incomplete detail={}", message);
        };

        switch (decision) {
            case PathCfg::PackagedRuntimeDecision::UsePackaged:
                CFW_LOG_INFO("event=python.interpreter.packaged detail={}", packaged.describe());
                break;
            case PathCfg::PackagedRuntimeDecision::UseFallback:
                CFW_LOG_WARNING(
                    "event=python.interpreter.fallback reason={} python_home={} portable=false",
                    packaged.describe(), configured_home);
                break;
            case PathCfg::PackagedRuntimeDecision::RejectNoPackaged:
                fail_init(packaged.describe() +
                              "; CORONA_REQUIRE_PACKAGED_PYTHON=1 forbids falling back to the "
                              "build-machine Python ('" +
                              configured_home + "')",
                          "packaged_runtime_required");
                return false;
            case PathCfg::PackagedRuntimeDecision::RejectNoFallback:
                fail_init(packaged.describe() +
                              "; no build-machine Python fallback available ('" + configured_home + "')",
                          "no_python_runtime");
                return false;
        }
    }

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
        CFW_LOG_CRITICAL("event=python.interpreter.step_failed step={} detail={}", step, status.err_msg ? status.err_msg : "unknown Python error");
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

    const auto bundled_stdlib_path = Utils::utf8_to_path(bundled_stdlib);
    const auto bundled_dlls_path = Utils::utf8_to_path(bundled_dlls);
    const auto bundled_lib_path = Utils::utf8_to_path(bundled_lib);
    const auto bundled_editor_main_path = Utils::utf8_to_path(bundled_editor) / "main.py";
    if ((!std::filesystem::exists(bundled_stdlib_path) &&
         !std::filesystem::exists(bundled_lib_path)) ||
        !std::filesystem::exists(bundled_dlls_path) ||
        !std::filesystem::exists(bundled_editor_main_path)) {
        const std::string message =
            "Bundled Python runtime is incomplete. Expected python stdlib zip or Lib directory, " +
            bundled_dlls + ", and " + Utils::path_to_utf8(bundled_editor_main_path);
        lifecycle_.transition(PythonLifecycleState::Failed);
        {
            std::lock_guard lock(lifecycle_mtx_);
            lifecycle_snapshot_.state = lifecycle_.state();
            lifecycle_snapshot_.phase = "bundled_runtime_validation";
            lifecycle_snapshot_.error = message;
        }
        CFW_LOG_CRITICAL("event=python.interpreter.bundled_incomplete detail={}", message);
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
    CFW_LOG_INFO("event=python.lifecycle.state state=interpreter_ready");
    CFW_LOG_INFO("event=python.interpreter.paths home={} lib={} dlls={} site_packages={}",
                 bundled_home, bundled_lib, bundled_dlls, bundled_site_packages);

    return true;
}

bool PythonAPI::initializeInterpreter() {
    std::lock_guard init_lock(initMtx);
    if (backend_initialized_.load(std::memory_order_acquire)) {
        return true;
    }
    if (!initializeInterpreterLocked()) {
        return false;
    }
    if (!lifecycle_.transition(PythonLifecycleState::BackendInitializing)) {
        return lifecycle_.state() == PythonLifecycleState::Running;
    }

    // Failure policy 1 and 2: a missing entry or a throwing run() is a hard
    // failure - the engine must refuse to start, so the reason has to be visible
    // in the snapshot and in the log.
    const auto fail = [this](const std::string& message, const char* phase) {
        lifecycle_.transition(PythonLifecycleState::Failed);
        {
            std::lock_guard lock(lifecycle_mtx_);
            lifecycle_snapshot_.state = lifecycle_.state();
            lifecycle_snapshot_.phase = phase;
            lifecycle_snapshot_.error = message;
        }
        CFW_LOG_CRITICAL("event=python.host_entry.failed phase={} detail={}", phase, message);
    };

    {
        nanobind::gil_scoped_acquire gil;
        const char* phase = "python_entry_resolution";
        try {
            nanobind::module_ entrance = nanobind::module_::import_("main");
            // The only two names C++ resolves by string: everything else is called
            // from inside Python now.
            pStartFunc = nanobind::getattr(entrance, "run");
            loopFunc = nanobind::getattr(entrance, "loop");
            phase = "python_run";
            pStartFunc();
        } catch (const nanobind::python_error& error) {
            fail(error.what(), phase);
            log_python_error(error);
            pStartFunc.reset();
            loopFunc.reset();
            return false;
        }
    }

    backend_initialized_.store(true, std::memory_order_release);
    lifecycle_.transition(PythonLifecycleState::Running);
    {
        std::lock_guard lock(lifecycle_mtx_);
        lifecycle_snapshot_.state = lifecycle_.state();
        lifecycle_snapshot_.phase = "running";
    }
    CFW_LOG_INFO("event=python.lifecycle.state state=running");
    return true;
}

void PythonAPI::enter_runtime() {
    // thread_loop keeps calling update() after the loop exits; only the first call
    // may enter, every later one must return immediately.
    if (entered_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    bool loop_failed = false;
    bool exited_unexpectedly = false;
    std::string failure_detail;
    std::string snapshot_json;
    bool snapshot_valid = false;

    if (!backend_initialized_.load(std::memory_order_acquire) || !loopFunc.is_valid()) {
        loop_failed = true;
        failure_detail = "main.loop was never resolved";
        CFW_LOG_CRITICAL("event=python.host_loop.failed detail=main.loop was never resolved");
    } else if (!runtime_coordinator_.bind_consumer_thread()) {
        // Same rule as the old per-frame entry: only the bound consumer may run the
        // loop, because the pull entries are consumer-thread-only.
        loop_failed = true;
        failure_detail = "python host loop entered on a non-owner thread";
        CFW_LOG_CRITICAL("event=python.runtime.non_owner_thread");
    } else {
        runtime_coordinator_.set_execution_phase("waiting");
        // Published here, not in initializeInterpreter(): UiSystem subscribes during
        // initialize_all() and ScriptSystem initializes first (priority 60), so the
        // synchronous bus would drop a publish made there. The first update is the
        // earliest moment the subscriber can hear it.
        if (auto* event_bus = Kernel::KernelContext::instance().event_bus()) {
            event_bus->publish<Events::ScriptFinishStartEvent>({});
        }
        nanobind::gil_scoped_acquire gil;
        try {
            nanobind::object snapshot = loopFunc();
            nlohmann::json converted;
            if (python_to_json(snapshot.ptr(), converted)) {
                snapshot_json = converted.dump();
                snapshot_valid = true;
            } else if (PyErr_Occurred()) {
                // Consume the conversion error: it must not leak into later C-API use.
                nanobind::python_error error;
                CFW_LOG_ERROR("event=python.host_loop.snapshot_failed detail={}", error.what());
                log_python_error(error);
            }
            // Failure policy 3 covers "throwing OR returning early": a clean return
            // with no stop request means the host loop vanished, not that the engine
            // asked it to stop.
            if (!runtime_coordinator_.stop_requested()) {
                loop_failed = true;
                exited_unexpectedly = true;
                failure_detail = "main.loop returned without a stop request";
                CFW_LOG_CRITICAL(
                    "event=python.host_loop.exited_unexpectedly "
                    "detail=main.loop returned without a stop request");
            }
        } catch (const nanobind::python_error& error) {
            // Failure policy 3: degrade, never request engine shutdown.
            loop_failed = true;
            failure_detail = error.what();
            CFW_LOG_CRITICAL("event=python.host_loop.failed detail={}", error.what());
            log_python_error(error);
        } catch (const std::exception& error) {
            loop_failed = true;
            failure_detail = error.what();
            CFW_LOG_CRITICAL("event=python.host_loop.failed detail={}", error.what());
        } catch (...) {
            loop_failed = true;
            failure_detail = "unknown exception";
            CFW_LOG_CRITICAL("event=python.host_loop.failed detail=unknown exception");
        }
        // Released here, while the interpreter and the GIL still exist. The deleted
        // LifecycleControl branch used to do this; without it every normal shutdown
        // would detach these two live references and log the "survived cooperative
        // shutdown" ERROR from shutdown().
        pStartFunc.reset();
        loopFunc.reset();
    }
    backend_initialized_.store(false, std::memory_order_release);

    // Both the normal and the degraded exit settle here: pending tickets do not
    // wake up on their own, and a stuck submit_and_wait() would hang the editor.
    shutting_down_.store(true, std::memory_order_release);
    runtime_coordinator_.begin_python_stopping();
    runtime_coordinator_.stop();
    lifecycle_.request_stop();
    lifecycle_.transition(PythonLifecycleState::Stopping);
    lifecycle_.transition(PythonLifecycleState::Stopped);
    // The loop is gone; never leave the diagnostic phase describing a dead runtime
    // as "waiting".
    runtime_coordinator_.set_execution_phase(loop_failed ? "host_loop_failed" : "host_loop_exited");
    {
        std::lock_guard lock(lifecycle_mtx_);
        if (snapshot_valid) {
            last_python_shutdown_snapshot_json_ = std::move(snapshot_json);
        }
        if (loop_failed && !failure_detail.empty()) {
            lifecycle_snapshot_.error = std::move(failure_detail);
        }
        lifecycle_snapshot_.state = lifecycle_.state();
        if (loop_failed) {
            lifecycle_snapshot_.phase =
                exited_unexpectedly ? "host_loop_exited_unexpectedly" : "host_loop_failed";
        } else {
            lifecycle_snapshot_.phase = "host_loop_exited";
        }
        lifecycle_snapshot_.shutting_down = true;
    }
    CFW_LOG_NOTICE("event=python.lifecycle.state state=stopped");
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
