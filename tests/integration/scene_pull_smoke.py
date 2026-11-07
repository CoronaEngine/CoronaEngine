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
            # Python callback tokens are owned by the Python callback registry.
            CoronaEngine.unregister_python_script_callback(token)
"""


def wait(read, predicate, label, seconds=18):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        last = read()
        if predicate(last):
            return last
        time.sleep(.1)
    raise AssertionError(f"{label}: {last!r}")


def wait_json_file(path, label):
    def read():
        try:
            return json.loads(path.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return None
    return wait(read, lambda value: value is not None, label)


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
    state = main.evaluate("corona.scene.readActive()")
    scope = {"archive_session": state["archive_session"]}
    def read_actor(actor_id, archive=scope["archive_session"]):
        # Test-host serialization only; the production request and reply use V8 values.
        return main.evaluate(f"corona.scene.readActorProperties({json.dumps(state['scene'])},"
                             f"{json.dumps(actor_id)},{{archive_session:{json.dumps(archive)}}})")
    actor = read_actor("Probe")
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
    main.evaluate(f"{service_lookup}.then(service=>service.openNativePanel({{detached:true, "
                  "panelId:'Object', routePath:'/Object', width:560, height:600, x:100, y:80}))")
    second_target = wait(lambda: [p for p in targets() if p["id"] != main_id and "/Object" in p["url"]],
                         bool, "detached Object window")[0]
    second = Frontend(port, target_id=second_target["id"])
    panels = [main, second]
    wait(lambda: second.evaluate("!!document.querySelector('.object-panel-shell')"), bool, "second Object panel")
    main.evaluate(f"corona.scene.selectActor({json.dumps(state['scene'])}, 'actor', 'Probe')")
    def x(web):
        return web.evaluate("(()=>{const input=document.querySelector('[data-guidance=object-position-x]'); return input?Number(input.value):null})()")
    for panel in panels:
        wait(lambda: x(panel), lambda value: value == initial[0], "initial Actor value")
        panel.evaluate(f"""(async () => {{
          window.probeEvents=[]; window.probeReads=[]; window.probeTokens=[];
          const invoke=corona.editor.invoke;
          corona.editor.invoke=async (api,args) => {{
            if (/^SceneTools\\.(read_|get_|list_|take_)/.test(api)) window.probeReads.push(api);
            return invoke(api,args);
          }};
          for (const event of {json.dumps(EVENTS)}) {{
            const token=await corona.editor.on(event, name => {{
              if (event.startsWith('SceneTools.') || event==='ProjectLauncher.projectOpened') window.probeEvents.push(event);
            }});
            window.probeTokens.push(token);
          }}
        }})()""")
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
            # Simulate hidden windows, not document destruction: pagehide retires
            # the Web messaging owner permanently and cannot be used as a pause.
            panel.evaluate("Object.defineProperty(document,'hidden',{configurable:true,value:true}); document.dispatchEvent(new Event('visibilitychange'))")
        time.sleep(.5)  # Drain requests already issued before pause.
        for panel in panels:
            panel.evaluate("window.probeReads=[];window.probeEvents=[]")
        # The sole V8 transform patch updates the same native state used by pull readers.
        updated = main.evaluate(f"corona.scene.setActorTransform({json.dumps(state['scene'])},{json.dumps(guid)},{json.dumps({**scope, 'position': changed})})")
        assert updated["status"] == "success", updated
        # This command also used to push a scene event even when no queries were in progress.
        main.evaluate(f"corona.scene.selectActor({json.dumps(state['scene'])}, 'actor', 'Probe')")
        time.sleep(.9)
        paused = [panel.evaluate("({reads:window.probeReads,events:window.probeEvents})") for panel in panels]
        assert all(p == {"reads": [], "events": []} for p in paused), paused
        assert [x(p) for p in panels] == [initial[0], initial[0]]
        (control / "resume").touch()
        for panel in panels:
            panel.evaluate("delete document.hidden; document.dispatchEvent(new Event('visibilitychange'))")
        for panel in panels:
            wait(lambda: x(panel), lambda value: value == changed[0], "resumed panel")
        py = wait_json_file(control / "result", "Python reading resumed")
        assert py == {"before": initial, "after": changed, "pausedReads": [], "deliveries": []}, py
        resumed_reads = [p.evaluate("window.probeReads") for p in panels]
        assert all("SceneTools.read_transforms" in reads for reads in resumed_reads), resumed_reads
        report = {"windows": [main_id, second_target["id"]], "actorGuid": guid, "before": initial,
                "pausedWindows": paused, "resumedWindowX": [x(p) for p in panels],
                "resumedWindowReads": [sorted(set(reads)) for reads in resumed_reads], "python": py,
                "vuePropertyWrites": ['body_type', 'mass', 'follow_camera', 'linear_lock', 'camera_lock']}
    finally:
        (control / "resume").touch()
        stopped = api(main, "Scripts.stop", {"runId": started["runId"], "restore_state": True})
        assert stopped.get("restored"), stopped
    restored = api(main, "SceneTools.read_transforms", {**scope, "actors": [guid]})["actors"][0]["position"]
    assert restored == initial, restored
    other = start_file("separate_run.py", f"""
