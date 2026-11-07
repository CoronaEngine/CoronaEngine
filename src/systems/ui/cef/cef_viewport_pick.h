#pragma once

#include "cef_window_operations.h"
#include <corona/shared_data_hub.h>
#include <chrono>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace Corona::Systems::UI {

enum class ViewportMethod { Pick };

struct ViewportPickPoint {
    bool valid = false, inside = false;
    std::uint32_t x = 0, y = 0;
};
ViewportPickPoint map_viewport_pick_point(double x, double y, double width, double height,
                                          std::uint32_t camera_width, std::uint32_t camera_height);

struct ViewportPickRequest {
    WindowRequest camera{WindowMethod::ConfigureViewport, 0};
    double x = 0, y = 0, width = 0, height = 0;
};
struct ViewportPickReply {
    std::string status, archive_session, scene, camera_id;
    std::string actor_guid, actor_name, actor_type, actor_handle;
    std::optional<std::uint32_t> node_index;
    std::uint32_t x = 0, y = 0;
    std::string error_code, error_message;
};

// Native GPU readback ownership, not a generic transport or a Web result queue.
// Tokens are generated here; Web cannot supply or collide with them.
class ViewportPickRequests {
public:
    using Clock = std::chrono::steady_clock;
    using Completion = std::function<void(const ViewportPickRequest&, const Corona::ActorPickCompletion&)>;
    static constexpr std::chrono::milliseconds timeout{500};
    std::string add(ViewportPickRequest request, Completion completion, Clock::time_point now = Clock::now());
    bool complete(const Corona::ActorPickCompletion& result);
    void expire(Clock::time_point now = Clock::now());
    void cancel_context(std::uint64_t context_id);
    void cancel_browser(int browser_id);
    std::size_t pending_count() const;
private:
    struct Pending { ViewportPickRequest request; Completion completion; Clock::time_point deadline; };
    mutable std::mutex mutex_;
    std::uint64_t sequence_ = 0;
    std::unordered_map<std::string, Pending> pending_;
};

ViewportPickRequests& viewport_pick_requests();
// Main thread: validate the surface/lease and submit owned GPU work.
void begin_viewport_pick(ViewportPickRequest request, ViewportPickRequests::Completion completion);
void drain_viewport_picks();
// CEF UI (scene owner): revalidate identity and resolve authoritative Actor values.
ViewportPickReply finish_viewport_pick(const ViewportPickRequest& request, const Corona::ActorPickCompletion& result);

}  // namespace Corona::Systems::UI
