#pragma once

#include <include/cef_app.h>

namespace Corona::Systems::UI {

// The browser process always uses the C++-owned single-process policy.
CefRefPtr<CefApp> create_cef_app();
bool was_cef_process_dispatch_completed();

void append_cef_command_line_switches(CefRefPtr<CefCommandLine> command_line,
                                      bool gpu_enabled);

}  // namespace Corona::Systems::UI
