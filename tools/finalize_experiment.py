"""Remove one completed experiment's disposable files after writing its final report."""

import argparse
import json
import os
from pathlib import Path
import shutil
import stat


EXPERIMENTS = Path(__file__).resolve().parents[1] / ".workspace/experiments"
PROTECTED_NAMES = {".git", "baseline", "baselines"}


def _linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(
        getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    )


def _raise_walk_error(error):
    raise error


def _retry_readonly(function, path, error):
    if os.name == "nt" and function == os.unlink and isinstance(error, PermissionError):
        info = os.lstat(path)
        if (stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and not _linked(info)
                and info.st_file_attributes & stat.FILE_ATTRIBUTE_READONLY):
            os.chmod(path, stat.S_IWRITE)
            function(path)
            return
    raise error


def _remove_error(function, path, exc_info):
    _retry_readonly(function, path, exc_info[1])


def _check_disposable(output, report, entry):
    target = output / entry
    if target == output or not target.is_relative_to(output) or report.is_relative_to(target):
        raise ValueError(f"Invalid disposable path: {target}")
    parts = target.relative_to(output).parts
    if any(part in {".", ".."} or ":" in part or part != part.rstrip(" .")
           or part.casefold() in PROTECTED_NAMES for part in parts):
        raise ValueError(f"Invalid disposable path: {target}")

    current = output
    for part in parts:
        current /= part
        try:
            info = current.lstat()
        except FileNotFoundError:
            return None
        if _linked(info):
            raise ValueError(f"Disposable path is linked: {current}")
    if stat.S_ISDIR(info.st_mode):
        for folder, dirs, files in os.walk(target, onerror=_raise_walk_error):
            for name in dirs + files:
                child = Path(folder) / name
                if name.rstrip(" .").casefold() in PROTECTED_NAMES or _linked(child.lstat()):
                    raise ValueError(f"Disposable directory contains linked or protected content: {child}")
        return target, True
    if stat.S_ISREG(info.st_mode):
        return target, False
    raise ValueError(f"Disposable path is not an ordinary file or directory: {target}")


def finalize(output: Path, report: Path, *, disposable=None) -> None:
    """Clean one run's scratch directory or its explicit list of disposable paths.

    Call after all producers and analysis steps have finished. Explicit paths
    are relative to the run directory unless given as absolute paths.
    """
    if _linked(output.lstat()):
        raise ValueError(f"Experiment directory is linked: {output}")
    output = output.resolve(strict=True)
    scratch = output / "scratch"
    report = report.resolve()
    if (not report.is_relative_to(output) or report.is_relative_to(scratch)
            or not report.is_file() or not report.stat().st_size):
        raise ValueError("A written final report outside scratch is required")
    if report.suffix.lower() == ".json":
        status = json.loads(report.read_text(encoding="utf-8-sig"))
        if isinstance(status, dict) and status.get("complete") is False:
            raise ValueError(f"Experiment report is incomplete: {report}")

    targets = {}
    for entry in (scratch,) if disposable is None else disposable:
        checked = _check_disposable(output, report, entry)
        if checked is not None:
            target, is_directory = checked
            if disposable is None and not is_directory:
                raise ValueError(f"Scratch must be an ordinary directory: {scratch}")
            targets[target] = is_directory
    for target in sorted(targets, key=lambda path: len(path.parts), reverse=True):
        if targets[target]:
            shutil.rmtree(target, onerror=_remove_error)
        else:
            try:
                target.unlink()
            except PermissionError as error:
                _retry_readonly(os.unlink, target, error)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("experiment", type=Path, help="run directory under .workspace/experiments")
    parser.add_argument("report", type=Path, help="final report path relative to the run directory")
    args = parser.parse_args(argv)
    output = args.experiment.absolute()
    try:
        if output.parent != EXPERIMENTS or output.resolve(strict=True).parent != EXPERIMENTS:
            raise ValueError(f"Choose a run directory directly under {EXPERIMENTS}")
        finalize(output, output / args.report)
    except (OSError, ValueError) as error:
        parser.exit(1, f"Experiment cleanup failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
