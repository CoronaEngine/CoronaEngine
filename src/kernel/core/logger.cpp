#include <core/util/logging.h>
#include <core/util/logging_quill.h>
#include <corona/kernel/core/i_logger.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace Corona::Kernel {
namespace {

horizon::core::LogLevel to_horizon_level(LogLevel level) {
    switch (level) {
        case LogLevel::trace:
            return horizon::core::LogLevel::Trace;
        case LogLevel::debug:
            return horizon::core::LogLevel::Debug;
        case LogLevel::info:
            return horizon::core::LogLevel::Info;
        case LogLevel::warning:
            return horizon::core::LogLevel::Warning;
        case LogLevel::error:
            return horizon::core::LogLevel::Error;
        case LogLevel::fatal:
            return horizon::core::LogLevel::Critical;
    }
    return horizon::core::LogLevel::Info;
}

}  // namespace

void EngineLogger::initialize() {
    // Thread-safe initialization retries if configuring the backend throws.
    [[maybe_unused]] static const bool initialized = [] {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local_time{};
#ifdef _WIN32
        localtime_s(&local_time, &now);
#else
        localtime_r(&now, &local_time);
#endif
        std::ostringstream filename;
        filename << std::put_time(&local_time, "%Y-%m-%d_%H-%M-%S") << "_corona.log";

        horizon::core::LoggingOptions options;
        options.file_path = std::filesystem::path("logs") / filename.str();
        options.install_signal_handlers = true;
        options.configure_utf8_console = true;
#ifdef CORONA_LOG_LEVEL
        options.level = to_horizon_level(static_cast<LogLevel>(CORONA_LOG_LEVEL));
#endif
        horizon::core::initialize_logging(options);
        return true;
    }();
}

void EngineLogger::set_log_level(LogLevel level) {
    initialize();
    horizon::core::set_log_level(to_horizon_level(level));
}

void EngineLogger::flush() {
    initialize();
    horizon::core::log_flush();
}

quill::Logger* EngineLogger::get_logger() {
    initialize();
    return horizon::core::get_quill_logger();
}

}  // namespace Corona::Kernel
