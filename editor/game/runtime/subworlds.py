"""Persistent, independent story subworlds. No engine imports or scene-opening calls."""
from __future__ import annotations

import configparser
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
import uuid


class SubworldError(RuntimeError):
    """A failed preparation must not replace the currently open world."""


METADATA = Path('.game/story-link.json')
CHILD = Path('.game/subworld')


def _exists(path: Path) -> bool:
    return os.path.lexists(path)


def _plain(path: Path) -> None:
    # Junctions can alias another world just as symlinks can on Windows.
    if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
        raise SubworldError(f'世界目录不能使用符号链接或目录联接：{path}')


def story_scene(root: Path) -> bool:
    """The native API's default mode is not proof of an explicit story marker."""
    _plain(root)
    _plain(root / 'scene.ini')
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    try:
        with (root / 'scene.ini').open(encoding='utf-8-sig') as stream:
            parser.read_file(stream)
        sections = {section.lower(): parser[section] for section in parser.sections()}
        return (sections.get('format', {}).get('type') == 'corona_scene_folder'
                and sections.get('format', {}).get('version') == '1'
                and sections.get('world', {}).get('type') == 'story')
    except (OSError, configparser.Error, UnicodeError) as error:
        raise SubworldError(f'无法读取剧情世界配置：{root}；{error}') from error


def _require_story(root: Path) -> None:
    if not story_scene(root):
        raise SubworldError(f'目标不是显式标记的可移植剧情世界：{root}')


def _read_link(root: Path) -> dict | None:
    _plain(root / '.game')
    path = root / METADATA
    _plain(path)
    if not _exists(path):
        return None
    try:
        link = json.loads(path.read_text(encoding='utf-8'))
        if (not isinstance(link, dict) or link.get('version') != 1
                or link.get('role') not in ('main', 'child')
                or not isinstance(link.get('id'), str)
                or str(uuid.UUID(link['id'])) != link['id']
                or link.get('target') != ('.game/subworld' if link['role'] == 'main' else '../..')):
            raise ValueError('关联字段无效')
        return link
    except (OSError, ValueError, TypeError) as error:
        raise SubworldError(f'主子世界关联损坏：{path}；{error}') from error


def _new_link(root: Path, role: str, link_id: str) -> None:
    path = root / METADATA
    path.parent.mkdir(exist_ok=True)
    # Exclusive creation: never replace a pre-existing association.
    stream = path.open('x', encoding='utf-8')
    try:
        with stream:
            json.dump({'version': 1, 'role': role, 'id': link_id,
                       'target': '.game/subworld' if role == 'main' else '../..'}, stream, indent=2)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        # Only a successful exclusive open gives us ownership of this file.
        path.unlink(missing_ok=True)
        raise


def _relation(root: Path) -> tuple[str, Path, bool]:
    """Validate both ends, including on no-op keys. Missing links never recreate children."""
    link = _read_link(root)
    looks_like_child = (os.path.normcase(root.name) == 'subworld'
                        and os.path.normcase(root.parent.name) == '.game')
    if link is None:
        if looks_like_child:
            raise SubworldError('子世界的主世界关联缺失，不能创建嵌套子世界')
        if _exists(root / CHILD):
            raise SubworldError('子世界目录已存在但关联缺失，拒绝覆盖')
        return 'main', root / CHILD, False
    role = link['role']
    if role == 'child' and not looks_like_child:
        raise SubworldError('子世界目录与持久化关联不匹配')
    if role == 'main' and looks_like_child:
        raise SubworldError('不允许嵌套子世界')
    target = root / CHILD if role == 'main' else root.parent.parent
    _plain(target)
    if not target.is_dir():
        raise SubworldError(f'关联的目标世界缺失：{target}')
    target = target.resolve()
    reverse = _read_link(target)
    if (reverse is None or reverse['id'] != link['id']
            or reverse['role'] != ('child' if role == 'main' else 'main')):
        raise SubworldError('主子世界双向关联损坏，拒绝重建或覆盖')
    if (role == 'main' and target.parent.parent != root
            or role == 'child' and (target / CHILD).resolve() != root):
        raise SubworldError('主子世界相对路径不匹配')
    _require_story(target)
    return role, target, True


@contextmanager
def _exclusive(root: Path):
    folder = root / '.game'
    _plain(folder)
    folder.mkdir(exist_ok=True)
    lock = folder / 'story-navigation.lock'
    try:
        fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError as error:
        raise SubworldError(f'世界正在切换，或上次操作异常中断；请检查锁文件：{lock}') from error
    try:
        os.close(fd)
        yield
    finally:
        lock.unlink()


def _copy_scene(source: Path, target: Path) -> None:
    def ignore(directory, names):
        skipped = {name for name in names if os.path.normcase(name) == '.game'} if Path(directory) == source else set()
        for name in names:
            if name not in skipped:
                _plain(Path(directory) / name)
        return skipped
    # copy2 creates independent files (never hardlinks); the root .game is excluded.
    shutil.copytree(source, target, ignore=ignore, copy_function=shutil.copy2)


class StorySubworlds:
    """Filesystem transaction; callbacks save/validate through existing editor APIs."""

    def __init__(self, *, save, validate, copy_scene=_copy_scene):
        self.save = save
        self.validate = validate
        self.copy_scene = copy_scene

    def prepare(self, source: str | Path, key: str) -> dict:
        if key not in ('O', 'P'):
            raise ValueError('Only O/P are navigation keys')
        root = Path(source).absolute()
        _plain(root)
        root = root.resolve(strict=True)
        _require_story(root)
        with _exclusive(root):
            role, target, exists = _relation(root)
            if (key == 'O' and role == 'child') or (key == 'P' and role == 'main'):
                return {'status': 'noop'}
            if exists:
                self.validate(target)
            self.save(root)
            # Saving may update asset manifests; validate the saved scene, not an old snapshot.
            _require_story(root)
            self.validate(root)
            if not exists:
                self._create(root, target)
            return {'status': 'ok', 'navigation': {
                'source': str(root), 'target': str(target), 'mode': 'story',
                'direction': 'enter' if key == 'O' else 'exit', 'created': not exists,
            }}

    def _create(self, root: Path, target: Path) -> None:
        staging = Path(tempfile.mkdtemp(prefix='.subworld-staging-', dir=root / '.game'))
        staged_world = staging / 'world'
        link_id = str(uuid.uuid4())
        parent_link_created = False
        try:
            self.copy_scene(root, staged_world)
            _require_story(staged_world)
            self.validate(staged_world)
            _new_link(staged_world, 'child', link_id)
            if _exists(target) or _exists(root / METADATA):
                raise SubworldError('复制期间出现目录或关联冲突，拒绝覆盖')
            # Record parent before publishing the child. A crash is fail-closed, never a
            # silently reusable half-copy. Ordinary failures roll back only our metadata.
            _new_link(root, 'main', link_id)
            parent_link_created = True
            if _exists(target):
                raise SubworldError('子世界目录冲突，拒绝覆盖')
            staged_world.rename(target)
        except BaseException:
            if parent_link_created:
                (root / METADATA).unlink(missing_ok=True)
            raise
        finally:
            # Only this freshly-created staging directory, never the source or final child.
            def remove_readonly(function, path, error):
                # copy2 preserves read-only asset attributes on Windows. Only
                # repair permissions within our disposable staging directory.
                if not isinstance(error, PermissionError):
                    raise error
                os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
                function(path)
            shutil.rmtree(staging, onexc=remove_readonly)
