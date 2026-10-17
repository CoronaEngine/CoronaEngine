"""Manage the Horizon source workspace at ``.workspace/Horizon``.

The workspace is an ordinary Git clone of the Horizon repository, kept outside
CoronaEngine's version control and compiled in place through ``add_subdirectory``.
The tooling deliberately treats it as a normal developer clone:

* Builds never change it. ``ensure_workspace()`` reports the state and always
  lets the build proceed, whatever that state is: uncommitted edits, a detached
  HEAD, local commits, or a HEAD that no longer matches the lock. A dirty
  workspace in particular is the expected condition while Horizon is being
  edited and verified from inside CoronaEngine.
* ``.workspace/horizon.lock.json`` is a starting point, not an authority. It is
  read once when the clone is created and afterwards only describes the baseline
  in reports. It is edited by hand; nothing here writes it.
* The only commands that touch the clone are the ones you ask for explicitly:
  the one-time ``clone`` and ``horizon-sync``. Neither discards uncommitted work
  -- Git refuses a checkout that would overwrite it -- and nothing rewrites,
  resets or detaches a branch behind your back.

Only configuration and environment faults stay fatal: a lock file that is missing
or unparseable when a clone has to be created, and a workspace path that is not a
Git checkout at all. With a clone already present, even an unreadable lock is just
reported, and ``horizon-sync`` then explains why it cannot align.
"""

from __future__ import annotations

import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from workflow import run_command, safe_remove


COMMIT_PATTERN = re.compile(r"^[0-9a-fA-F]{40}$")
LOCK_RELATIVE_PATH = Path(".workspace") / "horizon.lock.json"
WORKTREE_RELATIVE_PATH = Path(".workspace") / "Horizon"
SYNC_COMMAND = "python tools/dev.py horizon-sync"
PULL_COMMAND = "git -C .workspace/Horizon pull"
LOCK_HINT = f"edit {LOCK_RELATIVE_PATH.as_posix()} and run '{SYNC_COMMAND}'"

# One line of ``git status --porcelain`` is two status columns, a space, then the
# path. Untracked files are reported as ``??``; a rename reports the old path, a
# space and a separator before the new one.
_UNTRACKED_STATUS = "??"
_STATUS_WIDTH = 3
_RENAME_SEPARATOR = " -> "


@dataclass(frozen=True)
class HorizonLock:
    schema_version: int
    url: str
    ref: str
    commit: str


