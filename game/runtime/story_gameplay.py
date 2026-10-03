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
from . import placements
from . import world_rules

REQUEST_KEY = '__corona_story_gameplay_v1__'
SAVE_PATH = Path('.game/story-gameplay.json')
PLACEMENT_ACTIONS = ('loadPlacements', 'savePlacements', 'clearPlacements')
WORLD_RULE_ACTIONS = ('loadWorldRules', 'saveWorldRules', 'clearWorldRules')
DROP_ID = 'story.boss.world-fragment'
CONFIG = {
    'playerHp': 100, 'rageMax': 100, 'ragePerHit': 10, 'bossHp': 200, 'damage': 20,
    'cooldownMs': 400, 'bossBarRadius': 10, 'meleeRange': 2.5,
    'meleeHalfAngle': math.pi / 3, 'pickupRange': 2,
    'skills': {
        'heavy': {'name': '重斩', 'key': 'E', 'damage': 50, 'rageCost': 30,
                  'range': 2.5, 'halfAngle': math.pi / 3, 'cooldownMs': 1200},
        'sweep': {'name': '横扫', 'key': 'R', 'damage': 80, 'rageCost': 50,
                  'range': 3.5, 'halfAngle': math.pi, 'cooldownMs': 3000},
    },
}
_lock = threading.RLock()


def initial_state():
    return {'version': 2, 'revision': 0, 'legacyRevision': 0, 'rage': 0, 'boss': {'hp': CONFIG['bossHp']},
            'drop': None, 'inventory': {'worldFragment': 0}, 'operations': []}


def _vector(value):
    return (isinstance(value, list) and len(value) == 3
            and all(type(n) in (int, float) and math.isfinite(n) and abs(n) < 1e7 for n in value))


def _validate_v1(state):
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


def _operation(request):
    """Canonical durable command; clients never choose damage or resource costs."""
    action = request.get('action')
    if action not in ('hitBoss', 'castSkill', 'pickupDrop'):
        raise ValueError('未知玩法操作')
    ident, revision = request.get('operationId'), request.get('expectedRevision')
    if not isinstance(ident, str) or str(uuid.UUID(ident)) != ident:
        raise ValueError('无效的玩法操作 ID')
    if type(revision) is not int or revision < 0:
        raise ValueError('无效的玩法存档版本号')
    operation = {'operationId': ident, 'action': action, 'expectedRevision': revision}
    if action == 'castSkill':
        if request.get('skillId') not in CONFIG['skills'] or type(request.get('hit')) is not bool:
            raise ValueError('无效的技能或命中结果')
        operation.update(skillId=request['skillId'], hit=request['hit'])
    if action == 'hitBoss' or (action == 'castSkill' and operation['hit']):
        if not _vector(request.get('bossPosition')):
            raise ValueError('Boss 掉落位置无效')
        operation['bossPosition'] = request['bossPosition']
    if action == 'pickupDrop':
        if request.get('dropId') != DROP_ID:
            raise ValueError('未知的世界碎片')
        operation['dropId'] = DROP_ID
    return operation


def _apply(state, operation, *, legacy=False):
    action, damage = operation['action'], 0
    if action == 'hitBoss':
        if state['boss']['hp'] <= 0:
            raise ValueError('Boss 已死亡')
        damage = CONFIG['damage']
        if not legacy:
            state['rage'] = min(CONFIG['rageMax'], state['rage'] + CONFIG['ragePerHit'])
    elif action == 'castSkill':
        if legacy:
            raise ValueError('旧版操作记录不能包含技能')
        skill = CONFIG['skills'][operation['skillId']]
        if state['rage'] < skill['rageCost']:
            raise ValueError('怒气不足')
        if operation['hit'] and state['boss']['hp'] <= 0:
            raise ValueError('Boss 已死亡，不能再次命中')
        state['rage'] -= skill['rageCost']
        damage = skill['damage'] if operation['hit'] else 0
    else:
        if state['drop'] is None or state['drop']['collected']:
            raise ValueError('世界碎片不可拾取')
        state['drop']['collected'] = True
        state['inventory']['worldFragment'] += 1
    if damage:
        state['boss']['hp'] = max(0, state['boss']['hp'] - damage)
        if state['boss']['hp'] == 0:
            state['drop'] = {'id': DROP_ID, 'position': operation['bossPosition'], 'collected': False}


def _validate(state):
    if not isinstance(state, dict) or type(state.get('version')) is not int:
        raise ValueError('不支持的玩法存档版本')
    if state['version'] == 1:
        _validate_v1(state)
        state = deepcopy(state)
        state.update(version=2, legacyRevision=state['revision'], rage=0)
    if state['version'] != 2:
        raise ValueError('不支持的玩法存档版本')
    revision, boundary, operations = state['revision'], state['legacyRevision'], state['operations']
    if (type(revision) is not int or revision < 0 or type(boundary) is not int
            or not 0 <= boundary <= min(revision, 11) or not isinstance(operations, list)
            or len(operations) != revision or type(state['rage']) is not int
            or type(state['boss']['hp']) is not int or type(state['inventory']['worldFragment']) is not int):
        raise ValueError('玩法状态或存档版本号无效')
    replay, seen = initial_state(), set()
    for i, operation in enumerate(operations):
        if not isinstance(operation, dict) or _operation(operation) != operation:
            raise ValueError('玩法操作记录无效')
        ident = operation['operationId']
        if ident in seen or operation['expectedRevision'] != i:
            raise ValueError('玩法操作记录无效')
        seen.add(ident)
        _apply(replay, operation, legacy=i < boundary)
    for key in ('boss', 'drop', 'inventory', 'rage'):
        if replay[key] != state[key]:
            raise ValueError('玩法状态与操作记录不一致')
    drop = state['drop']
    if drop is not None and (type(drop.get('collected')) is not bool or not _vector(drop.get('position'))):
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


