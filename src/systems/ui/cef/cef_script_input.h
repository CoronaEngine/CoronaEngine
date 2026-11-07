#pragma once

#include <corona/systems/script/script_input.h>
#include "cef_window_operations.h"

namespace Corona::Systems::UI {
enum class ScriptInputMethod { Dispatch };
struct ScriptInputRequest {
    WindowRequest source{WindowMethod::ConfigureViewport, 0};
    InputEvent event;
};
struct ScriptInputReply {
    std::string error_code, error_message;
};
// BrowserManager main thread only, following scene-owner identity resolution.
ScriptInputReply dispatch_script_input(ScriptInputRequest request, std::function<bool()> context_active);
} // namespace Corona::Systems::UI
