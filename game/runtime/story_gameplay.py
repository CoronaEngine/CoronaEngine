"""Small, durable story combat adapter over the existing Scratch string bridge."""
from __future__ import annotations

from copy import deepcopy
import json
import math
import os
from pathlib import Path
import tempfile
import threading
import uuid

from .subworlds import _exclusive, _plain, _relation, _require_story
from .story_navigation import _unwrap

REQUEST_KEY = '__corona_story_gameplay_v1__'
SAVE_PATH = Path('.game/story-gameplay.json')
DROP_ID = 'story.boss.world-fragment'
CONFIG = {
    'playerHp': 100, 'playerMp': 100, 'bossHp': 200, 'damage': 20,
    'cooldownMs': 400, 'bossBarRadius': 10, 'meleeRange': 2.5,
    'meleeHalfAngle': math.pi / 3, 'pickupRange': 2,
}
_lock = threading.RLock()


def initial_state():
    return {'version': 1, 'revision': 0, 'boss': {'hp': CONFIG['bossHp']},
            'drop': None, 'inventory': {'worldFragment': 0}, 'operations': []}


def _vector(value):
    return (isinstance(value, list) and len(value) == 3
            and all(type(n) in (int, float) and math.isfinite(n) and abs(n) < 1e7 for n in value))


def _validate(state):
    """Reject broken saves instead of resetting a previously earned reward."""
    if not isinstance(state, dict) or state.get('version') != 1:
        raise ValueError('不支持的玩法存档版本')
    hp = state['boss']['hp']
    count = state['inventory']['worldFragment']
    revision = state['revision']
    operations = state['operations']
    if (type(hp) is not int or not 0 <= hp <= CONFIG['bossHp'] or hp % CONFIG['damage']
            or type(count) is not int or count not in (0, 1)
            or type(revision) is not int or revision != (CONFIG['bossHp'] - hp) // CONFIG['damage'] + count
            or not isinstance(operations, list) or len(operations) != revision):
        raise ValueError('玩法状态或存档版本号无效')
    seen = set()
    for i, operation in enumerate(operations):
        if (not isinstance(operation, dict) or operation.get('expectedRevision') != i
                or operation.get('action') != ('hitBoss' if i < CONFIG['bossHp'] // CONFIG['damage'] else 'pickupDrop')):
            raise ValueError('玩法操作记录无效')
        ident = operation['operationId']
        if str(uuid.UUID(ident)) != ident or ident in seen:
            raise ValueError('玩法操作标识无效')
        seen.add(ident)
    drop = state['drop']
    if hp > 0:
        if drop is not None or count:
            raise ValueError('Boss 存活时不能存在奖励')
    elif (not isinstance(drop, dict) or drop.get('id') != DROP_ID
          or not _vector(drop.get('position')) or type(drop.get('collected')) is not bool
          or drop['collected'] != bool(count)):
        raise ValueError('世界碎片状态无效')
    return state


def _read(root):
    path = root / SAVE_PATH
    _plain(path)
    if not path.exists():
        return initial_state()
    try:
        if path.stat().st_size > 65536:
            raise ValueError('存档超过大小限制')
        return _validate(json.loads(path.read_text(encoding='utf-8')))
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
        raise ValueError(f'玩法存档损坏，未覆盖原文件：{error}') from error


def _write(root, state, assert_source):
    folder = root / '.game'
    fd, temporary = tempfile.mkstemp(prefix='.story-gameplay-', suffix='.tmp', dir=folder)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as stream:
            json.dump(state, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        assert_source()
        _plain(root / SAVE_PATH)
        os.replace(temporary, root / SAVE_PATH)
    finally:
        Path(temporary).unlink(missing_ok=True)


def handle_gameplay_request(payload: str, *, api=None):
    """Only three domain actions; no caller-selected write path or arbitrary code."""
    try:
        if not isinstance(payload, str) or len(payload) > 8192:
            raise ValueError('玩法请求格式无效')
        request = json.loads(payload)
        if not isinstance(request, dict) or request.get('action') not in ('load', 'hitBoss', 'pickupDrop'):
            raise ValueError('未知玩法操作')
        if not isinstance(request.get('projectPath'), str) or not request['projectPath'].strip():
            raise ValueError('缺少来源世界')
        if api is None:
            from api.editor_api import CoronaEditorApi
            api = CoronaEditorApi
        source = Path(request['projectPath']).absolute()
        _plain(source)
        source = source.resolve(strict=True)

        def assert_source():
            active = _unwrap(api.project_settings.get_active_project_info())
            if (not isinstance(active, dict) or active.get('mode') != 'story'
                    or not active.get('project_path')
                    or Path(active['project_path']).resolve() != source):
                raise ValueError('当前世界已改变，已拒绝旧世界玩法请求')

        with _lock:
            assert_source()
            _require_story(source)
            role, target, _ = _relation(source)
            owner = target if role == 'child' else source
            with _exclusive(owner):
                assert_source()
                state = _read(owner)

                def response(status='ok', **extra):
                    return {'status': status, 'role': role, 'state': state, 'config': dict(CONFIG), **extra}

                action = request['action']
                if action == 'load':
                    return response()
                if role != 'main':
                    raise ValueError('子世界没有 Boss 或战斗掉落')
                operation_id = request.get('operationId')
                if not isinstance(operation_id, str) or str(uuid.UUID(operation_id)) != operation_id:
                    raise ValueError('无效的玩法操作 ID')
                expected = request.get('expectedRevision')
                if type(expected) is not int or expected < 0:
                    raise ValueError('无效的玩法存档版本号')
                operation = {'operationId': operation_id, 'action': action, 'expectedRevision': expected}
                if action == 'hitBoss':
                    if not _vector(request.get('bossPosition')):
                        raise ValueError('Boss 掉落位置无效')
                    operation['bossPosition'] = request['bossPosition']
                else:
                    if request.get('dropId') != DROP_ID:
                        raise ValueError('未知的世界碎片')
                    operation['dropId'] = DROP_ID
                prior = next((item for item in state['operations'] if item['operationId'] == operation_id), None)
                if prior is not None:
                    if prior != operation:
                        raise ValueError('同一操作 ID 不能用于不同请求')
                    return response()  # A committed request whose reply was lost.
                if expected != state['revision']:
                    return response('error', code='REVISION_CONFLICT', message='玩法进度已更新，已重新同步，请重试')
                next_state = deepcopy(state)
                if action == 'hitBoss':
                    if state['boss']['hp'] <= 0:
                        raise ValueError('Boss 已死亡')
                    next_state['boss']['hp'] = max(0, state['boss']['hp'] - CONFIG['damage'])
                    if next_state['boss']['hp'] == 0:
                        next_state['drop'] = {'id': DROP_ID, 'position': request['bossPosition'], 'collected': False}
                else:
                    if state['drop'] is None or state['drop']['collected']:
                        raise ValueError('世界碎片不可拾取')
                    next_state['drop']['collected'] = True
                    next_state['inventory']['worldFragment'] += 1
                next_state['revision'] += 1
                next_state['operations'].append(operation)
                _validate(next_state)
                _write(owner, next_state, assert_source)
                state = next_state
                return response()
    except Exception as error:
        return {'status': 'error', 'message': f'玩法存档操作失败：{error}'}
