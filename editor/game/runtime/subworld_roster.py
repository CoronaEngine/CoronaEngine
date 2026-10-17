"""Small-world roster: the named small worlds a player owns.

Pure filesystem logic, mirroring the placement save: no engine imports, no scene
access, and an atomic write so a crash can never leave a half-written roster. The
roster is owned by the world that keeps the navigation metadata (the link owner),
so both worlds read the same list and neither can fork it into a second copy.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import tempfile

SAVE_PATH = Path('.game/story-subworld-roster.json')
MAX_BYTES = 65536
MAX_SUBWORLDS = 24
MAX_NAME_LEN = 32
_ID_ALLOWED = frozenset('abcdefghijklmnopqrstuvwxyz0123456789-')
# The first entry is created for free; naming an entry is what unlocks it.
DEFAULT_NAME = '未命名小世界'


class RosterError(RuntimeError):
    """Invalid data must never overwrite a good roster."""


def _valid_id(value: object) -> bool:
    return isinstance(value, str) and 1 <= len(value) <= 40 and all(c in _ID_ALLOWED for c in value)


def _valid_name(value: object) -> bool:
    if not isinstance(value, str):
        return False
    name = value.strip()
    return 1 <= len(name) <= MAX_NAME_LEN


def _clean_name(value: object) -> str:
    if not isinstance(value, str):
        raise RosterError('小世界名字必须是文本')
    name = value.strip()
    if not 1 <= len(name) <= MAX_NAME_LEN:
        raise RosterError('小世界名字长度必须在 1 到 %d 之间' % MAX_NAME_LEN)
    return name


def validate_entry(entry: object) -> dict:
    if not isinstance(entry, dict):
        raise RosterError('名册条目必须是对象')
    ident, name = entry.get('id'), entry.get('name')
    if not _valid_id(ident):
        raise RosterError('小世界标识无效')
    if not _valid_name(name):
        raise RosterError('小世界名字无效')
    return {'id': ident, 'name': _clean_name(name)}


def validate_state(state: object) -> dict:
    if not isinstance(state, dict) or state.get('version') != 1:
        raise RosterError('不支持的名册存档版本')
    raw = state.get('subworlds')
    if not isinstance(raw, list) or len(raw) > MAX_SUBWORLDS:
        raise RosterError('名册列表无效')
    subworlds, seen = [], set()
    for entry in raw:
        subworld = validate_entry(entry)
        if subworld['id'] in seen:
            raise RosterError('名册标识重复')
        seen.add(subworld['id'])
        subworlds.append(subworld)
    return {'version': 1, 'subworlds': subworlds}


def initial_state() -> dict:
    return {'version': 1, 'subworlds': []}


def load(root: Path) -> dict:
    """A missing file is an empty roster; a corrupt one is an error, never a reset."""
    path = Path(root) / SAVE_PATH
    if not path.exists():
        return initial_state()
    try:
        if path.stat().st_size > MAX_BYTES:
            raise RosterError('名册存档超过大小限制')
        return validate_state(json.loads(path.read_text(encoding='utf-8')))
    except (OSError, ValueError, TypeError, KeyError) as error:
        raise RosterError(f'名册存档损坏，未覆盖原文件：{error}') from error


def save(root: Path, subworlds: object) -> dict:
    state = validate_state({'version': 1, 'subworlds': subworlds})
    folder = Path(root) / '.game'
    folder.mkdir(exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.story-roster-', suffix='.tmp', dir=folder)
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


def add(root: Path) -> dict:
    """Add one small world to the roster and persist it; the caller keeps the id."""
    state = load(root)
    if len(state['subworlds']) >= MAX_SUBWORLDS:
        raise RosterError('小世界数量已达上限')
    # A fresh id is derived from the slot count so it stays stable and readable.
    ident = 'subworld-%d' % (len(state['subworlds']) + 1)
    state['subworlds'] = [*(s for s in state['subworlds']), {'id': ident, 'name': DEFAULT_NAME}]
    return save(root, state['subworlds'])


def rename(root: Path, ident: str, name: object) -> dict:
    state = load(root)
    if not _valid_id(ident):
        raise RosterError('小世界标识无效')
    clean = _clean_name(name)
    if any(s['id'] != ident and s['name'] == clean for s in state['subworlds']):
        raise RosterError('已有同名的小世界')
    found = False
    for entry in state['subworlds']:
        if entry['id'] == ident:
            entry['name'] = clean
            found = True
    if not found:
        raise RosterError('小世界不存在')
    return save(root, state['subworlds'])


def clear(root: Path) -> dict:
    return save(root, [])
