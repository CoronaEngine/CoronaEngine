"""CPU-only fifth-row analysis. Reads the completed four-way experiment unchanged."""
from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import importlib.util
import json
import math
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
OLD = HERE.parents[1] / 'rtxgi-multibounce-20261107'
PHYSICS = ['resolution', 'source_sha256', 'scene_sha256',
           'geometry_world_float32_triangle_sha256', 'camera', 'materials', 'lighting', 'transport']
GI = {'candidate': ['ReSTIR indirect initial samples'],
      'temporal': ['ReSTIR indirect temporal reuse'],
      'spatial_and_resolve': ['ReSTIR indirect spatial reuse and shading']}
PREP = ['rt_geom', 'FrameBuffer-SaveSurfaceExtHistory']
CANDIDATE_PRODUCER = 'ReSTIR direct initial candidates and temporal reuse'
EXPECTED_SETTINGS = dict(max_depth=16, min_depth=5, rr_threshold=1,
    direct=dict(open=True, M_light=1, M_bsdf=1, max_age=30, max_recursion=1, pairwise=True, diffuse_factor=.3,
        temporal=dict(open=False, history_limit=20, radius=0, theta=35, depth=.1),
        spatial=dict(open=False, sample_num=1, radius=30, theta=35, depth=.1)),
    indirect=dict(open=True, max_age=30, diffuse_factor=.3,
        temporal=dict(open=True, history_limit=5, radius=0, theta=35, depth=.3),
        spatial=dict(open=True, sample_num=2, radius=12, theta=35, depth=.3)))


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def require_settings(actual, expected, path='settings'):
    if isinstance(expected, dict):
        require(isinstance(actual, dict) and set(actual) == set(expected), path + ': keys differ')
        for key, value in expected.items():
            require_settings(actual[key], value, path + '.' + key)
    elif isinstance(expected, bool):
        require(actual is expected, path + ': boolean differs')
    else:
        require(isinstance(actual, (int, float)) and not isinstance(actual, bool)
                and math.isclose(actual, expected, rel_tol=0, abs_tol=1e-6), path + ': value differs')


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as source:
        for b in iter(lambda: source.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def old_module(name):
    path = OLD / 'analysis' / (name + '.py')
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


analysis = old_module('analyze_multibounce')


def describe(values):
    x = np.asarray(values, dtype=float)
    return {'count': len(x), 'mean_ms': float(x.mean()), 'median_ms': float(np.median(x)),
            'p10_ms': float(np.quantile(x, .1)), 'p90_ms': float(np.quantile(x, .9)),
            'p95_ms': float(np.quantile(x, .95))}


def check_pins(config, resolve):
    for path, digest in config['immutable_inputs'].items():
        require(sha256(resolve(path)) == digest, f'immutable input changed: {path}')


def validate_evidence(config, resolve):
    evidence = config['acceptance_evidence']
    required = ['physical_guide_domain_audit', 'native_route_transport_audit', 'joint_seed_stream_audit']
    records = {}
    for key in required:
        item = evidence[key]
        path = resolve(item['path'])
        require(sha256(path) == item['sha256'], f'{key}: evidence checksum differs')
        record = load(path)
        require(record['status'] == 'pass', f'{key}: evidence not accepted')
        records[key] = {'path': str(path), 'sha256': item['sha256']}
    return records


def audit_run(run, repeat, phase, config, contract):
    c, done, route = (load(run / name) for name in ['contract.json', 'result.json', 'route-validation.json'])
    require(Path(c['output']).resolve() == run, 'contract belongs to another output')
    require(done['status'] == 'complete' and c['kind'] == 'native' and c['phase'] == phase,
            'completed native route absent')
    require(route['observed'] is True and route['expected_log'] == config['native_route_log'],
            'native runtime route marker differs')
    require(c['render_frames'] == (128 if phase == 'quality' else 360)
            and c['warmup_frames'] == (0 if phase == 'quality' else 120)
            and c['capture_raw'] is (phase == 'quality'), 'frame/capture protocol differs')
    actual_frames = done['gpu']['frames']
    require(len(actual_frames) == c['render_frames']
            and {f['sample_index'] for f in actual_frames} == set(range(c['render_frames'])),
            'actual rendered frame coverage differs')
    require(done['source_scene_sha256'] == contract['scene_sha256']
            and Path(c['scene_path']).resolve() == Path(contract['corona_scene']).resolve()
            and sha256(c['scene_path']) == contract['scene_sha256'],
            'actual loaded source scene differs')
    with (run / 'frames.csv').open(encoding='utf-8-sig', newline='') as stream:
        frame_rows = list(csv.DictReader(stream))
    expected_indices = list(range(1, c['render_frames'] + 1))
    require([int(r['frame']) for r in frame_rows] == expected_indices
            and [int(r['integrator_frame']) for r in frame_rows] == expected_indices,
            'actual frames.csv coverage differs')
    if phase == 'quality':
        require(done['raw_boundaries_unchanged_after_shutdown'] is True, 'quality raw capture changed after shutdown')
    else:
        require(not (run / 'raw').exists(), 'raw exports unexpectedly present in timing process')
    require(c['primary_jitter'] is False, 'primary jitter enabled')
    env = c['benchmark_environment']
    require(int(env['VISION_BENCH_SAMPLE_OFFSET']) == repeat['sample_offset'], 'offset differs')
    require(all(env.get(k) == v for k, v in config['required_environment'].items()), 'native environment differs')
    require(env.get('VISION_RESTIR_GI_MODE', 'off') in ['', 'off', '0', 'native'], 'ported GI route enabled')
    actual = done['effective_after']['settings']
    for value in [c['settings'], done['effective_before']['settings'], actual]:
        require_settings(value, EXPECTED_SETTINGS)
    camera = done['capture']['camera']
    require(done['capture']['ok'] is True and camera['vision_render_mode'] == 'restir'
            and camera['vision_denoise'] is False and camera['vision_stable_planes'] is False
            and camera['vision_accumulation'] is False and camera['width'] == 1920 and camera['height'] == 1080,
            'actual native camera route/filtering/resolution differs')
    physical_camera = contract['camera']['corona_api']
    for key in ['position', 'forward', 'world_up']:
        require(np.allclose(camera[key], physical_camera[key], rtol=0, atol=1e-6),
                'actual native camera pose differs: ' + key)
    for key in ['fov', 'near_plane', 'far_plane', 'projection_x_sign']:
        require(math.isclose(camera[key], physical_camera[key], rel_tol=0, abs_tol=1e-5),
                'actual native camera projection differs: ' + key)
    for settings in [c['settings'], actual]:
        require(all(settings.get(k) == v for k, v in [('min_depth', 5), ('max_depth', 16), ('rr_threshold', 1)]),
                'actual native transport differs from 5/16/1')
        require(settings['indirect']['open'] is True and settings['indirect']['temporal']['open'] is True
                and settings['indirect']['spatial']['open'] is True, 'native reuse disabled')
        require(settings['direct']['M_bsdf'] == 1, 'native GI requires one primary BSDF proposal from DI producer')
    require(all(c['frozen_scene_contract'].get(k) == contract.get(k) for k in PHYSICS), 'physical contract differs')
    before, after = load(run / 'identity-before.json'), load(run / 'identity-after.json')
    freeze = {g: {p: v['sha256'] for p, v in before[g].items()} for g in ['source', 'runtime']}
    require(freeze == {g: {p: v['sha256'] for p, v in after[g].items()} for g in freeze},
            'source/runtime changed during process')
    require(freeze == config['frozen_identity'], 'run differs from frozen source/runtime')
    require(done['identities_unchanged'] is True, 'capture helper reports identity mutation')
    names = ['contract.json', 'result.json', 'route-validation.json', 'identity-before.json', 'identity-after.json', 'frames.csv']
    return {'run': str(run), 'sample_offset': repeat['sample_offset'], 'actual_settings': actual,
            'process_identity': done['capture_wall_window_ns']['begin'],
            'source_runtime_identity_sha256': hashlib.sha256(json.dumps(freeze, sort_keys=True).encode()).hexdigest(),
            'evidence': [{'path': str(run / n), 'sha256': sha256(run / n)} for n in names]}


def native_timing(result):
    bound = result['gpu']['frames']
    require(len(bound) == 360 and {f['sample_index'] for f in bound} == set(range(360)),
            'timing requires exact unique frame coverage 0..359')
    records = []
    values = {k: [] for k in ['GI_exclusive', 'primary_preparation', 'GI_plus_primary_preparation']}
    values.update({'primary_geometry_preparation': [], 'candidate_producer_fused': []})
    values.update({'component_' + k: [] for k in GI})
    all_gi = sum(GI.values(), [])
    for f in sorted(bound, key=lambda f: f['sample_index']):
        if f['sample_index'] < 120:
            continue
        stages = {}
        for d in f['dispatches']:
            ms = float(d['gpu_ms'])
            require(math.isfinite(ms) and ms >= 0, 'invalid GPU timestamp')
            stages.setdefault(d['name'], []).append(ms)
        for name in all_gi + PREP + [CANDIDATE_PRODUCER]:
            require(name in stages and len(stages[name]) == 1 and stages[name][0] > 0,
                    f"frame {f['sample_index']}: missing/duplicated/nonpositive {name}")
        require(not any(n.startswith('RestirGI reference ') for n in stages), 'ported GI event in native timing')
        gi = sum(stages[n][0] for n in all_gi)
        geometry_prep = sum(stages[n][0] for n in PREP)
        producer = stages[CANDIDATE_PRODUCER][0]
        prep = geometry_prep + producer
        components = {k: sum(stages[n][0] for n in names) for k, names in GI.items()}
        for k, v in [('GI_exclusive', gi), ('primary_preparation', prep), ('GI_plus_primary_preparation', gi + prep)]:
            values[k].append(v)
        for k, v in components.items():
            values['component_' + k].append(v)
        values['primary_geometry_preparation'].append(geometry_prep)
        values['candidate_producer_fused'].append(producer)
        records.append({'frame': f['sample_index'], 'GI_exclusive_ms': gi, 'primary_preparation_ms': prep,
                        'primary_geometry_preparation_ms': geometry_prep, 'candidate_producer_fused_ms': producer,
                        'component_ms': components})
    return {'frames': records, 'scopes': {k: describe(v) for k, v in values.items()},
            'required_GI_scopes': all_gi, 'primary_preparation_scopes': PREP + [CANDIDATE_PRODUCER],
            'full_GPU_frame': None,
            'scope_note': 'Three native GI dispatches measure suffix tracing/reuse/shading. Native primary BSDF proposal is produced by fused DI initial+temporal; its full GPU scope is charged to preparation. Spatial reuse and shading are fused. No full GPU range.'}, values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    base = args.manifest.resolve().parent
    resolve = lambda p: (base / p).resolve()
    config = load(args.manifest)
    check_pins(config, resolve)
    evidence = validate_evidence(config, resolve)
    for key, relative in config['accepted_pilot_guides'].items():
        require(sha256(resolve(relative)) == config['accepted_guide_sha256'][key],
                'accepted pilot guide bytes changed')
    contract = load(resolve(config['scene_contract']))
    require(contract['resolution'] == [1920, 1080], 'requires frozen 1080p contract')
    old_manifest_path = resolve(config['old_manifest'])
    old_manifest = load(old_manifest_path)
    old_resolve = lambda p: (old_manifest_path.parent / p).resolve()
    guides, mask = analysis.compare_guides(old_manifest['guides'], old_resolve, 1920, 1080)
    regions = {'full_frame': np.ones((1080, 1920), bool), 'common_valid_primary': mask,
               'guide_consistent_primary_diagnostic': guides['_agreement_mask']}
    references, reference, noise = analysis.analyze_references(old_manifest['references'], old_resolve, 1920, 1080, regions)
    old_results = load(resolve(config['old_results']))
    repeats, proofs, processes = [], [], set()
    for phase in ['quality', 'timing']:
        require([r['sample_offset'] for r in config[phase]] == [0, 10000, 20000], 'repeat offsets differ')
    seen_paths = set()
    for repeat in config['quality']:
        run = resolve(repeat['run'])
        proof = audit_run(run, repeat, 'quality', config, contract)
        require(proof['process_identity'] not in processes, 'reused process identity')
        processes.add(proof['process_identity'])
        proof['primary_domain_basis'] = 'Accepted pilot physical guides plus identical source/runtime/scene and actual camera; formal guide export disabled.'
        paths = [run / config['quality_frame_pattern'].format(frame=f) for f in range(96, 128)]
        require(len(set(paths)) == 32 and not (set(paths) & seen_paths), 'reused quality input path')
        seen_paths.update(paths)
        sequence = analysis.summarize_sequence((analysis.normalized_rgb(analysis.read_rgba(p, 1920, 1080), 1.) @ analysis.LUMA for p in paths),
                                               reference, noise, regions)
        sequence.update(label=repeat['label'], sample_offset=repeat['sample_offset'], frame_indices=list(range(96, 128)),
                        input_files=[{'path': str(p), 'sha256': sha256(p)} for p in paths], runtime_proof=proof)
        repeats.append(sequence)
        proofs.append(proof)
    quality = {'pooled': analysis.pool_sequences(repeats, reference, noise, regions), 'repeats': analysis.public(repeats)}
    timings, pooled_values = [], {}
    for repeat in config['timing']:
        run = resolve(repeat['run'])
        proof = audit_run(run, repeat, 'timing', config, contract)
        require(proof['process_identity'] not in processes, 'reused process identity')
        processes.add(proof['process_identity'])
        result, values = native_timing(load(run / 'result.json'))
        result.update(label=repeat['label'], runtime_proof=proof)
        timings.append(result)
        for k, v in values.items():
            pooled_values.setdefault(k, []).extend(v)
    timing = {'repeats': timings, 'pooled': {k: describe(v) for k, v in pooled_values.items()}}
    q = quality['pooled']['regions']['full_frame']
    row = {'label': 'CoronaEngine native ReSTIRGI', 'GI_GPU_ms': timing['pooled']['GI_exclusive'],
           'GI_plus_preparation_GPU_ms': timing['pooled']['GI_plus_primary_preparation'], 'full_GPU_ms': None,
           'candidate_producer_fused_GPU_ms': timing['pooled']['candidate_producer_fused'],
           'primary_geometry_preparation_GPU_ms': timing['pooled']['primary_geometry_preparation'],
           'GI_scope_note': 'Native suffix/reuse/shading only; mandatory primary candidate producer is in GI_plus_preparation. Old port preparation did not charge its unrelated native DI initial scope.',
           'temporal_noise_percent': q['temporal_RMS_noise_over_reference_L2'] * 100,
           'brightness_bias_percent': q['relative_signed_mean_bias'] * 100,
           'per_frame_L2_NRMSE_percent': q['per_frame_L2_NRMSE_rms'] * 100,
           'reference_noise_adjusted_NRMSE_percent': q['reference_noise_adjusted_frame_L2_NRMSE'] * 100,
           'mean_luminance': q['mean_luminance'], 'time_mean_NRMSE_percent': q['time_mean_L2_NRMSE'] * 100,
           'quality_repeat_ranges': quality['pooled']['repeat_ranges'],
           'quality_repeats': [{'label': r['label'],
                'bias_percent': r['regions']['full_frame']['relative_signed_mean_bias'] * 100,
                'noise_percent': r['regions']['full_frame']['temporal_RMS_noise_over_reference_L2'] * 100,
                'NRMSE_percent': r['regions']['full_frame']['per_frame_L2_NRMSE_rms'] * 100} for r in repeats],
           'timing_repeat_GI_means_ms': [r['scopes']['GI_exclusive']['mean_ms'] for r in timings],
           'stage_means_ms': {k: timing['pooled']['component_' + k]['mean_ms'] for k in GI},
           'native_settings': proofs[0]['actual_settings']}
    combined = copy.deepcopy(old_results)
    require(set(combined['renderers']) == {'original', 'rtxpt', 'rtxdi', 'corona_port'}, 'old table contains unexpected rows')
    combined['renderers']['corona_native'] = row
    require(all(combined['renderers'][k] == v for k, v in old_results['renderers'].items()), 'old metrics changed')
    output = {'schema': 'corona-native-fifth-row.v1', 'status': 'complete',
              'manifest_sha256': sha256(args.manifest), 'analysis_script_sha256': sha256(__file__),
              'acceptance_evidence': evidence, 'immutable_inputs': config['immutable_inputs'],
              'references': references, 'quality': quality, 'timing': timing, 'combined_results': combined,
              'notes': ['Four old rows copied unchanged. Same eight reference halves and same old guide masks.',
                        'Quality: 3x32 late frames, 93 within-repeat degrees of freedom; no cross-repeat lag pairs.',
                        'Timing: 3x240 measured frames after 120 warmup; no CPU wall substitute for full GPU frame.',
                        'Native raw_indirect is decoded RGB21 producer output stored as float32; readback does not recover quantized bits.',
                        'One new candidate path does not imply equal reuse/ray work; native spatial+resolve is fused.']}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({'status': output['status'], 'output': str(args.output.resolve()), 'native_row': row}, ensure_ascii=False))


if __name__ == '__main__':
    main()
