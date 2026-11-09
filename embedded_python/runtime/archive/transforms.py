"""Renderer-independent affine math (row-major, column vectors, no numpy)."""
from __future__ import annotations

import math


def identity():
    return [float(r == c) for r in range(4) for c in range(4)]


def multiply(a, b):
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4))
            for r in range(4) for c in range(4)]


def transpose(m):
    return [m[c * 4 + r] for r in range(4) for c in range(4)]


def affine(m):
    if len(m) != 16 or not all(math.isfinite(x) for x in m):
        raise ValueError("Expected 16 finite matrix components")
    if any(abs(m[12 + i] - v) > 1e-6 for i, v in enumerate((0, 0, 0, 1))):
        raise ValueError("Projective geometry transforms are unsupported")
    return list(m)


def translate(v):
    m = identity()
    m[3], m[7], m[11] = v
    return affine(m)


def scale(v):
    m = identity()
    m[0], m[5], m[10] = v
    return affine(m)


def normalize(v):
    n = math.sqrt(sum(x * x for x in v))
    if not math.isfinite(n) or n < 1e-15:
        raise ValueError("Expected a nonzero finite vector")
    return [x / n for x in v]


def cross(a, b):
    return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]


def rotate(angle, axis):
    x, y, z = normalize(axis)
    c, s = math.cos(math.radians(angle)), math.sin(math.radians(angle))
    t = 1 - c
    return affine([t*x*x+c, t*x*y-s*z, t*x*z+s*y, 0,
                   t*x*y+s*z, t*y*y+c, t*y*z-s*x, 0,
                   t*x*z-s*y, t*y*z+s*x, t*z*z+c, 0, 0, 0, 0, 1])


def look_at(origin, target, up):
    forward = normalize([b-a for a, b in zip(origin, target)])
    right = normalize(cross(up, forward))
    up = cross(forward, right)
    return [right[0], up[0], forward[0], origin[0],
            right[1], up[1], forward[1], origin[1],
            right[2], up[2], forward[2], origin[2], 0, 0, 0, 1]


def inverse(m):
    rows = [list(m[r*4:r*4+4]) + [float(r == c) for c in range(4)] for r in range(4)]
    for c in range(4):
        p = max(range(c, 4), key=lambda r: abs(rows[r][c]))
        if abs(rows[p][c]) < 1e-15:
            raise ValueError("Singular transform")
        rows[c], rows[p] = rows[p], rows[c]
        d = rows[c][c]
        rows[c] = [x/d for x in rows[c]]
        for r in range(4):
            if r != c:
                d = rows[r][c]
                rows[r] = [a-d*b for a, b in zip(rows[r], rows[c])]
    return affine([x for row in rows for x in row[4:]])
