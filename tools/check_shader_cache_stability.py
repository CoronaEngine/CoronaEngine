"""Validate PT/ReSTIR cache identity from captured profiling reports."""
from collections import Counter


def compare_runs(runs):
    errors = []
    phases = ("PT.initial", "PT_to_ReSTIR", "ReSTIR_to_PT", "PT_to_ReSTIR.repeat")
    for index, run in enumerate(runs, 1):
        if run["process"]["status"] != "success":
            errors.append(f"run {index}: {run['process']['status']}")
        profile = run["profile"]
        errors.extend(f"run {index}: {warning}" for warning in profile["warnings"])
        if [p["name"] for p in profile["phases"]] != list(phases):
            errors.append(f"run {index}: incomplete switch phases")
        if any(p["duration_ms"] is None for p in profile["phases"]):
            errors.append(f"run {index}: incomplete measured phase")
        for phase in phases[:2]:
            if not any(s["phase"] == phase for s in profile["shaders"]):
                errors.append(f"run {index}: no shader evidence for {phase}")
        if any(s["phase"] in phases[2:] for s in profile["shaders"]):
            errors.append(f"run {index}: repeated switch rebuilt shaders")
        expected_padding = [[phase, str(run["padding"])] for phase in phases[:2]] if run["padding"] else []
        if run["padding_events"] != expected_padding:
            errors.append(f"run {index}: binding allocation perturbation was not confirmed")
        if [entry[0] for entry in run["bindings"]] != list(phases):
            errors.append(f"run {index}: actual binding slot evidence is incomplete")
    if len(runs) != 2:
        return errors + ["two completed processes are required"]
    def identities(run):
        return Counter((s["phase"], s["name"]) for s in run["profile"]["shaders"])
    if identities(runs[0]) != identities(runs[1]):
        errors.append("changing binding slots changed shader identities")
    if any(s["cache"] != "hit" for s in runs[1]["profile"]["shaders"]):
        errors.append("second process did not hit every warmed SPIR-V cache entry")
    before = {phase: (int(visibility), int(surfaces)) for phase, visibility, surfaces in runs[0]["bindings"]}
    after = {phase: (int(visibility), int(surfaces)) for phase, visibility, surfaces in runs[1]["bindings"]}
    for phase in phases[:2]:
        if phase not in before or phase not in after:
            errors.append(f"{phase}: actual shader buffer slots are missing")
        # PT does not allocate the ReSTIR-only surfaces buffer.
        elif any(a == b or 0xffffffff in (a, b)
                 for a, b in list(zip(before[phase], after[phase]))[:1 if phase == "PT.initial" else 2]):
            errors.append(f"{phase}: real shader buffer slots must change between processes")
    return errors
