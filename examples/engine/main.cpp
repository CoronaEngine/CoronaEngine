#include <corona/engine.h>
#include <horizon/core/logging.h>
#include <corona/systems/ui/cef_runtime.h>

#include <csignal>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

// 全局引擎实例指针，用于信号处理
static Corona::Engine* g_engine = nullptr;

#ifdef _WIN32
void log_signal_stack_trace() {
    void* frames[64] = {};
    const USHORT frame_count = CaptureStackBackTrace(0, 64, frames, nullptr);
    CFW_LOG_WARNING("[Signal] Captured {} stack frame(s)", static_cast<unsigned>(frame_count));
    for (USHORT i = 0; i < frame_count; ++i) {
        CFW_LOG_WARNING("[Signal] stack[{}]=0x{:x}",
                        static_cast<unsigned>(i),
                        reinterpret_cast<std::uintptr_t>(frames[i]));
    }
}
#endif

/**
 * @brief 信号处理函数
 *
 * 捕获 Ctrl+C 等中断信号，优雅退出引擎
 */
void signal_handler(int signal) {
    auto signal_name = "Unknown";
    switch (signal) {
        case SIGINT:
            signal_name = "SIGINT (Ctrl+C)";
            break;
        case SIGTERM:
            signal_name = "SIGTERM";
            break;
        case SIGABRT:
            signal_name = "SIGABRT";
            break;
        case SIGSEGV:
            signal_name = "SIGSEGV (Segmentation Fault)";
            break;
        case SIGFPE:
            signal_name = "SIGFPE (Floating Point Exception)";
            break;
        case SIGILL:
            signal_name = "SIGILL (Illegal Instruction)";
            break;
#ifdef SIGBREAK
        case SIGBREAK:
            signal_name = "SIGBREAK (Ctrl+Break)";
            break;
#endif
        default:
            break;
    }

    CFW_LOG_WARNING("[Signal] Received signal {}: {}, requesting engine shutdown...", signal, signal_name);
#ifdef _WIN32
    log_signal_stack_trace();
#endif
    CFW_LOG_FLUSH();

    if (g_engine) {
        g_engine->request_exit();
    }
}

/**
 * @brief CoronaEngine 主程序
 *
 * 功能：
 * 1. 初始化 CoronaEngine
 * 2. 注册信号处理器
 * 3. 启动主循环
 * 4. 优雅关闭引擎
 */
int main(int argc, char* argv[]) {
    if (const auto exit_code =
            Corona::Systems::UI::execute_cef_subprocess_if_needed(argc, argv);
        exit_code.has_value()) {
        return *exit_code;
    }

    // Horizon now leaves output configuration to the application.
    const auto now = std::time(nullptr);
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif
    std::ostringstream log_filename;
    log_filename << std::put_time(&local_time, "%Y-%m-%d_%H-%M-%S") << "_corona.log";
    horizon::core::LoggingOptions logging_options;
    logging_options.file_path = std::filesystem::path("logs") / log_filename.str();
    logging_options.install_signal_handlers = true;
    logging_options.configure_utf8_console = true;
    horizon::core::initialize_logging(logging_options);
    Corona::Kernel::CoronaLogger::set_log_level(Corona::Kernel::LogLevel::debug);

    CFW_LOG_NOTICE(
        "\n"
        "    +==================================================================+\n"
        "    |                                                                  |\n"
        "    |                      CoronaEngine v0.5.0                         |\n"
        "    |                                                                  |\n"
        "    |              A Modern Game Engine Framework                      |\n"
        "    |                                                                  |\n"
        "    +==================================================================+\n");

    // 创建引擎实例
    Corona::Engine engine;

    g_engine = &engine;

    // 注册信号处理器
    std::signal(SIGINT, signal_handler);   // Ctrl+C
    std::signal(SIGTERM, signal_handler);  // 终止请求
    std::signal(SIGABRT, signal_handler);  // 异常终止
    std::signal(SIGSEGV, signal_handler);  // 段错误
    std::signal(SIGFPE, signal_handler);   // 浮点异常
    std::signal(SIGILL, signal_handler);   // 非法指令
#ifdef SIGBREAK
    std::signal(SIGBREAK, signal_handler);  // Windows Ctrl+Break
#endif

    // ========================================
    // 1. 初始化引擎
    // ========================================
    CFW_LOG_INFO("[Main] Initializing engine...");

    if (!engine.initialize()) {
        CFW_LOG_ERROR("[Main] Failed to initialize engine!");
        CFW_LOG_FLUSH();
        return -1;
    }

    CFW_LOG_INFO("[Main] Engine initialized successfully");

    // ========================================
    // 2. 启动主循环（在主线程中运行）
    // ========================================
    // 注意：SDL/ImGui 必须在创建窗口的同一线程中处理事件
    // 因此 engine.run() 必须在主线程中运行
    CFW_LOG_INFO("[Main] Starting engine main loop...");
    CFW_LOG_INFO("[Main] Press Ctrl+C to exit");

    // 在主线程运行引擎主循环
    engine.run();

    // ========================================
    // 3. 关闭引擎
    // ========================================
    CFW_LOG_INFO("[Main] Shutting down engine...");

    engine.shutdown();

    CFW_LOG_NOTICE(
        "[Main] Engine shutdown complete\n"
        "\n"
        "+==================================================================+\n"
        "|                Thank you for using CoronaEngine!                 |\n"
        "+==================================================================+\n");

    CFW_LOG_FLUSH();
    g_engine = nullptr;

    // 使用 TerminateProcess 强杀自身，跳过 DLL_PROCESS_DETACH / atexit。
    // ExitProcess / return 0 在执行 DLL detach 时会被 torch_python.dll、
    // python313.dll 或 libcef.dll 的全局析构/DllMain 阻塞，导致僵尸进程。
    // TerminateProcess 直接通知内核终止，OS 回收所有资源。
    TerminateProcess(GetCurrentProcess(), 0);

    // 永远不会执行到这里，但保留以消除编译器警告
    return 0;
}
