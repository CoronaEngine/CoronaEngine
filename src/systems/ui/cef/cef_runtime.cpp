#include <corona/systems/ui/cef_runtime.h>

#include <atomic>

#include "cef_process.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Corona::Systems::UI {

namespace {

std::atomic_bool process_dispatch_completed{false};

}  // namespace

std::optional<int> execute_cef_subprocess_if_needed(int argc, char* argv[]) {
#ifdef _WIN32
    CefMainArgs main_args(GetModuleHandleW(nullptr));
#else
    CefMainArgs main_args(argc, argv);
#endif

    const int exit_code = CefExecuteProcess(main_args, create_cef_app(), nullptr);
    if (exit_code >= 0) {
        return exit_code;
    }

    process_dispatch_completed.store(true, std::memory_order_release);
    return std::nullopt;
}

bool was_cef_process_dispatch_completed() {
    return process_dispatch_completed.load(std::memory_order_acquire);
}

}  // namespace Corona::Systems::UI
