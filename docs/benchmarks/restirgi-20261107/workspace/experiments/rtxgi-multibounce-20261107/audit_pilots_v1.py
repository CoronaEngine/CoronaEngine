"""Read-only CPU audit of the root-owned RTXPT/RTXDI four-frame pilots."""
from pathlib import Path
import hashlib
import json
import numpy as np

EXP = Path(__file__).resolve().parent
PREVIOUS = EXP.parent / "rtxgi-kitchen-bench-20261107"
SHAPE = (1080, 1920, 4)
PIXELS = SHAPE[0] * SHAPE[1]


def read(path):
    data = np.fromfile(path, dtype="<f4")
    assert data.size == PIXELS * 4, str(path)
    return data.reshape(-1, 4)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def transport_stats(path, primary_valid):
    raw = read(path)
    assert np.isfinite(raw).all(), str(path)
    assert np.array_equal(raw, np.round(raw)), str(path)
    data = raw.astype(np.int32)
    depth, tests, survivals, reason = data.T
    expected_tests = np.maximum(np.minimum(depth, 15) - 5, 0)
    count_mismatch = (tests != expected_tests) | (survivals + (reason == 2) != tests)
    malformed = (depth < 0) | (depth > 16) | (reason < 0) | (reason > 3) | (survivals < 0) | (survivals > tests)
    premature = (tests > 0) & (depth < 6)
    rr_terminal_malformed = (reason == 2) & ((depth < 6) | (depth >= 16))
    max_terminal_malformed = (reason == 3) != (depth == 16)
    first_test = depth >= 6
    first_survival = survivals >= 1
    first_tests_count = int(first_test.sum())
    first_survivals_count = int(first_survival.sum())
    later_tests = int(tests.sum()) - first_tests_count
    later_survivals = int(survivals.sum()) - first_survivals_count
    secondary_vertices = np.maximum(depth - 1, 0)
    # A miss adds one traced continuation beyond the last visited surface.
    # Valid-guide depth0 diagnostics are excluded from this reconstructed count.
    traced_scatter_rays = secondary_vertices + ((reason == 1) & (depth >= 1))
    return {
        "file": str(path.relative_to(EXP)),
        "sha256": digest(path),
        "last_depth_histogram": np.bincount(depth, minlength=17).tolist(),
        "termination_histogram": np.bincount(reason, minlength=4).tolist(),
        "RR_tests": int(tests.sum()),
        "RR_survivals": int(survivals.sum()),
        "first_RR_tests": first_tests_count,
        "first_RR_survivals": first_survivals_count,
        "empirical_first_RR_survival": first_survivals_count / first_tests_count,
        "expected_first_RR_survival": 0.16807,
        "later_RR_tests": later_tests,
        "later_RR_survivals": later_survivals,
        "empirical_later_RR_survival": later_survivals / later_tests,
        "expected_later_RR_survival": 0.7,
        "malformed_records": int(malformed.sum()),
        "RR_count_mismatches": int(count_mismatch.sum()),
        "RR_before_surface6": int(premature.sum()),
        "RR_terminal_malformed": int(rr_terminal_malformed.sum()),
        "max16_terminal_malformed": int(max_terminal_malformed.sum()),
        "invalid_primary_not_reason0": int(((~primary_valid) & (reason != 0)).sum()),
        "valid_primary_with_depth0": int((primary_valid & (depth == 0)).sum()),
        "max_depth_observed": int(depth.max()),
        "paths_reaching16": int((depth == 16).sum()),
        "mean_last_visited_surface_depth_valid_primary": float(depth[primary_valid].mean()),
        "visited_secondary_vertices_total": int(secondary_vertices.sum()),
        "mean_visited_secondary_vertices_per_valid_primary": float(secondary_vertices[primary_valid].mean()),
        "reconstructed_scatter_rays_total": int(traced_scatter_rays.sum()),
        "mean_reconstructed_scatter_rays_per_valid_primary": float(traced_scatter_rays[primary_valid].mean()),
        "work_note": "secondary vertices equal sun-NEE evaluation attempts at surfaces2..lastDepth; scatter-ray count reconstructed from terminal depth/reason, excluding camera ray and any native reconnect visibility rays",
    }


def radiance_stats(path, valid):
    data = read(path)
    rgb = data[:, :3]
    return {
        "file": str(path.relative_to(EXP)),
        "sha256": digest(path),
        "nonfinite_components": int((~np.isfinite(data)).sum()),
        "negative_RGB_components": int((rgb < 0).sum()),
        "whole_frame_RGB_mean": rgb.mean(axis=0, dtype=np.float64).tolist(),
        "whole_frame_scalar_RGB_mean": float(rgb.mean(dtype=np.float64)),
        "valid_primary_RGB_mean": rgb[valid].mean(axis=0, dtype=np.float64).tolist(),
        "minimum_RGB": float(rgb.min()),
        "maximum_RGB": float(rgb.max()),
        "scalar_RGB_percentiles": np.percentile(rgb.mean(axis=1), [0, 50, 95, 99, 99.9, 100]).tolist(),
        "alpha_values": np.unique(data[:, 3]).tolist(),
    }