def handle_placement_request(request, source, api):
    """Exhibits belong to the small world, so the main world reads an empty layout.

    A small world keeps its own arrangement, which is why the file lives beside the
    copied scene instead of in the main world.
    """
    action = request['action']

    def assert_source():
        active = _unwrap(api.project_settings.get_active_project_info())
        if (not isinstance(active, dict) or active.get('mode') != 'story'
                or not active.get('project_path')
                or Path(active['project_path']).resolve() != source):
            raise ValueError('当前世界已改变，已拒绝旧世界陈列请求')

    with _lock:
        assert_source()
        _require_story(source)
        role, target, _ = _relation(source)
        # The link owner keeps the metadata for both worlds, and its `.game` folder is
        # the only one the navigation lock guards. Storing a small world's layout
        # beside `story-link.json` therefore keeps reads and writes mutually exclusive
        # with the copy/restore transaction that owns that same folder.
        owner = target if role == 'child' else source

        def response(state, status='ok'):
            return {'status': status, 'role': role, 'state': state}

        if role != 'child':
            if action != 'loadPlacements':
                raise ValueError('只有小世界可以陈列主世界取得的物质')
            return response(placements.initial_state())
        with _exclusive(owner):
            assert_source()
            if action == 'loadPlacements':
                return response(placements.load(owner))
            if action == 'clearPlacements':
                return response(placements.clear(owner))
            listed = request.get('placements')
            if not isinstance(listed, list):
                raise ValueError('陈列列表无效')
            return response(placements.save(owner, listed))


def handle_world_rule_request(request, source, api):
    """World rules belong to the small world whose prophet installed them.

    Ownership mirrors placements: the rule set is written beside the link owner's
    `story-link.json`, so writes stay mutually exclusive with the copy/restore
    transaction that owns that folder, and the main world only ever reads an empty set.
    """
    action = request['action']

    def assert_source():
        active = _unwrap(api.project_settings.get_active_project_info())
        if (not isinstance(active, dict) or active.get('mode') != 'story'
                or not active.get('project_path')
                or Path(active['project_path']).resolve() != source):
            raise ValueError('当前世界已改变，已拒绝旧世界世界规则请求')

    with _lock:
        assert_source()
        _require_story(source)
        role, target, _ = _relation(source)
        owner = target if role == 'child' else source

        def response(state, status='ok'):
            return {'status': status, 'role': role, 'state': state}

        if role != 'child':
            if action != 'loadWorldRules':
                raise ValueError('只有小世界可以装载碎片规则')
            return response(world_rules.initial_state())
        with _exclusive(owner):
            assert_source()
            if action == 'loadWorldRules':
                return response(world_rules.load(owner))
            if action == 'clearWorldRules':
                return response(world_rules.clear(owner))
            listed = request.get('rules')
            if not isinstance(listed, list):
                raise ValueError('世界规则列表无效')
            return response(world_rules.save(owner, listed))


def handle_gameplay_request(payload: str, *, api=None):
    """Only bounded domain actions; no caller-selected write path or arbitrary code."""
    try:
        if not isinstance(payload, str) or len(payload) > 65536:
            raise ValueError('玩法请求格式无效')
        request = json.loads(payload)
        known = ('load', 'hitBoss', 'castSkill', 'pickupDrop', *PLACEMENT_ACTIONS, *WORLD_RULE_ACTIONS)
        if not isinstance(request, dict) or request.get('action') not in known:
            raise ValueError('未知玩法操作')
        if not isinstance(request.get('projectPath'), str) or not request['projectPath'].strip():
            raise ValueError('缺少来源世界')
        if api is None:
            from api.editor_api import CoronaEditorApi
            api = CoronaEditorApi
        source = Path(request['projectPath']).absolute()
        _plain(source)
        source = source.resolve(strict=True)
        if request['action'] in PLACEMENT_ACTIONS:
            return handle_placement_request(request, source, api)
        if request['action'] in WORLD_RULE_ACTIONS:
            return handle_world_rule_request(request, source, api)

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
                operation = _operation(request)
                operation_id = operation['operationId']
                expected = operation['expectedRevision']
                prior = next((item for item in state['operations'] if item['operationId'] == operation_id), None)
                if prior is not None:
                    if prior != operation:
                        raise ValueError('同一操作 ID 不能用于不同请求')
                    return response()  # A committed request whose reply was lost.
                if expected != state['revision']:
                    return response('error', code='REVISION_CONFLICT', message='玩法进度已更新，已重新同步，请重试')
                next_state = deepcopy(state)
                _apply(next_state, operation)
                next_state['revision'] += 1
                next_state['operations'].append(operation)
                _validate(next_state)
                _write(owner, next_state, assert_source)
                state = next_state
                return response()
    except Exception as error:
        return {'status': 'error', 'message': f'玩法存档操作失败：{error}'}
