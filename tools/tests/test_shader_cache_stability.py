import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check_shader_cache_stability import compare_runs
from switch_profile_report import parse_log


def run_fixture(padding, cache):
    lines = []
    phases = ("PT.initial", "PT_to_ReSTIR", "ReSTIR_to_PT", "PT_to_ReSTIR.repeat")
    for index, phase in enumerate(phases):
        for boundary in ("B", "E"):
            lines.append("CORONA_PROFILE " + json.dumps(dict(
                ph=boundary, name=phase, ts=index * 10 + (boundary == "E"),
                cat="switch", tid=1)))
            if boundary == "B" and index < 2:
                lines.append(f"shader PTX cache {cache}: kernel_{index}_stablehash.ptx")
    return dict(process=dict(status="success"), profile=parse_log("\n".join(lines)),
                bindings=[[p, str(10 + padding), str(0xffffffff if p in (phases[0], phases[2]) else 20 + padding)]
                          for p in phases],
                padding=padding, padding_events=[[p, str(padding)] for p in phases[:2]] if padding else [])


class ShaderCacheStabilityTests(unittest.TestCase):
    def setUp(self):
        self.runs = [run_fixture(0, "miss"), run_fixture(7, "hit")]

    def test_cold_then_warm_shifted_bindings_succeed(self):
        self.assertEqual(compare_runs(json.loads(json.dumps(self.runs))), [])

    def test_misses_or_different_identities_fail_even_if_process_succeeded(self):
        for field, value, message in (("cache", "miss", "every warmed"),
                                      ("name", "new_hash.ptx", "identities")):
            runs = copy.deepcopy(self.runs)
            runs[1]["profile"]["shaders"][0][field] = value
            self.assertIn(message, " ".join(compare_runs(runs)))

    def test_missing_or_incomplete_evidence_is_not_a_pass(self):
        for field, value in (("shaders", []), ("phases", []), ("warnings", ["incomplete stage"])):
            runs = copy.deepcopy(self.runs)
            runs[1]["profile"][field] = value
            self.assertTrue(compare_runs(runs))
        self.assertTrue(compare_runs(self.runs[:1]))

    def test_unconfirmed_slot_perturbation_fails(self):
        self.runs[1]["padding_events"] = []
        self.assertIn("perturbation", " ".join(compare_runs(self.runs)))

    def test_unused_padding_without_real_binding_changes_fails(self):
        self.runs[1]["bindings"] = self.runs[0]["bindings"]
        self.assertIn("real shader buffer slots", " ".join(compare_runs(self.runs)))

    def test_timeout_and_repeated_compilation_fail(self):
        self.runs[1]["process"]["status"] = "timeout"
        self.runs[1]["profile"]["shaders"].append(dict(
            name="repeated.ptx", cache="hit", phase="PT_to_ReSTIR.repeat"))
        errors = " ".join(compare_runs(self.runs))
        self.assertIn("timeout", errors)
        self.assertIn("rebuilt shaders", errors)


if __name__ == "__main__":
    unittest.main()
