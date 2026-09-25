#include <corona/kernel/core/i_logger.h>
#include <corona/kernel/utils/storage.h>

// Public engine headers must coexist with the legacy renderer's core headers.
#ifdef OC_CORE_API
#error "Corona kernel headers leaked Horizon Core implementation macros"
#endif
#include <core/util/logging_quill.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool test_logging() {
    using Corona::Kernel::CoronaLogger;
    using Corona::Kernel::LogLevel;

    // Each invocation owns its output so stale logs cannot make the test pass.
    const auto test_dir = std::filesystem::current_path() /
                          ("kernel-compat-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(test_dir);
    std::filesystem::current_path(test_dir);

    CFW_LOG_INFO("before second initialization {}", 17);
    CoronaLogger::initialize();
    if (!check(CoronaLogger::get_logger() == horizon::core::get_quill_logger(),
               "Corona and Horizon must share the logging backend")) {
        return false;
    }

    const std::pair<LogLevel, quill::LogLevel> levels[] = {
        {LogLevel::trace, quill::LogLevel::TraceL3},
        {LogLevel::debug, quill::LogLevel::Debug},
        {LogLevel::info, quill::LogLevel::Info},
        {LogLevel::warning, quill::LogLevel::Warning},
        {LogLevel::error, quill::LogLevel::Error},
        {LogLevel::fatal, quill::LogLevel::Critical},
    };
    for (const auto& [level, expected] : levels) {
        CoronaLogger::set_log_level(level);
        if (!check(CoronaLogger::get_logger()->get_log_level() == expected, "incorrect log level mapping")) {
            return false;
        }
    }

    CoronaLogger::set_log_level(LogLevel::warning);
    int evaluated = 0;
    CFW_LOG_DEBUG("filtered argument {}", ++evaluated);
    if (!check(evaluated == 0, "filtered log arguments must not be evaluated")) {
        return false;
    }
    CFW_LOG_ERROR("nonfatal error {}", 23);
    CFW_LOG_CRITICAL("nonfatal critical {}", 29);
    PY_LOG_WARNING("python marker {}", 31);
    VUE_LOG_WARNING("vue marker {}", 37);
    CFW_LOG_FLUSH();

    std::string output;
    std::size_t log_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("logs")) {
        if (entry.path().extension() == ".log") {
            ++log_count;
            std::ifstream stream(entry.path());
            output.append(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
    }
    bool ok = check(log_count == 1, "initialization must create exactly one log file");
    for (const char* message : {"before second initialization 17", "nonfatal error 23", "nonfatal critical 29",
                                "[Python] python marker 31", "[Vue] vue marker 37", "test_horizon_kernel_compat.cpp"}) {
        ok &= check(output.find(message) != std::string::npos, message);
    }
    ok &= check(output.find("filtered argument") == std::string::npos, "filtered message reached the log");
    return ok;
}

bool test_storage() {
    Corona::Kernel::Utils::Storage<int, 2, 1> storage;
    const auto first = storage.allocate();
    const auto second = storage.allocate();
    const auto third = storage.allocate();
    if (!check(storage.count() == 3 && storage.capacity() >= 3, "storage must grow past its initial buffer")) {
        return false;
    }
    {
        auto writer = storage.acquire_write(first);
        if (!check(writer.valid(), "allocated storage must be writable")) {
            return false;
        }
        *writer = 41;
    }
    {
        auto reader = storage.acquire_read(first);
        if (!check(reader.valid() && *reader == 41, "storage lost the value across handle lifetimes")) {
            return false;
        }
    }
    storage.deallocate(first);
    storage.deallocate(second);
    storage.deallocate(third);
    return check(storage.empty() && !storage.contains(first), "deallocation must release occupied slots");
}

}  // namespace

int main() {
    return test_logging() && test_storage() ? 0 : 1;
}
