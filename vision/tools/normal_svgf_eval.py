import argparse
import csv
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from statistics import mean


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = REPO_ROOT / "cmake-build-codex-msvc3"
DEFAULT_RUNTIME_DIR = DEFAULT_BUILD_DIR / "bin"
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / "eval-out" / "normal-svgf-fpa"
RESULTS_TSV = REPO_ROOT / ".agent-os" / "normal-svgf-results.tsv"
ITERATE_JSONL = REPO_ROOT / ".agent-os" / "normal-svgf-iterate-log.jsonl"

SCENES = {
    "spaceship": Path(r"D:\yzy\code\cpp\CoronaTestScenes\test_vision\render_scene\spaceship\vision_rt.json"),
    "kitchen": Path(r"D:\yzy\code\cpp\CoronaTestScenes\test_vision\render_scene\kitchen\vision_rt.json"),
    "bathroom2": Path(r"D:\yzy\code\cpp\CoronaTestScenes\test_vision\render_scene\bathroom2\vision_rt.json"),
}

RESULT_FIELDS = [
    "iteration",
    "status",
    "scene_key",
    "mode",
    "source_commit",
    "source_dirty",
    "resolution",
    "save_spp",
    "golden_spp",
    "rmse",
    "average_frame_ms",
    "average_fps",
    "firefly_outlier_ratio",
    "firefly_p999_luma",
    "firefly_max_luma",
    "artifact_dir",
    "output_png",
    "golden_png",
    "notes",
]


def parse_resolution(value: str) -> tuple[int, int]:
    m = re.fullmatch(r"\s*(\d+)\s*x\s*(\d+)\s*", value.lower())
    if not m:
        raise argparse.ArgumentTypeError("resolution must look like 1920x1080")
    return int(m.group(1)), int(m.group(2))


