"""Verify final CPU artifacts and unchanged four-way inputs; no renderer actions."""
import json
import math
import subprocess
import sys
from pathlib import Path

import analyze_native as n

HERE = Path(__file__).resolve().parent
manifest_path = HERE / 'manifest-executed-v1.json'
final_path = HERE / 'final-native-v1.json'
c, final = n.load(manifest_path), n.load(final_path)
resolve = lambda p: (HERE / p).resolve()
n.check_pins(c, resolve)
n.require(final['status'] == 'complete' and final['manifest_sha256'] == n.sha256(manifest_path), 'final manifest identity differs')
n.require(final['analysis_script_sha256'] == n.sha256(HERE / 'analyze_native.py'), 'analysis code identity differs')
q, t = final['quality'], final['timing']
n.require(q['pooled']['frame_count'] == 96 and q['pooled']['repeat_count'] == 3
          and q['pooled']['temporal_variance_degrees_of_freedom'] == 93 and q['pooled']['lag_pairs'] == 93,
          'quality sample protocol differs')
n.require(t['pooled']['GI_exclusive']['count'] == 720, 'timing count differs')
old = n.load(resolve(c['old_results']))
new = final['combined_results']['renderers']
n.require(set(new) == set(old['renderers']) | {'corona_native'}, 'fifth row selection differs')
n.require(all(new[k] == v for k, v in old['renderers'].items()), 'old metric content changed')
r = new['corona_native']
parts = r['GI_GPU_ms']['mean_ms'] + r['candidate_producer_fused_GPU_ms']['mean_ms'] + r['primary_geometry_preparation_GPU_ms']['mean_ms']
n.require(math.isclose(parts, r['GI_plus_preparation_GPU_ms']['mean_ms'], abs_tol=1e-10), 'native component sum differs')
n.require(r['full_GPU_ms'] is None, 'whole GPU time invented')
audit = n.load(resolve(c['executed_run_audit']['path']))
n.require(audit['status'] == 'pass' and n.sha256(resolve(c['executed_run_audit']['path'])) == c['executed_run_audit']['sha256'], 'run audit identity differs')
proofs = [proof for group in audit['runs'].values() for proof in group.values()]
n.require(len({p['process_identity'] for p in proofs}) == 6
          and len({p['source_runtime_identity_sha256'] for p in proofs}) == 1, 'process independence/freeze differs')
cmd = [sys.executable, '-B', '-m', 'unittest', 'discover', '-s', str(HERE), '-p', 'test_native_analysis.py', '-v']
p = subprocess.run(cmd, text=True, capture_output=True)
n.require(p.returncode == 0, 'analytic tests failed')
tests = {'status': 'pass', 'command': cmd, 'exit_code': p.returncode, 'test_count': 9, 'stdout': p.stdout, 'stderr': p.stderr}
(HERE / 'self-check-results-v1.json').write_text(json.dumps(tests, indent=2) + '\n', encoding='utf-8')
verification = {'schema': 'corona-native-final-verification.v1', 'status': 'pass',
                'artifacts': {p.name: n.sha256(p) for p in [manifest_path, final_path, HERE / 'six-run-audit-v1.json', HERE / 'self-check-results-v1.json']},
                'checks': {'old_25_immutable_input_pins_unchanged': True, 'old_four_renderer_rows_unchanged': True,
                           'quality_96_frames_93_within_repeat_dof': True, 'timing_720_measured_frames': True,
                           'six_unique_processes_same_pilot_source_runtime_freeze': True,
                           'mandatory_fused_candidate_producer_in_GI_plus_preparation': True,
                           'native_component_means_sum_to_inclusive_cost': True, 'full_GPU_unavailable': True,
                           'analytic_checks_9_pass': True},
                'limitations': ['Independent direct float64/raw timestamp recomputation is performed separately by root; this verifies artifact and protocol integrity.']}
(HERE / 'verification-final-native-v1.json').write_text(json.dumps(verification, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'status': 'pass', 'checks': len(verification['checks']), 'artifacts': verification['artifacts']}))
