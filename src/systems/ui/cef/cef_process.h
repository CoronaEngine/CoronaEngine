#pragma once

// The CEF process objects we create and the V8 endpoint we attach to a context. These three
// were 5-14 line headers that each pulled in the CEF headers anyway. The light declaration
// headers next to them stay separate on purpose: folding them in would drag the CEF headers
// into translation units that only wanted a one-line declaration.

#include <include/cef_app.h>
#include <include/cef_v8.h>

#include <memory>
#include <string>

namespace Corona::Systems::UI {

class CoronaV8Endpoint;

// The browser process always uses the C++-owned single-process policy.
CefRefPtr<CefApp> create_cef_app();
bool was_cef_process_dispatch_completed();

void append_cef_command_line_switches(CefRefPtr<CefCommandLine> command_line,
                                      bool gpu_enabled);

CefRefPtr<CefRenderProcessHandler> create_cef_render_process_handler();

// Created/released on the context-owning renderer thread. Engine tasks pass
// native DTOs back through its task runner; they never access V8 handles.
std::shared_ptr<CoronaV8Endpoint> attach_corona_v8_binding(CefRefPtr<CefV8Context> context);
void release_corona_v8_binding(const std::shared_ptr<CoronaV8Endpoint>& endpoint);

// Native window facts/intents, delivered as DTOs on each context's task runner.
void notify_corona_window_closed(int tab_id, const std::string& panel_id);
void request_corona_window_close(int browser_id);

}  // namespace Corona::Systems::UI
