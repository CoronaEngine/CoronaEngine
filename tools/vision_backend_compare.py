"""Compare rendered reference and candidate images without parsing scene archives."""
from __future__ import annotations

import argparse
import json
from pathlib import Path




def compare(first: Path, second: Path, output: Path, first_label="Reference", second_label="Candidate"):
    import numpy as np
    from PIL import Image, ImageDraw
    a, b = [np.asarray(Image.open(path).convert("RGB"), dtype=np.float64) / 255 for path in (first, second)]
    if a.shape != b.shape:
        raise ValueError(f"Image sizes differ: {a.shape} vs {b.shape}")
    luminance = np.array([0.2126, 0.7152, 0.0722])
    x, y = a @ luminance, b @ luminance

    def mean(value, radius=3):
        size = 2 * radius + 1
        padded = np.pad(value, radius, mode="reflect")
        integral = np.pad(padded, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
        return (integral[size:, size:] - integral[:-size, size:] - integral[size:, :-size] + integral[:-size, :-size]) / size**2

    mx, my = mean(x), mean(y)
    vx, vy, covariance = mean(x*x)-mx*mx, mean(y*y)-my*my, mean(x*y)-mx*my
    ssim = ((2*mx*my + .01**2)*(2*covariance + .03**2) /
            ((mx*mx+my*my+.01**2)*(vx+vy+.03**2)))
    mse = float(np.mean((a-b)**2))
    metrics = dict(first_label=first_label, second_label=second_label,
                   width=a.shape[1], height=a.shape[0], mae=float(np.mean(abs(a-b))),
                   rmse=float(np.sqrt(mse)), psnr_db=float(-10*np.log10(max(mse, 1e-20))),
                   ssim_luminance=float(ssim.mean()), reference_mean_luminance=float(x.mean()),
                   candidate_mean_luminance=float(y.mean()), luminance_ratio=float(y.mean()/max(x.mean(), 1e-20)))
    linear_paths = [path.with_suffix('.pfm') for path in (first, second)]
    if all(path.is_file() for path in linear_paths):
        def read_linear(path):
            with path.open('rb') as stream:
                if stream.readline().strip() != b'PF':
                    raise ValueError(f'Expected RGB PFM: {path}')
                width, height = map(int, stream.readline().split())
                scale = float(stream.readline())
                pixels = np.frombuffer(stream.read(), dtype='<f4' if scale < 0 else '>f4')
                return pixels.reshape(height, width, 3).astype(np.float64) * abs(scale)
        linear_a, linear_b = map(read_linear, linear_paths)
        if linear_a.shape != a.shape or linear_b.shape != a.shape:
            raise ValueError('Linear and display image sizes differ')
        nonfinite = [int(np.count_nonzero(~np.isfinite(pixels))) for pixels in (linear_a, linear_b)]
        metrics['linear_nonfinite_values'] = dict(reference=nonfinite[0], candidate=nonfinite[1])
        if not any(nonfinite):
            linear_rmse = float(np.sqrt(np.mean((linear_a-linear_b)**2)))
            metrics['linear_rmse'] = linear_rmse
            metrics['linear_relative_rmse'] = linear_rmse / max(float(np.sqrt(np.mean(linear_a**2))), 1e-20)
            metrics['linear_luminance_ratio'] = float((linear_b @ luminance).mean()/max((linear_a @ luminance).mean(), 1e-20))
    output.mkdir(parents=True, exist_ok=True)
    (output / "comparison.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    width, height = a.shape[1], a.shape[0]
    panel = Image.new("RGB", (3*width, height+32), "#161a22")
    for i, (label, pixels) in enumerate([(first_label, a), (second_label, b), ("Absolute difference x4", np.clip(abs(a-b)*4,0,1))]):
        panel.paste(Image.fromarray(np.uint8(np.clip(pixels*255,0,255))), (i*width,32))
        ImageDraw.Draw(panel).text((i*width+8,8),label,fill="white")
    panel.save(output / "comparison.png")
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    comparison = commands.add_parser("compare")
    comparison.add_argument("first", type=Path)
    comparison.add_argument("second", type=Path)
    comparison.add_argument("output", type=Path)
    comparison.add_argument("--first-label", default="Reference")
    comparison.add_argument("--second-label", default="Candidate")
    args = vars(parser.parse_args())
    command = args.pop("command")
    compare(**args)
