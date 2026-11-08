"""Run packaged Vue regressions in a test-owned world and engine process tree."""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import traceback
from urllib.error import URLError
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from profile_process import _Job
from vision_frontend_probe import Frontend
import web_environment_smoke
import web_focus_smoke
import web_graph_target_smoke
import web_actor_properties_smoke
import web_panel_lifecycle_smoke
import web_project_creation_smoke
import web_project_picker_smoke
import web_calibration_smoke
import web_assistant_smoke
import web_world_lifetime_smoke
import web_join_lifetime_smoke
import web_launcher_lifetime_smoke


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('--output', type=Path, default=Path('build/web-packaged-ui-smoke'))
    parser.add_argument('--suite', choices=['focus', 'environment', 'graph', 'properties', 'panels', 'creation', 'picker', 'calibration', 'assistant', 'join', 'launcher', 'lifetime', 'all'], default='all')
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    executable = runtime / 'corona_engine.exe'
    if not executable.is_file():
        parser.error(f'Engine executable missing: {executable}')
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='run-', dir=args.output.resolve()))
    world = output / 'world'
    (world / 'Assets').mkdir(parents=True)
    (world / 'scene.ini').write_text(
        '[format]\ntype=corona_scene_folder\nversion=1\n'
        '[scene]\nname=Packaged UI smoke\n[world]\ntype=creative\n'
        '[actors]\n[scripts]\npath=\n[terrain]\ntype=\npath=\n'
        '[camera]\ncount=2\ncamera0.id=main-camera\ncamera0.name=Main camera\n'
        'camera0.render_backend=vision\ncamera0.width=320\ncamera0.height=240\n'
        'camera1.id=focus-camera\ncamera1.name=Focus camera\ncamera1.render_backend=vision\n'
        'camera1.width=320\ncamera1.height=240\ncamera1.view_open=false\n', encoding='utf-8')
    (world / 'assets.manifest.json').write_text(
        '{"format":"corona_scene_assets","version":1,"bundles":[]}', encoding='utf-8')
    model = world / 'triangle.ply'
    model.write_text('ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\n'
                     'property float y\nproperty float z\nelement face 1\n'
                     'property list uchar int vertex_indices\nend_header\n'
                     '0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n', encoding='ascii')
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    env = dict(os.environ, CORONA_CEF_REMOTE_DEBUG_PORT=str(port),
               CORONA_CEF_ROOT_CACHE=str(output / 'cache'), CORONA_OPTICS_MAX_FPS='15')
    evidence = dict(started_utc=datetime.now(timezone.utc).isoformat(), passed=False,
                    runtime=str(runtime), world=str(world), suite=args.suite, results={},
                    engine_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                    frontend_sha256=hashlib.sha256((runtime / 'CabbageEditor/Frontend/dist/index.html').read_bytes()).hexdigest())
    if args.suite in ('panels', 'all'):
        source_manifest = Path(__file__).resolve().parents[2] / 'CoronaEditor/editor/Frontend/src/config/pluginManifest.js'
        deployed_manifest = runtime / 'CabbageEditor/Frontend/src/config/pluginManifest.js'
        assert source_manifest.read_bytes() == deployed_manifest.read_bytes(), 'deployed panel registry differs from the acceptance registry'
        evidence['panel_manifest_sha256'] = hashlib.sha256(source_manifest.read_bytes()).hexdigest()
    stage = 'launch'
    job, child, client, target = _Job(), None, None, None
    try:
        with (output / 'stdout.log').open('wb') as stdout, (output / 'stderr.log').open('wb') as stderr:
            child = subprocess.Popen([str(executable)], cwd=runtime, env=env,
                                     stdout=stdout, stderr=stderr, stdin=subprocess.DEVNULL,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            job.attach(child)
            evidence['pid'] = child.pid
            def ready():
                if child.poll() is not None:
                    raise RuntimeError(f'Engine exited: {child.returncode}')
                try:
                    with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=2) as response:
                        return next((target for target in json.load(response)
                                     if target['type'] == 'page' and '/CabbageEditor/Frontend/' in target['url']), None)
                except (URLError, TimeoutError):
                    return None
            target = web_environment_smoke.wait(ready, bool, 'packaged editor target', seconds=90)
            client = Frontend(port, target_id=target['id'])
            web_environment_smoke.wait(lambda: client.evaluate('typeof globalThis.corona?.scene?.readActive'),
                                       lambda value: value == 'function', 'typed scene binding')
            for suite, methods in [('creation', ['createWorldProject', 'createMultiplayerProject']),
                                   ('picker', ['openProjectFile'])]:
                if args.suite in (suite, 'all'):
                    stage = f'{suite} binding availability'
                    for method in methods:
                        assert client.evaluate(f'typeof corona.project?.{method}') == 'function', method
            assert client.evaluate('typeof corona.project?.openProject') == 'function', 'openProject'
            stage = 'open disposable creative world'
            opened = client.evaluate('corona.project.openProject(' + json.dumps(str(world)) + ')')
            assert opened.get('ok') and 'data' not in opened, opened
            session = client.evaluate('corona.scene.readActive()')['archive_session']
            created = client.evaluate('corona.scene.createActor("scene.ini",' + json.dumps(str(model)) + ','
                                      + json.dumps({'archive_session': session, 'actor_type': 'model', 'actor_name': 'Probe'}) + ')')
            assert created.get('name') == 'Probe' and created.get('archive_session') == session, created
            client.socket.close()
            client = None
            for name, suite in [('focus', web_focus_smoke), ('environment', web_environment_smoke),
                                ('graph', web_graph_target_smoke), ('properties', web_actor_properties_smoke),
                                ('panels', web_panel_lifecycle_smoke),
                                ('creation', web_project_creation_smoke), ('picker', web_project_picker_smoke),
                                ('calibration', web_calibration_smoke), ('assistant', web_assistant_smoke),
                                ('join', web_join_lifetime_smoke),
                                ('launcher', web_launcher_lifetime_smoke),
                                ('lifetime', web_world_lifetime_smoke)]:
                if args.suite not in ('all', name):
                    continue
                stage = f'packaged Vue {name}'
                print(stage, flush=True)
                if name in ('focus', 'panels', 'picker'):
                    evidence['results'][name] = suite.run(port, world, child.pid)
                elif name == 'creation':
                    evidence['results'][name] = suite.run(port, world, runtime)
                else:
                    evidence['results'][name] = suite.run(port, world)
            evidence['passed'] = True
    except Exception:
        evidence.update(failed_stage=stage, error=traceback.format_exc())
        print(evidence['error'], flush=True)
        if target:
            diagnostic = None
            try:
                diagnostic = Frontend(port, target_id=target['id'])
                evidence['last_document'] = diagnostic.evaluate(
                    '({url:location.href,text:document.body.innerText.slice(0,4000)})')
                screenshot = diagnostic.call('Page.captureScreenshot', {'format': 'png'})
                (output / 'failure.png').write_bytes(base64.b64decode(screenshot['data']))
            except Exception as error:
                evidence['diagnostic_error'] = str(error)
            finally:
                if diagnostic:
                    diagnostic.socket.close()
    finally:
        if client:
            client.socket.close()
        evidence['cleanup_ok'] = job.cleanup()
        if child:
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                evidence['cleanup_ok'] = False
        if not evidence['cleanup_ok']:
            # WDDM may retain a killed GPU process beyond the first bounded
            # wait. Recheck only this launch's Job Object before closing it.
            evidence['cleanup_rechecked'] = True
            evidence['cleanup_ok'] = job.cleanup()
        job.close()
        evidence['passed'] = evidence['passed'] and evidence['cleanup_ok']
        optics_failures = [line for line in (output / 'stdout.log').read_text(encoding='utf-8', errors='replace').splitlines()
                           if '[SystemsThread/NodeFail] system=Optics' in line and ' error=' in line]
        evidence['optics_failures'] = optics_failures
        if optics_failures:
            evidence.setdefault('failed_stage', 'Optics runtime log')
            evidence.setdefault('error', 'Optics failed during packaged UI acceptance; see optics_failures')
        evidence['passed'] = evidence['passed'] and not optics_failures
        (output / 'result.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(dict(output=str(output), **evidence), ensure_ascii=False, indent=2), flush=True)
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    raise SystemExit(main())
