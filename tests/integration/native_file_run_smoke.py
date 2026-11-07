"""Run a Web-generated file through native_file_probe, with no Blockly panel open."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
import uuid

from blockly_actor_file_smoke import wait_for, dispatch_script_key


def verify(port, project, peer, relative, control):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
    from vision_frontend_probe import Frontend

    web = Frontend(port)
    sequence = 0

    def call(name, *args, reject=False):
        nonlocal sequence
        tmp = control / "command.tmp"
        tmp.write_text(json.dumps({"api": name, "args": args}), encoding="utf8")
        tmp.replace(control / f"{sequence}.request")
        reply = control / f"{sequence}.reply"
        sequence += 1
        wait_for(reply.exists, bool, name)
        result = json.loads(reply.read_bytes())
        if reject:
            assert not result["success"] and "PROJECT_CONTEXT_CHANGED" in result["error"], result
            return
        assert result["success"], result
        return result["data"]

    def view(name, *args):
        reply = web.api(name, list(args))
        assert reply["success"], reply
        return reply["data"]

    def open_project(path):
        result = wait_for(lambda: view("ProjectLauncher.open_project", str(path)),
                          lambda r: r.get("status") != "service_initializing", "open project")
        assert result["ok"], result
        return web.evaluate("corona.scene.readActive()")["archive_session"]

    def position():
        snapshot = view("SceneTools.get_scene_snapshot", "scene.ini")
        return next(a["geometry"]["position"] for a in snapshot["actors"] if a["name"] == "Probe")

    def files():
        return {p: hashlib.sha256(p.read_bytes()).hexdigest() for root in (project, peer)
                for p in root.rglob("*") if p.is_file()}

    def tap(code="KeyD"):
        dispatch_script_key(web, 'keyDown', code)
        dispatch_script_key(web, 'keyUp', code)

    wait_for(lambda: web.evaluate("!!document.querySelector('#app[data-v-app] button')"), bool, "editor ready")
    session = open_project(project)
    assert call("SceneTools.read_active_scene")["archive_session"] == session
    source = (project / relative).read_bytes()
    assert b"from script_runtime import file_api" in source and b"script_runtime.engine" not in source
    # A native caller also selects ordinary .py files using only a scoped path.
    plain = project / "Scripts" / "native_probe.py"
    plain.write_text("import sys\nfrom pathlib import Path\nfrom script_runtime import file_api\n"
                     "def run():\n    global marker\n    assert 'marker' not in globals()\n    marker = 1\n"
                     f"    (Path({str(control)!r}) / file_api.current_run().id).write_text(str(id(sys.modules)))\n",
                     encoding="utf-8")
    initial, archived = position(), files()
    moved = [initial[0] + 1, *initial[1:]]
    base = {"archive_session": session, "path": relative, "project_path": str(project),
            "revision": hashlib.sha256(source).hexdigest(), "target_type": "actor",
            "actor_name": "Probe", "scene_name": "scene.ini"}
    runs = []
    for ending in ("stop", "stop", "reload"):
        assert "nodegraph" not in web.evaluate("location.hash.toLowerCase()")
        request = dict(base, requestId=uuid.uuid4().hex)
        if not runs:
            started = call("Scripts.run_file", request)
        else:
            started = call("Scripts.run_path", {k: request[k] for k in ("archive_session", "path", "requestId", "actor_name")})
        run = started["runId"]
        assert run not in runs
        wait_for(lambda: call("Scripts.get_status"),
                 lambda s: s.get("runId") == run and s.get("marker", {}).get("id") == "start", "file ready")
        assert call("Scripts.get_run", {"requestId": request["requestId"]})["runId"] == run
        if runs:
            assert call("Scripts.stop", {"runId": runs[-1]})["code"] == "STALE_RUN"
        runs.append(run)
        # A non-US layout can produce 'd' from a different physical key.
        tap("KeyJ" if len(runs) == 1 else "KeyD")
        wait_for(position, lambda v: v == moved, "D moves exactly once")
        if ending == "reload":
            assert view("SceneTools.reload_scene", "scene.ini", str(project))["status"] == "success"
        else:
            assert call("Scripts.stop", {"runId": run})["restored"]
        stopped = call("Scripts.get_status")
        assert not stopped["threadAlive"] and not stopped["snapshotCaptured"], stopped
        tap()
        time.sleep(.15)
        assert position() == initial and files() == archived
    interpreters = []
    for _ in range(2):
        started = call("Scripts.run_path", {"archive_session": call("SceneTools.read_active_scene")["archive_session"],
                       "requestId": uuid.uuid4().hex, "path": "Scripts/native_probe.py"})
        run = started["runId"]
        wait_for(lambda: call("Scripts.get_status"), lambda r: r.get("status") == "completed", "plain file")
        interpreters.append((control / run).read_text())
        assert call("Scripts.stop", {"runId": run})["restored"]
    assert len(set(interpreters)) == 1
    assert open_project(peer) != session
    assert open_project(project) != session
    call("Scripts.run_path", {"archive_session": session, "path": relative, "requestId": uuid.uuid4().hex}, reject=True)
    assert position() == initial and files() == archived
    return {"nativeRuns": runs, "before": initial, "moved": moved, "restored": position(),
            "plainFileModuleTable": interpreters[0], "directReload": True, "staleStartRejected": True, "filesUnchanged": len(archived)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int)
    parser.add_argument("project", type=Path)
    parser.add_argument("peer", type=Path)
    parser.add_argument("relative")
    parser.add_argument("control", type=Path)
    parser.add_argument("--trace", type=Path)
    args = parser.parse_args()
    report = verify(args.port, args.project.resolve(), args.peer.resolve(), args.relative, args.control)
    if args.trace:
        rows = [json.loads(line) for line in args.trace.read_text(encoding="utf-8").splitlines()]
        assert any(row["event"] == "archive" for row in rows)
        assert [row["id"] for row in rows if row["event"] == "execute"] == report["nativeRuns"]
        assert len({(r["pid"], r["interpreter"], r["prefix"], r["modules"]) for r in rows}) == 1
        assert str(rows[0]["modules"]) == report["plainFileModuleTable"]
    print(json.dumps(report))
