#include "cef_script_input.h"
#include "cef_client.h"
#include <corona/systems/ui/camera_viewport_manager.h>
#include <mutex>

namespace Corona::Systems::UI {
namespace {
struct QueuedInput { InputEvent event; std::function<bool()> current; };
std::mutex input_mutex;
std::vector<QueuedInput> input_queue;
}

void enqueue_input_event(InputEvent event, std::function<bool()> current) {
    std::lock_guard lock(input_mutex);
    input_queue.push_back({std::move(event), std::move(current)});
}

std::vector<InputEvent> drain_input_events() {
    std::vector<QueuedInput> pending;
    { std::lock_guard lock(input_mutex); pending.swap(input_queue); }
    std::vector<InputEvent> events;
    events.reserve(pending.size());
    for (auto& item : pending) if (!item.current || item.current()) events.push_back(std::move(item.event));
    return events;
}

ScriptInputReply dispatch_script_input(ScriptInputRequest request, std::function<bool()> context_active) {
    const auto& camera = request.source;
    BrowserTab* source = nullptr;
    for (const auto& entry : BrowserManager::instance().tabs_snapshot()) {
        auto* tab = entry.tab;
        if (!tab || !tab->open || !tab->client) continue;
        const auto browser = tab->client->GetBrowser();
        if (browser && browser->GetIdentifier() == camera.browser_id) { source = tab; break; }
    }
    if (!source) return {"WINDOW_CLOSED", "Input window has closed"};
    const auto record = source->camera_view ? CameraViewportManager::instance().find_by_tab(source->tab_id) : std::nullopt;
    if ((source->camera_view && (!record || record->camera_handle != camera.camera_handle)) ||
        (!source->camera_view && (!source->main_workspace || source->web_camera_handle != camera.camera_handle)))
        return {"PERMISSION_DENIED", "Input must belong to this window's viewport"};
    const auto authority = camera.camera_authority;
    if (!authority) return {"PROJECT_CONTEXT_CHANGED", "Input archive is no longer active"};
    std::lock_guard lease(authority->mutex);
    if (!authority->active || !context_active()) return {"PROJECT_CONTEXT_CHANGED", "Input context is no longer active"};
    enqueue_input_event(std::move(request.event), [authority, context_active = std::move(context_active)] {
        std::lock_guard lease(authority->mutex);
        return authority->active && context_active();
    });
    return {};
}
} // namespace Corona::Systems::UI
