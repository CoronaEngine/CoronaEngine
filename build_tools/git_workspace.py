"""Shared Git workspace lifecycle for source dependencies.

Builds clone missing sources at the baseline in source_workspaces.py and otherwise
only report local state. Explicit sync preserves local edits/branches and aligns
submodules too. Preparation and sync are serialized across build presets.
"""
from __future__ import annotations

import subprocess
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

from build_tools.paths import WORKSPACE_LOCK_DIR
from build_tools.file_lock import file_lock
from build_tools.source_workspaces import SourceWorkspace
from build_tools.workflow import run_command, safe_remove

_UNTRACKED_STATUS = "??"
_STATUS_WIDTH = 3
_RENAME_SEPARATOR = " -> "


@dataclass(frozen=True)
class WorkspaceStatus:
    """Read-only snapshot of a source workspace.

    ``drift``, ``dirty_files``, ``ahead`` and ``behind`` are all reported to the
    developer and never acted upon: the build proceeds regardless.
    """

    lock: SourceWorkspace
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
        return _normalized_url(self.remote_url) == _normalized_url(self.lock.url)

    @property
    def in_sync(self) -> bool:
        """Whether HEAD equals the locked commit. Reported, never enforced."""
        return self.cloned and self.head == self.lock.commit

    @property
    def needs_attention(self) -> bool:
        """Local state worth warning about; the build still proceeds."""
        return self.cloned and (self.dirty or not self.in_sync or not self.url_matches)

    def summary(self) -> str:
        lines = [f"{self.lock.name} lock:   {self.lock.commit} ({self.lock.ref})"]
        if self.error:
            lines.append(f"{self.lock.name} clone:  unusable at {self.worktree}")
            lines.append(f"{self.lock.name} error:  {self.error}")
            return "\n".join(lines)
        if not self.cloned:
            lines.append(f"{self.lock.name} clone:  missing at {self.worktree}")
            lines.append(f"{self.lock.name} note:   the next install, configure or build clones it")
            return "\n".join(lines)
        lines.append(f"{self.lock.name} HEAD:   {self.head} ({self._head_note()})")
        if self.detached:
            lines.append(f"{self.lock.name} branch: detached HEAD (commits here are not on any branch)")
        else:
            lines.append(f"{self.lock.name} branch: {self.branch}{self._upstream_note()}")
        if not self.url_matches:
            lines.append(f"{self.lock.name} origin: {self.remote_url} (expected {self.lock.url})")
        lines.append(f"{self.lock.name} tree:   {self._tree_summary()}")
        return "\n".join(lines)

    def advice(self) -> tuple[str, ...]:
        """Human-readable suggestions. Empty when nothing needs saying."""
        if self.error:
            return (f"{self.lock.name}: {self.error}",)
        if not self.cloned:
            return ()
        notes: list[str] = []
        if self.dirty:
            notes.append(
                f"{self.lock.name} has {self._tree_summary()}; they are used as-is and left untouched. "
                "Commit or push them from inside the workspace when they are ready."
            )
        if not self.in_sync:
            notes.append(
                f"{self.lock.name} HEAD {self.head} is not the locked commit {self.lock.commit} "
                f"({self.drift}); the build continues regardless. To follow the lock: {self.lock.lock_hint}."
            )
        if not self.url_matches:
            notes.append(
                f"{self.lock.name} origin is '{self.remote_url}', but the lock expects '{self.lock.url}'."
            )
        if self.detached:
            notes.append(
                f"{self.lock.name} is on a detached HEAD, so commits made here belong to no branch. "
                f"Run 'git -C {self.lock.directory.as_posix()} switch -c <name>' before committing."
            )
        if self.branch and self.upstream_configured and (self.ahead or self.behind):
            counts = _upstream_counts(self.ahead, self.behind)
            notes.append(
                f"{self.lock.name} '{self.branch}' is {counts} {self.upstream}. "
                f"'git -C {self.lock.directory.as_posix()} pull' to integrate, or push from inside the workspace to publish."
            )
        elif self.branch and not self.upstream_configured:
            notes.append(
                f"{self.lock.name} branch '{self.branch}' has no upstream; "
                "push it from inside the workspace, or set one with 'git branch --set-upstream-to'."
            )
        return tuple(notes)

    def _head_note(self) -> str:
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
    """Run a Git command whose non-zero exit code is handled by the caller."""
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


