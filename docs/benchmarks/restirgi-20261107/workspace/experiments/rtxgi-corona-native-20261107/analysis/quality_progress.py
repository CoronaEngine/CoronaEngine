"""Descriptive completed late-frame means; never a final quality result."""
import json
from pathlib import Path

import analyze_native as n

HERE = Path(__file__).resolve().parent
config = n.load(HERE / 'manifest-selected-v1.json')
reference_path = n.OLD / 'analysis/common-reference-8192spp.rgba32f'
a = n.analysis
reference = a.normalized_rgb(a.read_rgba(reference_path, 1920, 1080), 1.) @ a.LUMA
out = {'status': 'descriptive_progress_only', 'reference_sha256': n.sha256(reference_path), 'repeats': []}
for repeat in config['quality']:
    run = (HERE / repeat['run']).resolve()
    records = []
    for f in range(96, 128):
        p = run / config['quality_frame_pattern'].format(frame=f)
        if p.exists() and p.stat().st_size == 1920 * 1080 * 16:
            x = a.normalized_rgb(a.read_rgba(p, 1920, 1080), 1.) @ a.LUMA
            records.append({'frame': f, 'mean_luminance': float(x.mean())})
    mean = sum(r['mean_luminance'] for r in records) / len(records) if records else None
    out['repeats'].append({'label': repeat['label'], 'available_late_frames': len(records),
                           'run_complete': (run / 'result.json').exists() and n.load(run / 'result.json')['status'] == 'complete',
                           'mean_luminance': mean,
                           'brightness_bias_percent': (mean / float(reference.mean()) - 1) * 100 if mean is not None else None,
                           'frames': records})
(HERE / 'quality-progress-v1.json').write_text(json.dumps(out, indent=2) + '\n', encoding='utf-8')
print(json.dumps({**out, 'repeats': [{k: v for k, v in r.items() if k != 'frames'} for r in out['repeats']]}))
