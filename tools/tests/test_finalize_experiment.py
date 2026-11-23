import os
import stat
import subprocess
import sys

import pytest

from tools import finalize_experiment


def test_finalization_removes_only_scratch_and_can_be_repeated(tmp_path):
    report = tmp_path / "report.json"
    report.write_text("{}", encoding="utf-8")
    final_image = tmp_path / "image.pfm"
    final_image.touch()
    baseline = tmp_path / "baseline"
    baseline.mkdir()
    (baseline / "reuse.bin").touch()
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    (scratch / "readback.bin").touch()

    finalize_experiment.finalize(tmp_path, report)
    finalize_experiment.finalize(tmp_path, report)

    assert not scratch.exists()
    assert report.is_file() and final_image.is_file() and (baseline / "reuse.bin").is_file()


def test_independent_experiment_cleans_only_its_declared_paths(tmp_path):
    output = tmp_path / "run"
    capture = output / "capture"
    cache = capture / "cef-cache"
    cache.mkdir(parents=True)
    (cache / "data").touch()
    trace = capture / "api.jsonl"
    trace.touch()
    image = capture / "final.pfm"
    image.touch()
    reference = capture / "frozen-input.bin"
    reference.touch()
    report = output / "result.json"
    report.write_text("{}", encoding="utf-8")

    for _ in range(2):
        finalize_experiment.finalize(output, report, disposable=["capture/cef-cache", trace])

    assert not cache.exists() and not trace.exists()
    assert report.is_file() and image.is_file() and reference.is_file()


@pytest.mark.parametrize("unsafe", ["..", "../outside", "baseline/reference.bin", "capture", "capture/.."])
def test_invalid_explicit_candidate_prevents_any_cleanup(tmp_path, unsafe):
    output = tmp_path / "run"
    capture = output / "capture"
    capture.mkdir(parents=True)
    report = capture / "result.json"
    report.write_text("{}", encoding="utf-8")
    trace = output / "trace"
    trace.touch()
    baseline = output / "baseline"
    baseline.mkdir()
    (baseline / "reference.bin").touch()
    outside = tmp_path / "outside"
    outside.touch()

    with pytest.raises(ValueError, match="disposable"):
        finalize_experiment.finalize(output, report, disposable=[trace, unsafe])

    assert trace.is_file() and outside.is_file() and report.is_file()


def test_requires_an_existing_report_outside_scratch(tmp_path):
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    inside_report = scratch / "report.json"
    inside_report.write_text("{}", encoding="utf-8")

    for report in (inside_report, tmp_path / "missing.json"):
        with pytest.raises(ValueError, match="report"):
            finalize_experiment.finalize(tmp_path, report)
        assert inside_report.is_file()


def test_incomplete_report_cannot_trigger_cleanup(tmp_path):
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    report = tmp_path / "summary.json"
    report.write_text('{"complete": false}', encoding="utf-8")

    with pytest.raises(ValueError, match="incomplete"):
        finalize_experiment.finalize(tmp_path, report)

    assert scratch.is_dir()


def test_cli_rejects_output_that_resolves_to_experiments_parent(tmp_path, monkeypatch):
    experiments = tmp_path / "experiments"
    experiments.mkdir()
    monkeypatch.setattr(finalize_experiment, "EXPERIMENTS", experiments)
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    (tmp_path / "report.md").write_text("Complete", encoding="utf-8")

    with pytest.raises(SystemExit) as error:
        finalize_experiment.main([str(experiments / ".."), "report.md"])
    assert error.value.code == 1
    assert scratch.is_dir()


def test_scratch_with_link_is_left_intact(tmp_path):
    report = tmp_path / "report.json"
    report.write_text("{}", encoding="utf-8")
    baseline = tmp_path / "baseline"
    baseline.mkdir()
    (baseline / "reference.bin").touch()
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    link = scratch / "external"
    if sys.platform == "win32":
        subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(link), str(baseline)], check=True, capture_output=True)
    else:
        link.symlink_to(baseline, target_is_directory=True)

    with pytest.raises(ValueError, match="linked"):
        finalize_experiment.finalize(tmp_path, report)

    assert os.path.lexists(link)
    assert (baseline / "reference.bin").is_file()


@pytest.mark.skipif(sys.platform != "win32", reason="Windows prevents unlinking read-only files")
def test_windows_can_remove_readonly_scratch_copies(tmp_path):
    report = tmp_path / "report.md"
    report.write_text("Complete", encoding="utf-8")
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    artifact = scratch / "engine.dll"
    artifact.touch()
    artifact.chmod(stat.S_IREAD)

    finalize_experiment.finalize(tmp_path, report)

    assert not scratch.exists()
