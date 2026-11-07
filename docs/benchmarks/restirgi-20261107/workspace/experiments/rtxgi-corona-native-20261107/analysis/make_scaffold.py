"""Record old immutable inputs; deliberately leave native evidence unaccepted."""
import json
import os
from pathlib import Path

import analyze_native as native

HERE = Path(__file__).resolve().parent
OLD = native.OLD
relative = lambda path: os.path.relpath(path, HERE).replace('\\', '/')
old_manifest_path = OLD / 'analysis/manifest-executed-seedstreams-v1.json'
old_manifest = native.load(old_manifest_path)
paths = [OLD / p for p in ['results-final.json', 'scene_contract.json',
         'analysis/manifest-executed-seedstreams-v1.json', 'analysis/analyze_multibounce.py',
         'analysis/analyze_timing.py', 'analysis/final-quality-v1.json', 'analysis/final-timing-v1.json',
         'analysis/reference-acceptance-v1.json', 'analysis/common-reference-8192spp.rgba32f']]
for spec in old_manifest['references'].values():
    paths.extend((old_manifest_path.parent / spec[h]['path']).resolve() for h in ['half_a', 'half_b'])
for spec in old_manifest['guides'].values():
    paths.extend((old_manifest_path.parent / spec[k]).resolve() for k in ['position', 'normal'])
config = {'schema': 'corona-native-fifth-row-manifest.v1', 'status': 'scaffold_not_accepted',
          'scene_contract': relative(OLD / 'scene_contract.json'),
          'old_manifest': relative(old_manifest_path), 'old_results': relative(OLD / 'results-final.json'),
          'immutable_inputs': {relative(p): native.sha256(p) for p in dict.fromkeys(paths)},
          'acceptance_evidence': {k: {'path': '', 'sha256': ''} for k in
              ['physical_guide_domain_audit', 'native_route_transport_audit', 'joint_seed_stream_audit']},
          'native_route_log': 'PENDING actual native route marker',
          'required_environment': {'VISION_RESTIR_GI_MULTIBOUNCE_BENCH': '1',
              'VISION_RESTIR_GI_NATIVE_ALIGNED_LAMBERT': '1',
              'VISION_RESTIR_GI_NATIVE_MB_DIAGNOSTIC': '0'},
          'frozen_identity': {},
          'accepted_pilot_guides': {'position': '../runs/corona-native-pilot-fresh-v1/raw/frame_0000/reference_position_depth.f32',
                                   'normal': '../runs/corona-native-pilot-fresh-v1/raw/frame_0000/reference_world_normal.f32'},
          'accepted_guide_sha256': {'position': '', 'normal': ''},
          'quality_frame_pattern': 'raw/frame_{frame:04d}/raw_indirect.f32'}
for phase in ['quality', 'timing']:
    config[phase] = [{'label': f'r{i}', 'sample_offset': offset,
                      'run': f'../runs/corona-native-{phase}-r{i}-v1'}
                     for i, offset in enumerate([0, 10000, 20000], 1)]
(HERE / 'manifest-scaffold-v1.json').write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'status': config['status'], 'immutable_files': len(config['immutable_inputs'])}))
