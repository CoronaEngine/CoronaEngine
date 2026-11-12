#include "cef_viewport_pick.h"
#include "cef_client.h"
#include "viewport_image_input.h"
#include <corona/systems/ui/camera_viewport_manager.h>
#include <algorithm>
#include <cmath>

namespace Corona::Systems::UI {

ViewportPickPoint map_viewport_pick_point(double x, double y, double width, double height,
                                          const Corona::CameraDevice& camera) {
    ViewportPickPoint point;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0 || height <= 0 || !camera.width || !camera.height) return point;
    point.valid = true;
    const auto image = InputDetail::map_viewport_image_point(camera, x, y, width, height);
    if (!image) return point;
    point.inside = true;
    point.x = static_cast<std::uint32_t>(std::min(image->x, double(camera.width - 1)));
    point.y = static_cast<std::uint32_t>(std::min(image->y, double(camera.height - 1)));
    return point;
}

std::string ViewportPickRequests::add(ViewportPickRequest request, Completion completion, Clock::time_point now) {
    std::lock_guard lock(mutex_);
    const auto id = "v8-pick-" + std::to_string(++sequence_);
    pending_.emplace(id, Pending{std::move(request), std::move(completion), now + timeout});
    return id;
}

bool ViewportPickRequests::complete(const Corona::ActorPickCompletion& result) {
    std::optional<Pending> pending;
    {
        std::lock_guard lock(mutex_);
        const auto found = pending_.find(result.request_id);
        if (found == pending_.end() || found->second.request.camera.camera_handle != result.camera_handle ||
            found->second.request.camera.scene_id != result.scene_id) return false;
        pending.emplace(std::move(found->second));
        pending_.erase(found);
    }
    if (pending->completion) pending->completion(pending->request, result);
    return true;
}

void ViewportPickRequests::expire(Clock::time_point now) {
    std::vector<std::pair<std::string, Pending>> expired;
    {
        std::lock_guard lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.deadline > now) { ++it; continue; }
            expired.emplace_back(it->first, std::move(it->second));
            it = pending_.erase(it);
        }
    }
    for (const auto& [id, item] : expired) if (item.completion)
        item.completion(item.request, {item.request.camera.camera_handle, item.request.camera.scene_id, id, "timeout"});
}

void ViewportPickRequests::cancel_context(std::uint64_t context_id) {
    std::lock_guard lock(mutex_);
    std::erase_if(pending_, [&](const auto& item) { return item.second.request.camera.context_id == context_id; });
}
void ViewportPickRequests::cancel_browser(int browser_id) {
    std::lock_guard lock(mutex_);
    std::erase_if(pending_, [&](const auto& item) { return item.second.request.camera.browser_id == browser_id; });
}
std::size_t ViewportPickRequests::pending_count() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}
ViewportPickRequests& viewport_pick_requests() {
    static ViewportPickRequests requests;
    return requests;
}

void begin_viewport_pick(ViewportPickRequest request, ViewportPickRequests::Completion completion) {
    const auto& camera = request.camera;
    const auto finish = [&](const char* status) {
        if (completion) completion(request, {camera.camera_handle, camera.scene_id, {}, status});
    };
    BrowserTab* source = nullptr;
    for (const auto& entry : BrowserManager::instance().tabs_snapshot()) {
        auto* tab = entry.tab;
        if (!tab || !tab->open || !tab->client) continue;
        const auto browser = tab->client->GetBrowser();
        if (browser && browser->GetIdentifier() == camera.browser_id) { source = tab; break; }
    }
    if (!source) { finish("window_closed"); return; }
    const auto record = source->camera_view ? CameraViewportManager::instance().find_by_tab(source->tab_id) : std::nullopt;
    if ((source->camera_view && (!record || record->camera_handle != camera.camera_handle)) ||
        (!source->camera_view && (!source->main_workspace || source->web_camera_handle != camera.camera_handle))) {
        finish("permission_denied"); return;
    }
    const auto authority = camera.camera_authority;
    std::unique_lock<std::mutex> lease;
    if (authority) lease = std::unique_lock(authority->mutex);
    if (!authority || !authority->active) { finish("stale_scene"); return; }
    auto& hub = Corona::SharedDataHub::instance();
    auto native_camera = hub.camera_storage().try_acquire_read(camera.camera_handle);
    if (!native_camera) { finish("camera_unavailable"); return; }
    const auto point = map_viewport_pick_point(request.x, request.y, request.width, request.height, *native_camera);
    if (!point.valid) { finish("invalid_argument"); return; }
    if (!point.inside) { finish("miss"); return; }
    if (!native_camera->actor_pick_handle || !hub.actor_pick_storage().try_acquire_read(native_camera->actor_pick_handle)) {
        finish("pick_unavailable"); return;
    }
    const auto camera_handle = camera.camera_handle;
    const auto scene_id = camera.scene_id;
    const auto id = viewport_pick_requests().add(std::move(request), std::move(completion));
    hub.enqueue_actor_pick_request({camera_handle, scene_id, id, point.x, point.y});
}

void drain_viewport_picks() {
    auto& requests = viewport_pick_requests();
    for (const auto& result : Corona::SharedDataHub::instance().drain_actor_pick_completions()) requests.complete(result);
    requests.expire();
}

}  // namespace Corona::Systems::UI
