"""Descriptive native pilot radiance against frozen common reference, no acceptance claim."""
import argparse
import json
from pathlib import Path

import numpy as np

import analyze_native as native


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--frames', type=int, default=32)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    a = native.analysis
    reference_path = native.OLD / 'analysis/common-reference-8192spp.rgba32f'
    reference = a.normalized_rgb(a.read_rgba(reference_path, 1920, 1080), 1.) @ a.LUMA
    records, total = [], np.zeros_like(reference)
    for f in range(args.frames):
        path = args.run.resolve() / f'raw/frame_{f:04d}/raw_indirect.f32'
        x = a.normalized_rgb(a.read_rgba(path, 1920, 1080), 1.) @ a.LUMA
        total += x
        records.append({'frame': f, 'mean_luminance': float(x.mean()), 'sha256': native.sha256(path),
                        'signed_brightness_bias_percent': float((x.mean() / reference.mean() - 1.) * 100),
                        'L2_NRMSE_percent': float(np.sqrt(np.mean((x - reference) ** 2) / np.mean(reference ** 2)) * 100)})
    mean = total / args.frames
    out = {'schema': 'corona-native-pilot-radiance.v1', 'status': 'diagnostic_requires_review',
           'run': str(args.run.resolve()), 'reference': {'path': str(reference_path), 'sha256': native.sha256(reference_path),
                                                       'mean_luminance': float(reference.mean())},
           'frame_count': args.frames, 'frames': records, 'mean_luminance': float(mean.mean()),
           'mean_signed_brightness_bias_percent': float((mean.mean() / reference.mean() - 1.) * 100),
           'time_mean_L2_NRMSE_percent': float(np.sqrt(np.mean((mean - reference) ** 2) / np.mean(reference ** 2)) * 100),
           'note': 'Fresh proposal or reused output semantics must be established by actual route/settings/source. Mean agreement alone does not establish transport correctness.'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(out, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({k: out[k] for k in ['status', 'frame_count', 'mean_luminance', 'mean_signed_brightness_bias_percent', 'time_mean_L2_NRMSE_percent']}))


if __name__ == '__main__':
    main()