import json, sys
from pathlib import Path
def run():
    result = [str(id(sys.modules)), globals().get('isolation_probe', 'unset')]
    Path({str(control / 'isolation')!r}).write_text(json.dumps(result))
""")
    assert other.get("runId"), {"start": other, "previousStop": stopped}
    try:
        isolated = wait_json_file(control / "isolation", "independent file run")
        assert isolated == [(control / "interpreter").read_text(), "unset"], isolated
        assert other["runId"] != started["runId"]
        assert (root / "scene.ini").read_bytes() == saved_ini
        assert api(main, "Scripts.stop", {"runId": other["runId"]})["restored"]
        # Exercise production Vue event handlers, not only direct binding calls.
        def read_probe():
            return read_actor(guid)
        original_properties = read_probe()
        main.evaluate("[...document.querySelectorAll('#actor-body-type button')].find(b=>b.textContent==='动态').click()")
        wait(read_probe, lambda actor: actor['mechanics']['body_type'] == 'dynamic', 'Vue body-type patch')
        main.evaluate("(()=>{const input=document.querySelector('[data-guidance=object-physics-mass]'); input.value='3'; input.dispatchEvent(new Event('input',{bubbles:true})); input.dispatchEvent(new Event('change',{bubbles:true}));})()")
        wait(read_probe, lambda actor: actor['mechanics']['mass'] == 3, 'Vue mass patch')
        main.evaluate("[...document.querySelectorAll('.object-panel-shell .segmented button')].find(b=>b.textContent==='屏幕 UI').click()")
        wait(read_probe, lambda actor: actor['follow_camera'] is True, 'Vue render-space patch')
        assert read_probe()['mechanics']['body_type'] == 'dynamic', 'render-space does not implicitly change mechanics'
        second.evaluate("[...document.querySelectorAll('#actor-body-type button')].find(b=>b.textContent==='动态').click()")
        wait(lambda: second.evaluate("!!document.querySelector('.lock-row input')"), bool, 'second Vue lock controls')
        second.evaluate("document.querySelector('.lock-row input').click()")
        wait(read_probe, lambda actor: actor['mechanics']['linear_lock'][0] != original_properties['mechanics']['linear_lock'][0], 'second Vue window lock patch')
        main.evaluate("document.querySelector('.switch-label input').click()")
        wait(read_probe, lambda actor: actor['camera_lock']['enabled'] is True, 'Vue camera-lock patch')
        main.evaluate("document.querySelector('.switch-label input').click()")
        wait(read_probe, lambda actor: actor['camera_lock']['enabled'] is False, 'Vue camera-lock disable')
        restore_properties = {**scope, 'visible': original_properties['visible'],
            'follow_camera': original_properties['follow_camera'], 'mechanics': original_properties['mechanics'],
            'camera_lock': {key: original_properties['camera_lock'][key] for key in ['enabled', 'position_offset', 'rotation_offset']}}
        main.evaluate(f"corona.scene.setActorProperties({json.dumps(state['scene'])},{json.dumps(guid)},{json.dumps(restore_properties)})")
        restore_transform = {**scope, **{key: original_properties['geometry'][key] for key in ['position', 'rotation', 'scale']}}
        main.evaluate(f"corona.scene.setActorTransform({json.dumps(state['scene'])},{json.dumps(guid)},{json.dumps(restore_transform)})")

        # Save the Actor from one real window after the other has saved the name.
        new_name = "Two windows"
        moved = [initial[0] + 2, *initial[1:]]
        assert panels[1].evaluate(f"corona.scene.setActorTransform({json.dumps(state['scene'])},{json.dumps(guid)},{json.dumps({**scope, 'position': moved})})")["status"] == "success"
        metadata = panels[0].evaluate('corona.projectSettings.getActiveProjectInfo()')
        panels[0].evaluate('corona.projectSettings.saveActiveProjectInfo(' + json.dumps({
            **scope, 'project_path': metadata['project_path'], 'name': new_name,
            'core_version': metadata['core_version']}) + ')')
        assert api(panels[1], "MainView.scene_save", state["scene"], scope)["ok"]
        assert api(main, "ProjectLauncher.open_project", str(root))["ok"]
        fresh = main.evaluate("corona.scene.readActive()")
        assert fresh["archive_session"] != scope["archive_session"]
        assert main.evaluate('corona.projectSettings.getActiveProjectInfo()')["name"] == new_name
        assert read_actor(guid, fresh["archive_session"])["geometry"]["position"] == moved
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
