"""Line budget for the C++ -> web interface layer.

Every slimming task lowers ``CORE_BUDGET`` and the per-file ceilings *before*
deleting code, so the guard is the task's RED and the deletion is its GREEN.
A budget that only ratchets down turns "the layer is too heavy" into something
CI can fail on.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import web_layer_budget as budget


def test_every_tracked_file_exists():
    missing = [rel for rel, _ in budget.INTERFACE_FILES if not (budget.ROOT / rel).exists()]
    assert not missing, f"tracked interface files disappeared: {missing}"


def test_no_file_grows_past_its_ceiling():
    over = {
        rel: (n, budget.SIZE_CEILING[rel])
        for rel, n in budget.interface_core_lines().items()
        if n > budget.SIZE_CEILING[rel]
    }
    assert not over, f"files grew past their ceiling (measured, ceiling): {over}"


def test_interface_core_does_not_exceed_budget():
    lines = budget.interface_core_lines()
    total = sum(lines.values())
    assert total <= budget.CORE_BUDGET, (
        f"interface core is {total} lines, budget is {budget.CORE_BUDGET}"
    )


def test_budget_never_exceeds_the_recorded_baseline():
    assert budget.CORE_BUDGET <= budget.BASELINE_LINES
