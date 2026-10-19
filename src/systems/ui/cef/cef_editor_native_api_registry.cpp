#include "cef_editor_native_api_registry.h"

#include <horizon/core/logging.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "native_api_timing.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Corona::Systems::UI {

namespace {

std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::uint32_t current_thread_id() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint32_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

std::mutex g_in_flight_mutex;
NativeApiInFlight g_in_flight;
std::atomic<std::uint64_t> g_in_flight_sequence{0};
std::atomic<std::uint64_t> g_reported_stuck_sequence{0};
std::atomic<std::int64_t> g_last_stuck_poll_ms{0};

class InFlightRequestGuard {
public:
    explicit InFlightRequestGuard(std::uint64_t sequence) : sequence_(sequence) {}
    ~InFlightRequestGuard() {
        std::lock_guard<std::mutex> lock(g_in_flight_mutex);
        if (g_in_flight.sequence == sequence_) {
            g_in_flight.active = false;
        }
    }
    InFlightRequestGuard(const InFlightRequestGuard&) = delete;
    InFlightRequestGuard& operator=(const InFlightRequestGuard&) = delete;

private:
    std::uint64_t sequence_;
};

/// Records the request the CEF UI thread is about to run.  A handler that never
/// returns leaves this marked active, which is what the stuck report observes.
void begin_native_request(const NativeRequest& request, std::uint64_t sequence) {
    std::lock_guard<std::mutex> lock(g_in_flight_mutex);
    g_in_flight.module = request.module;
    g_in_flight.function = request.function;
    g_in_flight.started_ms = steady_now_ms();
    g_in_flight.sequence = sequence;
    g_in_flight.dispatcher_thread_id = current_thread_id();
    g_in_flight.active = true;
}

NativeApiInFlight in_flight_snapshot() {
    std::lock_guard<std::mutex> lock(g_in_flight_mutex);
    return g_in_flight;
}

/// Reports a request that occupied the CEF UI thread for too long.  Requests are
/// dispatched on that same thread, so the log line names the handler that made
/// the editor UI unresponsive while the render loop kept running.
void report_slow_native_request(const NativeRequest& request,
                                const NativeResult& result,
                                std::chrono::steady_clock::time_point started) {
    if (!result.handled) {
        return;
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    if (!is_slow_native_request(elapsed_ms)) {
        return;
    }
    const NativeApiTiming timing{request.module, request.function, elapsed_ms,
                                 result.handled, result.success};
    CFW_LOG_WARNING("[NativeApi] slow request {} elapsed={}ms handled={} success={}",
                    describe_native_request(timing), elapsed_ms, result.handled,
                    result.success);
}

}  // namespace

void report_stuck_native_api_request() {
    const auto now = steady_now_ms();
    const auto last_poll = g_last_stuck_poll_ms.load(std::memory_order_relaxed);
    if (last_poll != 0 && now - last_poll < 1000) {
        return;
    }
    g_last_stuck_poll_ms.store(now, std::memory_order_relaxed);

    const auto request = in_flight_snapshot();
    if (!is_stuck_native_request(
            request, now, g_reported_stuck_sequence.load(std::memory_order_relaxed))) {
        return;
    }
    g_reported_stuck_sequence.store(request.sequence, std::memory_order_relaxed);
    const NativeApiTiming timing{request.module, request.function, now - request.started_ms,
                                 true, false};
    CFW_LOG_CRITICAL(
        "[NativeApi] STUCK {} running for {}ms on thread {} - the CEF UI thread is blocked "
        "inside this handler and the editor UI will not repaint or accept input",
        describe_native_request(timing), now - request.started_ms,
        request.dispatcher_thread_id);
}

void register_scene_tools_api_handlers(NativeApiRegistry& registry);
void register_main_view_api_handlers(NativeApiRegistry& registry);
void register_project_launcher_api_handlers(NativeApiRegistry& registry);
void register_file_manager_api_handlers(NativeApiRegistry& registry);
void register_project_settings_api_handlers(NativeApiRegistry& registry);
void register_network_api_handlers(NativeApiRegistry& registry);
void register_lanchat_api_handlers(NativeApiRegistry& registry);
void register_editor_api_handlers(NativeApiRegistry& registry);
void register_python_script_api_handlers(NativeApiRegistry& registry);

NativeApiRegistry& NativeApiRegistry::instance() {
    static NativeApiRegistry registry;
    return registry;
}

void NativeApiRegistry::register_module(std::string module, NativeHandlerFn handler) {
    handlers_[std::move(module)] = std::move(handler);
}

std::optional<NativeResult> NativeApiRegistry::dispatch(
    const NativeRequest& request,
    const NativeContext& context) const {
    const auto it = handlers_.find(request.module);
    if (it == handlers_.end()) {
        return std::nullopt;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto sequence = g_in_flight_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    begin_native_request(request, sequence);
    NativeResult result;
    {
        // Clears the in-flight mark on every exit path, including a throwing
        // handler, so a completed request can never be reported as stuck.
        const InFlightRequestGuard guard(sequence);
        result = it->second(request, context);
    }
    report_slow_native_request(request, result, started);
    if (!result.handled) {
        return std::nullopt;
    }
    return result;
}

NativeResult native_success(nlohmann::json data, std::string route) {
    NativeResult result;
    result.success = true;
    result.data = std::move(data);
    result.route = std::move(route);
    return result;
}

NativeResult native_failure(std::string error, int error_code, std::string route) {
    NativeResult result;
    result.handled = true;
    result.success = false;
    result.error_code = error_code;
    result.error = std::move(error);
    result.route = std::move(route);
    return result;
}

NativeResult native_unhandled() {
    NativeResult result;
    result.handled = false;
    result.success = false;
    result.route = "unhandled";
    return result;
}

std::string native_success_json(const NativeRequest& request, const NativeResult& result) {
    nlohmann::json payload;
    payload["success"] = true;
    payload["data"] = result.data;
    payload["function"] = request.function;
    payload["module"] = request.module;
    payload["route"] = result.route;
    return payload.dump();
}

std::string unsupported_editor_api_route_json(const NativeRequest& request) {
    nlohmann::json payload;
    payload["success"] = false;
    payload["error"] = request.module + "." + request.function + " is not supported by Editor API";
    payload["module"] = request.module;
    payload["function"] = request.function;
    payload["route"] = "unsupported";
    return payload.dump();
}

void register_builtin_native_api_handlers() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto& registry = NativeApiRegistry::instance();
        register_file_manager_api_handlers(registry);
        register_project_launcher_api_handlers(registry);
        register_main_view_api_handlers(registry);
        register_project_settings_api_handlers(registry);
        register_scene_tools_api_handlers(registry);
        register_network_api_handlers(registry);
        register_lanchat_api_handlers(registry);
        register_editor_api_handlers(registry);
        register_python_script_api_handlers(registry);
    });
}

