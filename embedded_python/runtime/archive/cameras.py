"""Normalize source perspective cameras without losing image handedness.

Mitsuba: https://mitsuba.readthedocs.io/en/stable/src/generated/plugins_sensors.html
PBRT: https://pbrt.org/fileformat-v4#cameras
"""
from __future__ import annotations

import math

from . import transforms as tr


def finite(value, name):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"Camera {name} must be finite")
    return result


def resolution(width, height):
    result = []
    for name, value in (("width", width), ("height", height)):
        number = finite(value, name)
        if number <= 0 or number != int(number):
            raise ValueError(f"Camera {name} must be a positive integer")
        result.append(int(number))
    return tuple(result)


def vertical_fov(fov, axis, width, height):
    """Convert film FOV to the vertical degrees expected by Corona/KTM."""
    width, height = resolution(width, height)
    fov = finite(fov, "fov")
    if not 0 < fov < 180:
        raise ValueError("Camera fov must be between 0 and 180 degrees")
    aspect = width / height
    if axis == "smaller":
        axis = "y" if aspect > 1 else "x"
    elif axis == "larger":
        axis = "x" if aspect > 1 else "y"
    tangent = math.tan(math.radians(fov) / 2)
    if axis == "x":
        tangent /= aspect
    elif axis == "diagonal":
        tangent /= math.hypot(aspect, 1)
    elif axis != "y":
        raise ValueError(f"Unsupported camera fov axis: {axis}")
    return math.degrees(2 * math.atan(tangent))


def perspective_camera(to_world, *, image_x_sign, fov, width, height,
                       near_plane, far_plane):
    """Keep reflection in projection, rather than reflecting world geometry.

    Corona rebuilds its LH view right axis as cross(up, forward). Mitsuba's
    local +X points left in the image; PBRT's local +X points right, but PBRT
    input camera matrices may themselves contain a reflection.
    """
    matrix = tr.affine(to_world)
    axes = [[matrix[row * 4 + col] for row in range(3)] for col in range(3)]
    # Exported decimal matrices have small roundoff. Scale/shear must not be
    # discarded by converting the camera to just forward/up.
    tolerance = 1e-4
    for i, axis in enumerate(axes):
        for j, other in enumerate(axes):
            dot = sum(a * b for a, b in zip(axis, other))
            if abs(dot - float(i == j)) > tolerance:
                raise ValueError("Non-orthonormal camera transform (scale/shear) is unsupported")
    x_axis, up, forward = map(tr.normalize, axes)
    image_right = [image_x_sign * x for x in x_axis]
    native_right = tr.normalize(tr.cross(up, forward))
    projection_x_sign = 1 if sum(a * b for a, b in zip(image_right, native_right)) > 0 else -1
    width, height = resolution(width, height)
    fov = vertical_fov(fov, "y", width, height)
    near_plane, far_plane = finite(near_plane, "near_plane"), finite(far_plane, "far_plane")
    if not 0 < near_plane < far_plane:
        raise ValueError("Camera clipping planes must satisfy 0 < near < far")
    return {
        "id": "source-camera", "name": "SourceCamera", "deletable": False,
        "position": [matrix[3], matrix[7], matrix[11]],
        "forward": forward, "world_up": up, "fov": fov,
        "width": width, "height": height,
        "view_open": False, "view_x": 120, "view_y": 120,
        "view_width": 960, "view_height": max(1, round(960 * height / width)),
        "move_speed": 1.0,
        "projection_x_sign": projection_x_sign,
        "near_plane": near_plane, "far_plane": far_plane,
        "render_backend": "native", "output_mode": "final_color",
        "vision_render_mode": "path_tracing", "vision_denoise": True,
        "vision_denoiser": "reblur", "vision_accumulation": False,
    }