@dataclass(frozen=True)
class HorizonStatus:
    """Read-only snapshot of the Horizon workspace.

    ``drift``, ``dirty_files``, ``ahead`` and ``behind`` are all reported to the
    developer and never acted upon: the build proceeds regardless.
    """

    lock: HorizonLock | None
    worktree: Path
    cloned: bool
    head: str = ""
    branch: str = ""
    upstream: str = ""
    remote_url: str = ""
    drift: str = ""
    ahead: int = 0
    behind: int = 0
    dirty_files: tuple[str, ...] = ()
    error: str = ""
    lock_error: str = ""

    @property
    def dirty(self) -> bool:
        return bool(self.dirty_files)

    @property
    def modified(self) -> tuple[str, ...]:
        return tuple(line for line in self.dirty_files if not line.startswith(_UNTRACKED_STATUS))

    @property
    def untracked(self) -> tuple[str, ...]:
        return tuple(line for line in self.dirty_files if line.startswith(_UNTRACKED_STATUS))

    @property
    def detached(self) -> bool:
        return self.cloned and not self.branch

    @property
    def upstream_configured(self) -> bool:
        return bool(self.upstream)

    @property
    def url_matches(self) -> bool:
        if self.lock is None:
            return True
        return _normalized_url(self.remote_url) == _normalized_url(self.lock.url)

    @property
    def in_sync(self) -> bool:
        """Whether HEAD equals the locked commit. Reported, never enforced."""
        return self.cloned and self.lock is not None and self.head == self.lock.commit

    @property
    def needs_attention(self) -> bool:
        """Local state worth warning about; the build still proceeds."""
        return self.cloned and (self.dirty or not self.in_sync or not self.url_matches)

    def summary(self) -> str:
        if self.lock is not None:
            lock_line = f"{self.lock.commit} ({self.lock.ref})"
        elif self.lock_error:
            lock_line = f"unreadable: {self.lock_error}"
        else:
            lock_line = "not read"
        lines = ["Horizon lock:   " + lock_line]
        if not self.cloned:
            lines.append(f"Horizon clone:  missing at {self.worktree}")
            if not self.error:
                lines.append("Horizon note:   the next install, configure or build clones it")
            return "\n".join(lines)
        if self.error:
            lines.append(f"Horizon clone:  unusable at {self.worktree}")
            lines.append(f"Horizon error:  {self.error}")
            return "\n".join(lines)
        if self.lock is None:
            lines.append(f"Horizon HEAD:   {self.head}")
        else:
            lines.append(f"Horizon HEAD:   {self.head} ({self._head_note()})")
        if self.detached:
            lines.append("Horizon branch: detached HEAD (commits here are not on any branch)")
        else:
            lines.append(f"Horizon branch: {self.branch}{self._upstream_note()}")
        if not self.url_matches:
            lines.append(f"Horizon origin: {self.remote_url} (expected {self.lock.url})")
        lines.append(f"Horizon tree:   {self._tree_summary()}")
        return "\n".join(lines)

    def advice(self) -> tuple[str, ...]:
        """Human-readable suggestions. Empty when nothing needs saying."""
        if not self.cloned:
            return ()
        if self.error:
            return (f"Horizon: {self.error}",)
        if self.lock is None:
            return (f"Horizon: {self.lock_error}",)
        notes: list[str] = []
        if self.dirty:
            notes.append(
                f"Horizon has {self._tree_summary()}; they are compiled as-is and left untouched. "
                "Commit or push them from inside the workspace when they are ready."
            )
        if not self.in_sync:
            notes.append(
                f"Horizon HEAD {self.head} is not the locked commit {self.lock.commit} "
                f"({self.drift}); the build continues regardless. To follow the lock: {LOCK_HINT}."
            )
        if not self.url_matches:
            notes.append(
                f"Horizon origin is '{self.remote_url}', but the lock expects '{self.lock.url}'."
            )
        if self.detached:
            notes.append(
                "Horizon is on a detached HEAD, so commits made here belong to no branch. "
                f"Run 'git -C {WORKTREE_RELATIVE_PATH.as_posix()} switch -c <name>' before committing."
            )
        if self.branch and self.upstream_configured and (self.ahead or self.behind):
            counts = _upstream_counts(self.ahead, self.behind)
            notes.append(
                f"Horizon '{self.branch}' is {counts} {self.upstream}. "
                f"'{PULL_COMMAND}' to integrate, or push from inside the workspace to publish."
            )
        elif self.branch and not self.upstream_configured:
            notes.append(
                f"Horizon branch '{self.branch}' has no upstream; "
                "push it from inside the workspace, or set one with 'git branch --set-upstream-to'."
            )
        return tuple(notes)

    def _head_note(self) -> str:
        assert self.lock is not None
        return "matches the lock" if self.in_sync else self.drift

    def _upstream_note(self) -> str:
        if not self.upstream_configured:
            return ""
        return f" -> {self.upstream}{_upstream_counts(self.ahead, self.behind, prefix=', ')}"

    def _tree_summary(self) -> str:
        if not self.dirty:
            return "clean"
        parts: list[str] = []
        if self.modified:
            parts.append(f"{len(self.modified)} modified")
        if self.untracked:
            parts.append(f"{len(self.untracked)} untracked")
        shown = ", ".join(_status_path(line) for line in self.dirty_files[:5])
        hidden = len(self.dirty_files) - 5
        suffix = f" (+{hidden} more)" if hidden > 0 else ""
        return f"{' and '.join(parts)}: {shown}{suffix}"


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


def _git(
    worktree: Path,
    *arguments: str,
    capture: bool = False,
    quiet: bool = False,
    keep_leading: bool = False,
) -> str:
    """Run git, optionally capturing stdout.

    ``git status --porcelain`` is positional: a staged-only change starts with a
    space, so its output must not be stripped on the left or the status prefix
    shifts and eats the first character of the path.
    """
    result = run_command(
        ("git", "-C", worktree, *arguments),
        cwd=worktree.parent,
        capture_output=capture,
        quiet=quiet,
    )
    if not capture:
        return ""
    return result.stdout.rstrip("\r\n") if keep_leading else result.stdout.strip()


