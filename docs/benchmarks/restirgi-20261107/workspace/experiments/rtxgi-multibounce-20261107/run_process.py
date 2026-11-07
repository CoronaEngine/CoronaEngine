"""Run one standalone renderer; retain immutable inputs, process logs and GPU telemetry.

Usage: python run_process.py run-spec.json
The run spec supplies executable, args, env, output, and optional timeout_seconds.
This launcher imposes no per-frame wait or render-rate limit.
"""
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

def digest(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def gpu_query():
    return subprocess.run(['nvidia-smi', '--query-gpu=timestamp,name,driver_version,temperature.gpu,power.draw,clocks.current.graphics,clocks.current.memory,utilization.gpu', '--format=csv'], capture_output=True, text=True).stdout

spec_path = Path(sys.argv[1]).resolve()
spec = json.loads(spec_path.read_text(encoding='utf-8-sig'))
exe = Path(spec['executable']).resolve()
output = Path(spec['output']).resolve()
output.mkdir(parents=True, exist_ok=True)
manifest_path = output/'launch-manifest.json'
if manifest_path.exists():
    raise SystemExit(f'Refusing to overwrite an existing run: {output}')
env = os.environ.copy()
env.update({k: str(v) for k, v in spec.get('env', {}).items()})
manifest = dict(spec=spec, spec_path=str(spec_path), executable_sha256=digest(exe),
                working_directory=str(exe.parent), start_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                gpu_before=gpu_query(), wall_seconds=None, exit_code=None)
contract_path = spec.get('experiment', {}).get('scene_contract')
if contract_path:
    contract = json.loads(Path(contract_path).read_text(encoding='utf-8-sig'))
    manifest['frozen_scene_contract'] = contract
    manifest['scene_contract_sha256'] = digest(contract_path)
    (output/'frozen-scene-contract.json').write_text(json.dumps(contract,indent=2),encoding='utf-8')
manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
start = time.perf_counter()
with open(output/'stdout.txt', 'w', encoding='utf-8') as out, open(output/'stderr.txt', 'w', encoding='utf-8') as err:
    process = subprocess.Popen([str(exe), *map(str, spec.get('args', []))], cwd=exe.parent, env=env,
                               stdout=out, stderr=err)
    manifest['pid'] = process.pid
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    try:
        code = process.wait(timeout=spec.get('timeout_seconds', 900))
    except subprocess.TimeoutExpired:
        process.kill()
        code = process.wait()
        manifest['timed_out'] = True
manifest.update(wall_seconds=time.perf_counter()-start, exit_code=code,
                end_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), gpu_after=gpu_query())
manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
print(json.dumps(dict(output=str(output), exit_code=code, wall_seconds=manifest['wall_seconds']), indent=2))
raise SystemExit(0 if code == 0 else 1)
