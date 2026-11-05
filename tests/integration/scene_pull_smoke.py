"""Exercise two real, detached CEF Object panels and an embedded Python file run in an isolated editor."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from vision_frontend_probe import Frontend

EVENTS = ["SceneTools.actorChanged", "SceneTools.actorSelectionChanged", "SceneTools.actorTransformUpdated",
          "SceneTools.sceneTreeChanged", "ProjectLauncher.projectOpened"]

PYTHON_PROBE = """
import json
import sys
import time
from pathlib import Path
import CoronaEngine
from api.editor_api import get_script_runtime_editor_api

def run():
    folder = Path(CONTROL)
    callbacks, reads, tokens = [], [], []
    scene = get_script_runtime_editor_api().scene
    originals = {}
    def counted(original):
        def wrapped(name, args):
            if name.startswith('SceneTools.') or name.startswith('MainView.'):
                reads.append(name)
            return original(name, args)
        return wrapped
    try:
        for name in ('_invoke_cpp_script_api', '_invoke_cpp_editor_api'):
            originals[name] = getattr(CoronaEngine, name)
            setattr(CoronaEngine, name, counted(originals[name]))
        for event in EVENTS:
            token = CoronaEngine.register_python_script_callback(event, lambda *args, event=event: callbacks.append(event))
            assert token, event
            tokens.append(token)
        session = scene.read_active()['archive_session']
        before = scene.read_actor(session, GUID)['geometry']['position']
        archive = scene.read_archive(session)
        assert archive['archive_session'] == session
        assert next(a for a in archive['scene']['actors'] if a['actor_guid'] == GUID)['geometry']['position'] == before
        global isolation_probe
        isolation_probe = 37
        (folder/'interpreter').write_text(str(id(sys.modules)))
        reads.clear()
        (folder/'ready').write_text(json.dumps(before))
        end = time.monotonic() + 25
        while not (folder/'resume').exists():
            if time.monotonic() > end: raise TimeoutError('resume')
            time.sleep(.02)
        during = list(reads)
        after = scene.read_transforms(session, [GUID])['actors'][0]['position']
        (folder/'result').write_text(json.dumps(dict(before=before, after=after, pausedReads=during, deliveries=callbacks)))
    finally:
        for name, value in originals.items(): setattr(CoronaEngine, name, value)
        for token in tokens:
            CoronaEngine._invoke_cpp_editor_api('EditorApi.unregister_callback', json.dumps([token]))
