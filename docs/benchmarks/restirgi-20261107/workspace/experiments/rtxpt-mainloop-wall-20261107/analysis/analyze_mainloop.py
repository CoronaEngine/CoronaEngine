"""Audit native buffered main-loop timing CSVs; no renderer execution."""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[1]
HOST = ['wall_ms', 'animate_host_ms', 'beginframe_gap_host_ms', 'render_host_ms',
        'present_host_ms', 'other_host_ms', 'start_interval_ms']
GPU = ['Frame.RenderGPU', 'GI.Exclusive', 'GI.WithPreparation']


def require(condition, message):
    if not condition:
        raise ValueError(message)


def stats(values):
    values = sorted(values)
    def percentile(p):
        rank = (len(values) - 1) * p
        lo = int(rank)
        hi = min(lo + 1, len(values) - 1)
        return values[lo] + (values[hi] - values[lo]) * (rank - lo)
    return dict(count=len(values), mean_ms=statistics.fmean(values),
                median_ms=statistics.median(values), p10_ms=percentile(.1),
                p90_ms=percentile(.9), p95_ms=percentile(.95),
                min_ms=values[0], max_ms=values[-1])


def read_csv(path):
    with path.open(newline='', encoding='utf-8-sig') as stream:
        return list(csv.DictReader(stream))


def audit_metadata(directory, offset):
    def load(path):
        return json.loads(path.read_text(encoding='utf-8-sig'))
    root = directory.parents[1]
    old = root.parent/'rtxgi-multibounce-20261107'/'runs'/'rtxpt-timing-r1-v1'
    actual = load(directory/'actual-settings.json')
    baseline = load(old/'actual-settings.json')
    require(actual['sample_offset'] == offset, 'actual sample offset mismatch')
    excluded = {'sample_offset', 'sample_frame', 'benchmark_frame'}
    require({k:v for k,v in actual.items() if k not in excluded} ==
            {k:v for k,v in baseline.items() if k not in excluded}, 'actual settings differ from frozen baseline')
    require(actual['sample_frame'] == actual['benchmark_frame'] == 367, 'terminal actual frame mismatch')
    require(load(directory/'effective-settings.json') == load(old/'effective-settings.json'), 'effective settings changed')
    require(load(directory/'frozen-scene-contract.json') == load(old/'frozen-scene-contract.json'), 'scene contract changed')
    build = load(root/'build-record.json')
    launch = load(directory/'launch-manifest.json')
    require(build['build_exit_code'] == launch['exit_code'] == 0, 'build or launch failed')
    require(launch['executable_sha256'] == build['executable_sha256'], 'launch executable differs from frozen build')
    require(hashlib.sha256(Path(launch['spec']['executable']).read_bytes()).hexdigest() == build['executable_sha256'], 'executable changed after launch')
    require(build['shader_source_files_verified_equal_to_previous'] == 624, 'shader source freeze mismatch')
    env = launch['spec']['env']
    expected_env = {'RESTIR_REPO_BENCH_FRAMES':368, 'RESTIR_REPO_BENCH_INTERVAL_MS':0,
                    'RESTIR_GI_CAPTURE':'none', 'RESTIR_GI_SAMPLE_OFFSET':offset,
                    'RESTIR_GI_MODE':'temporalspatial', 'RESTIR_MAINLOOP_WALL':'1'}
    require(all(str(env.get(k)) == str(v) for k,v in expected_env.items()), 'launch timing/capture environment mismatch')
    completed = load(directory/'completed.json')
    require(completed == dict(GI_executed=True, frames=368, uncapped=True, readback_during_timed_phase=False), 'completion metadata mismatch')
    meta = load(directory/'mainloop-meta.json')
    expected_meta = dict(is_steady=True, tick_frequency_hz=1000000000, frame_target=368,
                         complete_loop_rows=367, terminal_incomplete_frame_excluded=367,
                         csv_write_only_in_unmeasured_tail=True, added_gpu_wait=False, added_frame_pacing=False)
    require(all(meta.get(k) == v for k,v in expected_meta.items()), 'mainloop metadata mismatch')
    timing = launch['frozen_scene_contract']['timing']
    require(timing['vsync'] is False and timing['fps_limit'] == timing['interval_ms'] == 0, 'frozen pacing settings mismatch')
    return dict(status='pass', actual_settings_equal_frozen_baseline_except_frame_and_offset=True,
                launch_executable_sha256=launch['executable_sha256'], shader_files_equal=624,
                process_exit_code=launch['exit_code'], mainloop_metadata=meta,
                vsync_evidence='Frozen scene contract and boundary-audit source review; no runtime vsync field exported',
                baseline_actual_settings_sha256=hashlib.sha256((old/'actual-settings.json').read_bytes()).hexdigest(),
                build_record_sha256=hashlib.sha256((root/'build-record.json').read_bytes()).hexdigest())


