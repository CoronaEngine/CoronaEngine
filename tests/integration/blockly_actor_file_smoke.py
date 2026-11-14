"""Exercise the deployed node panel in a dedicated editor with a fresh archive fixture.

Usage: python blockly_actor_file_smoke.py <local CEF debug port> <fixture directory>
Create the Probe actor with corona_archive_roundtrip_tests fixture before running.

For the interpreter/timeout check, call install_host_probe(editor, trace_path)
after bootstrap in the isolated test deployment only. Supply the same fresh
trace file and the engine stdout log using --trace and --engine-log.
"""
import argparse
import hashlib
import json
import os
import sys
import threading
import time
from pathlib import Path
from urllib.request import urlopen
from urllib.parse import parse_qs, urlsplit

def open_web_project(web, path):
    """Use the production Web owner so old-window saves finish before replacement."""
    return web.evaluate("""(async()=>{
      const module=await import(document.querySelector('script[type=module]').src);
      const launcher=Object.values(module).find(value=>typeof value?.openProject==='function'
        &&!value.createWorldProject);
      if(!launcher)throw Error('Production project launcher missing');
      return launcher.openProject(""" + json.dumps(str(path)) + ");})()")

def install_host_probe(editor, trace_path, *, delay_first=True):
    """Record the interpreter actually parsing the archive and executing the file."""
    import _interpreters
    trace_lock = threading.Lock()

    def record(event, identity=None):
        row = {"event": event, "id": identity, "pid": os.getpid(),
               "interpreter": int(_interpreters.get_current()[0]), "modules": id(sys.modules),
               "prefix": sys.prefix}
        with trace_lock, Path(trace_path).open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(row) + "\n")

    def audit(event, args):
        if event == "exec" and "/scripts/files/" in args[0].co_filename.replace("\\", "/").lower():
            ctx = sys._getframe(1).f_locals["ctx"]
            record("execute", ctx.id)
        elif event == "open" and str(args[0]).replace("\\", "/").endswith("/scene.ini"):
            frame = sys._getframe(1)
            while frame is not None:
                if frame.f_code.co_name == "parse_archive":
                    record("archive")
                    break
                frame = frame.f_back

    dispatch = editor.dispatch_script_request_from_cpp
    delayed = not delay_first

    def dispatch_with_delayed_receipt(cls, message):
        nonlocal delayed
        response = dispatch(message)
        request = json.loads(message)
        if request["module"] == "ProjectArchive" and request["function"] == "save":
            record("save", json.loads(response)["data"].get("status"))
        if request["module"] == "FileScripts" and request["function"] == "run_file" and not delayed:
            delayed = True
            record("delay", request["args"][0]["requestId"])
            time.sleep(2.8)  # Longer than the 2 s native service deadline.
        elif request["module"] == "FileScripts" and request["function"] == "get_file_run":
            record("lookup", request["args"][0]["requestId"])
        elif request["module"] == "FileScripts" and request["function"] in ("stop_file", "stop_script_execution"):
            if json.loads(response).get("data", {}).get("restored"):
                from api.editor_api import get_script_runtime_editor_api
                scene = get_script_runtime_editor_api().scene.get_snapshot()
                actor = next(a for a in scene["actors"] if a["name"] == "Probe")
                record("restored", {"project": scene["project_path"], "position": actor["geometry"]["position"]})
        return response

    record("bootstrap")
    sys.addaudithook(audit)
    editor.dispatch_script_request_from_cpp = classmethod(dispatch_with_delayed_receipt)


