"""Prepare identical ReSTIR fixtures and compare CUDA/Vulkan evaluation images.

Original assets/scenes are read only. Run vision-eval under vision_thermal_run.py;
use VISION_EVAL_FRAME_INTERVAL_MS to keep temporal convergence thermally safe.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def prepare(source: Path, destination: Path, width: int, height: int, denoiser: str):
    destination.mkdir(parents=True, exist_ok=True)
    inventory = []
    for directory in sorted(path for path in source.iterdir() if path.is_dir()):
        path = directory / "vision_scene.json"
        record = {"scene": directory.name, "source": str(path)}
        try:
            data = json.loads(path.read_text(encoding="utf-8-sig"))
            missing = []

            def resolve_assets(value):
                if isinstance(value, dict):
                    for key, item in value.items():
                        if key == "fn" and isinstance(item, str) and item:
                            asset = (directory / item).resolve()
                            value[key] = asset.as_posix()
                            if not asset.is_file():
                                missing.append(str(asset))
                        else:
                            resolve_assets(item)
                elif isinstance(value, list):
                    for item in value:
                        resolve_assets(item)

            resolve_assets(data.get("scene", {}))
            if missing:
                raise ValueError(f"Missing assets: {missing[:5]}")
            render = data.setdefault("render", {})
            integrator = render.setdefault("integrator", {})
            integrator["type"] = "rt"
            parameters = integrator.setdefault("param", {})
            for channel in ["direct", "indirect"]:
                settings = parameters.setdefault(channel, {})
                settings.setdefault("spatial", {})
                settings.setdefault("temporal", {})
            if denoiser == "svgf":
                dn = parameters.setdefault("denoiser", {})
                dn["type"] = "svgf"
                dn.setdefault("param", {})["enabled"] = True
            else:
                parameters["denoiser"] = {"type": denoiser, "param": {"enabled": True}}
            render.setdefault("sampler", {}).setdefault("param", {})["spp"] = 1
            fb = data.setdefault("pipeline", {}).setdefault("param", {}).setdefault("frame_buffer", {})
            if fb.get("type", "normal") != "normal":
                raise ValueError("Light-field framebuffer requires a separate comparison protocol")
            fb["type"] = "normal"
            fb.setdefault("param", {}).update(resolution=[width, height], accumulation=False)
            data["output"] = {"spp": 32, "denoise": True, "save_exit": False, "fn": "result.png"}
            output = destination / directory.name / "vision_scene.json"
            output.parent.mkdir(exist_ok=True)
            output.write_text(json.dumps(data, indent=2), encoding="utf-8")
            record.update(status="ready", fixture=str(output), resolution=[width, height], denoiser=denoiser)
        except (OSError, ValueError) as error:
            record.update(status="skipped", reason=str(error))
        inventory.append(record)
    (destination / "inventory.json").write_text(json.dumps(inventory, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(inventory, ensure_ascii=False, indent=2))


def compare(first: Path, second: Path, output: Path, first_label="CUDA", second_label="Vulkan"):
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
                   ssim_luminance=float(ssim.mean()), cuda_mean_luminance=float(x.mean()),
                   vulkan_mean_luminance=float(y.mean()), luminance_ratio=float(y.mean()/max(x.mean(), 1e-20)))
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
        metrics['linear_nonfinite_values'] = dict(cuda=nonfinite[0], vulkan=nonfinite[1])
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
    prep = commands.add_parser("prepare")
    prep.add_argument("source", type=Path)
    prep.add_argument("destination", type=Path)
    prep.add_argument("--width", type=int, default=640)
    prep.add_argument("--height", type=int, default=360)
    prep.add_argument("--denoiser", choices=["svgf", "ReBLUR", "ReLAX"], default="svgf")
    comparison = commands.add_parser("compare")
    comparison.add_argument("first", type=Path)
    comparison.add_argument("second", type=Path)
    comparison.add_argument("output", type=Path)
    comparison.add_argument("--first-label", default="CUDA")
    comparison.add_argument("--second-label", default="Vulkan")
    args = vars(parser.parse_args())
    command = args.pop("command")
    (prepare if command == "prepare" else compare)(**args)