def _inside_repo(repo_root: Path, relative: Path) -> Path:
    target = (repo_root / relative).resolve()
    if target == repo_root or not target.is_relative_to(repo_root):
        raise RuntimeError(f"Workspace path escapes repository: {target}")
    return target


@contextmanager
def _workspace_lock(repo_root: Path, workspace: SourceWorkspace, timeout: float = 600):
    """An OS lock is released even if a configuring process exits unexpectedly."""
    lock_dir = _inside_repo(repo_root, WORKSPACE_LOCK_DIR)
    with file_lock(lock_dir / f"{workspace.key}.lock", timeout):
        yield


def inspect_workspace(repo_root: Path, workspace: SourceWorkspace) -> WorkspaceStatus:
    """Inspect without fetching or changing the checkout."""
    repo_root = Path(repo_root).resolve()
    worktree = _inside_repo(repo_root, workspace.directory)
    if not worktree.exists():
        return WorkspaceStatus(lock=workspace, worktree=worktree, cloned=False)
    if not (worktree / ".git").exists():
        return WorkspaceStatus(
            lock=workspace, worktree=worktree, cloned=False,
            error=f"'{workspace.directory.as_posix()}' exists but is not a Git checkout. "
                  "Repair it or move it aside before preparing sources again.",
        )
    head = current_commit(worktree)
    branch = _git_or_default(worktree, "symbolic-ref", "--quiet", "--short", "HEAD")
    upstream = _git_or_default(worktree, "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}")
    ahead, behind = _ahead_behind(worktree) if upstream else (0, 0)
    dirty_files = tuple(line for line in _git(
        worktree, "status", "--porcelain", capture=True, quiet=True, keep_leading=True
    ).splitlines() if line.strip())
    return WorkspaceStatus(
        lock=workspace, worktree=worktree, cloned=True, head=head, branch=branch,
        upstream=upstream, remote_url=_git_or_default(worktree, "remote", "get-url", "origin"),
        drift="" if head == workspace.commit else _drift_summary(worktree, head, workspace.commit),
        ahead=ahead, behind=behind, dirty_files=dirty_files,
    )


def _update_submodules(worktree: Path) -> None:
    # Force checkout mode instead of a user's update=merge/rebase setting. Never
    # use --force: Git must refuse an update that would overwrite local edits.
    _git(worktree, "submodule", "update", "--init", "--recursive", "--checkout")


def _clone_locked(repo_root: Path, worktree: Path, workspace: SourceWorkspace) -> None:
    # Claim the final directory before entering the cleanup scope. If it already
    # exists, mkdir fails without touching it. Git accepts an empty destination.
    worktree.mkdir(parents=True, exist_ok=False)
    try:
        run_command(("git", "clone", "--no-checkout", workspace.url, worktree), cwd=repo_root)
        _git(worktree, "fetch", "--tags", "origin", workspace.ref)
        _git(worktree, "cat-file", "-e", f"{workspace.commit}^{{commit}}")
        _git(worktree, "checkout", "-B", workspace.ref, workspace.commit)
        _git(worktree, "branch", "--set-upstream-to", f"origin/{workspace.ref}", workspace.ref)
        _update_submodules(worktree)
    except BaseException:
        # Only remove the directory created by this attempt, so failures can be
        # retried without treating a partial checkout as existing developer work.
        safe_remove(repo_root, worktree)
        raise


def _ensure_workspace(repo_root: Path, workspace: SourceWorkspace) -> WorkspaceStatus:
    worktree = _inside_repo(repo_root, workspace.directory)
    status = inspect_workspace(repo_root, workspace)
    if status.error:
        raise RuntimeError(f"{status.error} (at {worktree})")
    if not worktree.exists():
        _clone_locked(repo_root, worktree, workspace)
        print(f"[INFO] {workspace.name} workspace created at {worktree} (locked commit {workspace.commit})")
        status = inspect_workspace(repo_root, workspace)
    _report(status)
    return status


