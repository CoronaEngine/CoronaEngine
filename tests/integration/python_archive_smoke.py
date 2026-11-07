"""Run against a disposable portable project and a peer in the real editor."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
from tempfile import TemporaryDirectory

from blockly_actor_file_smoke import wait_for


def verify(port, project, peer):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
    from vision_frontend_probe import Frontend
    web = Frontend(port)

    def v8(method, *args):
        # Only the test host/CDP encodes expressions; V8 receives structured values.
        return web.evaluate(method + "(..." + json.dumps(args) + ")")

    def read_actor(actor, archive_session):
        return v8("corona.scene.readActorProperties", "scene.ini", actor, {"archive_session": archive_session})

    def open_project(root):
        opened = wait_for(lambda: v8("corona.project.openProject", str(root)),
                          lambda r: r.get("status") != "service_initializing", "open archive")
        assert opened["ok"], opened
        return web.evaluate("corona.scene.readActive()")["archive_session"]

    def fingerprint():
        return {p.relative_to(project): (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest())
                for p in project.rglob("*") if p.is_file()}

    wait_for(lambda: web.evaluate("!!document.querySelector('#app[data-v-app] button')"), bool, "editor ready")
    session = open_project(project)
    scope = {"archive_session": session, "project_path": str(project)}
    doc = {**scope, "document": "CabbageAssistant/archive-smoke.json"}
    read = v8("corona.projectSettings.readProjectDocument", doc)
    assert read["success"] and not read.get("revision"), read
    update = {**doc, "content": {"sample": 1}, "revision": read.get("revision", "")}
    saved = v8("corona.projectSettings.commitProjectDocument", update)
    assert saved["success"] and v8("corona.projectSettings.commitProjectDocument", update)["error"] == "DOCUMENT_CONFLICT"

    prefab = v8("corona.files.createActorTemplate", {**scope, "path": "", "name": "Probe template"})
    assert (project / prefab["path"]).is_file()
    v8("corona.files.renameItem", {**scope, "path": prefab["path"], "name": "Renamed.actor"})
    v8("corona.files.deleteItem", {**scope, "path": "Renamed.actor"})
    original = read_actor("Probe", session)
    with TemporaryDirectory(dir=project.parent) as source:
        source = Path(source)
        shutil.copyfile(project / original["model"], source / "shape.ply")
        descriptor = source / "object.actor"
        descriptor.write_text("[base]\nname=Spawned\npath=shape.ply\n[geometry]\nposition=2,3,4\n", encoding="utf8")
        created = v8("corona.scene.createActor", "scene.ini", str(descriptor), {"archive_session": session, "actor_type": "actor", "actor_name": "Spawned"})
        assert created["name"] == "Spawned" and created["actor_guid"], created
    assert v8("corona.scene.save", "scene.ini", {"archive_session": session})["status"] == "saved"
    open_project(peer)
    session = open_project(project)
    assert session != scope["archive_session"]
    stale = v8("corona.projectSettings.commitProjectDocument", {**update, "revision": saved["revision"], "content": "stale"})
    assert stale["error"] == "PROJECT_CONTEXT_CHANGED", stale
    for name, args in (
        ("corona.files.createActorTemplate", [{**scope, "path": "", "name": "stale"}]),
        ("corona.scene.importResourceFile", ["scene.ini", {"archive_session": scope["archive_session"], "file_type": "model"}]),
        ("corona.scene.createActor", ["scene.ini", original["model"], {"archive_session": scope["archive_session"], "actor_type": "model"}]),
    ):
        reply = web.evaluate(name + "(..." + json.dumps(args) +
                             ").then(value=>value, error=>({code:error.code,error:error.message}))")
        assert reply.get("code") == "PROJECT_CONTEXT_CHANGED", (name, reply)
    assert not (project / "stale.actor").exists()
    scope = {**scope, "archive_session": session}
    assert v8("corona.projectSettings.readProjectDocument", {**doc, **scope})["content"] == {"sample": 1}
    actor = read_actor("Spawned", session)
    assert actor["geometry"]["position"] == [2, 3, 4], actor
    script = project / "Scripts" / "remove_actor_for_restore.py"
    script.parent.mkdir(exist_ok=True)
    script.write_text("from api.editor_api import get_script_runtime_editor_api\n"
                      "from script_runtime import file_api\n"
                      "def run():\n    get_script_runtime_editor_api().scene_tools.remove_actor('scene.ini', 'Spawned')\n"
                      "    file_api.mark('removed')\n    while True: file_api.wait(.05)\n", encoding="utf8")
    before = fingerprint()
    start = v8("corona.scripts.runFile", {**scope, "path": script.relative_to(project).as_posix(), "target_type": "project",
                 "scene_name": "scene.ini", "requestId": "archive-restore", "revision": hashlib.sha256(script.read_bytes()).hexdigest()})
    wait_for(lambda: v8("corona.scripts.getStatus"), lambda r: r.get("marker", {}).get("id") == "removed", "runtime removal")
    assert all(a["actor_guid"] != actor["actor_guid"] for a in v8("corona.scene.readActorIndex", "scene.ini", {"archive_session": session})["actors"])
    assert fingerprint() == before
    stop = v8("corona.scripts.stop", start["runId"])
    assert stop["restored"] and not stop["snapshotCaptured"], stop
    restored = read_actor(actor["actor_guid"], session)
    assert restored["geometry"]["position"] == actor["geometry"]["position"] and fingerprint() == before
    print("Python document conflict, stale session, Actor import/reopen and memory-only restore passed", len(before), "files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int)
    parser.add_argument("project", type=Path)
    parser.add_argument("peer", type=Path)
    args = parser.parse_args()
    verify(args.port, args.project.resolve(), args.peer.resolve())