def parse_extra_env(values: list[str]) -> dict[str, str]:
    env: dict[str, str] = {}
    for item in values:
        if "=" not in item:
            raise ValueError(f"--env must be KEY=VALUE, got {item!r}")
        key, value = item.split("=", 1)
        key = key.strip()
        if not key:
            raise ValueError(f"--env has an empty key: {item!r}")
        env[key] = value
    return env


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def git_commit() -> str:
    try:
        return subprocess.run(
            ["git", "rev-parse", "--short=12", "HEAD"],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
    except Exception:
        return ""


def git_dirty() -> bool:
    try:
        out = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        return bool(out.strip())
    except Exception:
        return True


def is_relative_asset(value: str) -> bool:
    if not value or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", value):
        return False
    return not Path(value).is_absolute()


def rewrite_fn_paths(scene_text: str, scene_dir: Path) -> str:
    def repl(match: re.Match[str]) -> str:
        prefix = match.group(1)
        value = match.group(2)
        suffix = match.group(3)
        if is_relative_asset(value):
            abs_path = (scene_dir / value).resolve().as_posix()
            return f'{prefix}{abs_path}{suffix}'
        return match.group(0)

    return re.sub(r'("fn"\s*:\s*")([^"]+)(")', repl, scene_text)


def rewrite_resolution(scene_text: str, width: int, height: int) -> tuple[str, int]:
    pattern = re.compile(r'("resolution"\s*:\s*\[\s*)\d+\s*,\s*\d+(\s*\])', re.DOTALL)
    scene_text, count = pattern.subn(rf"\g<1>{width},\n                        {height}\g<2>", scene_text)
    return scene_text, count


def rewrite_sampler_spp(scene_text: str, spp: int = 1) -> str:
    pattern = re.compile(r'("sampler"\s*:\s*\{.*?"param"\s*:\s*\{.*?"spp"\s*:\s*)\d+', re.DOTALL)
    return pattern.sub(rf"\g<1>{spp}", scene_text, count=1)


def rewrite_output_spp(scene_text: str, spp: int = 1) -> str:
    pattern = re.compile(r'("output"\s*:\s*\{.*?"spp"\s*:\s*)\d+', re.DOTALL)
    return pattern.sub(rf"\g<1>{spp}", scene_text, count=1)


def rewrite_output_fn(scene_text: str, output_path: Path) -> str:
    pattern = re.compile(r'("output"\s*:\s*\{.*?"fn"\s*:\s*")[^"]+(")', re.DOTALL)
    replacement = output_path.resolve().as_posix()
    updated, count = pattern.subn(rf"\g<1>{replacement}\2", scene_text, count=1)
    if count == 0:
        raise RuntimeError("scene has no output.fn field to rewrite")
    return updated


def rewrite_direct_m_bsdf(scene_text: str, value: int | None) -> str:
    if value is None:
        return scene_text
    pattern = re.compile(r'("direct"\s*:\s*\{.*?"M_bsdf"\s*:\s*)\d+', re.DOTALL)
    updated, count = pattern.subn(rf"\g<1>{value}", scene_text, count=1)
    if count != 1:
        raise RuntimeError("scene has no direct.M_bsdf field to rewrite")
    return updated


def rewrite_svgf_spatial_filter(scene_text: str, value: bool | None) -> str:
    if value is None:
        return scene_text
    token = "true" if value else "false"
    existing = re.compile(
        r'("denoiser"\s*:\s*\{.*?"type"\s*:\s*"svgf".*?"spatial_filter"\s*:\s*)(true|false)',
        re.DOTALL | re.IGNORECASE,
    )
    updated, count = existing.subn(rf"\g<1>{token}", scene_text, count=1)
    if count:
        return updated
    insert = re.compile(
        r'("denoiser"\s*:\s*\{\s*"type"\s*:\s*"svgf"\s*,\s*"param"\s*:\s*\{)',
        re.DOTALL | re.IGNORECASE,
    )
    updated, count = insert.subn(rf'\g<1>\n                    "spatial_filter": {token},', scene_text, count=1)
    if count != 1:
        raise RuntimeError("scene has no SVGF denoiser param block to rewrite")
    return updated


def generate_scene_copy(
    scene_key: str,
    source: Path,
    dest_dir: Path,
    width: int,
    height: int,
    direct_m_bsdf: int | None = None,
    svgf_spatial_filter: bool | None = None,
) -> Path:
    if not source.exists():
        raise FileNotFoundError(source)
    text = source.read_text(encoding="utf-8")
    text = rewrite_fn_paths(text, source.parent)
    text, resolution_count = rewrite_resolution(text, width, height)
    text = rewrite_sampler_spp(text, 1)
    text = rewrite_output_spp(text, 1)
    text = rewrite_direct_m_bsdf(text, direct_m_bsdf)
    text = rewrite_svgf_spatial_filter(text, svgf_spatial_filter)
    if resolution_count < 2:
        raise RuntimeError(f"{source} had only {resolution_count} resolution block(s); expected camera and framebuffer")
    if not re.search(r'"type"\s*:\s*"normal"', text):
        raise RuntimeError(f"{source} does not look like a normal framebuffer scene")
    if not re.search(r'"type"\s*:\s*"rt"', text):
        raise RuntimeError(f"{source} does not look like an rt scene")
    if not re.search(r'"type"\s*:\s*"svgf"', text, re.IGNORECASE):
        raise RuntimeError(f"{source} does not look like an SVGF scene")

    dest_dir.mkdir(parents=True, exist_ok=True)
    out = dest_dir / f"{scene_key}_{width}x{height}_normal_svgf.json"
    out.write_text(text, encoding="utf-8", newline="\n")
    return out


def generate_run_scene(base_scene: Path, output_path: Path, dest: Path) -> Path:
    text = base_scene.read_text(encoding="utf-8")
    text = rewrite_output_fn(text, output_path)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(text, encoding="utf-8", newline="\n")
    return dest


def percentile(sorted_values: list[float], q: float) -> float:
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return sorted_values[0]
    pos = (len(sorted_values) - 1) * q
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return sorted_values[lo]
    t = pos - lo
    return sorted_values[lo] * (1.0 - t) + sorted_values[hi] * t


def firefly_stats(image_path: Path) -> dict:
    stats = {
        "available": False,
        "pixel_count": 0,
        "mean_luma": 0.0,
        "p99_luma": 0.0,
        "p999_luma": 0.0,
        "max_luma": 0.0,
        "outlier_threshold": 0.0,
        "outlier_count": 0,
        "outlier_ratio": 0.0,
        "error": "",
    }
    try:
        from PIL import Image
    except Exception as exc:
        stats["error"] = f"Pillow unavailable: {exc}"
        return stats
    if not image_path.exists():
        stats["error"] = f"image missing: {image_path}"
        return stats
    try:
        with Image.open(image_path) as img:
            rgb = img.convert("RGB")
            values = []
            for r, g, b in rgb.getdata():
                values.append((0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0)
    except Exception as exc:
        stats["error"] = str(exc)
        return stats
    values.sort()
    p99 = percentile(values, 0.99)
    p999 = percentile(values, 0.999)
    max_luma = values[-1] if values else 0.0
    avg = mean(values) if values else 0.0
    threshold = max(p999 * 4.0, p99 + 0.25, 1.0)
    outliers = sum(1 for value in values if value > threshold)
    stats.update(
        {
            "available": True,
            "pixel_count": len(values),
            "mean_luma": avg,
            "p99_luma": p99,
            "p999_luma": p999,
            "max_luma": max_luma,
            "outlier_threshold": threshold,
            "outlier_count": outliers,
            "outlier_ratio": outliers / len(values) if values else 0.0,
            "error": "",
        }
    )
    return stats


def run_vision_eval(
    exe: Path,
    runtime_dir: Path,
    scene: Path,
    output: Path | None,
    metrics: Path,
    save_spp: int,
    warmup_frames: int,
    profile_frames: int,
    timeout_sec: int,
    reference: Path | None = None,
    disable_denoiser: bool = False,
    denoise: bool = True,
    stage_profile: bool = True,
    extra_env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
    metrics.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(exe),
        "-r",
        str(runtime_dir),
        "-m",
        "cli",
        "-s",
        str(scene),
        "--metrics-file",
        str(metrics),
        "--warmup-frames",
        str(warmup_frames),
        "--profile-frames",
        str(profile_frames),
        "--save-spp",
        str(save_spp),
    ]
    if output is not None:
        cmd.extend(["-o", str(output)])
    if reference is not None:
        cmd.extend(["--rmse-reference", str(reference)])
    if disable_denoiser:
        cmd.append("--disable-denoiser")
    elif denoise:
        cmd.append("--denoise")
    if stage_profile:
        cmd.append("--stage-profile")

    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    stdout_log = metrics.with_name(f"{metrics.stem}-{stamp}.stdout.log")
    stderr_log = metrics.with_name(f"{metrics.stem}-{stamp}.stderr.log")
    completed = subprocess.run(
        cmd,
        cwd=runtime_dir,
        env=env,
        text=True,
        capture_output=True,
        timeout=timeout_sec,
        check=False,
    )
    stdout_log.write_text(completed.stdout, encoding="utf-8", errors="replace")
    stderr_log.write_text(completed.stderr, encoding="utf-8", errors="replace")
    if completed.returncode != 0:
        raise RuntimeError(
            f"vision-eval failed with code {completed.returncode}; stdout={stdout_log}; stderr={stderr_log}"
        )
    return completed


def load_metrics(path: Path) -> dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def append_tsv(row: dict) -> None:
    RESULTS_TSV.parent.mkdir(parents=True, exist_ok=True)
    has_header = RESULTS_TSV.exists() and RESULTS_TSV.read_text(encoding="utf-8", errors="ignore").strip()
    with RESULTS_TSV.open("a", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=RESULT_FIELDS, delimiter="\t", lineterminator="\n")
        if not has_header:
            writer.writeheader()
        writer.writerow({field: row.get(field, "") for field in RESULT_FIELDS})


def append_jsonl(record: dict) -> None:
    ITERATE_JSONL.parent.mkdir(parents=True, exist_ok=True)
    with ITERATE_JSONL.open("a", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(record, sort_keys=True, ensure_ascii=False) + "\n")


def evaluate_scene(args: argparse.Namespace, scene_key: str, iteration: str) -> dict:
    width, height = args.resolution
    artifact_root = args.artifact_root.resolve()
    scenes_dir = artifact_root / "scenes"
    source_scene = SCENES[scene_key]
    generated_scene = generate_scene_copy(
        scene_key,
        source_scene,
        scenes_dir,
        width,
        height,
        direct_m_bsdf=args.direct_m_bsdf,
        svgf_spatial_filter=args.svgf_spatial_filter,
    )

    scene_hash = sha256_file(generated_scene)
    iter_dir = artifact_root / f"iter-{iteration}" / scene_key
    golden_dir = artifact_root / "golden" / scene_key
    iter_dir.mkdir(parents=True, exist_ok=True)
    golden_dir.mkdir(parents=True, exist_ok=True)

    golden_png = golden_dir / f"golden_{args.golden_spp}spp_{width}x{height}.png"
    golden_metrics = golden_dir / f"golden_{args.golden_spp}spp_metrics.json"
    output_png = iter_dir / f"svgf_{args.save_spp}spp_{width}x{height}.png"
    output_metrics = iter_dir / "metrics.json"
    firefly_json = iter_dir / "firefly_stats.json"
    golden_scene = golden_dir / f"scene_golden_{args.golden_spp}spp_{width}x{height}.json"
    output_scene = iter_dir / f"scene_svgf_{args.save_spp}spp_{width}x{height}.json"

    exe = args.exe.resolve()
    runtime_dir = args.runtime_dir.resolve()

    if args.generate_golden and (args.force_golden or not golden_png.exists()):
        run_scene = generate_run_scene(generated_scene, golden_png, golden_scene)
        run_vision_eval(
            exe,
            runtime_dir,
            run_scene,
            None,
            golden_metrics,
            args.golden_spp,
            args.golden_warmup_frames,
            args.golden_profile_frames,
            args.timeout_sec,
            reference=None,
            disable_denoiser=True,
            denoise=False,
            stage_profile=args.stage_profile,
            extra_env=parse_extra_env(args.env),
        )

    if not golden_png.exists():
        raise FileNotFoundError(f"golden missing: {golden_png}")

    run_scene = generate_run_scene(generated_scene, output_png, output_scene)
    run_vision_eval(
        exe,
        runtime_dir,
        run_scene,
        None,
        output_metrics,
        args.save_spp,
        args.warmup_frames,
        args.profile_frames,
        args.timeout_sec,
        reference=golden_png,
        disable_denoiser=args.disable_denoiser_output,
        denoise=not args.disable_denoiser_output,
        stage_profile=args.stage_profile,
        extra_env=parse_extra_env(args.env),
    )

    metrics = load_metrics(output_metrics)
    ff = firefly_stats(output_png)
    firefly_json.write_text(json.dumps(ff, indent=2) + "\n", encoding="utf-8")
    source_commit = git_commit()
    source_dirty = git_dirty()
    rmse = metrics.get("rmse", 0.0)
    average_frame_ms = metrics.get("average_frame_ms", 0.0)
    average_fps = metrics.get("average_fps", 0.0)
    non_black = float(ff.get("p999_luma", 0.0)) > 0.01 or float(ff.get("max_luma", 0.0)) > 0.05
    status = "pass" if non_black and rmse < 0.03 and average_fps > 40.0 else "diagnostic"
    notes = (
        f"scene_sha256={scene_hash}; generated_scene={generated_scene}; "
        f"golden_sha256={sha256_file(golden_png)}; mode={args.mode}; "
        f"env={','.join(args.env) if args.env else ''}; direct_m_bsdf={args.direct_m_bsdf}; "
        f"svgf_spatial_filter={args.svgf_spatial_filter}"
    )
    row = {
        "iteration": iteration,
        "status": status,
        "scene_key": scene_key,
        "mode": args.mode,
        "source_commit": source_commit,
        "source_dirty": str(source_dirty).lower(),
        "resolution": f"{width}x{height}",
        "save_spp": args.save_spp,
        "golden_spp": args.golden_spp,
        "rmse": f"{float(rmse):.7f}",
        "average_frame_ms": f"{float(average_frame_ms):.4f}",
        "average_fps": f"{float(average_fps):.4f}",
        "firefly_outlier_ratio": f"{float(ff.get('outlier_ratio', 0.0)):.8f}",
        "firefly_p999_luma": f"{float(ff.get('p999_luma', 0.0)):.6f}",
        "firefly_max_luma": f"{float(ff.get('max_luma', 0.0)):.6f}",
        "artifact_dir": str(iter_dir),
        "output_png": str(output_png),
        "golden_png": str(golden_png),
        "notes": notes,
    }
    append_tsv(row)
    append_jsonl({**row, "metrics": metrics, "firefly": ff})
    summary = {
        **row,
        "metrics_file": str(output_metrics),
        "firefly_file": str(firefly_json),
        "generated_scene": str(generated_scene),
        "run_scene": str(output_scene),
    }
    (iter_dir / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Evaluate ordinary 2D RT+SVGF scenes for the FPA demo sprint.")
    parser.add_argument("--scene-key", choices=[*SCENES.keys(), "all"], default="all")
    parser.add_argument("--mode", default="baseline")
    parser.add_argument("--iteration", default="normal-svgf-0001")
    parser.add_argument("--resolution", type=parse_resolution, default=parse_resolution("1920x1080"))
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--runtime-dir", type=Path, default=DEFAULT_RUNTIME_DIR)
    parser.add_argument("--exe", type=Path, default=DEFAULT_RUNTIME_DIR / "vision-eval.exe")
    parser.add_argument("--save-spp", type=int, default=1)
    parser.add_argument("--golden-spp", type=int, default=4096)
    parser.add_argument("--warmup-frames", type=int, default=8)
    parser.add_argument("--profile-frames", type=int, default=32)
    parser.add_argument("--golden-warmup-frames", type=int, default=0)
    parser.add_argument("--golden-profile-frames", type=int, default=1)
    parser.add_argument("--timeout-sec", type=int, default=7200)
    parser.add_argument("--generate-golden", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--force-golden", action="store_true")
    parser.add_argument("--stage-profile", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--disable-denoiser-output", action="store_true")
    parser.add_argument(
        "--direct-m-bsdf",
        type=int,
        default=None,
        help="Optional direct ReSTIR M_bsdf override for generated scene copies.",
    )
    parser.add_argument(
        "--svgf-spatial-filter",
        action=argparse.BooleanOptionalAction,
        default=None,
        help="Optional SVGF spatial_filter override for generated scene copies.",
    )
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Additional environment variable for vision-eval; repeatable for diagnostic pass switches.",
    )
    args = parser.parse_args()

    scene_keys = list(SCENES) if args.scene_key == "all" else [args.scene_key]
    summaries = []
    failures = []
    for scene_key in scene_keys:
        try:
            summaries.append(evaluate_scene(args, scene_key, args.iteration))
        except Exception as exc:
            record = {
                "iteration": args.iteration,
                "status": "eval_fail",
                "scene_key": scene_key,
                "mode": args.mode,
                "source_commit": git_commit(),
                "source_dirty": str(git_dirty()).lower(),
                "resolution": f"{args.resolution[0]}x{args.resolution[1]}",
                "save_spp": args.save_spp,
                "golden_spp": args.golden_spp,
                "artifact_dir": str((args.artifact_root / f"iter-{args.iteration}" / scene_key).resolve()),
                "notes": str(exc).replace("\t", " ").replace("\n", " "),
            }
            append_tsv(record)
            append_jsonl(record)
            failures.append(record)
    output = {"summaries": summaries, "failures": failures}
    json.dump(output, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