def verify_trace(trace_path, engine_log, report):
    rows = [json.loads(line) for line in trace_path.read_text(encoding="utf-8").splitlines()]
    assert engine_log.read_text(encoding="utf-8", errors="replace").count(
        "PythonAPI: Python interpreter initialized successfully") == 1
    assert len({(r["pid"], r["interpreter"], r["modules"], r["prefix"]) for r in rows}) == 1, rows
    assert sum(r["event"] == "bootstrap" for r in rows) == 1 and any(r["event"] == "archive" for r in rows)
    runs = report["runs"]
    assert any(r["event"] == "save" and r["id"] == "success" for r in rows), rows
    executions = [run["runId"] for run in runs]
    lookups = [runs[0]["requestId"], runs[0]["requestId"], runs[1]["requestId"]]
    if "switch" in report:
        switch = report["switch"]
        executions.append(switch["runId"])
        lookups.append(switch["requestId"])
        restored = [r for r in rows if r["event"] == "restored"][-1]
        assert Path(restored["id"]["project"]).resolve() == Path(switch["from"]), restored
        assert restored["id"]["position"] == runs[0]["before"]
        assert any(r["event"] == "archive" for r in rows[rows.index(restored) + 1:])
    assert [r["id"] for r in rows if r["event"] == "execute"] == executions
    assert [r["id"] for r in rows if r["event"] == "delay"] == [runs[0]["requestId"]]
    # First run has a recovery lookup in addition to the explicit probe query.
    assert [r["id"] for r in rows if r["event"] == "lookup"] == lookups


def wait_for(read, condition, description):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        value = read()
        if condition(value):
            return value
        time.sleep(0.1)
    raise AssertionError(f"{description}: {value!r}")


def dispatch_script_key(web, event_type, code='KeyD', key='d', *, port):
    """Send scoped input from a real CameraView owning the selected viewport."""
    from vision_frontend_probe import Frontend

    scope = web.evaluate('corona.scene.readActive()')
    cameras = web.evaluate('corona.scene.readScene(' + json.dumps({
        'scene': scope['scene'], 'archive_session': scope['archive_session'], 'cameras': True}) + ')')
    camera_id = cameras['cameras'][0]['camera_id']
    event = {'archive_session': scope['archive_session'], 'type': event_type, 'code': code, 'key': key}
    if event_type == 'keyDown':
        event['modifiers'] = ''
    service = """(async()=>{const m=await import(document.querySelector('script[type=module]').src);
      return Object.values(m).find(v=>typeof v?.openNativePanel==='function');})()"""
    wait_for(lambda: web.evaluate(f'{service}.then(s=>s.readEditorUiPolicy())'),
             lambda policy: policy and policy.get('enabled'), 'file script window owner ready')
    camera = {**cameras['cameras'][0], 'scene_id': scope['scene'],
              'archive_session': scope['archive_session'], 'view_width': 320, 'view_height': 240}
    created = web.evaluate(f'{service}.then(s=>s.createCameraView({json.dumps(camera)}))')

    def target():
        with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
            for item in json.load(response):
                fragment = urlsplit(item['url']).fragment
                if item['type'] != 'page' or not fragment.startswith('/CameraView?'):
                    continue
                query = parse_qs(fragment.split('?', 1)[1], keep_blank_values=True)
                if query.get('camera') == [camera_id] and query.get('archive') == [scope['archive_session']]:
                    return item

    page = wait_for(target, bool, 'file script CameraView')
    viewport = Frontend(port, target_id=page['id'])
    try:
        # The target was selected by the camera and archive in its URL, which is
        # stronger evidence than a tab id was.
        wait_for(lambda: viewport.evaluate("!!document.querySelector('.input-layer')"),
                 bool, 'file script CameraView mounted')
        result = viewport.evaluate(f'corona.input.dispatch({json.dumps(scope["scene"])},{json.dumps(camera_id)},{json.dumps(event)})')
        assert result == {'status': 'queued'}, result
    finally:
        viewport.socket.close()


