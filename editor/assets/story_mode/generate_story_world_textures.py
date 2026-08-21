from __future__ import annotations

import math
import random
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "textures"
OUT.mkdir(parents=True, exist_ok=True)
SIZE = 256


def noise(x: int, y: int, seed: int) -> float:
    value = (x * 374761393 + y * 668265263 + seed * 69069) & 0xFFFFFFFF
    value = ((value ^ (value >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((value ^ (value >> 16)) & 0xFFFFFFFF) / 0xFFFFFFFF


def smooth_noise(x: float, y: float, seed: int) -> float:
    x0, y0 = math.floor(x), math.floor(y)
    tx, ty = x - x0, y - y0
    sx, sy = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    a = noise(x0, y0, seed) * (1 - sx) + noise(x0 + 1, y0, seed) * sx
    b = noise(x0, y0 + 1, seed) * (1 - sx) + noise(x0 + 1, y0 + 1, seed) * sx
    return a * (1 - sy) + b * sy


def fbm(x: float, y: float, seed: int) -> float:
    total = 0.0
    weight = 0.58
    scale = 1.0
    norm = 0.0
    for octave in range(4):
        total += smooth_noise(x * scale, y * scale, seed + octave * 37) * weight
        norm += weight
        weight *= 0.5
        scale *= 2.0
    return total / norm


def create_texture(name, base, shade, height, strength):
    heights = [[height(x, y) for x in range(SIZE)] for y in range(SIZE)]
    diffuse = Image.new("RGBA", (SIZE, SIZE))
    normal = Image.new("RGBA", (SIZE, SIZE))
    diffuse_px = diffuse.load()
    normal_px = normal.load()
    for y in range(SIZE):
        for x in range(SIZE):
            factor = shade(x, y)
            diffuse_px[x, y] = tuple(max(0, min(255, round(channel * factor))) for channel in base) + (255,)
            left = heights[y][(x - 1) % SIZE]
            right = heights[y][(x + 1) % SIZE]
            up = heights[(y - 1) % SIZE][x]
            down = heights[(y + 1) % SIZE][x]
            nx, ny, nz = -(right - left) * strength, -(down - up) * strength, 1.0
            length = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            nx, ny, nz = nx / length, ny / length, nz / length
            normal_px[x, y] = (round((nx * 0.5 + 0.5) * 255), round((ny * 0.5 + 0.5) * 255), round((nz * 0.5 + 0.5) * 255), 255)
    diffuse.save(OUT / f"{name}_diffuse.png", optimize=True)
    normal.save(OUT / f"{name}_normal.png", optimize=True)


create_texture(
    "grass", (91, 126, 57),
    lambda x, y: 0.82 + (fbm(x / 35, y / 35, 11) - 0.5) * 0.36 + (0.18 if noise(x, y, 7) > 0.986 else 0),
    lambda x, y: fbm(x / 23, y / 23, 91) + (0.2 if noise(x, y, 2) > 0.97 else 0), 2.1,
)
create_texture(
    "dirt", (126, 91, 54),
    lambda x, y: 0.82 + (fbm(x / 31, y / 31, 23) - 0.5) * 0.34 + (noise(x // 3, y // 3, 5) - 0.5) * 0.08,
    lambda x, y: fbm(x / 29, y / 29, 44) + noise(x // 3, y // 3, 8) * 0.1, 2.8,
)
create_texture(
    "stone", (118, 122, 116),
    lambda x, y: 0.55 if x % 42 < 3 or (y + (x // 42 % 2) * 18) % 36 < 3 else 0.82 + (fbm(x / 24, y / 24, 31) - 0.5) * 0.32,
    lambda x, y: 0.08 if x % 42 < 3 or (y + (x // 42 % 2) * 18) % 36 < 3 else 0.68 + fbm(x / 18, y / 18, 32) * 0.23, 4.2,
)
create_texture(
    "plaster", (218, 211, 187),
    lambda x, y: 0.91 + (fbm(x / 17, y / 17, 43) - 0.5) * 0.18 - (0.1 if noise(x // 6, y // 6, 17) > 0.97 else 0),
    lambda x, y: fbm(x / 14, y / 14, 53) * 0.35, 1.5,
)
create_texture(
    "wood", (96, 53, 28),
    lambda x, y: 0.72 + (fbm(x / 28, y / 28, 59) - 0.5) * 0.18 + math.sin((x + 8 * math.sin(y / 19)) * 0.16) * 0.08,
    lambda x, y: 0.45 + math.sin((x + 7 * math.sin(y / 23)) * 0.18) * 0.19 + fbm(x / 22, y / 22, 71) * 0.08, 3.2,
)
create_texture(
    "roof_tile", (48, 61, 65),
    lambda x, y: 0.54 if (x + (y // 22 % 2) * 11) % 22 < 2 or y % 22 < 2 else 0.75 + (1 - abs(((x + (y // 22 % 2) * 11) % 22) - 11) / 11) * 0.18 + (fbm(x / 31, y / 31, 73) - 0.5) * 0.15,
    lambda x, y: 0.05 if (x + (y // 22 % 2) * 11) % 22 < 2 or y % 22 < 2 else 0.32 + math.sin((((x + (y // 22 % 2) * 11) % 22) / 22) * math.pi) * 0.55, 4.8,
)
create_texture(
    "rock", (103, 105, 99),
    lambda x, y: 0.72 + (fbm(x / 20, y / 20, 89) - 0.5) * 0.36 + math.sin((x + y) * 0.08) * 0.05,
    lambda x, y: fbm(x / 13, y / 13, 101), 4.5,
)
create_texture(
    "reed", (107, 124, 54),
    lambda x, y: 0.74 + (fbm(x / 27, y / 27, 103) - 0.5) * 0.3 + math.sin(x * 0.31) * 0.08,
    lambda x, y: fbm(x / 25, y / 25, 117) + math.sin(x * 0.3) * 0.14, 2.1,
)

print(f"Generated {len(list(OUT.glob('*.png')))} deterministic Story World texture maps in {OUT}")