def ensure_workspace(repo_root: Path, workspace: SourceWorkspace) -> WorkspaceStatus:
    """Create a missing clone, or report and use existing developer sources as-is."""
    repo_root = Path(repo_root).resolve()
    with _workspace_lock(repo_root, workspace):
        return _ensure_workspace(repo_root, workspace)


def _settle_on_branch(status: WorkspaceStatus) -> None:
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
            f"[WARN] {status.lock.name} HEAD is detached at the locked commit and branch '{branch}' was left "
            f"at {tip}: it holds {_commits(count)} the lock does not, and moving it would push "
            "them off the branch. Push or merge them first, or move the branch yourself with "
            f"'git -C {status.lock.directory.as_posix()} branch -f {branch} {status.lock.commit}'."
        )
        return
    if _git_probe(status.worktree, "-c", "submodule.recurse=false", "checkout", "-B", branch, status.lock.commit).returncode != 0:
        print(f"[INFO] {status.lock.name} stays on a detached HEAD: branch '{branch}' is in use elsewhere")
        return
    if created:
        print(f"[INFO] {status.lock.name} is checked out on branch '{branch}'")
    elif fast_forwarded:
        print(f"[INFO] {status.lock.name} branch '{branch}' fast-forwarded to the locked commit")
    else:
        print(f"[INFO] {status.lock.name} is back on branch '{branch}'")


def sync_workspace(repo_root: Path, workspace: SourceWorkspace) -> WorkspaceStatus:
    """Explicitly adopt the code baseline and its submodules without resetting work.

    Git protects conflicting edits. Branches holding extra commits remain intact.
    Submodule failures are fatal and retryable, including when HEAD already matches.
    """
    repo_root = Path(repo_root).resolve()
    with _workspace_lock(repo_root, workspace):
        status = _ensure_workspace(repo_root, workspace)
        if not status.in_sync:
            if not _commit_present(status.worktree, workspace.commit):
                print(f"[INFO] Fetching '{workspace.ref}' from origin to obtain {workspace.commit}")
                # Existing Editor clones may still be shallow; an explicit sync
                # can expand them to retrieve an older pinned commit.
                args = ["fetch", "--tags", "origin", workspace.ref]
                if _git_or_default(status.worktree, "rev-parse", "--is-shallow-repository") == "true":
                    args.insert(1, "--unshallow")
                _git(status.worktree, *args)
                if not _commit_present(status.worktree, workspace.commit):
                    raise RuntimeError(f"Locked {workspace.name} commit {workspace.commit} is not reachable "
                                       f"from '{workspace.ref}' at {workspace.url}; check build_tools/source_workspaces.py.")
            checkout = run_command(
                ("git", "-C", status.worktree, "-c", "submodule.recurse=false",
                 "checkout", "--detach", workspace.commit),
                cwd=repo_root, capture_output=True, quiet=True, check=False,
            )
            if checkout.returncode != 0:
                detail = (checkout.stderr or checkout.stdout).strip()
                raise RuntimeError(
                    f"{workspace.name} could not move to the locked commit {workspace.commit}; "
                    f"Git refused the checkout, so local changes were left untouched. {detail} "
                    f"Commit or stash them inside {workspace.directory.as_posix()} and run "
                    f"'{workspace.sync_command}' again, or keep building your current sources."
                )
            print(f"[INFO] {workspace.name} moved: {status.head} -> {workspace.commit}")
            _settle_on_branch(inspect_workspace(repo_root, workspace))
        else:
            print(f"[INFO] {workspace.name} is already at the locked commit {workspace.commit} ({workspace.ref})")
        _update_submodules(status.worktree)
        print("[INFO] Sources synchronized; run 'python build_tools/dev.py configure' or 'build' "
              "before compiling (build-fast does not refresh dependencies).")
        result = inspect_workspace(repo_root, workspace)
        _report(result)
        return result


def _report(status: WorkspaceStatus) -> None:
    print(status.summary())
    for note in status.advice():
        print(f"[WARN] {note}")
    if status.needs_attention:
        print(f"[WARN] {status.lock.name} differs from the baseline or has local changes; the build continues either way.")
