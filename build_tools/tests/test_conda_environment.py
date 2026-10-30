"""Git and Node installers must serialize mutations to one Conda prefix."""

from pathlib import Path
import subprocess
import sys
import time

import pytest

from build_tools.conda_environment import conda_environment_lock


WORKER = r'''
from pathlib import Path
import subprocess
import sys
import time

from build_tools import git_tool
from build_tools.scripts import ensure_nodejs

prefix = Path(sys.argv[1])
kind, worker = sys.argv[2:4]
sys.prefix = str(prefix)
marker = prefix / (kind + '.installed')

def install(command, **kwargs):
    # Exclusive creation catches overlapping Git/Node installs even if the
    # timing of their event log writes would otherwise hide the overlap.
    active = prefix / 'active-install'
    with active.open('x'):
        with (prefix / 'events').open('a') as events:
            events.write(kind + ':start\n')
        time.sleep(0.15)
        marker.touch()
        with (prefix / 'events').open('a') as events:
            events.write(kind + ':end\n')
    active.unlink()
    return subprocess.CompletedProcess(command, 0)

subprocess.run = install
git_tool.find_conda = lambda prefix: 'test-conda'
git_tool.find_git = lambda prefix, **kwargs: prefix / 'git.exe' if marker.exists() else None
ensure_nodejs.probe_node = lambda prefix: prefix if marker.exists() else None
(prefix / ('ready-' + worker)).touch()
if kind == 'git':
    git_tool.ensure_git()
else:
    ensure_nodejs.ensure_nodejs('test-conda', prefix)
'''


@pytest.mark.parametrize("kinds", [("git", "git"), ("git", "node")])
def test_installers_share_a_lock_and_recheck_after_waiting(tmp_path, kinds):
    prefix = tmp_path / "shared environment"
    (prefix / "conda-meta").mkdir(parents=True)
    workers = []
    try:
        with conda_environment_lock(prefix):
            for index, kind in enumerate(kinds):
                workers.append(subprocess.Popen(
                    [sys.executable, "-B", "-c", WORKER, str(prefix), kind, str(index)],
                    cwd=Path(__file__).resolve().parents[2],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                ))
            deadline = time.monotonic() + 10
            while not all((prefix / f"ready-{index}").exists() for index in range(len(workers))):
                assert time.monotonic() < deadline, "Installer workers did not start"
                assert all(worker.poll() is None for worker in workers), "Installer worker exited early"
                time.sleep(0.02)
            assert not (prefix / "events").exists(), "Installer ignored the environment lock"
        for worker in workers:
            stdout, stderr = worker.communicate(timeout=10)
            assert worker.returncode == 0, stdout + stderr
    finally:
        for worker in workers:
            if worker.poll() is None:
                worker.kill()
                worker.communicate()

    events = (prefix / "events").read_text().splitlines()
    if kinds == ("git", "git"):
        assert events == ["git:start", "git:end"]
    else:
        assert events in (
            ["git:start", "git:end", "node:start", "node:end"],
            ["node:start", "node:end", "git:start", "git:end"],
        )
