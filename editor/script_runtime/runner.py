"""Execution boundary for generated Blockly/Scratch scripts.

Generated files are owned by the Script Runtime under ``runtime/generated``.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from typing import Any


_GENERATED_SCRIPT_RELATIVE_PATH = Path("runtime") / "generated" / "blockly_code.py"


def _clear_generated_script_cache() -> None:
    for module_name in list(sys.modules):
        if (
            module_name.startswith("corona_generated_blockly")
        ):
            sys.modules.pop(module_name, None)


def run_generated_script(repo_root: str | Path) -> Any:
    """Load and execute the generated project script from ``repo_root``.

    Missing output is reported as ``None`` so callers can preserve the existing
    "no Blockly script" behavior.
    """

    root = Path(repo_root)
    generated_script_path = root / _GENERATED_SCRIPT_RELATIVE_PATH
    if not generated_script_path.is_file():
        return None

    importlib.invalidate_caches()
    _clear_generated_script_cache()
    module_name = "corona_generated_blockly"
    script_path = generated_script_path

    spec = importlib.util.spec_from_file_location(module_name, script_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"Unable to load generated script: {script_path}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    run = getattr(module, "run", None)
    if not callable(run):
        raise AttributeError(f"Generated script has no callable run(): {script_path}")
    return run()


__all__ = ["run_generated_script"]
