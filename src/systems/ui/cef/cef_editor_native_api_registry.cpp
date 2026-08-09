#include "cef_editor_native_api_registry.h"

#include <mutex>
#include <stdexcept>

namespace Corona::Systems::UI {

void register_scene_tools_api_handlers(NativeApiRegistry& registry);
void register_scene_datas_api_handlers(NativeApiRegistry& registry);
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
    auto result = it->second(request, context);
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
        register_scene_datas_api_handlers(registry);
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
