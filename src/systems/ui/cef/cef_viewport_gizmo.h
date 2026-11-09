#pragma once

#include <corona/kernel/entity_id.h>
#include <array>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include "camera_window_lifetime.h"

namespace Corona::Systems::UI {

struct ViewportGizmoTargetRequest {
    std::string scene_id;
    std::string camera_id;
    std::string archive_session;
    std::optional<std::string> actor_guid;
    EntityId camera_handle = 0;
    EntityId actor_handle = 0;
    std::string error_code, error_message;
    std::shared_ptr<CameraWindowAuthority> camera_authority;
};

struct ViewportGizmoPointerRequest : ViewportGizmoTargetRequest {
    std::string request_id;
    std::string event;
    double x = 0, y = 0, width = 0, height = 0;
    int button = 0, buttons = 0, modifiers = 0;
};

struct ViewportGizmoReply {
    std::string axis = "none";
    bool consumed = false, dragging = false, ended = false, cancelled = false;
    std::array<float, 3> position{};
    std::string error_code, error_message;
};

ViewportGizmoReply execute_viewport_gizmo_target(const ViewportGizmoTargetRequest& request);
ViewportGizmoReply execute_viewport_gizmo_pointer(const ViewportGizmoPointerRequest& request);

}  // namespace Corona::Systems::UI