def compare_guides(pt_run, di_run):
    pta = pt_run / "primary-position.rgba32f"
    dia = di_run / "capture.position.rgba32f"
    ptn = pt_run / "primary-geonormal.rgba32f"
    din = di_run / "capture.normal.rgba32f"
    a, b = read(pta), read(dia)
    na, nb = read(ptn), read(din)
    va, vb = a[:, 3] > 0, b[:, 3] > 0
    common = va & vb
    distance = np.linalg.norm(a[common, :3].astype(np.float64) - b[common, :3], axis=1)
    normal_dot = np.sum(na[common, :3].astype(np.float64) * nb[common, :3], axis=1)
    result = {
        "rtxpt_valid_primary": int(va.sum()),
        "rtxdi_valid_primary": int(vb.sum()),
        "validity_mismatch_pixels": int((va != vb).sum()),
        "common_valid_pixels": int(common.sum()),
        "position_distance_m_percentiles_p50_p95_p99_p999_max": np.percentile(distance, [50, 95, 99, 99.9, 100]).tolist(),
        "positions_within_1mm_fraction": float((distance < 0.001).mean()),
        "normal_dot_percentiles_min_p01_p1_p50": np.percentile(normal_dot, [0, 0.01, 1, 50]).tolist(),
        "normal_dot_above_0999_fraction": float((normal_dot > 0.999).mean()),
        "prior_final_input_guides_byte_identical": {},
    }
    for name, new, old in [
        ("rtxpt_position", pta, PREVIOUS / "runs/rtxpt-quality-r1-v2/primary-position.rgba32f"),
        ("rtxpt_normal", ptn, PREVIOUS / "runs/rtxpt-quality-r1-v2/primary-geonormal.rgba32f"),
        ("rtxdi_position", dia, PREVIOUS / "runs/rtxdi-quality-r1-v2/capture.position.rgba32f"),
        ("rtxdi_normal", din, PREVIOUS / "runs/rtxdi-quality-r1-v2/capture.normal.rgba32f"),
    ]:
        result["prior_final_input_guides_byte_identical"][name] = digest(new) == digest(old)
    return result, va, vb


