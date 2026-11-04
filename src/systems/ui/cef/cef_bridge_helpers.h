#pragma once

#include <cef_browser.h>
#include <wrapper/cef_message_router.h>

#include <string>
#include <vector>

namespace Corona::Systems::UI {

// Input 事件结构：V8 injectInput → CEF ProcessMessage → 事件队列 → Python 消费
// Also forward-declared in engine_bindings.cpp (kept in sync).
struct InputEvent {
    int type;           // 0=keyDown, 1=keyUp, 2=mouseEvent
    std::string arg0;  // key code / eventType
    std::string arg1;  // modifiers / button / displayKey
    std::string arg2;  // displayKey (keyDown only)
    double arg3{0.0};  // x (mouse)
    double arg4{0.0};  // y (mouse)
};

// 消费所有积攒的输入事件（由 Python update_runtime 每帧消费）
void enqueue_input_event(InputEvent event);

std::vector<InputEvent> drain_input_events();

// Called on the CEF/UI thread once per frame to complete and dispatch pending GPU picks.
void drain_actor_pick_completion_events();

bool handle_realtime_process_message(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     const CefRefPtr<CefProcessMessage>& message);

bool forward_process_message_to_router(const CefRefPtr<CefMessageRouterBrowserSide>& browser_side_router,
                                       CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefFrame> frame,
                                       CefProcessId source_process,
                                       CefRefPtr<CefProcessMessage> message);

}  // namespace Corona::Systems::UI
