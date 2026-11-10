#pragma once

#include "editor_api_caller.h"

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

struct NativeRequest {
    std::string module;
    std::string function;
    nlohmann::json args = nlohmann::json::array();
};

// CEF-free by contract: no browser/frame handles cross this boundary. Callers
// that need CEF behaviour carry an EditorApiCaller tag instead. Internal C++
// orchestration defaults to Native; the editor dispatch layer overwrites the
// tag from the request's caller before a handler runs.
struct NativeContext {
    EditorApiCaller caller = EditorApiCaller::Native;
};

struct NativeResult {
    bool handled = true;
    bool success = true;
    int error_code = 1;
    std::string error;
    nlohmann::json data = nlohmann::json::object();
    std::string route = "native-cpp";
};


NativeResult native_success(nlohmann::json data = nlohmann::json::object(),
                            std::string route = "native-cpp");
NativeResult native_failure(std::string error,
                            int error_code = 1,
                            std::string route = "native-cpp");
NativeResult native_unhandled();


void tick_native_animation_updates();

/// Reports a native editor API request that entered dispatch and never returned.
/// A blocked CEF UI thread runs no handler completion, so nothing else in the
/// logging can name a handler that deadlocked.  Call this from a thread that is
/// still alive (the engine main loop); it is rate limited internally and reports
/// each stuck request once.
void report_stuck_native_api_request();

std::string arg_string(const nlohmann::json& args, size_t index, std::string fallback = "");
bool arg_bool(const nlohmann::json& args, size_t index, bool fallback = false);
uint16_t arg_uint16(const nlohmann::json& args, size_t index, uint16_t fallback = 0);
uint64_t arg_uint64(const nlohmann::json& args, size_t index, uint64_t fallback = 0);
std::uintptr_t arg_uintptr(const nlohmann::json& args, size_t index, std::uintptr_t fallback = 0);
nlohmann::json arg_object(const nlohmann::json& args, size_t index);

}  // namespace Corona::Systems::UI
