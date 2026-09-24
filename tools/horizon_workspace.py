from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from workflow import CommandError, run_command, safe_remove


COMMIT_PATTERN = re.compile(r"^[0-9a-fA-F]{40}$")
LOCK_RELATIVE_PATH = Path(".workspace") / "horizon.lock.json"
WORKTREE_RELATIVE_PATH = Path(".workspace") / "Horizon"
SYNC_COMMAND = "python tools/dev.py horizon-sync"
UPDATE_COMMAND = "python tools/dev.py update"


@dataclass(frozen=True)
class HorizonLock:
    schema_version: int
    url: str
    ref: str
    commit: str


@dataclass(frozen=True)
class WorkspaceState:
    lock: HorizonLock
    worktree: Path
    cloned: bool
    head: str = ""
    remote_url: str = ""
    drift: str = ""
    dirty_files: tuple[str, ...] = ()

    @property
    def dirty(self) -> bool:
        return bool(self.dirty_files)

    @property
    def url_matches(self) -> bool:
        return _normalized_url(self.remote_url) == _normalized_url(self.lock.url)

    @property
    def in_sync(self) -> bool:
        return self.cloned and self.head == self.lock.commit

    @property
    def needs_repair(self) -> bool:
        return self.cloned and (not self.in_sync or not self.url_matches)

    def summary(self) -> str:
        lines = [f"Horizon lock:   {self.lock.commit} ({self.lock.ref})"]
        if not self.cloned:
            lines.append(f"Horizon clone:  missing at {self.worktree}; install/configure clones the locked commit")
            return "\n".join(lines)
        if not self.url_matches:
            lines.append(f"Horizon origin: {self.remote_url} (expected {self.lock.url})")
        head_note = "matches the lock" if self.in_sync else self.drift
        lines.append(f"Horizon HEAD:   {self.head} ({head_note})")
        lines.append(f"Horizon tree:   {self._tree_summary()}")
        if not self.url_matches:
            lines.append(f"Horizon fix:    restore the expected origin, then run '{SYNC_COMMAND}'")
        elif not self.in_sync:
            lines.append(
                f"Horizon fix:    run '{SYNC_COMMAND}' to restore the locked commit, "
                f"or '{UPDATE_COMMAND}' to move the lock"
            )
        return "\n".join(lines)

    def _tree_summary(self) -> str:
        if not self.dirty:
            return "clean"
        shown = ", ".join(self.dirty_files[:5])
        hidden = len(self.dirty_files) - 5
        suffix = f" (+{hidden} more)" if hidden > 0 else ""
        return f"dirty: {shown}{suffix}"


