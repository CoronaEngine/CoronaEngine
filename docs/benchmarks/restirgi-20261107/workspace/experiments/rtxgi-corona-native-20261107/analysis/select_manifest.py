"""Bind accepted native pilot/source evidence to planned, uniquely named formal runs."""
import json
from pathlib import Path

import analyze_native as n

HERE = Path(__file__).resolve().parent
c = n.load(HERE / 'manifest-scaffold-v1.json')
physical = n.load(HERE / 'physical-guide-domain-acceptance-v1.json')
c['status'] = 'selected_pending_completion_of_six_formal_processes'
c['native_route_log'] = 'Native ReSTIR GI multibounce benchmark:'
c['frozen_identity'] = physical['frozen_identity']
c['accepted_guide_sha256'] = physical['native_guide_sha256']
c['acceptance_evidence'] = {k: {'path': p, 'sha256': n.sha256(HERE / p)} for k, p in
    [('physical_guide_domain_audit', 'physical-guide-domain-acceptance-v1.json'),
     ('native_route_transport_audit', '../native-contract-regression.json'),
     ('joint_seed_stream_audit', '../native-contract-regression.json')]}
for e in c['acceptance_evidence'].values():
    n.require(n.load(HERE / e['path'])['status'] == 'pass', 'unaccepted evidence')
(HERE / 'manifest-selected-v1.json').write_text(json.dumps(c, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'status': c['status'], 'native_source_files': len(c['frozen_identity']['source']),
                  'evidence': c['acceptance_evidence']}))
