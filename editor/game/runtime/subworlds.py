"""Persistent, independent story subworlds. No engine imports or scene-opening calls."""
from __future__ import annotations

import configparser
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
import time
import uuid


class SubworldError(RuntimeError):
    """A failed preparation must not replace the currently open world."""


METADATA = Path('.game/story-link.json')
CHILD = Path('.game/subworld')
# Written only while a copy-fallback publication is in flight. Without it a crash in
# that window is indistinguishable from a tampered world, and the strict link checks
# would refuse to repair it forever.
PENDING = Path('.game/.subworld-publishing')

# Windows denies a directory rename with WinError 5 while *any* file inside the tree
# (or in the destination) is open, including by the editor that owns the world. Those
# holders are usually short-lived: an antivirus scan of the bytes we just wrote, a
# thumbnailer, our own readers. Wait them out before paying for a second copy.
RENAME_ATTEMPTS = 5
RENAME_BACKOFF = 0.02
CHUNK = 1024 * 1024


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


def _linked_pair(root: Path, child: Path) -> bool:
    """True only when both ends exist and describe each other."""
    try:
        if not child.is_dir():
            return False
        link, reverse = _read_link(root), _read_link(child)
    except SubworldError:
        return False
    return bool(link and reverse and link['id'] == reverse['id']
                and link['role'] == 'main' and reverse['role'] == 'child')


def _remove_readonly(function, path, error):
    # copy2 preserves read-only asset attributes on Windows. Only repair permissions
    # inside trees this module created and therefore owns.
    if not isinstance(error, PermissionError):
        raise error
    os.chmod(path, stat.S_IWRITE | stat.S_IREAD)
    function(path)


def _discard(path: Path) -> None:
    if _exists(path):
        shutil.rmtree(path, onexc=_remove_readonly, ignore_errors=True)


def _park(child: Path, root: Path) -> Path:
    """Move a stale child aside. Never deleted, never merged into."""
    parked = root / '.game' / f'.subworld-stale-{uuid.uuid4().hex[:8]}'
    try:
        child.rename(parked)
    except OSError as error:
        raise SubworldError(f'无法替换已存在的子世界目录：{child}；{error}') from error
    return parked


def _recover_interrupted(root: Path) -> None:
    """Repair a publication whose copy fallback crashed mid-write.

    Everything else stays fail-closed: an unexpected child directory is still an
    error, because only this module's own marker proves that the child is by
    construction an incomplete copy of the parent rather than someone's data.
    """
    marker = root / PENDING
    if _exists(marker):
        child = root / CHILD
        if _linked_pair(root, child):
            marker.unlink(missing_ok=True)  # the publication completed; marker is residue
        else:
            if _exists(child):
                _discard(_park(child, root))
            (root / METADATA).unlink(missing_ok=True)
            marker.unlink(missing_ok=True)
    # Staging trees are only ever created under the lock and are always disposable,
    # so a tree an earlier crash could not delete is swept here instead of lingering.
    for stale in (root / '.game').glob('.subworld-staging-*'):
        _discard(stale)


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


def _copy_scene(source: Path, target: Path) -> dict[str, str]:
    """Copy the world, hashing each file from the very read that produced the copy.

    Returns route -> sha256 for every copied file. Because the digest of a file is
    taken from the bytes written to the staged tree, proving the staged copy matches
    the source manifest costs no second pass over the assets.
    """
    digests: dict[str, str] = {}

    def ignore(directory, names):
        skipped = {name for name in names if os.path.normcase(name) == '.game'} if Path(directory) == source else set()
        for name in names:
            if name not in skipped:
                _plain(Path(directory) / name)
        return skipped

    def copy_function(src, dst):
        # Stands in for copy2: independent files (never hardlinks), same metadata.
        hasher = hashlib.sha256()
        with open(src, 'rb') as reader, open(dst, 'wb') as writer:
            while True:
                chunk = reader.read(CHUNK)
                if not chunk:
                    break
                hasher.update(chunk)
                writer.write(chunk)
        shutil.copystat(src, dst)
        digests[_route(Path(src).relative_to(source))] = hasher.hexdigest()

    shutil.copytree(source, target, ignore=ignore, copy_function=copy_function)
    return digests


def _route(path: Path) -> str:
    return os.path.normcase(path.as_posix())


def _manifest_assets(world: Path) -> list[tuple[str, str, int]]:
    """route as recorded, sha256, size — taken from the world's own asset manifest."""
    try:
        document = json.loads((world / 'assets.manifest.json').read_text(encoding='utf-8'))
    except (OSError, ValueError, UnicodeError):
        # A world without a usable manifest never reaches here: native validation
        # rejects it, and this function must not guess an integrity verdict.
        return []
    if not isinstance(document, dict) or not isinstance(document.get('bundles'), list):
        return []
    assets = []
    for bundle in document['bundles']:
        if not isinstance(bundle, dict) or not isinstance(bundle.get('files'), list):
            continue
        for item in bundle['files']:
            if not isinstance(item, dict):
                continue
            route, digest, size = item.get('path'), item.get('sha256'), item.get('size')
            if isinstance(route, str) and route and isinstance(digest, str) and isinstance(size, int):
                assets.append((route, digest.lower(), size))
    return assets


