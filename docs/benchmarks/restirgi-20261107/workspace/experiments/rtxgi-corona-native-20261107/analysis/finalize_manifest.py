"""Freeze executed native manifest only after all six actual processes pass audit."""
import json
from pathlib import Path

import analyze_native as n

HERE = Path(__file__).resolve().parent
selected = HERE / 'manifest-selected-v1.json'
config = n.load(selected)
resolve = lambda p: (HERE / p).resolve()
n.check_pins(config, resolve)
n.validate_evidence(config, resolve)
contract = n.load(resolve(config['scene_contract']))
audit = {'schema': 'corona-native-six-run-audit.v1', 'status': 'pass', 'runs': {},
         'selected_manifest_sha256': n.sha256(selected)}
processes = set()
for phase in ['quality', 'timing']:
    audit['runs'][phase] = {}
    for repeat in config[phase]:
        run = resolve(repeat['run'])
        proof = n.audit_run(run, repeat, phase, config, contract)
        n.require(proof['process_identity'] not in processes, 'reused process')
        processes.add(proof['process_identity'])
        if phase == 'quality':
            paths = [run / config['quality_frame_pattern'].format(frame=f) for f in range(96, 128)]
            n.require(all(p.stat().st_size == 1920 * 1080 * 16 for p in paths), 'missing late raw frame')
            proof['late_frame_files'] = [{'path': str(p), 'sha256': n.sha256(p)} for p in paths]
        else:
            timing, _ = n.native_timing(n.load(run / 'result.json'))
            proof['scope_means'] = timing['scopes']
            proof['scope_note'] = timing['scope_note']
        audit['runs'][phase][repeat['label']] = proof
audit_path = HERE / 'six-run-audit-v1.json'
audit_path.write_text(json.dumps(audit, indent=2) + '\n', encoding='utf-8')
config['status'] = 'six_formal_processes_complete_and_audited'
config['executed_run_audit'] = {'path': audit_path.name, 'sha256': n.sha256(audit_path)}
executed = HERE / 'manifest-executed-v1.json'
executed.write_text(json.dumps(config, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'status': config['status'], 'unique_processes': len(processes),
                  'manifest': str(executed), 'sha256': n.sha256(executed)}))
