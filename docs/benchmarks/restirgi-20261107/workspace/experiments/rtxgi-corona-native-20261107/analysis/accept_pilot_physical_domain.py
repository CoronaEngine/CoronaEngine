"""Accept only the reviewed native pilot's physical primary domain, not its estimator."""
import json
from pathlib import Path

import numpy as np

import analyze_native as n

HERE = Path(__file__).resolve().parent
run = HERE.parent / 'runs/corona-native-pilot-fresh-v1'
diagnostic_path = HERE / 'pilot-guide-diagnostics-v1.json'
radiance_path = HERE / 'pilot-fresh-radiance-v1.json'
diag = n.load(diagnostic_path)
pair = diag['guides']['pairwise']['corona_port__corona_native']
n.require(pair['validity_mismatch_pixels'] == 0 and pair['position_distance_m_p50_p95_p99_p999_max'][-1] == 0
          and pair['normal_pixels_below_dot_0999'] == 0
          and pair['normal_dot_min_p001_p1_p50'][0] > 1 - 1e-12, 'native physical guide differs from accepted port domain')
c, done, before, after = (n.load(run / p) for p in ['contract.json', 'result.json', 'identity-before.json', 'identity-after.json'])
n.require(done['status'] == 'complete' and done['identities_unchanged'] is True, 'pilot incomplete or unfrozen')
freeze = {g: {p: v['sha256'] for p, v in before[g].items()} for g in ['source', 'runtime']}
n.require(freeze == {g: {p: v['sha256'] for p, v in after[g].items()} for g in freeze}, 'pilot source/runtime changed')
contract = n.load(n.OLD / 'scene_contract.json')
n.require(all(c['frozen_scene_contract'].get(k) == contract[k] for k in n.PHYSICS), 'pilot physical contract differs')
camera = done['capture']['camera']
for k in ['position', 'forward', 'world_up']:
    n.require(np.allclose(camera[k], contract['camera']['corona_api'][k], rtol=0, atol=1e-6), 'pilot camera differs')
for k in ['fov', 'near_plane', 'far_plane', 'projection_x_sign']:
    n.require(abs(camera[k] - contract['camera']['corona_api'][k]) < 1e-5, 'pilot projection differs')
out = {'schema': 'corona-native-physical-guide-domain-acceptance.v1', 'status': 'pass',
       'scope': 'Physical primary position/faceforward-geometric-normal/valid-mask domain of native fresh pilot; estimator correctness separately audited.',
       'pilot_run': str(run.resolve()), 'native_guide_sha256': diag['native_guide_sha256'],
       'frozen_identity': freeze,
       'evidence': [{'path': str(p.resolve()), 'sha256': n.sha256(p)} for p in
                    [diagnostic_path, radiance_path] + [run / x for x in ['contract.json', 'result.json', 'identity-before.json', 'identity-after.json', 'route-validation.json']]],
       'review': {'matched_previously_accepted_Corona_port_valid_domain': pair,
                  'same_physical_contract_and_actual_camera': True,
                  'pilot_source_runtime_stable': True,
                  'valid_pixels': diag['guides']['inputs']['corona_native']['valid_pixels'],
                  'fresh32_mean_bias_percent': n.load(radiance_path)['mean_signed_brightness_bias_percent']},
       'limitations': ['Full RGBA byte hashes differ because depth/alpha/invalid payload semantics differ; valid canonical positions and normalized normals agree.',
                      'Retains the old four-way masks and all previously reviewed edge disagreements. No old metric or capture is replaced.',
                      'Physical exported normals do not assert native reuse surface guides were rewritten; native estimator/reuse logic remains separately audited.',
                      'Fresh32 mean agreement is corroborating evidence, not proof of unbiasedness or physical transport by itself.']}
(HERE / 'physical-guide-domain-acceptance-v1.json').write_text(json.dumps(out, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'status': out['status'], 'valid_pixels': out['review']['valid_pixels'],
                  'fresh32_mean_bias_percent': out['review']['fresh32_mean_bias_percent']}))
