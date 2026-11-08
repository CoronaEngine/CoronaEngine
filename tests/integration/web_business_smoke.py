"""Run migrated Web business probes against a disposable fixture set; owns its processes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import shutil
import time
import traceback
from urllib.request import urlopen

REPO = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(REPO / 'tools'), str(REPO / 'tests/integration')]
from profile_process import _Job
import blockly_actor_file_smoke
import python_archive_smoke
import scene_pull_smoke
import native_file_run_smoke

parser = argparse.ArgumentParser()
parser.add_argument('runtime', type=Path)
parser.add_argument('fixtures', type=Path)
parser.add_argument('stage', choices=['blockly', 'archive', 'scene-pull', 'native-file', 'resources'])
args = parser.parse_args()
runtime, fixtures = args.runtime.resolve(), args.fixtures.resolve()
output = Path(tempfile.mkdtemp(prefix=args.stage + '-', dir=fixtures))
with socket.socket() as listener:
    listener.bind(('127.0.0.1', 0))
    port = listener.getsockname()[1]
env = dict(os.environ, CORONA_CEF_REMOTE_DEBUG_PORT=str(port),
           CORONA_CEF_ROOT_CACHE=str(output / 'cache'), CORONA_OPTICS_MAX_FPS='15')
exe = runtime / 'corona_engine.exe'
if args.stage == 'native-file':
    # Run the newest host alongside the fully deployed engine dependencies.
    source = REPO / 'build/conan/tests/relwithdebinfo/tests/integration/RelWithDebInfo/corona_native_file_probe.exe'
    exe = runtime / source.name
    shutil.copyfile(source, exe)
    control = output / 'control'
    control.mkdir()
    env['CORONA_NATIVE_FILE_PROBE'] = str(control)
evidence = dict(passed=False, stage=args.stage, output=str(output),
                engine_sha256=hashlib.sha256((runtime / 'corona_engine.exe').read_bytes()).hexdigest(),
                frontend_sha256=hashlib.sha256((runtime / 'CabbageEditor/Frontend/dist/index.html').read_bytes()).hexdigest())
if args.stage == 'native-file':
    evidence['native_host_sha256'] = hashlib.sha256(exe.read_bytes()).hexdigest()
job, child = _Job(), None
try:
    with (output / 'stdout.log').open('wb') as stdout, (output / 'stderr.log').open('wb') as stderr:
        child = subprocess.Popen([str(exe)], cwd=runtime, env=env, stdout=stdout,
                                 stderr=stderr, stdin=subprocess.DEVNULL,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        job.attach(child)
        evidence['pid'] = child.pid
        deadline = time.monotonic() + 90
        while True:
            if child.poll() is not None:
                raise RuntimeError(f'Engine exited: {child.returncode}')
            try:
                with urlopen(f'http://127.0.0.1:{port}/json/list', timeout=2) as response:
                    if any(p['type'] == 'page' and '/CabbageEditor/Frontend/' in p['url']
                           for p in json.load(response)):
                        break
            except OSError:
                pass
            if time.monotonic() > deadline:
                raise TimeoutError('CEF target missing')
            time.sleep(.2)
        if args.stage == 'blockly':
            evidence['result'] = blockly_actor_file_smoke.verify(port, fixtures / 'blockly', fixtures / 'peer')
        elif args.stage == 'archive':
            evidence['result'] = python_archive_smoke.verify(port, fixtures / 'archive', fixtures / 'peer')
        elif args.stage == 'scene-pull':
            control = output / 'control'
            control.mkdir()
            evidence['result'] = scene_pull_smoke.run(port, fixtures / 'scene-pull', control)
        elif args.stage == 'native-file':
            reports = sorted(fixtures.glob('blockly-*/result.json'), key=lambda p: p.stat().st_mtime_ns)
            successful = [json.loads(path.read_text(encoding='utf-8')) for path in reports
                          if json.loads(path.read_text(encoding='utf-8')).get('passed')]
            assert successful, 'A successful real Blockly generation is required first'
            relative = successful[-1]['result']['path']
            evidence['result'] = native_file_run_smoke.verify(port, fixtures / 'blockly', fixtures / 'peer', relative, control)
        else:
            import web_resource_import_smoke
            evidence['result'] = web_resource_import_smoke.run(port, fixtures / 'resources', child.pid, REPO)
        evidence['passed'] = True
except Exception:
    evidence['error'] = traceback.format_exc()
finally:
    evidence['cleanup_ok'] = job.cleanup()
    if child:
        try:
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            evidence['cleanup_ok'] = False
    if not evidence['cleanup_ok']:
        evidence['cleanup_ok'] = job.cleanup()
    job.close()
    log = (output / 'stdout.log').read_text(encoding='utf-8', errors='replace')
    evidence['optics_failures'] = [line for line in log.splitlines()
                                  if '[SystemsThread/NodeFail] system=Optics' in line and ' error=' in line]
    evidence['passed'] = evidence['passed'] and evidence['cleanup_ok'] and not evidence['optics_failures']
    (output / 'result.json').write_text(json.dumps(evidence, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(evidence, ensure_ascii=False, indent=2), flush=True)
raise SystemExit(0 if evidence['passed'] else 1)
