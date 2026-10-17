"""Exhibits a small world displays, stored beside that world.

Pure filesystem logic: no engine imports, no scene access, mirroring the atomic
write used by the gameplay save so a crash can never leave a half-written layout.
Placements live inside the small world folder, so a world travels with its own
arrangement and the main world never shows another world's exhibits.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import tempfile

SAVE_PATH = Path('.game/story-placements.json')
MAX_BYTES = 65536
MAX_PLACEMENTS = 64
_COORD_LIMIT = 1e6


class PlacementError(RuntimeError):
    """Invalid data must never overwrite a good layout."""


def _finite(value: object) -> bool:
    # bool is an int subclass and would silently pass the float check.
    return type(value) in (int, float) and -_COORD_LIMIT <= value <= _COORD_LIMIT


def _vector(value: object) -> bool:
    return isinstance(value, list) and len(value) == 3 and all(_finite(item) for item in value)


def _positive(value: object) -> bool:
    return type(value) in (int, float) and 0 < value <= _COORD_LIMIT


def validate_placement(entry: object) -> dict:
    if not isinstance(entry, dict):
        raise PlacementError('陈列记录必须是对象')
    prop_id, index = entry.get('propId'), entry.get('index')
    site_id = entry.get('siteId')
    if not isinstance(prop_id, str) or not prop_id or len(prop_id) > 64:
        raise PlacementError('陈列物标识无效')
    if type(index) is not int or not 0 <= index < MAX_PLACEMENTS:
        raise PlacementError('陈列序号无效')
    if site_id is not None and (not isinstance(site_id, str) or len(site_id) > 64):
        raise PlacementError('陈列点标识无效')
    if not _vector(entry.get('position')) or not _vector(entry.get('rotation')):
        raise PlacementError('陈列位置或朝向无效')
    if not _positive(entry.get('scale')):
        raise PlacementError('陈列缩放无效')
    return {'propId': prop_id, 'index': index, 'siteId': site_id,
            'position': list(entry['position']), 'rotation': list(entry['rotation']),
            'scale': entry['scale']}


def validate_state(state: object) -> dict:
    if not isinstance(state, dict) or state.get('version') != 1:
        raise PlacementError('不支持的陈列存档版本')
    raw = state.get('placements')
    if not isinstance(raw, list) or len(raw) > MAX_PLACEMENTS:
        raise PlacementError('陈列列表无效')
    placements, seen = [], set()
    for entry in raw:
        placement = validate_placement(entry)
        if placement['index'] in seen:
            raise PlacementError('陈列序号重复')
        seen.add(placement['index'])
        placements.append(placement)
    placements.sort(key=lambda item: item['index'])
    return {'version': 1, 'placements': placements}


def initial_state() -> dict:
    return {'version': 1, 'placements': []}


def load(root: Path) -> dict:
    """A missing file is an empty layout; a corrupt one is an error, never a reset."""
    path = Path(root) / SAVE_PATH
    if not path.exists():
        return initial_state()
    try:
        if path.stat().st_size > MAX_BYTES:
            raise PlacementError('陈列存档超过大小限制')
        return validate_state(json.loads(path.read_text(encoding='utf-8')))
    except (OSError, ValueError, TypeError, KeyError) as error:
        raise PlacementError(f'陈列存档损坏，未覆盖原文件：{error}') from error


def save(root: Path, placements: object) -> dict:
    state = validate_state({'version': 1, 'placements': placements})
    folder = Path(root) / '.game'
    folder.mkdir(exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.story-placements-', suffix='.tmp', dir=folder)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as stream:
            json.dump(state, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, Path(root) / SAVE_PATH)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return state


def clear(root: Path) -> dict:
    return save(root, [])