def audit_run(directory, offset):
    rows = read_csv(directory / 'mainloop.csv')
    require([int(r['frame']) for r in rows] == list(range(367)),
            f'{directory}: expected exactly mainloop frames 0..366')
    require(all(int(b['device_frame']) > int(a['device_frame']) for a, b in zip(rows, rows[1:])),
            f'{directory}: device_frame must increase')
    for i, row in enumerate(rows):
        start, end = int(row['loop_start_ns']), int(row['loop_end_ns'])
        require(end > start, f'frame {i}: invalid native wall ticks')
        for key in HOST:
            value = float(row[key])
            require(math.isfinite(value) and value >= -1e-6, f'frame {i}: invalid {key}')
        require(abs(float(row['wall_ms']) - (end-start)/1e6) <= 1e-5,
                f'frame {i}: wall tick conversion mismatch')
        if i:
            previous = rows[i-1]
            require(start >= int(previous['loop_end_ns']), f'frame {i}: overlapping loop intervals')
            require(abs(float(row['start_interval_ms']) - (start-int(previous['loop_start_ns']))/1e6) <= 1e-5,
                    f'frame {i}: start interval mismatch')
    gpu_rows = read_csv(directory / 'gpu.csv')
    require({int(r['frame']) for r in gpu_rows} == set(range(368)),
            f'{directory}: expected GPU frames 0..367 including drain tail')
    lookup = {}
    for row in gpu_rows:
        frame = int(row['frame'])
        require(int(row['sample_frame']) == frame, f'GPU frame {frame}: relative sample sequence mismatch')
        value = float(row['ms'])
        require(math.isfinite(value) and value >= 0, f'GPU frame {frame}: invalid duration')
        if row['name'] in GPU:
            key = (frame, row['name'])
            require(key not in lookup, f'duplicate GPU scope {key}')
            lookup[key] = value
    measured = rows[120:360]
    data = {key: [float(r[key]) for r in measured] for key in HOST}
    for scope in GPU:
        require(all((frame, scope) in lookup for frame in range(368)), f'missing GPU scope {scope}')
        data['gpu_' + scope] = [lookup[(frame, scope)] for frame in range(120, 360)]
    require(all(v > 0 for v in data['wall_ms'] + data['start_interval_ms']), 'nonpositive primary timing')
    require(all(v > 0 for key in data if key.startswith('gpu_') for v in data[key]), 'nonpositive GPU scope')
    data['host_component_residual_ms'] = [float(r['wall_ms']) - sum(float(r[k]) for k in HOST[1:-1]) for r in measured]
    require(all(abs(v) <= 1e-5 for v in data['host_component_residual_ms']), 'host phase partition mismatch')
    data['between_loop_gap_ms'] = [float(rows[i]['start_interval_ms']) - float(rows[i-1]['wall_ms']) for i in range(120, 360)]
    evidence = {}
    for name in ['mainloop.csv', 'gpu.csv', 'launch-manifest.json', 'completed.json', 'actual-settings.json', 'effective-settings.json', 'mainloop-meta.json', 'frozen-scene-contract.json']:
        path = directory / name
        if path.exists():
            evidence[name] = dict(path=str(path.resolve()), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
    return dict(run=directory.name, sample_offset=offset, measured_frames=[120, 359],
                warmup_count=120, measured_count=240, excluded_mainloop_tail_count=7,
                metrics={key: stats(values) for key, values in data.items()}, evidence=evidence), data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runs', nargs=3, type=Path, default=[ROOT/'runs'/f'rtxpt-loop-r{i}-v1' for i in range(1, 4)])
    parser.add_argument('--output', type=Path, default=ROOT/'analysis'/'mainloop-results.json')
    args = parser.parse_args()
    repeats, pooled = [], {}
    for directory, offset in zip(args.runs, [0, 10000, 20000]):
        metadata = audit_metadata(directory, offset)
        result, data = audit_run(directory, offset)
        result['metadata_checks'] = metadata
        repeats.append(result)
        for key, values in data.items():
            pooled.setdefault(key, []).extend(values)
    result = dict(status='complete', schema='rtxpt-mainloop-wall.v1', timer='steady_clock duration_cast nanoseconds; 1e9 ticks/s',
                  primary_metric='wall_ms: actual outer main-loop end minus start, including natural waits',
                  secondary_metric='start_interval_ms: current loop start minus previous loop start',
                  interpretation='Normal pipelined three-buffer loop throughput, not per-frame GPU completion latency. CPU wall and GPU overlap; never sum them.',
                  quantiles='Linear interpolation at (n-1)*p; pooled percentiles use all 720 raw samples',
                  repeats=repeats, pooled={key: stats(values) for key, values in pooled.items()})
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    print(json.dumps(dict(output=str(args.output), pooled_wall=result['pooled']['wall_ms'],
                          pooled_interval=result['pooled']['start_interval_ms']), indent=2))


if __name__ == '__main__':
    main()
