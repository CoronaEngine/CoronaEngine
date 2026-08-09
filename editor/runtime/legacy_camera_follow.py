"""Compatibility adapter for the legacy per-frame camera-follow behavior."""

import logging

from runtime.legacy_scene_store import legacy_scene_store


logger = logging.getLogger(__name__)


def update_camera_follow(host):
    """Update legacy camera-follow state without making it part of the host API."""
    host._follow_frame_count += 1
    if not host._follow_logged_init:
        host._follow_logged_init = True
        logger.debug("[CAMFOLLOW] _update_camera_follow is being called")
    if not host._camera_follow_actor:
        return
    if host._follow_frame_count % 60 == 0:
        logger.debug(
            "[CAMFOLLOW] actor=%s held_keys=%s offset=%s",
            host._camera_follow_actor,
            host._held_keys,
            host._camera_follow_offset,
        )
    try:
        actor = None
        scene = None
        if host._camera_follow_scene:
            scene = legacy_scene_store.get(host._camera_follow_scene)
            if scene:
                actor = scene.find_actor(host._camera_follow_actor)
        if actor is None:
            actor = legacy_scene_store.find_actor(host._camera_follow_actor)
        if actor is None:
            return
        cam = scene.get_active_camera() if scene else None
        if cam is None:
            for scene_name in legacy_scene_store.list_all():
                candidate_scene = legacy_scene_store.get(scene_name)
                if candidate_scene:
                    cam = candidate_scene.get_active_camera()
                    if cam:
                        break
        if cam is None:
            return
        obj_pos = actor.get_position()
        if host._editor_camera_input_enabled:
            try:
                import ctypes
                key_state = ctypes.windll.user32.GetAsyncKeyState
                w_down = key_state(0x57) & 0x8000
                a_down = key_state(0x41) & 0x8000
                s_down = key_state(0x53) & 0x8000
                d_down = key_state(0x44) & 0x8000
            except Exception:
                w_down = a_down = s_down = d_down = 0
            for key in list(host._held_keys):
                if key == "w":
                    w_down = 0x8000
                elif key == "a":
                    a_down = 0x8000
                elif key == "s":
                    s_down = 0x8000
                elif key == "d":
                    d_down = 0x8000
            if w_down or a_down or s_down or d_down:
                ox, oy, oz = host._camera_follow_offset
                look_dir = host._normalize([-ox, -oy, -oz])
                forward = host._normalize([look_dir[0], 0.0, look_dir[2]])
                right = host._normalize(host._cross([0.0, 1.0, 0.0], forward))
                move = [0.0, 0.0, 0.0]
                step = 0.5
                if w_down:
                    move[0] += forward[0] * step
                    move[2] += forward[2] * step
                if s_down:
                    move[0] -= forward[0] * step
                    move[2] -= forward[2] * step
                if a_down:
                    move[0] -= right[0] * step
                    move[2] -= right[2] * step
                if d_down:
                    move[0] += right[0] * step
                    move[2] += right[2] * step
                obj_pos = [obj_pos[0] + move[0], obj_pos[1], obj_pos[2] + move[2]]
                actor.set_position(obj_pos, if_init=True)
                logger.debug("[CAMFOLLOW] WASD move to %s", obj_pos)
            try:
                rmb_down = ctypes.windll.user32.GetAsyncKeyState(0x02) & 0x8000
            except Exception:
                rmb_down = 0
            if rmb_down:
                current_mouse = None
                try:
                    class POINT(ctypes.Structure):
                        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]
                    point = POINT()
                    ctypes.windll.user32.GetCursorPos(ctypes.byref(point))
                    current_mouse = (point.x, point.y)
                except Exception:
                    pass
                if not host._follow_rmb_down:
                    host._follow_rmb_down = True
                    host._follow_prev_mouse = current_mouse
                elif current_mouse and host._follow_prev_mouse:
                    dx = current_mouse[0] - host._follow_prev_mouse[0]
                    dy = current_mouse[1] - host._follow_prev_mouse[1]
                    host._follow_prev_mouse = current_mouse
                    if dx or dy:
                        ox, oy, oz = host._camera_follow_offset
                        look_dir = host._normalize([-ox, -oy, -oz])
                        forward = host._normalize([look_dir[0], 0.0, look_dir[2]])
                        right = host._normalize(host._cross([0.0, 1.0, 0.0], forward))
                        speed = 0.02
                        move = [right[0] * dx * speed + forward[0] * (-dy) * speed, 0.0, right[2] * dx * speed + forward[2] * (-dy) * speed]
                        obj_pos = [obj_pos[0] + move[0], obj_pos[1], obj_pos[2] + move[2]]
                        actor.set_position(obj_pos, if_init=True)
                        logger.debug("[CAMFOLLOW] RMB move to %s", obj_pos)
            else:
                host._follow_rmb_down = False
                host._follow_prev_mouse = None
        else:
            host._follow_rmb_down = False
            host._follow_prev_mouse = None
        ox, oy, oz = host._camera_follow_offset
        cam.set_position([obj_pos[0] + ox, obj_pos[1] + oy, obj_pos[2] + oz])
        if host._follow_cam_look_at:
            cam.set_forward(host._normalize([-ox, -oy, -oz]))
            cam.set_world_up([0.0, 1.0, 0.0])
    except Exception as error:
        logger.error("[CAMFOLLOW] error: %s", error)