std::string arg_string(const nlohmann::json& args, size_t index, std::string fallback) {
    if (!args.is_array() || index >= args.size()) {
        return fallback;
    }
    const auto& value = args[index];
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer() || value.is_number_unsigned() || value.is_number_float()) {
        return value.dump();
    }
    return fallback;
}

bool arg_bool(const nlohmann::json& args, size_t index, bool fallback) {
    if (!args.is_array() || index >= args.size()) {
        return fallback;
    }
    const auto& value = args[index];
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    return fallback;
}

uint16_t arg_uint16(const nlohmann::json& args, size_t index, uint16_t fallback) {
    if (!args.is_array() || index >= args.size()) {
        return fallback;
    }
    try {
        const auto& value = args[index];
        if (value.is_number_unsigned()) {
            return value.get<uint16_t>();
        }
        if (value.is_number_integer()) {
            const auto raw = value.get<int64_t>();
            return raw >= 0 ? static_cast<uint16_t>(raw) : fallback;
        }
        if (value.is_string()) {
            return static_cast<uint16_t>(std::stoul(value.get<std::string>()));
        }
    } catch (...) {
    }
    return fallback;
}

uint64_t arg_uint64(const nlohmann::json& args, size_t index, uint64_t fallback) {
    if (!args.is_array() || index >= args.size()) {
        return fallback;
    }
    try {
        const auto& value = args[index];
        if (value.is_number_unsigned()) {
            return value.get<uint64_t>();
        }
        if (value.is_number_integer()) {
            const auto raw = value.get<int64_t>();
            return raw >= 0 ? static_cast<uint64_t>(raw) : fallback;
        }
        if (value.is_string()) {
            return std::stoull(value.get<std::string>());
        }
    } catch (...) {
    }
    return fallback;
}

std::uintptr_t arg_uintptr(const nlohmann::json& args, size_t index, std::uintptr_t fallback) {
    return static_cast<std::uintptr_t>(arg_uint64(args, index, static_cast<uint64_t>(fallback)));
}

nlohmann::json arg_object(const nlohmann::json& args, size_t index) {
    if (!args.is_array() || index >= args.size() || !args[index].is_object()) {
        return nlohmann::json::object();
    }
    return args[index];
}

}  // namespace Corona::Systems::UI