"""


def wait(read, predicate, label, seconds=18):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        last = read()
        if predicate(last):
            return last
        time.sleep(.1)
    raise AssertionError(f"{label}: {last!r}")


def run(port, root, control):
    def targets():
        with urlopen(f"http://127.0.0.1:{port}/json/list", timeout=3) as response:
            return [p for p in json.load(response) if p["type"] == "page" and "/CabbageEditor/Frontend/" in p["url"]]

    main_id = targets()[0]["id"]
    main = Frontend(port, target_id=main_id)
    def api(web, name, *args):
        result = web.api(name, list(args))
        assert result["success"], (name, result)
        return result["data"]

    wait(lambda: main.evaluate("document.readyState === 'complete' && !!document.querySelector('#app[data-v-app] button')"),
         bool, "main frontend mounted")
    opened = wait(lambda: api(main, "ProjectLauncher.open_project", str(root)),
                  lambda r: r.get("status") != "service_initializing", "open")
    assert opened.get("ok"), opened
    state = api(main, "SceneTools.read_active_scene")
    scope = {"archive_session": state["archive_session"]}
    actor = api(main, "SceneTools.read_actor", {**scope, "actor": "Probe"})
    guid, initial = actor["actor_guid"], actor["geometry"]["position"]
    saved_ini = (root / "scene.ini").read_bytes()
    changed = [initial[0] + 5, *initial[1:]]
    tree = api(main, "SceneTools.list_scene_tree", state["scene"], scope)
    def conditional_tree():
        nonlocal tree
        tree = api(main, "SceneTools.list_scene_tree", state["scene"], {**scope, "known_revision": tree["tree_revision"]})
        return tree.get("unchanged")
    # Asset loading may advance the revision while the first snapshot is in flight.
    wait(conditional_tree, bool, "conditional tree read")
    main.evaluate("localStorage.setItem('corona.activeProjectPath'," + json.dumps(str(root)) +
                  "); location.hash='/Object'")
    wait(lambda: main.evaluate("!!document.querySelector('.object-panel-shell')"), bool, "main Object panel")
    # Use the deployed Web session controller so the detached window has a valid owner/generation.
    service_lookup = """(async () => {
      const module = await import(document.querySelector('script[type="module"]').src);
      return Object.values(module).find(value => typeof value?.openNativePanel === 'function');
    })()"""
    wait(lambda: main.evaluate(f"{service_lookup}.then(service=>service.readEditorUiPolicy())"),
         lambda p: p and p.get("enabled"), "main window policy")
    main.evaluate(f"{service_lookup}.then(service=>service.openNativePanel({{cmd:'createDetachedPanel', "
                  "panelId:'Object', routePath:'/Object', width:560, height:600, x:100, y:80}))")
    second_target = wait(lambda: [p for p in targets() if p["id"] != main_id and "/Object" in p["url"]],
                         bool, "detached Object window")[0]
    second = Frontend(port, target_id=second_target["id"])
    panels = [main, second]
    wait(lambda: second.evaluate("!!document.querySelector('.object-panel-shell')"), bool, "second Object panel")
    api(main, "SceneTools.select_actor", state["scene"], "actor", "Probe", {})
    def x(web):
        return web.evaluate("(()=>{const input=document.querySelector('[data-guidance=object-position-x]'); return input?Number(input.value):null})()")
    for panel in panels:
        wait(lambda: x(panel), lambda value: value == initial[0], "initial Actor value")
        panel.evaluate("""(() => {
          window.probeEvents=[]; window.probeReads=[];
          const dispatch=window.__coronaEditorApiDispatch;
          window.__coronaEditorApiDispatch=function(e){
            const name=(typeof e==='string'?JSON.parse(e):e).event;
            if (name.startsWith('SceneTools.') || name==='ProjectLauncher.projectOpened') window.probeEvents.push(name);
            return dispatch?.call(this,e)};
          // The native cefQuery binding is read-only. Count its serialized manifest requests.
          const serialize=JSON.stringify;
          JSON.stringify=function(value,...args){
            if (value && Array.isArray(value.args) && /^SceneTools\\.(read_|get_|list_|take_)/.test(value.api)) {
              window.probeReads.push(value.api);
            }
            return serialize.call(this,value,...args)};
        })()""")
        for event in EVENTS:
            token = api(panel, "EditorApi.register_callback", event, {})
            assert token.get("callback_token"), token
        wait(lambda: panel.evaluate("window.probeReads"), lambda reads: "SceneTools.read_transforms" in reads,
             "window reads the selected Actor before pause")

    def start_file(filename, source):
        path = root / "Scripts" / filename
        path.parent.mkdir(exist_ok=True)
        source = source.encode()
        path.write_bytes(source)
        return api(main, "Scripts.run_file", {**scope, "project_path": str(root), "path": path.relative_to(root).as_posix(),
                   "revision": hashlib.sha256(source).hexdigest(), "target_type": "actor", "scene_name": state["scene"],
                   "actor_name": "Probe", "requestId": filename})

    started = start_file("pull_probe.py", f"CONTROL={str(control)!r}\nGUID={guid!r}\nEVENTS={EVENTS!r}\n" + PYTHON_PROBE)
    assert started.get("runId"), started
    try:
        wait(lambda: (control / "ready").is_file(), bool, "Python reader")
        for panel in panels:
            panel.evaluate("window.dispatchEvent(new Event('pagehide'))")
        time.sleep(.5)  # Drain requests already issued before pause.
        for panel in panels:
            panel.evaluate("window.probeReads=[];window.probeEvents=[]")
        # The fast bridge changes the native geometry; it has no scene-valued reply.
        ok = main.evaluate(f"window.coronaBridge.actorTransform({actor['handle']},0,{json.dumps(changed)})")
        assert ok is not False, ok
        # This command also used to push a scene event even when no queries were in progress.
        api(main, "SceneTools.select_actor", state["scene"], "actor", "Probe", {})
        time.sleep(.9)
        paused = [panel.evaluate("({reads:window.probeReads,events:window.probeEvents})") for panel in panels]
        assert all(p == {"reads": [], "events": []} for p in paused), paused
        assert [x(p) for p in panels] == [initial[0], initial[0]]
        (control / "resume").touch()
        for panel in panels:
            panel.evaluate("window.dispatchEvent(new Event('pageshow'))")
        for panel in panels:
            wait(lambda: x(panel), lambda value: value == changed[0], "resumed panel")
        wait(lambda: (control / "result").is_file(), bool, "Python reading resumed")
        py = json.loads((control / "result").read_text())
        assert py == {"before": initial, "after": changed, "pausedReads": [], "deliveries": []}, py
        resumed_reads = [p.evaluate("window.probeReads") for p in panels]
        assert all("SceneTools.read_transforms" in reads for reads in resumed_reads), resumed_reads
        report = {"windows": [main_id, second_target["id"]], "actorGuid": guid, "before": initial,
                "pausedWindows": paused, "resumedWindowX": [x(p) for p in panels],
                "resumedWindowReads": [sorted(set(reads)) for reads in resumed_reads], "python": py}
    finally:
        (control / "resume").touch()
        api(main, "Scripts.stop", {"runId": started["runId"], "restore_state": True})
    restored = api(main, "SceneTools.read_transforms", {**scope, "actors": [guid]})["actors"][0]["position"]
    assert restored == initial, restored
    other = start_file("separate_run.py", f"""
