"""CPU-only native pilot vs existing four primary-domain diagnostic; no auto acceptance."""
import argparse
import copy
import json
from pathlib import Path

import analyze_native as native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--frame', type=int, default=0)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    old_manifest = native.OLD / 'analysis/manifest-executed-seedstreams-v1.json'
    manifest = native.load(old_manifest)
    guides = copy.deepcopy(manifest['guides'])
    for spec in guides.values():
        for k in ['position', 'normal']:
            spec[k] = str((old_manifest.parent / spec[k]).resolve())
        spec['validity']['path'] = str((old_manifest.parent / spec['validity']['path']).resolve())
    raw = args.run.resolve() / f'raw/frame_{args.frame:04d}'
    guides['corona_native'] = {'position': str(raw / 'reference_position_depth.f32'),
                               'normal': str(raw / 'reference_world_normal.f32'),
                               'validity': {'path': str(raw / 'reference_position_depth.f32'), 'channel': 3},
                               'internal_to_canonical_scale': [1, 1, -1]}
    report, _ = native.analysis.compare_guides(guides, Path, 1920, 1080)
    report = native.analysis.public(report)
    out = {'schema': 'corona-native-physical-guide-diagnostic.v1', 'status': 'diagnostic_requires_review',
           'run': str(args.run.resolve()), 'guides': report,
           'native_guide_sha256': {k: native.sha256(guides['corona_native'][k]) for k in ['position', 'normal']},
           'notes': ['Same canonical coordinates after Corona Z reflection. Validity comes from position alpha.',
                     'Metrics do not prove Lambert, direct exclusion, depth/RR or route; source/runtime evidence must establish those.',
                     'Review validity mismatch, positions above1mm, and normal edge disagreements before writing acceptance status pass.']}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(out, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({'status': out['status'], 'native_pairs': {k: v for k, v in report['pairwise'].items() if 'corona_native' in k}}))


if __name__ == '__main__':
    main()