def main():
    pt = EXP / "runs/rtxpt-pilot-none-v1"
    di = EXP / "runs/rtxdi-pilot-none-v1"
    guide_result, pt_valid, di_valid = compare_guides(pt, di)
    report = {
        "schema": "four-way-multibounce-pilot-acceptance.v1",
        "status": "pending",
        "scope": "RTXPT and RTXDI four-frame no-reuse pilots only; no source/build/GPU mutation",
        "guides": guide_result,
        "renderers": {},
        "issues": [],
        "limitations": [
            "Four-sample whole-frame means are descriptive and do not establish converged reference agreement or brightness bias.",
            "The old one-indirect-reflection radiance bound is inapplicable to multibounce transport.",
            "Reconstructed path-work counters describe candidate/fresh path generation, excluding native reconnect visibility and mandatory preparation.",
        ],
    }
    di_stdout = (di / "stdout.txt").read_text()
    report["rtxdi_actual_secondary_shader"] = {
        "USE_RAY_QUERY": 0,
        "REGIR_MODE": "ONION",
        "bytes": 156232,
        "fnv64": "72dbfc65e26cf6cd",
        "runtime_matches_frozen_shader": "BENCH_SHADER_SELECTED_LIBRARY size=156232 fnv64=72dbfc65e26cf6cd" in di_stdout,
        "note": "Actual per-pass library log overrides the app's broader ray_query=true settings field.",
    }
    gates = []
    for renderer, run, valid in [("rtxpt", pt, pt_valid), ("rtxdi", di, di_valid)]:
        frames = []
        for frame in range(4):
            if renderer == "rtxpt":
                native = run / f"gi-frame-{frame}.rgba32f"
                fresh = run / f"gi-fresh-frame-{frame}.rgba32f"
                diag = run / f"gi-diagnostic-frame-{frame}.rgba32f"
            else:
                native = run / f"capture.frame{frame}.rgba32f"
                fresh = run / f"capture.fresh.frame{frame}.rgba32f"
                diag = run / f"capture.pathstats.frame{frame}.rgba32f"
            stats = transport_stats(diag, valid)
            entry = {"frame": frame, "path": stats, "native": radiance_stats(native, valid), "fresh": radiance_stats(fresh, valid)}
            if renderer == "rtxdi":
                entry["independent_fresh_path"] = transport_stats(run / f"capture.freshpathstats.frame{frame}.rgba32f", valid)
                captured_summary = json.loads((run / f"capture.pathstats.frame{frame}.json").read_text())
                summary_matches = all(
                    captured_summary[label][field] == actual[field]
                    for label, actual in [("candidate", stats), ("independent_fresh", entry["independent_fresh_path"])]
                    for field in ["last_depth_histogram", "termination_histogram", "RR_tests", "RR_survivals"]
                )
            else:
                captured_summary = json.loads((run / f"path-depth-frame-{frame}.json").read_text())
                summary_matches = (
                    captured_summary["terminal_surface_histogram"] == stats["last_depth_histogram"]
                    and captured_summary["RR_tests"] == stats["RR_tests"]
                    and captured_summary["RR_survivals"] == stats["RR_survivals"]
                    and [captured_summary[f"reason_{label}"] for label in ["invalid", "miss", "RR", "max16"]] == stats["termination_histogram"]
                )
            entry["captured_JSON_histograms_match_raw_RGBA"] = summary_matches
            gates.append(summary_matches)
            # RTXPT fresh and diagnostic share the same unpacked generated path;
            # RTXDI independently retraces the fresh path only in capture modes.
            for pathstats in [stats] + ([entry["independent_fresh_path"]] if renderer == "rtxdi" else []):
                gates.append(all(pathstats[key] == 0 for key in ["malformed_records", "RR_count_mismatches", "RR_before_surface6", "RR_terminal_malformed", "max16_terminal_malformed"]))
                gates.append(pathstats["max_depth_observed"] == 16 and pathstats["paths_reaching16"] > 0)
                gates.append(pathstats["mean_visited_secondary_vertices_per_valid_primary"] > 1)
            for rad in [entry["native"], entry["fresh"]]:
                gates.append(rad["nonfinite_components"] == 0 and rad["negative_RGB_components"] == 0)
            frames.append(entry)
        report["renderers"][renderer] = {
            "frames": frames,
            "four_frame_whole_frame_fresh_scalar_mean": float(np.mean([f["fresh"]["whole_frame_scalar_RGB_mean"] for f in frames])),
            "four_frame_whole_frame_native_scalar_mean": float(np.mean([f["native"]["whole_frame_scalar_RGB_mean"] for f in frames])),
            "mean_visited_secondary_vertices_per_valid_primary": float(np.mean([f["path"]["mean_visited_secondary_vertices_per_valid_primary"] for f in frames])),
            "mean_reconstructed_scatter_rays_per_valid_primary": float(np.mean([f["path"]["mean_reconstructed_scatter_rays_per_valid_primary"] for f in frames])),
            "launch_exit_code": json.loads((run / "launch-manifest.json").read_text())["exit_code"],
        }
    report["issues"].append({
        "severity": "diagnostic-only",
        "renderer": "rtxpt",
        "description": "Depth0 invalid-primary diagnostics use termination reason1 (miss) although declared shared reason0 means invalid primary; some valid-guide pixels also report depth0. Audit uses guide validity independently and does not treat these rows as one-bounce paths.",
        "invalid_primary_not_reason0_per_frame": [f["path"]["invalid_primary_not_reason0"] for f in report["renderers"]["rtxpt"]["frames"]],
        "valid_primary_with_depth0_per_frame": [f["path"]["valid_primary_with_depth0"] for f in report["renderers"]["rtxpt"]["frames"]],
    })
    gates.extend([guide_result["positions_within_1mm_fraction"] == 1.0, guide_result["validity_mismatch_pixels"] <= 1])
    gates.append(all(guide_result["prior_final_input_guides_byte_identical"].values()))
    gates.append(report["rtxdi_actual_secondary_shader"]["runtime_matches_frozen_shader"])
    report["status"] = "pass_with_diagnostic_convention_note" if all(gates) else "fail"
    report["runtime_transport_gate_passed"] = bool(all(gates))
    report["reference_agreement_gate"] = "pending independent high-sample split references"
    output = EXP / "pilot-acceptance-v1.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "guides": guide_result, "renderers": {k: {x: v[x] for x in ["four_frame_whole_frame_fresh_scalar_mean", "four_frame_whole_frame_native_scalar_mean", "mean_visited_secondary_vertices_per_valid_primary", "mean_reconstructed_scatter_rays_per_valid_primary"]} for k, v in report["renderers"].items()}, "issues": report["issues"]}, indent=2))


if __name__ == "__main__":
    main()
