"""World rules installed into a small world by the prophet.

Mirrors placements.py on purpose: the file lives beside the small world, so a world
carries its own rules, and a corrupt file is an error rather than a silent reset.
Pure filesystem logic: no engine imports, no scene access.

A rule is the installed form of a fragment from game/data/fragments.json. The catalogue
stays read-only content; only what the player installed belongs here.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import tempfile

SAVE_PATH = Path('.game/story-world-rules.json')
MAX_BYTES = 65536
MAX_RULES = 16
_TYPES = ('sway',)
_AXES = ('x', 'y', 'z')
_AMPLITUDE_LIMIT = 100.0
_PERIOD_LIMIT_MS = 600_000


class WorldRuleError(RuntimeError):
    """Invalid data must never overwrite a good rule set."""


def _positive(value: object, *, limit: float) -> bool:
    # bool is an int subclass and would silently pass a bare number check.
    return type(value) in (int, float) and 0 < value <= limit


def validate_rule(entry: object) -> dict:
    if not isinstance(entry, dict):
        raise WorldRuleError('世界规则必须是对象')
    fragment_id = entry.get('fragmentId')
    if not isinstance(fragment_id, str) or not fragment_id or len(fragment_id) > 64:
        raise WorldRuleError('碎片标识无效')
    if entry.get('type') not in _TYPES:
        raise WorldRuleError('未知的世界规则类型')
    if entry.get('axis') not in _AXES:
        raise WorldRuleError('未知的浮动轴')
    if not _positive(entry.get('amplitude'), limit=_AMPLITUDE_LIMIT):
        raise WorldRuleError('世界规则幅度无效')
    if not _positive(entry.get('periodMs'), limit=_PERIOD_LIMIT_MS):
        raise WorldRuleError('世界规则周期无效')
    return {'fragmentId': fragment_id, 'type': entry['type'], 'axis': entry['axis'],
            'amplitude': entry['amplitude'], 'periodMs': entry['periodMs']}


def validate_state(state: object) -> dict:
    if not isinstance(state, dict) or state.get('version') != 1:
        raise WorldRuleError('不支持的世界规则存档版本')
    raw = state.get('rules')
    if not isinstance(raw, list) or len(raw) > MAX_RULES:
        raise WorldRuleError('世界规则列表无效')
    rules, seen = [], set()
    for entry in raw:
        rule = validate_rule(entry)
        if rule['fragmentId'] in seen:
            raise WorldRuleError('同一碎片不能重复装入')
        seen.add(rule['fragmentId'])
        rules.append(rule)
    return {'version': 1, 'rules': rules}


def initial_state() -> dict:
    return {'version': 1, 'rules': []}


def load(root: Path) -> dict:
    """A missing file is an empty rule set; a corrupt one is an error, never a reset."""
    path = Path(root) / SAVE_PATH
    if not path.exists():
        return initial_state()
    try:
        if path.stat().st_size > MAX_BYTES:
            raise WorldRuleError('世界规则存档超过大小限制')
        return validate_state(json.loads(path.read_text(encoding='utf-8')))
    except (OSError, ValueError, TypeError, KeyError) as error:
        raise WorldRuleError(f'世界规则存档损坏，未覆盖原文件：{error}') from error


def save(root: Path, rules: object) -> dict:
    state = validate_state({'version': 1, 'rules': rules})
    folder = Path(root) / '.game'
    folder.mkdir(exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.story-world-rules-', suffix='.tmp', dir=folder)
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
