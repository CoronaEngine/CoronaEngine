#pragma once

#include <include/cef_v8.h>
#include <memory>
#include <string>

namespace Corona::Systems::UI {

class CoronaV8Endpoint;

// Created/released on the context-owning renderer thread. Engine tasks pass
// native DTOs back through its task runner; they never access V8 handles.
std::shared_ptr<CoronaV8Endpoint> attach_corona_v8_binding(CefRefPtr<CefV8Context> context);
void release_corona_v8_binding(const std::shared_ptr<CoronaV8Endpoint>& endpoint);

// Native window facts/intents, delivered as DTOs on each context's task runner.
void notify_corona_window_closed(int tab_id, const std::string& panel_id);
void request_corona_window_close(int browser_id);

}  // namespace Corona::Systems::UI