def load_lock(lock_file: Path) -> HorizonLock:
    try:
        data = json.loads(lock_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Could not read Horizon lock file '{lock_file}': {error}") from error
    if not isinstance(data, dict):
        raise RuntimeError(f"Invalid Horizon lock data: {lock_file}")
    lock = HorizonLock(
        schema_version=data.get("schema_version"),
        url=data.get("url", ""),
        ref=data.get("ref", ""),
        commit=data.get("commit", "").lower(),
    )
    if lock.schema_version != 1:
        raise RuntimeError(f"Unsupported Horizon lock schema: {lock.schema_version}")
    if not lock.url or not lock.ref or not COMMIT_PATTERN.fullmatch(lock.commit):
        raise RuntimeError(f"Invalid Horizon lock data: {lock_file}")
    return lock


def _git(worktree: Path, *arguments: str, capture: bool = False, quiet: bool = False) -> str:
    result = run_command(
        ("git", "-C", worktree, *arguments),
        cwd=worktree.parent,
        capture_output=capture,
        quiet=quiet,
    )
    return result.stdout.strip() if capture else ""


def _git_probe(worktree: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    """Run a read-only git query whose non-zero exit code is a valid answer."""
    return run_command(
        ("git", "-C", worktree, *arguments),
        cwd=worktree.parent,
        capture_output=True,
        quiet=True,
        check=False,
    )


def is_dirty(worktree: Path) -> bool:
    return bool(_git(worktree, "status", "--porcelain", capture=True, quiet=True))


def current_commit(worktree: Path) -> str:
    return _git(worktree, "rev-parse", "HEAD", capture=True, quiet=True).lower()


def _normalized_url(value: str) -> str:
    return value.rstrip("/").removesuffix(".git").lower()


def _commit_present(worktree: Path, commit: str) -> bool:
    return _git_probe(worktree, "cat-file", "-e", f"{commit}^{{commit}}").returncode == 0


def _is_ancestor(worktree: Path, ancestor: str, descendant: str) -> bool | None:
    code = _git_probe(worktree, "merge-base", "--is-ancestor", ancestor, descendant).returncode
    if code == 0:
        return True
    if code == 1:
        return False
    return None


def _commits(count: str) -> str:
    return f"{count} commit" if count == "1" else f"{count} commits"


def _drift_summary(worktree: Path, head: str, target: str) -> str:
    if not _commit_present(worktree, target):
        return f"the locked commit {target} is not present locally yet"
    if _is_ancestor(worktree, head, target) is True:
        count = _git(worktree, "rev-list", "--count", f"{head}..{target}", capture=True, quiet=True)
        return f"local HEAD is {_commits(count)} behind the lock"
    if _is_ancestor(worktree, target, head) is True:
        count = _git(worktree, "rev-list", "--count", f"{target}..{head}", capture=True, quiet=True)
        return f"local HEAD is {_commits(count)} ahead of the lock"
    return "local HEAD and the lock have diverged"


def inspect_workspace(repo_root: Path) -> WorkspaceState:
    """Snapshot the managed Horizon workspace without modifying it."""
    repo_root = Path(repo_root).resolve()
    lock = load_lock(repo_root / LOCK_RELATIVE_PATH)
    worktree = repo_root / WORKTREE_RELATIVE_PATH
    if not worktree.exists():
        return WorkspaceState(lock=lock, worktree=worktree, cloned=False)
    if not (worktree / ".git").exists():
        raise RuntimeError(
            f"Horizon workspace is not a Git checkout: {worktree}. "
            "Remove that directory so the tooling can clone the locked commit again."
        )
    head = current_commit(worktree)
    remote_url = _git(worktree, "remote", "get-url", "origin", capture=True, quiet=True)
    dirty_files = tuple(
        line
        for line in _git(worktree, "status", "--porcelain", capture=True, quiet=True).splitlines()
        if line.strip()
    )
    drift = "" if head == lock.commit else _drift_summary(worktree, head, lock.commit)
    return WorkspaceState(
        lock=lock,
        worktree=worktree,
        cloned=True,
        head=head,
        remote_url=remote_url,
        drift=drift,
        dirty_files=dirty_files,
    )


def ensure_workspace(repo_root: Path) -> HorizonLock:
    repo_root = Path(repo_root).resolve()
    lock_file = repo_root / LOCK_RELATIVE_PATH
    worktree = repo_root / WORKTREE_RELATIVE_PATH
    lock = load_lock(lock_file)
    created = False
    if not worktree.exists():
        worktree.parent.mkdir(parents=True, exist_ok=True)
        created = True
        try:
            run_command(("git", "clone", "--no-checkout", lock.url, worktree), cwd=repo_root)
            _git(worktree, "fetch", "--tags", "origin", lock.ref)
            _git(worktree, "cat-file", "-e", f"{lock.commit}^{{commit}}")
            _git(worktree, "checkout", "--detach", lock.commit)
        except Exception:
            if worktree.exists():
                safe_remove(repo_root, worktree)
            raise

    state = inspect_workspace(repo_root)
    if not state.url_matches:
        raise RuntimeError(f"Horizon origin is '{state.remote_url}', expected '{lock.url}'.")
    if not state.in_sync:
        raise RuntimeError(
            f"Horizon HEAD is {state.head}, but the lock requires {lock.commit} ({state.drift}). "
            f"The workflow will not reset local work; run '{SYNC_COMMAND}' to restore the locked commit, "
            f"or '{UPDATE_COMMAND}' to move the lock forward."
        )
    if created:
        print(f"[INFO] Horizon workspace created at {worktree} (locked commit {lock.commit})")
    return lock


def _ensure_commit_available(state: WorkspaceState) -> None:
    if _commit_present(state.worktree, state.lock.commit):
        return
    print(f"[INFO] Fetching '{state.lock.ref}' from origin to obtain {state.lock.commit}")
    _git(state.worktree, "fetch", "--tags", "origin", state.lock.ref)
    if not _commit_present(state.worktree, state.lock.commit):
        raise RuntimeError(
            f"Locked Horizon commit {state.lock.commit} is not reachable from '{state.lock.ref}' at "
            f"{state.lock.url}; the lock file may be stale or upstream history was rewritten."
        )


def sync_workspace(repo_root: Path) -> HorizonLock:
    """Restore the workspace to the commit pinned by the lock file."""
    state = inspect_workspace(repo_root)
    if not state.cloned:
        return ensure_workspace(repo_root)
    if not state.url_matches:
        raise RuntimeError(
            f"Horizon origin is '{state.remote_url}', expected '{state.lock.url}'. "
            "Restore the expected origin before syncing the locked commit."
        )
    if state.in_sync:
        print(f"[INFO] Horizon is already at the locked commit {state.lock.commit} ({state.lock.ref})")
        if state.dirty:
            print("[INFO] Horizon workspace has local changes; they are kept and compiled as-is.")
        return state.lock
    if state.dirty:
        listing = "\n".join(f"  {line}" for line in state.dirty_files[:10])
        raise RuntimeError(
            f"Horizon workspace has local changes ({len(state.dirty_files)} file(s)), "
            f"so the locked commit was not restored:\n{listing}\n"
            f"Commit or stash them inside {state.worktree}, then run '{SYNC_COMMAND}' again."
        )
    _ensure_commit_available(state)
    _git(state.worktree, "checkout", "--detach", state.lock.commit)
    print(f"[INFO] Horizon restored: {state.head} -> {state.lock.commit} ({state.drift})")
    print(
        "[INFO] Horizon sources changed; run 'python tools/dev.py configure' or 'build' before compiling "
        "(build-fast does not refresh dependencies)."
    )
    return state.lock


def update_workspace(repo_root: Path) -> HorizonLock:
    old_lock = sync_workspace(repo_root)
    repo_root = Path(repo_root).resolve()
    lock_file = repo_root / LOCK_RELATIVE_PATH
    worktree = repo_root / WORKTREE_RELATIVE_PATH
    if is_dirty(worktree):
        raise RuntimeError("Horizon workspace has local changes; update was refused.")

    _git(worktree, "fetch", "--tags", "origin", old_lock.ref)
    new_commit = _git(worktree, "rev-parse", "FETCH_HEAD", capture=True).lower()
    if new_commit == old_lock.commit:
        print(f"[INFO] Horizon is already locked at {new_commit}")
        return old_lock

    temporary_lock = lock_file.with_suffix(".json.tmp")
    data = {
        "schema_version": 1,
        "url": old_lock.url,
        "ref": old_lock.ref,
        "commit": new_commit,
    }
    temporary_lock.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    try:
        _git(worktree, "checkout", "--detach", new_commit)
        temporary_lock.replace(lock_file)
    except Exception:
        if temporary_lock.exists():
            temporary_lock.unlink()
        try:
            _git(worktree, "checkout", "--detach", old_lock.commit)
        except CommandError:
            pass
        raise
    print(f"[INFO] Horizon lock updated: {old_lock.commit} -> {new_commit}")
    return load_lock(lock_file)