def _git_probe(worktree: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    """Run a read-only git query whose non-zero exit code is a valid answer."""
    return run_command(
        ("git", "-C", worktree, *arguments),
        cwd=worktree.parent,
        capture_output=True,
        quiet=True,
        check=False,
    )


def current_commit(worktree: Path) -> str:
    return _git(worktree, "rev-parse", "HEAD", capture=True, quiet=True).lower()


def _normalized_url(value: str) -> str:
    return value.rstrip("/").removesuffix(".git").lower()


def _status_path(line: str) -> str:
    """The path portion of a ``git status --porcelain`` line."""
    if len(line) <= _STATUS_WIDTH:
        return line.strip()
    path = line[_STATUS_WIDTH:].strip()
    # ``R  old -> new``: keep the new name, which is what is on disk now.
    _, separator, renamed = path.rpartition(_RENAME_SEPARATOR)
    if separator:
        return renamed.strip().strip('"')
    return path


def _commit_present(worktree: Path, commit: str) -> bool:
    return _git_probe(worktree, "cat-file", "-e", f"{commit}^{{commit}}").returncode == 0


def _is_ancestor(worktree: Path, ancestor: str, descendant: str) -> bool | None:
    code = _git_probe(worktree, "merge-base", "--is-ancestor", ancestor, descendant).returncode
    if code == 0:
        return True
    if code == 1:
        return False
    return None


def _git_or_default(worktree: Path, *arguments: str) -> str:
    """Capture a read-only query that legitimately answers with nothing."""
    result = _git_probe(worktree, *arguments)
    return result.stdout.strip() if result.returncode == 0 else ""


def _commits(count: str) -> str:
    return f"{count} commit" if count == "1" else f"{count} commits"


def _upstream_counts(ahead: int, behind: int, *, prefix: str = " ") -> str:
    parts: list[str] = []
    if ahead:
        parts.append(f"{ahead} ahead")
    if behind:
        parts.append(f"{behind} behind")
    return prefix + " and ".join(parts) if parts else ""


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


def _ahead_behind(worktree: Path) -> tuple[int, int]:
    counts = _git_or_default(worktree, "rev-list", "--left-right", "--count", "@{upstream}...HEAD")
    fields = counts.split()
    if len(fields) != 2:
        return 0, 0
    try:
        behind, ahead = int(fields[0]), int(fields[1])
    except ValueError:
        return 0, 0
    return ahead, behind


def _read_lock(repo_root: Path) -> tuple[HorizonLock | None, str]:
    lock_file = repo_root / LOCK_RELATIVE_PATH
    if not lock_file.is_file():
        return None, f"the lock file '{LOCK_RELATIVE_PATH.as_posix()}' does not exist"
    try:
        return load_lock(lock_file), ""
    except RuntimeError as error:
        return None, str(error)


def inspect_workspace(repo_root: Path) -> HorizonStatus:
    """Snapshot the Horizon workspace without modifying it."""
    repo_root = Path(repo_root).resolve()
    lock, lock_error = _read_lock(repo_root)
    worktree = repo_root / WORKTREE_RELATIVE_PATH
    if not worktree.exists():
        return HorizonStatus(
            lock=lock, worktree=worktree, cloned=False, lock_error=lock_error
        )
    if not (worktree / ".git").exists():
        return HorizonStatus(
            lock=lock,
            worktree=worktree,
            cloned=False,
            lock_error=lock_error,
            error=(
                f"'{WORKTREE_RELATIVE_PATH.as_posix()}' exists but is not a Git checkout. "
                "Remove that directory so the tooling can clone it again."
            ),
        )

    head = current_commit(worktree)
    branch = _git_or_default(worktree, "symbolic-ref", "--quiet", "--short", "HEAD")
    upstream = _git_or_default(worktree, "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}")
    ahead, behind = _ahead_behind(worktree) if upstream else (0, 0)
    dirty_files = tuple(
        line
        for line in _git(
            worktree, "status", "--porcelain", capture=True, quiet=True, keep_leading=True
        ).splitlines()
        if line.strip()
    )
    drift = "" if lock is None or head == lock.commit else _drift_summary(worktree, head, lock.commit)
    return HorizonStatus(
        lock=lock,
        worktree=worktree,
        cloned=True,
        head=head,
        branch=branch,
        upstream=upstream,
        remote_url=_git_or_default(worktree, "remote", "get-url", "origin"),
        drift=drift,
        ahead=ahead,
        behind=behind,
        dirty_files=dirty_files,
        lock_error=lock_error,
    )


def _clone_locked(repo_root: Path, worktree: Path, lock: HorizonLock) -> None:
    """Create the workspace as an ordinary clone, resting on its upstream branch."""
    worktree.parent.mkdir(parents=True, exist_ok=True)
    run_command(
        ("git", "clone", "--no-checkout", "--recurse-submodules", lock.url, worktree),
        cwd=repo_root,
    )
    _git(worktree, "fetch", "--tags", "origin", lock.ref)
    _git(worktree, "cat-file", "-e", f"{lock.commit}^{{commit}}")
    _git(worktree, "checkout", "--detach", lock.commit)
    # Leave the workspace on a branch rather than a detached HEAD, so commits made
    # while editing Horizon inside CoronaEngine belong to something and can be
    # pushed like any other clone.
    _git(worktree, "checkout", "-B", lock.ref, lock.commit)
    _git(worktree, "branch", "--set-upstream-to", f"origin/{lock.ref}", lock.ref)
    run_command(
        ("git", "submodule", "update", "--init", "--recursive"),
        cwd=worktree,
        quiet=True,
        check=False,
    )


def ensure_workspace(repo_root: Path) -> HorizonStatus:
    """Report the Horizon workspace, creating the clone only when it is missing.

    Never fails because of the workspace *state*: a dirty tree, a detached HEAD,
    local commits and a HEAD that differs from the lock are all reported and the
    build proceeds. Only a broken lock file or a path that is not a Git checkout
    is fatal, and neither is a statement about the developer's work.
    """
    repo_root = Path(repo_root).resolve()
    worktree = repo_root / WORKTREE_RELATIVE_PATH
    status = inspect_workspace(repo_root)
    if worktree.exists() and status.error:
        # A present-but-unusable path is an environment fault. Raise instead of
        # re-cloning: the directory may hold the developer's own files.
        raise RuntimeError(f"{status.error} (at {worktree})")

    if not worktree.exists():
        lock, lock_error = _read_lock(repo_root)
        if lock is None:
            raise RuntimeError(f"Cannot create the Horizon workspace at {worktree}: {lock_error}.")
        try:
            _clone_locked(repo_root, worktree, lock)
        except Exception:
            if worktree.exists():
                safe_remove(repo_root, worktree)
            raise
        print(f"[INFO] Horizon workspace created at {worktree} (locked commit {lock.commit})")
        status = inspect_workspace(repo_root)
    _report(status)
    return status


def _settle_on_branch(status: HorizonStatus) -> None:
    """Put the just-checked-out commit on the lock's branch when that is safe.

    Syncing moves HEAD to a commit, which detaches it -- precisely the state the
    report warns about -- so the commit is reattached whenever no local commit can
    be pushed off a branch:

    * the branch does not exist yet -- it is created at the locked commit;
    * the branch is already at the locked commit -- HEAD simply rejoins it;
    * the branch holds nothing the locked commit lacks -- it fast-forwards.

    A branch tip is never moved backwards or sideways over the developer's own
    commits: in that case the branch is left exactly as it was and HEAD stays
    detached, which the report then explains.
    """
    assert status.lock is not None
    branch = status.lock.ref
    tip = _git_or_default(status.worktree, "rev-parse", "--verify", "--quiet", f"refs/heads/{branch}")
    created = not tip
    fast_forwarded = bool(tip) and tip != status.lock.commit and _is_ancestor(
        status.worktree, tip, status.lock.commit
    ) is True
    if not created and tip != status.lock.commit and not fast_forwarded:
        count = _git(
            status.worktree,
            "rev-list",
            "--count",
            f"{status.lock.commit}..{branch}",
            capture=True,
            quiet=True,
        )
        print(
            f"[WARN] Horizon HEAD is detached at the locked commit and branch '{branch}' was left "
            f"at {tip}: it holds {_commits(count)} the lock does not, and moving it would push "
            "them off the branch. Push or merge them first, or move the branch yourself with "
            f"'git -C {WORKTREE_RELATIVE_PATH.as_posix()} branch -f {branch} {status.lock.commit}'."
        )
        return
    if _git_probe(status.worktree, "checkout", "-B", branch, status.lock.commit).returncode != 0:
        print(f"[INFO] Horizon stays on a detached HEAD: branch '{branch}' is in use elsewhere")
        return
    if created:
        print(f"[INFO] Horizon is checked out on branch '{branch}'")
    elif fast_forwarded:
        print(f"[INFO] Horizon branch '{branch}' fast-forwarded to the locked commit")
    else:
        print(f"[INFO] Horizon is back on branch '{branch}'")


def sync_workspace(repo_root: Path) -> HorizonStatus:
    """Move the workspace to the commit the (hand-edited) lock file names.

    This is the one command that changes the clone, and it only ever moves HEAD:
    it never rewrites the lock file and never moves a branch backwards over the
    developer's commits. Uncommitted work is protected by Git itself, which
    refuses a checkout that would overwrite it. When moving the lock's branch
    forward would discard local commits, HEAD is left detached and the branch is
    reported rather than reset.
    """
    status = ensure_workspace(repo_root)
    if not status.cloned:
        return status
    if status.lock is None:
        raise RuntimeError(
            f"Cannot sync Horizon: {status.lock_error}. The build does not need the lock, so it "
            "can continue without one."
        )
    if status.in_sync:
        print(f"[INFO] Horizon is already at the locked commit {status.lock.commit} ({status.lock.ref})")
        return status
    if not _commit_present(status.worktree, status.lock.commit):
        print(f"[INFO] Fetching '{status.lock.ref}' from origin to obtain {status.lock.commit}")
        _git(status.worktree, "fetch", "--tags", "origin", status.lock.ref)
        if not _commit_present(status.worktree, status.lock.commit):
            raise RuntimeError(
                f"Locked Horizon commit {status.lock.commit} is not reachable from "
                f"'{status.lock.ref}' at {status.lock.url}; the lock file may be stale or "
                "upstream history was rewritten."
            )
    print(f"[INFO] Checking out {status.lock.commit}")
    checkout = run_command(
        ("git", "-C", status.worktree, "checkout", "--detach", status.lock.commit),
        cwd=status.worktree.parent,
        capture_output=True,
        quiet=True,
        check=False,
    )
    if checkout.returncode != 0:
        # The usual cause is Git refusing to overwrite uncommitted work. Surface
        # Git's own diagnosis rather than its bare exit code: the developer's
        # changes are intact, and Git says exactly which file is in the way.
        detail = (checkout.stderr or checkout.stdout).strip()
        raise RuntimeError(
            f"Horizon could not move to the locked commit {status.lock.commit}; "
            f"Git refused the checkout, so local changes were left untouched. {detail} "
            "Commit or stash them inside "
            f"{WORKTREE_RELATIVE_PATH.as_posix()} and run '{SYNC_COMMAND}' again, or stay where "
            "you are -- the build does not require the lock."
        )
    # Re-inspect before deciding about the branch: the snapshot above was taken
    # before HEAD moved, so its drift text is stale.
    moved = inspect_workspace(repo_root)
    print(f"[INFO] Horizon moved: {status.head} -> {moved.head}")
    _settle_on_branch(moved)
    print(
        "[INFO] Horizon sources changed; run 'python tools/dev.py configure' or 'build' before "
        "compiling (build-fast does not refresh dependencies)."
    )
    result = inspect_workspace(repo_root)
    _report(result)
    return result


def _report(status: HorizonStatus) -> None:
    """Print the workspace state and any advice. Never raises."""
    print(status.summary())
    for note in status.advice():
        print(f"[WARN] {note}")
    if status.cloned and status.needs_attention:
        print("[WARN] Horizon differs from the lock; the build continues either way.")