import json, sys
from pathlib import Path
def run():
    result = [str(id(sys.modules)), globals().get('isolation_probe', 'unset')]
    Path({str(control / 'isolation')!r}).write_text(json.dumps(result))
""")
    try:
        wait(lambda: (control / "isolation").is_file(), bool, "independent file run")
        isolated = json.loads((control / "isolation").read_text())
        assert isolated == [(control / "interpreter").read_text(), "unset"], isolated
        assert other["runId"] != started["runId"]
        assert (root / "scene.ini").read_bytes() == saved_ini
        assert api(main, "Scripts.stop", {"runId": other["runId"]})["restored"]
        # Save the Actor from one real window after the other has saved the name.
        new_name = "Two windows"
        moved = [initial[0] + 2, *initial[1:]]
        api(panels[1], "SceneTools.set_actor_transform", state["scene"], guid, {**scope, "position": moved})
        api(panels[0], "ProjectSettings.save_active_project_info", {**scope, "name": new_name})
        assert api(panels[1], "MainView.scene_save", state["scene"], scope)["ok"]
        assert api(main, "ProjectLauncher.open_project", str(root))["ok"]
        fresh = api(main, "SceneTools.read_active_scene")
        assert fresh["archive_session"] != scope["archive_session"]
        assert api(main, "ProjectSettings.get_active_project_info")["name"] == new_name
        assert api(main, "SceneTools.read_actor", {"archive_session": fresh["archive_session"], "actor": guid})["geometry"]["position"] == moved
        return {**report, "restored": restored, "sceneIniUnchangedDuringRun": True, "sameInterpreter": True,
                "separateRunValue": isolated[1], "twoWindowSave": {"name": new_name, "position": moved}}
    finally:
        api(main, "Scripts.stop", {"runId": other["runId"], "restore_state": True})
        for panel in panels: panel.socket.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int)
    parser.add_argument("project", type=Path)
    parser.add_argument("control", type=Path)
    args = parser.parse_args()
    args.control.mkdir(exist_ok=True)
    print(json.dumps(run(args.port, args.project.resolve(), args.control.resolve()), ensure_ascii=False))
