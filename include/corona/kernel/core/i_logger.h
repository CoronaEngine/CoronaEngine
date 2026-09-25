#pragma once

#include <quill/LogMacros.h>
#include <quill/Logger.h>

namespace Corona::Kernel {

enum class LogLevel {
    trace,
    debug,
    info,
    warning,
    error,
    fatal
};

// Horizon Core headers stay in the implementation to isolate their legacy macros.
// A distinct implementation type avoids symbols owned by Horizon's compatibility API.
class EngineLogger {
   public:
    // Initialize before other Horizon logging to retain Corona's file output.
    static void initialize();
    static void set_log_level(LogLevel level);
    static void flush();
    static quill::Logger* get_logger();

   private:
    EngineLogger() = delete;
};

using CoronaLogger = EngineLogger;

}  // namespace Corona::Kernel

// Use Quill directly to preserve call-site locations and lazy argument evaluation.
// In particular, ERROR and CRITICAL record a message without terminating the process.
#define CFW_LOG_FLUSH() ::Corona::Kernel::CoronaLogger::flush()
#define CFW_LOG_TRACE(fmt, ...) LOG_TRACE_L3(::Corona::Kernel::CoronaLogger::get_logger(), fmt, ##__VA_ARGS__)
#define CFW_LOG_DEBUG(fmt, ...) LOG_DEBUG(::Corona::Kernel::CoronaLogger::get_logger(), fmt, ##__VA_ARGS__)
#define CFW_LOG_INFO(fmt, ...) LOG_INFO(::Corona::Kernel::CoronaLogger::get_logger(), fmt, ##__VA_ARGS__)
#define CFW_LOG_NOTICE(fmt, ...) LOG_NOTICE(::Corona::Kernel::CoronaLogger::get_logger(), fmt, ##__VA_ARGS__)
#define CFW_LOG_WARNING(fmt, ...) LOG_WARNING(::Corona::Kernel::CoronaLogger::get_logger(), fmt, ##__VA_ARGS__)
#define CFW_LOG_ERROR(fmt, ...) LOG_ERROR(::Corona::Kernel::CoronaLogger::get_logger(), fmt, ##__VA_ARGS__)
#define CFW_LOG_CRITICAL(fmt, ...) LOG_CRITICAL(::Corona::Kernel::CoronaLogger::get_logger(), fmt, ##__VA_ARGS__)

#define PY_LOG_TRACE(fmt, ...) CFW_LOG_TRACE("[Python] " fmt, ##__VA_ARGS__)
#define PY_LOG_DEBUG(fmt, ...) CFW_LOG_DEBUG("[Python] " fmt, ##__VA_ARGS__)
#define PY_LOG_INFO(fmt, ...) CFW_LOG_INFO("[Python] " fmt, ##__VA_ARGS__)
#define PY_LOG_NOTICE(fmt, ...) CFW_LOG_NOTICE("[Python] " fmt, ##__VA_ARGS__)
#define PY_LOG_WARNING(fmt, ...) CFW_LOG_WARNING("[Python] " fmt, ##__VA_ARGS__)
#define PY_LOG_ERROR(fmt, ...) CFW_LOG_ERROR("[Python] " fmt, ##__VA_ARGS__)
#define PY_LOG_CRITICAL(fmt, ...) CFW_LOG_CRITICAL("[Python] " fmt, ##__VA_ARGS__)

#define VUE_LOG_TRACE(fmt, ...) CFW_LOG_TRACE("[Vue] " fmt, ##__VA_ARGS__)
#define VUE_LOG_DEBUG(fmt, ...) CFW_LOG_DEBUG("[Vue] " fmt, ##__VA_ARGS__)
#define VUE_LOG_INFO(fmt, ...) CFW_LOG_INFO("[Vue] " fmt, ##__VA_ARGS__)
#define VUE_LOG_NOTICE(fmt, ...) CFW_LOG_NOTICE("[Vue] " fmt, ##__VA_ARGS__)
#define VUE_LOG_WARNING(fmt, ...) CFW_LOG_WARNING("[Vue] " fmt, ##__VA_ARGS__)
#define VUE_LOG_ERROR(fmt, ...) CFW_LOG_ERROR("[Vue] " fmt, ##__VA_ARGS__)
#define VUE_LOG_CRITICAL(fmt, ...) CFW_LOG_CRITICAL("[Vue] " fmt, ##__VA_ARGS__)
