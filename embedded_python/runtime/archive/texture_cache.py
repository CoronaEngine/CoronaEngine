"""File-backed derived textures, without changing or copying source assets."""
from __future__ import annotations

import hashlib
import math
import os
from pathlib import Path
import struct
import tempfile

from .appearance import image, rgb


def checker_texture(a, b, uv, *, frequency=2, directory=None, warn=None):
    """Bake constant 2D checker colors; keep the source mapping in image UVs.

    The tile has two cells per axis. Mitsuba has two cells per UV unit,
    whereas PBRT has one. PFM stores linear float colors without an sRGB
    round trip, quantization to 8 bits, or clamping HDR values.
    """
    if isinstance(a, dict) or isinstance(b, dict):
        # A spatial child cannot be baked into a repeating constant-color tile.
        if warn:
            warn("TEXTURE_BAKE_DEFERRED", "Checker with spatial child textures retains its procedural expression")
        return dict(kind="checkerboard", a=a, b=b, uv=list(uv), frequency=frequency)
    if len(uv) != 9 or not all(math.isfinite(x) for x in uv) or not math.isfinite(frequency) or frequency <= 0:
        raise ValueError("Invalid checker mapping")
    colors = [struct.pack("<3f", *rgb(color)) for color in (a, b)]
    size = 128
    # ResourceManager decodes PFM bottom-up rows to its top-down image layout.
    rows = [colors[0] * (size // 2) + colors[1] * (size // 2),
            colors[1] * (size // 2) + colors[0] * (size // 2)]
    data = f"PF\n{size} {size}\n-1.0\n".encode("ascii") + rows[1] * (size // 2) + rows[0] * (size // 2)
    root = (Path(directory) / "textures" if directory is not None else
            Path(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir()) / "CoronaEngine" / "cache" / "scene_textures_v1")
    root.mkdir(parents=True, exist_ok=True)
    target = root / ("checker-" + hashlib.sha256(data).hexdigest() + ".pfm")
    if not target.is_file():
        fd, name = tempfile.mkstemp(prefix="checker-", suffix=".tmp", dir=root)
        temporary = Path(name)
        try:
            with os.fdopen(fd, "wb") as stream:
                stream.write(data)
            os.replace(temporary, target)
        finally:
            temporary.unlink(missing_ok=True)
    mapping = [value * frequency / 2 if i < 6 else value for i, value in enumerate(uv)]
    return image(target, encoding="linear", uv=mapping, wrap="repeat", filter="nearest")
