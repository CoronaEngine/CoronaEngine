"""Verify two packaged editors keep their native state and Web Dock owners isolated."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import traceback
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from profile_process import _Job
from vision_frontend_probe import Frontend
from web_environment_smoke import wait


SERVICE = """(async()=>{const module=await import(document.querySelector('script[type=module]').src);
return Object.values(module).find(value=>typeof value?.openNativePanel==='function');})()"""
STORE = "document.querySelector('#app').__vue_app__.config.globalProperties.$pinia._s.get('dock')"


def targets(port):
    with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=3) as response:
        return [item for item in json.load(response) if item['type'] == 'page'
                and '/CabbageEditor/Frontend/' in item['url']]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('--output', type=Path, default=Path('build/web-multi-instance-smoke'))
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='run-', dir=args.output.resolve()))
    world = output / 'shared-world'
    world.mkdir()
    (world / 'scene.ini').write_text(
        '[format]\ntype=corona_scene_folder\nversion=1\n[scene]\nname=Isolation fixture\n'
        '[world]\ntype=creative\n[actors]\n[scripts]\npath=\n[terrain]\ntype=\npath=\n'
        '[camera]\ncount=1\ncamera0.id=main-camera\ncamera0.name=Main camera\n'
        'camera0.render_backend=vision\ncamera0.width=320\ncamera0.height=240\n', encoding='utf-8')
    (world / 'assets.manifest.json').write_text(
        '{"format":"corona_scene_assets","version":1,"bundles":[]}', encoding='utf-8')
    executable = runtime / 'corona_engine.exe'
    evidence = dict(passed=False, shared_project=str(world), instances=[],
                    engine_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                    frontend_sha256=hashlib.sha256(
                        (runtime / 'CabbageEditor/Frontend/dist/index.html').read_bytes()).hexdigest())
    launches = []
    clients = []
    try:
        # Use one project path and matching numeric tab IDs deliberately. Neither
        # is sufficient authority to route a message between engine instances.
        for index in range(2):
            root = output / str(index)
            root.mkdir()
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                port = listener.getsockname()[1]
            env = dict(os.environ, CORONA_CEF_REMOTE_DEBUG_PORT=str(port),
                       CORONA_CEF_ROOT_CACHE=str(root / 'cache'), CORONA_OPTICS_MAX_FPS='15')
            job = _Job()
            launch = dict(job=job, child=None, port=port, root=root)
            launches.append(launch)
            with (root / 'stdout.log').open('wb') as stdout, (root / 'stderr.log').open('wb') as stderr:
                child = subprocess.Popen([str(executable)], cwd=runtime, env=env,
                                         stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
            launch['child'] = child
            job.attach(child)

            def ready():
                if child.poll() is not None:
                    raise RuntimeError(f'Owned engine {index} exited: {child.returncode}')
                try:
                    return next((item for item in targets(port) if 'uiOwner=' not in item['url']), None)
                except (OSError, TimeoutError):
                    return None

            target = wait(ready, bool, f'engine {index} main', seconds=90)
            client = Frontend(port, target_id=target['id'])
            clients.append(client)
            launch['main'] = client
            wait(lambda: client.evaluate("!!document.querySelector('#app[data-v-app] button')"),
                 bool, f'engine {index} Vue mounted')
            assert client.evaluate('typeof corona.editor') == 'undefined'
            opened = wait(lambda: client.evaluate('corona.project.openProject(' + json.dumps(str(world)) + ')'),
                          lambda value: value.get('status') != 'service_initializing', 'open shared fixture')
            assert opened['ok'], opened
            client.evaluate("location.hash='/'")
            policy = wait(lambda: client.evaluate(f'{SERVICE}.then(s=>s.readEditorUiPolicy())'),
                          lambda value: value and value.get('enabled'), 'production owner ready')
            launch['policy'] = policy
            launch['scene'] = client.evaluate('corona.scene.readActive()')
            client.evaluate(f"{STORE}.openPanel('SceneControls')")
            peer = wait(lambda: next((item for item in targets(port)
                                      if '#/SceneControls?' in item['url'] and 'uiOwner=' in item['url']), None),
                        bool, 'production Dock document')
            dock = Frontend(port, target_id=peer['id'])
            clients.append(dock)
            launch.update(dock=dock, dock_target=peer['id'])
            wait(lambda: dock.evaluate("!!document.querySelector('.scene-quick-light-toggle input:not(:disabled)')"),
                 bool, 'production Dock controls')
            dock_policy = dock.evaluate(f'{SERVICE}.then(s=>s.readEditorUiPolicy())')
            assert dock_policy['enabled'] and dock_policy['owner'] == policy['owner'], dock_policy
            assert dock_policy['generation'] == policy['generation'], dock_policy
            evidence['instances'].append(dict(pid=child.pid, port=port, owner=policy['owner'],
                                               generation=policy['generation'], archive=launch['scene']['archive_session']))
        first, second = launches
        assert first['policy']['owner'] != second['policy']['owner'], evidence
        assert first['scene']['archive_session'] != second['scene']['archive_session'], evidence

        def environment(launch):
            return launch['main'].evaluate("corona.scene.getEnvironment('scene.ini'," + json.dumps(
                {'archive_session': launch['scene']['archive_session']}) + ')')

        original = [environment(launch) for launch in launches]
        for index, launch in enumerate(launches):
            before = environment(launch)['sun']['enabled']
            launch['dock'].evaluate("document.querySelector('.scene-quick-light-toggle input').click()")
            wait(lambda: environment(launch)['sun']['enabled'], lambda value: value is not before,
                 f'engine {index} real Vue sun toggle')
            other = launches[1 - index]
            expected_other = original[1]['sun']['enabled'] if index == 0 else not original[0]['sun']['enabled']
            assert environment(other)['sun']['enabled'] == expected_other
        foreign = second['main'].evaluate("corona.scene.setEnvironment('scene.ini'," + json.dumps({
            'archive_session': first['scene']['archive_session'], 'grid': {'enabled': True}})
            + ').catch(error=>error.code)')
        assert foreign == 'PROJECT_CONTEXT_CHANGED', foreign
        first['main'].evaluate(f"{STORE}.closePanel('SceneControls')")
        wait(lambda: any(item['id'] == first['dock_target'] for item in targets(first['port'])),
             lambda value: not value, 'first instance Dock closed')
        assert any(item['id'] == second['dock_target'] for item in targets(second['port']))
        after = second['dock'].evaluate(f'{SERVICE}.then(s=>s.readEditorUiPolicy())')
        assert after['enabled'] and after['generation'] == second['policy']['generation'], after
        assert environment(second)['sun']['enabled'] is not original[1]['sun']['enabled']
        evidence.update(passed=True, independent_vue_edits=True, foreign_archive_rejected=foreign,
                        independent_dock_close=True, surviving_peer_owner_request=True)
    except Exception:
        evidence['error'] = traceback.format_exc()
        print(evidence['error'], flush=True)
    finally:
        # Keep cleanup restricted to the two launches owned by this runner.
        for client in clients:
            client.socket.close()
        evidence['cleanup_ok'] = True
        for launch in reversed(launches):
            clean = launch['job'].cleanup()
            child = launch['child']
            if child:
                try:
                    child.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    clean = False
            if not clean:
                clean = launch['job'].cleanup()
            launch['job'].close()
            evidence['cleanup_ok'] = evidence['cleanup_ok'] and clean
        evidence['optics_failures'] = [line for launch in launches
            for line in (launch['root'] / 'stdout.log').read_text(encoding='utf-8', errors='replace').splitlines()
            if '[SystemsThread/NodeFail] system=Optics' in line and ' error=' in line]
        evidence['passed'] = evidence['passed'] and evidence['cleanup_ok'] and not evidence['optics_failures']
        (output / 'result.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(dict(output=str(output), **evidence), ensure_ascii=False, indent=2), flush=True)
    return 0 if evidence['passed'] else 1


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    raise SystemExit(main())
