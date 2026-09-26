"""Narrow Scratch O/P adapter. Scene opening remains owned by the frontend launcher."""
from __future__ import annotations

from pathlib import Path
import threading

from .subworlds import StorySubworlds, SubworldError, story_scene

_preparing = threading.Lock()


def _unwrap(result):
    for _ in range(3):
        if isinstance(result, dict) and 'data' in result:
            result = result['data']
        else:
            break
    return result


def _success(result, operation):
    data = _unwrap(result)
    if not isinstance(data, dict) or data.get('ok') is not True:
        details = data.get('message') if isinstance(data, dict) else None
        if isinstance(data, dict) and not details:
            details = '；'.join(str(item.get('message', item)) if isinstance(item, dict) else str(item)
                               for item in data.get('diagnostics', []))
        raise SubworldError(f'{operation}失败：{details or "引擎未确认成功"}')
    return data


def handle_story_key(key: str, modifiers=(), *, api=None) -> dict | None:
    """None means ordinary Scratch behavior; never interpret native mirrored SDL input."""
    normalized = {'KeyO': 'O', 'KeyP': 'P', 'o': 'O', 'p': 'P', 'O': 'O', 'P': 'P'}.get(key)
    if not normalized or any(str(mod).lower() in ('ctrl', 'control', 'alt', 'meta', 'cmd', 'super') for mod in modifiers):
        return None
    if api is None:
        from api.editor_api import CoronaEditorApi
        api = CoronaEditorApi
    if not _preparing.acquire(blocking=False):
        return {'status': 'error', 'message': '正在准备世界切换，请稍后再试'}
    try:
        info = _unwrap(api.project_settings.get_active_project_info())
        if not isinstance(info, dict) or not info.get('project_path'):
            raise SubworldError('无法确定当前世界，已取消切换')
        if info.get('mode') != 'story':
            return None
        root = Path(info['project_path']).resolve(strict=True)
        if not story_scene(root):
            return None

        def assert_source():
            active = _unwrap(api.project_settings.get_active_project_info())
            if (not isinstance(active, dict) or active.get('mode') != 'story'
                    or Path(active.get('project_path', '')).resolve() != root):
                raise SubworldError('当前世界已改变，已取消旧世界切换')

        def save(source):
            assert_source()
            init = _unwrap(api.main.on_init())
            if not isinstance(init, dict):
                raise SubworldError('无法确定当前场景，已取消保存')
            scenes = init.get('scenes', [])
            index = init.get('active_index', 0)
            route = scenes[index].get('path') if isinstance(index, int) and 0 <= index < len(scenes) else init.get('path')
            if not isinstance(route, str) or not route or (source / route).resolve() != source / 'scene.ini':
                raise SubworldError('当前场景与剧情世界不匹配，已取消保存')
            assert_source()
            _success(api.main.scene_save(route), '保存当前世界')

        def validate(path):
            data = _success(api.project.validate_portable_scene({'path': str(path), 'verifyHashes': True}), '校验世界及资源')
            if data.get('status') != 'portable_v1':
                raise SubworldError('仅支持可移植剧情世界，不能切换旧格式场景')

        result = StorySubworlds(save=save, validate=validate).prepare(root, normalized)
        assert_source()
        return result
    except Exception as error:
        return {'status': 'error', 'message': f'剧情世界切换准备失败：{error}'}
    finally:
        _preparing.release()
