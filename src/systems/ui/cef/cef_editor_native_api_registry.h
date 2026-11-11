#pragma once

#include "native_api_timing.h"

#include <chrono>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

struct NativeRequest {
    std::string module;
    std::string function;
    nlohmann::json args = nlohmann::json::array();
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

/// Marks one editor request as in flight for as long as it lives.
///
/// A request that never returns leaves the mark active, which is what
/// `report_stuck_native_api_request()` observes from the engine main loop: the
/// blocked CEF UI thread runs no completion, so nothing else can name it. Construct
/// the scope on the thread that actually runs the operation (the editor owner
/// thread), not on the caller that is merely waiting for it.
class NativeApiRequestScope {
   public:
    explicit NativeApiRequestScope(const NativeRequest& request);
    ~NativeApiRequestScope();
    NativeApiRequestScope(const NativeApiRequestScope&) = delete;
    NativeApiRequestScope& operator=(const NativeApiRequestScope&) = delete;

    /// Logs the request when it occupied this thread longer than the slow threshold.
    void report_if_slow(const NativeResult& result) const;

   private:
    std::uint64_t sequence_ = 0;
    std::string module_;
    std::string function_;
    std::chrono::steady_clock::time_point started_{};
};


void tick_native_animation_updates();

/// Reports a native editor API request that entered dispatch and never returned.
/// A blocked CEF UI thread runs no handler completion, so nothing else in the
/// logging can name a handler that deadlocked.  Call this from a thread that is
/// still alive (the engine main loop); it is rate limited internally and reports
/// each stuck request once.
void report_stuck_native_api_request();

/// Test seam: the request the editor owner thread is currently running, or an
/// inactive record when nothing is in flight.
NativeApiInFlight native_api_in_flight_for_test();

std::string arg_string(const nlohmann::json& args, size_t index, std::string fallback = "");
bool arg_bool(const nlohmann::json& args, size_t index, bool fallback = false);
uint16_t arg_uint16(const nlohmann::json& args, size_t index, uint16_t fallback = 0);
uint64_t arg_uint64(const nlohmann::json& args, size_t index, uint64_t fallback = 0);
std::uintptr_t arg_uintptr(const nlohmann::json& args, size_t index, std::uintptr_t fallback = 0);
nlohmann::json arg_object(const nlohmann::json& args, size_t index);

}  // namespace Corona::Systems::UI
