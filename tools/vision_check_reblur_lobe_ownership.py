"""Check captured ReBLUR guides and pre-filter lobes for receiver mismatch.

Capture with VISION_EVAL_DEBUG_DIR/FRAME and REBLUR_FUSE_GUIDES=0. This checks
actual GPU readbacks, not a CPU reproduction of the path-tracing estimator.
"""
import argparse
import json
from pathlib import Path

import numpy as np


def check(directory: Path):
    results = []
    paths = sorted(directory.glob('guide_layer_*_guides.bin'))
    if not paths:
        raise ValueError('No full ReBLUR guide readbacks found')
    for path in paths:
        prefix = path.name.removesuffix('guides.bin')
        diffuse = np.fromfile(directory / (prefix + 'raw_diffuse.bin'), dtype='<f4').reshape(-1, 4)
        specular = np.fromfile(directory / (prefix + 'raw_specular.bin'), dtype='<f4').reshape(-1, 4)
        pixels = len(diffuse)
        guides = np.fromfile(path, dtype='<f4').reshape(5, pixels, 4)
        identity = guides[1].view('<u4')
        valid = (identity[:, 3] & 1) != 0
        absent_diffuse = valid & np.all(guides[3, :, :3] == 0, axis=1)
        # Allow roundoff from total - specular, including bright FP32 samples.
        tolerance = np.maximum(.01, np.max(abs(specular[:, :3]), axis=1) * 1e-5)
        mismatch = absent_diffuse & (np.max(abs(diffuse[:, :3]), axis=1) > tolerance)
        indices = np.flatnonzero(mismatch)
        results.append(dict(layer=prefix, pixels=pixels, zero_diffuse_guides=int(absent_diffuse.sum()),
                            mismatches=len(indices), example_indices=indices[:16].tolist(),
                            max_misrouted_diffuse=float(np.max(abs(diffuse[indices, :3]))) if len(indices) else 0.))
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    results = check(args.directory)
    print(json.dumps(results, indent=2))
    raise SystemExit(1 if any(result['mismatches'] for result in results) else 0)