def _hash_assets(world: Path, assets) -> dict[str, str]:
    """Hash the staged tree's recorded assets; used only when the copy reported none."""
    digests = {}
    for route, _, _ in assets:
        try:
            with (world / route).open('rb') as stream:
                digests[_route(Path(route))] = hashlib.file_digest(stream, 'sha256').hexdigest()
        except OSError:
            continue
    return digests


def _verify_staged(source: Path, staged: Path, digests: dict[str, str]) -> None:
    """Verify our own copy against the world it came from.

    Content: every asset the source manifest records was read (during the copy) as
    the recorded sha256, so a corrupt source or a corrupt read is caught here.
    Fidelity: the file now on disk carries the source's size and mtime, so a
    truncated or failed write cannot be published as a subworld.
    """
    for route, digest, size in _manifest_assets(source):
        observed = digests.get(_route(Path(route)))
        if observed is None:
            raise SubworldError(f'暂存副本缺少清单资源：{route}')
        if observed != digest:
            raise SubworldError(f'资源内容与清单记录不一致，已取消切换：{route}')
        staged_file, source_file = staged / route, source / route
        try:
            staged_stat, source_stat = staged_file.stat(), source_file.stat()
        except OSError as error:
            raise SubworldError(f'暂存副本资源不可读：{route}；{error}') from error
        _plain(staged_file)
        if staged_stat.st_size != size or source_stat.st_size != size:
            raise SubworldError(f'资源大小与清单记录不一致，已取消切换：{route}')
        if (staged_stat.st_size != source_stat.st_size
                or staged_stat.st_mtime_ns != source_stat.st_mtime_ns):
            raise SubworldError(f'暂存副本与来源文件不一致，已取消切换：{route}')


def _tree_signature(root: Path) -> dict[str, int]:
    signature = {}
    for path in root.rglob('*'):
        if path.is_file():
            signature[_route(path.relative_to(root))] = path.stat().st_size
    return signature


def _write_pending(root: Path) -> None:
    path = root / PENDING
    with path.open('w', encoding='utf-8') as stream:
        json.dump({'version': 1, 'child': CHILD.as_posix()}, stream)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())


def _move(staged: Path, target: Path) -> None:
    """Rename the staged tree into place, waiting out short-lived Windows locks."""
    delay = RENAME_BACKOFF
    last: OSError | None = None
    for attempt in range(RENAME_ATTEMPTS):
        if attempt:
            time.sleep(delay)
            delay *= 2
        try:
            staged.rename(target)
            return
        except OSError as error:
            last = error
    raise last


def _copy_into(staged: Path, target: Path, root: Path) -> None:
    """Publish by copying when the move stays denied, then prove the copy landed."""
    _write_pending(root)
    try:
        # dirs_exist_ok keeps this a copy of *our* tree; a stale destination is
        # parked by _publish before it can be merged with.
        shutil.copytree(staged, target, dirs_exist_ok=True, copy_function=shutil.copy2)
        expected = _tree_signature(staged)
        if expected != _tree_signature(target):
            raise SubworldError(f'子世界副本校验失败，未发布：{target}')
    except BaseException:
        # Never leave a half-written child behind: the world stays unchanged.
        _discard(target)
        raise
    finally:
        (root / PENDING).unlink(missing_ok=True)


def _publish(staged: Path, target: Path, root: Path) -> None:
    """Atomically replace the destination with the staged tree.

    The destination is expected to be absent; if it is not (a crash inside an earlier
    publication), it is parked first so a copy can never be merged with stale files.
    """
    parked = None
    try:
        if _exists(target):
            parked = _park(target, root)
        try:
            _move(staged, target)
        except OSError as error:
            if not _exists(staged):
                raise
            try:
                _copy_into(staged, target, root)
            except BaseException as fallback_error:
                raise SubworldError(f'子世界无法发布（移动与复制均失败）：{fallback_error}') from error
    finally:
        if parked is not None:
            _discard(parked)


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
            _recover_interrupted(root)
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
            reported = self.copy_scene(root, staged_world)
            _require_story(staged_world)
            # The staged tree is our own copy, so re-hashing the whole world here would
            # repeat work the copy already did: verify against the read that made it.
            digests = (reported if isinstance(reported, dict)
                       else _hash_assets(staged_world, _manifest_assets(root)))
            _verify_staged(root, staged_world, digests)
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
            _publish(staged_world, target, root)
        except BaseException:
            if parent_link_created:
                (root / METADATA).unlink(missing_ok=True)
            raise
        finally:
            # Only this freshly-created staging directory, never the source or final child.
            # Cleanup never decides the outcome: a lock that outlives the switch must not
            # turn a published child into a reported failure. Leftovers are swept by the
            # next _recover_interrupted instead.
            try:
                shutil.rmtree(staging, onexc=_remove_readonly)
            except OSError:
                pass