def verify(port, root, switch_root=None):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
    from vision_frontend_probe import Frontend

    web = Frontend(port)

    def v8(method, *args):
        # JSON belongs to the test host/CDP only; arguments reach V8 as values.
        return web.evaluate(method + "(..." + json.dumps(args) + ")")

    def bridge(service, function, *args):
        # Documents and script runs answer through the one Web -> Python entry.
        envelope = json.dumps({"service": service, "function": function, "args": list(args)})
        return json.loads(web.evaluate("corona.bridge.call(" + json.dumps(envelope) + ")"))

    def project_document(function, request):
        return bridge("ProjectFiles", function, request)

    def script_status():
        return bridge("FileScripts", "get_script_status")

    def position():
        return web.evaluate("(async()=>{const s=await corona.scene.readActive();return "
                            "(await corona.scene.readActorProperties(s.scene,'Probe',{archive_session:s.archive_session})).geometry.position;})()")

    def project_files(project=root):
        return {p.relative_to(project).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in project.rglob("*") if p.is_file()}

    def scope():
        return web.evaluate("corona.scene.readActive()")["archive_session"]

    def edit_position(value):
        result = web.evaluate(f"corona.scene.setActorTransform('scene.ini','Probe',{json.dumps({'position': value, 'archive_session': scope()})})")
        assert result["status"] == "success" and position() == value, result

    def key_down():
        dispatch_script_key(web, 'keyDown', port=port)

    def key_up():
        dispatch_script_key(web, 'keyUp', port=port)

    wait_for(lambda: web.evaluate("document.readyState === 'complete' && !!document.querySelector('#app[data-v-app] button')"),
             bool, "editor startup page")
    if switch_root:
        peer = wait_for(lambda: open_web_project(web, switch_root),
                        lambda r: r.get("status") != "service_initializing", "archive service")
        assert peer.get("ok"), peer
        peer_start = position()
        assert peer_start != [0, 0, 0], "the switch fixture needs a distinct starting position"
    opened = wait_for(lambda: open_web_project(web, root),
                      lambda r: r.get("status") != "service_initializing", "archive service")
    assert opened.get("ok"), opened
    initial = position()
    start = [initial[0] + 3, initial[1] + 2, initial[2] + 1]
    before_edit = project_files()
    edit_position(start)
    assert project_files() == before_edit, "An ordinary Actor edit wrote to disk"
    committed = v8("corona.scene.save", "scene.ini", {"archive_session": scope()})
    assert committed["status"] == "saved", committed
    after_save = project_files()
    assert {key for key in after_save if after_save[key] != before_edit.get(key)} == {"scene.ini"}
    assert after_save.keys() == before_edit.keys()
    edit_position(initial)
    assert project_files() == after_save
    assert open_web_project(web, root)["ok"]
    assert position() == start, "Reopen must read the saved edit, not the latest in-memory Actor"
    assert project_files() == after_save
    target_fields = {"SCOPE": "SCENE", "v": "presses", "x": 1}
    count = {"type": "variable_add", "id": "count-d", "fields": target_fields}
    move = {"type": "object_move_direction", "id": "move-probe",
            "fields": {"NAME": "Probe", "DIRECTION": "RIGHT", "SPEED": 20}, "next": {"block": count}}
    seen = {"type": "variable_exists", "id": "seen-d", "fields": {"SCOPE": "SCENE", "NAME": "presses"}}
    once = {"type": "control_else", "id": "once-per-run",
            "inputs": {"CONDITION": {"block": seen}, "ELSE": {"block": move}}}
    key = {"type": "event_keyboard", "id": "press-d", "x": 0, "y": 0, "fields": {"x": "KeyD"},
           "inputs": {"DO": {"block": once}}}
    graph = {"version": 1, "nodes": [{"id": "start", "nodeType": "start",
             "workspace": {"blocks": {"languageVersion": 0, "blocks": [key]}}}], "edges": []}
    first_session = scope()
    target = {"target_type": "actor", "scene_name": "scene.ini", "actor_name": "Probe", "script_kind": "node_graph",
              "id": "node_graph:actor:scene.ini:Probe", "enabled": True, "runnable": True, "validation_errors": []}
    locator = {"identity": ["node_graph", "actor", "scene.ini", "Probe"],
               "archive_session": first_session, "project_path": str(root)}
    saved = project_document("save", {**locator, "document": {"target": target, "workspace": graph},
                "code": "raise RuntimeError('node panel did not generate the script')"})
    assert saved["status"] == "saved", saved
    web.evaluate("localStorage.setItem('corona.activeProjectPath'," + json.dumps(str(root)) +
                 "); location.hash = '/NodeGraph'")
    select = "document.querySelector('select[aria-label=\"运行目标\"]')"
    wait_for(lambda: web.evaluate(f"[...({select}?.options ?? [])].map(o=>o.value)"),
             lambda values: "Probe" in values, "actor selector")
    web.evaluate(f"(()=>{{const s={select};s.value='Probe';s.dispatchEvent(new Event('change'));}})()")
    wait_for(lambda: web.evaluate(f"{select}?.value"), lambda value: value == "Probe", "actor graph")
    wait_for(lambda: web.evaluate("!!document.querySelector('.ng-node.type-start')"), bool, "saved Actor graph loaded")
    saved_files = None
    saved_workspace = None
    run_button = "document.querySelector('[data-guidance=\"node-run\"]')"
    runs = []
    for _ in range(2):
        web.evaluate(f"{run_button}.click()")
        wait_for(lambda: web.evaluate(f"{run_button}.classList.contains('running')"), bool, "run accepted by panel")
        status = script_status
        running = wait_for(status, lambda r: r.get("status") == "running" and r.get("marker", {}).get("id") == "start",
                           "generated file running")
        saved = project_document("load", locator)
        path = root / saved["code_path"]
        script = path.read_bytes()
        assert b"def run():" in script and b"KeyD" in script, script
        assert b"from script_runtime import file_api" in script and b"script_runtime.engine" not in script, script
        if saved_files is None:
            # The first run publishes Web's generated file in place of the test sentinel.
            saved_files = project_files()
            assert saved_files["scene.ini"] == after_save["scene.ini"]
            saved_workspace = (root / saved["document_path"]).read_text(encoding="utf-8")
        else:
            current = project_files()
            if current != saved_files:
                changes = [k for k in sorted(current.keys() | saved_files.keys()) if current.get(k) != saved_files.get(k)]
                raise AssertionError(("Run modified archive", changes, saved_workspace,
                                      (root / saved["document_path"]).read_text(encoding="utf-8")))
        request_id = running["requestId"]
        assert bridge("FileScripts", "get_file_run", {"requestId": request_id})["runId"] == running["runId"]
        replay = bridge("FileScripts", "run_file", {
            "archive_session": first_session,
            "project_path": str(root), "path": saved["code_path"], "revision": hashlib.sha256(script).hexdigest(),
            "target_type": "actor", "scene_name": "scene.ini", "actor_name": "Probe", "requestId": request_id})
        assert replay["runId"] == running["runId"]
        try:
            key_down()
            moved = wait_for(position, lambda p: p != start, "key moved actor")
            assert moved == [start[0] + 1, *start[1:]], moved
            assert project_files() == saved_files, "Starting the run or moving the Actor changed the archive"
            blocked = web.evaluate("corona.scene.save('scene.ini'," + json.dumps({"archive_session": scope()}) +
                                   ").then(()=>null,error=>({code:error.code,message:error.message}))")
            assert blocked and blocked["code"] == "SAVE_FAILED", blocked
            key_up()
            key_down()
            key_up()
            time.sleep(.2)
            assert position() == moved, "SCENE variable did not survive within this run"
        finally:
            web.evaluate(f"{run_button}.click()")
        stopped = wait_for(status, lambda r: r.get("status") == "stopped", "button stopped run")
        assert not stopped["snapshotCaptured"] and not stopped["inputLocked"], stopped
        assert position() == start
        assert project_files() == saved_files, "Stopping the script changed project files"
        key_down()
        key_up()
        time.sleep(0.2)
        assert position() == start
        runs.append({"runId": running["runId"], "requestId": request_id, "before": start,
                     "moved": moved, "afterStopAndKey": position()})
    assert runs[0]["runId"] != runs[1]["runId"] and runs[0]["requestId"] != runs[1]["requestId"]
    report = {"path": saved["code_path"], "runs": runs,
              "save": {"initial": initial, "reopened": start, "sceneSha256": saved_files["scene.ini"],
                       "unchangedFilesDuringRun": len(saved_files)}}
    if switch_root:
        # Start outside the graph panel so closing that panel does not own this
        # run. Input must originate from a live viewport before entering the
        # launcher, which intentionally retires editor windows.
        web.evaluate("location.hash='/'")
        wait_for(lambda: web.evaluate("!!document.querySelector('[data-viewport-surface]')"),
                 bool, 'main editor before switch run')
        peer_path = json.dumps(str(switch_root).replace("\\", "/").lower())
        row = ("[...document.querySelectorAll('.cursor-pointer')].find(e => "
               "e.innerText.toLowerCase().replaceAll(String.fromCharCode(92), '/').includes(" + peer_path + "))")
        switch_request = "switch-" + runs[-1]["requestId"]
        active = bridge("FileScripts", "run_file", {"archive_session": first_session, "requestId": switch_request, "project_path": str(root),
                     "path": report["path"], "revision": hashlib.sha256(script).hexdigest(),
                     "target_type": "actor", "actor_name": "Probe", "scene_name": "scene.ini"})
        wait_for(status, lambda r: r.get("runId") == active.get("runId") and r.get("marker", {}).get("id") == "start", "switch run")
        key_down()
        wait_for(position, lambda p: p == moved, "move in old project")
        key_up()
        web.evaluate("location.hash='/RecentGames'")
        wait_for(lambda: web.evaluate("!!(" + row + ")"), bool, "switch target in Recent Projects")
        still_running = status()
        assert still_running.get('runId') == active['runId'] and still_running.get('threadAlive'), still_running
        assert position() == moved, 'Entering the launcher must not restore a native-owned run'
        web.evaluate("(" + row + ").dispatchEvent(new MouseEvent('dblclick', {bubbles:true}))")
        wait_for(lambda: web.evaluate('corona.scene.readActive()'),
                 lambda s: Path(s["project_path"]).resolve() == switch_root, "project switch")
        assert position() == peer_start
        assert project_files() == saved_files
        assert bridge("FileScripts", "get_file_run", {"requestId": switch_request})["status"] == "stopped"
        assert not status()["snapshotCaptured"]
        key_down()
        key_up()
        time.sleep(0.2)
        assert position() == peer_start
        report["switch"] = {"runId": active["runId"], "requestId": switch_request, "from": str(root),
                            "to": str(switch_root), "newProjectPosition": peer_start}
        # Session-rejection checks do not exercise MainPage's assistant event
        # persistence. Keep a real editor owner without mounting that producer
        # again when the archive is reopened below.
        web.evaluate("location.hash='/SceneBar'")
        wait_for(lambda: web.evaluate("!!document.querySelector('[data-guidance=scene-import-model]')"
                                      "&&!document.querySelector('[data-viewport-surface]')"),
                 bool, 'scope isolation editor owner')
        peer_files = project_files(switch_root)
        stale_target = {"archive_session": first_session, "project_path": str(root),
                        "target_type": "actor", "scene_name": "scene.ini", "actor_name": "Probe"}
        stale_run = {**stale_target, "requestId": "late-start", "path": report["path"],
                     "revision": hashlib.sha256(script).hexdigest()}
        def verify_rejected():
            for function, payload in (
                ("load", locator),
                ("save", {**locator, "document": {}, "code": "raise AssertionError()"}),
            ):
                reply = project_document(function, payload)
                assert reply.get("code") == "PROJECT_CONTEXT_CHANGED", reply
            rejected = bridge("FileScripts", "run_file", stale_run)
            assert rejected.get("code") == "PROJECT_CONTEXT_CHANGED", rejected
            assert not status()["snapshotCaptured"]
            current, current_peer = project_files(), project_files(switch_root)
            assert current == saved_files and current_peer == peer_files, {
                'project_changed': [key for key in current.keys() | saved_files.keys()
                                    if current.get(key) != saved_files.get(key)],
                'peer_changed': [key for key in current_peer.keys() | peer_files.keys()
                                 if current_peer.get(key) != peer_files.get(key)],
            }
            return scope()

        sessions = [first_session, verify_rejected()]
        assert open_web_project(web, root).get("ok")
        sessions.append(verify_rejected())
        assert position() == start
        assert project_document("load", {**locator, "archive_session": sessions[-1]})["status"] == "loaded"
        assert open_web_project(web, root).get("ok")
        sessions.append(verify_rejected())
        assert len(set(sessions)) == 4, sessions
        report["sessionIsolation"] = {"openings": sessions, "staleLoadSaveRunRejected": True,
                                      "unchangedArchives": [str(root), str(switch_root)]}
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=int)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--engine-log", type=Path)
    parser.add_argument("--switch-fixture", type=Path)
    args = parser.parse_args()
    if bool(args.trace) != bool(args.engine_log):
        parser.error("--trace and --engine-log must be supplied together")
    report = verify(args.port, args.fixture.resolve(), args.switch_fixture.resolve() if args.switch_fixture else None)
    if args.trace:
        verify_trace(args.trace, args.engine_log, report)
    print(json.dumps(report))
