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

    def call(name, *args):
        response = web.api(name, list(args))
        assert response["success"], (name, response)
        return response["data"]

    def open_project(root):
        opened = wait_for(lambda: call("ProjectLauncher.open_project", str(root)),
                          lambda r: r.get("status") != "service_initializing", "open archive")
        assert opened["ok"], opened
        return call("SceneTools.read_active_scene")["archive_session"]

    def fingerprint():
        return {p.relative_to(project): (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest())
                for p in project.rglob("*") if p.is_file()}

    wait_for(lambda: web.evaluate("!!document.querySelector('#app[data-v-app] button')"), bool, "editor ready")
    session = open_project(project)
    scope = {"archive_session": session, "project_path": str(project)}
    doc = {**scope, "document": "CabbageAssistant/archive-smoke.json"}
    read = call("ProjectSettings.read_project_document", doc)
    assert read["success"] and not read["version"], read
    update = {**doc, "content": {"sample": 1}, "version": read["version"]}
    saved = call("ProjectSettings.commit_project_document", update)
    assert saved["success"] and call("ProjectSettings.commit_project_document", update)["error"] == "DOCUMENT_CONFLICT"

    prefab = call("FileManager.create_file", "", "Probe template", "actor", scope)
    assert (project / prefab["path"]).is_file()
    call("FileManager.rename_item", prefab["path"], "Renamed.actor", scope)
    call("FileManager.delete_item", "Renamed.actor", scope)
    original = call("SceneTools.read_actor", {**scope, "actor": "Probe"})
    with TemporaryDirectory(dir=project.parent) as source:
        source = Path(source)
        shutil.copyfile(project / original["route"], source / "shape.ply")
        descriptor = source / "object.actor"
        descriptor.write_text("[base]\nname=Spawned\npath=shape.ply\n[geometry]\nposition=2,3,4\n", encoding="utf8")
        created = call("SceneTools.create_actor", "scene.ini", str(descriptor), "actor", {**scope, "actor_name": "Spawned"})
        assert created["status"] == "success", created
    assert call("MainView.scene_save", "scene.ini", scope)["ok"]
    open_project(peer)
    session = open_project(project)
    assert session != scope["archive_session"]
    stale = call("ProjectSettings.commit_project_document", {**update, "version": saved["version"], "content": "stale"})
    assert stale["error"] == "PROJECT_CONTEXT_CHANGED", stale
    for name, args in (
        ("FileManager.create_file", ["", "stale", "actor", scope]),
        ("ProjectLauncher.import_portable_asset", [{**scope, "category": "model", "sourcePath": "missing.ply"}]),
        ("SceneTools.create_actor", ["scene.ini", original["route"], "model", scope]),
    ):
        request = json.dumps({"api": name, "args": args})
        reply = web.evaluate("new Promise(resolve => window.cefQuery({request:" + json.dumps(request)
                             + ",onSuccess:r=>resolve(JSON.parse(r)),onFailure:(code,error)=>resolve({code,error})}))")
        assert reply.get("code") == 409 and "PROJECT_CONTEXT_CHANGED" in reply["error"], (name, reply)
    assert not (project / "stale.actor").exists()
    scope = {**scope, "archive_session": session}
    assert call("ProjectSettings.read_project_document", {**doc, **scope})["content"] == {"sample": 1}
    actor = call("SceneTools.read_actor", {**scope, "actor": "Spawned"})
    assert actor["geometry"]["position"] == [2, 3, 4], actor
    script = project / "Scripts" / "remove_actor_for_restore.py"
    script.parent.mkdir(exist_ok=True)
    script.write_text("from api.editor_api import get_script_runtime_editor_api\n"
                      "from script_runtime import file_api\n"
                      "def run():\n    get_script_runtime_editor_api().scene_tools.remove_actor('scene.ini', 'Spawned')\n"
                      "    file_api.mark('removed')\n    while True: file_api.wait(.05)\n", encoding="utf8")
    before = fingerprint()
    start = call("Scripts.run_file", {**scope, "path": script.relative_to(project).as_posix(), "target_type": "project",
                 "scene_name": "scene.ini", "requestId": "archive-restore", "revision": hashlib.sha256(script.read_bytes()).hexdigest()})
    wait_for(lambda: call("Scripts.get_status"), lambda r: r.get("marker", {}).get("id") == "removed", "runtime removal")
    assert all(a["actor_guid"] != actor["actor_guid"] for a in call("SceneTools.get_scene_snapshot", "scene.ini")["actors"])
    assert fingerprint() == before
    stop = call("Scripts.stop", {"runId": start["runId"]})
    assert stop["restored"] and not stop["snapshotCaptured"], stop
    restored = call("SceneTools.read_actor", {**scope, "actor": actor["actor_guid"]})
    assert restored["geometry"]["position"] == actor["geometry"]["position"] and fingerprint() == before
    print("Python document conflict, stale session, Actor import/reopen and memory-only restore passed", len(before), "files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int)
    parser.add_argument("project", type=Path)
    parser.add_argument("peer", type=Path)
    args = parser.parse_args()
    verify(args.port, args.project.resolve(), args.peer.resolve())
