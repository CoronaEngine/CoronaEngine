"""Exact finite-domain regression for ReSTIR DI's initial visibility MIS.

Run with Python's standard library only:
    python vision/src/tests/test_restir_visibility_mis.py

The model enumerates proposal draws AND each streaming reservoir replacement.
It is a mathematical regression, not an execution of the GPU shader. Kitchen
and renderer integration tests must additionally verify the production wiring.
Use --estimator legacy or --estimator unshadowed to reproduce the two faulty
algorithms against the same independently specified integral expectations.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from fractions import Fraction as F
from itertools import product
import unittest


ESTIMATOR = "corrected"


@dataclass(frozen=True)
class Candidate:
    light: int
    weight: F
    occluded_correction: F


def candidate_draws(kind, light_pdf, bsdf_pdf, target, source_visible,
                    light_count, bsdf_count):
    """Include the probability mass of BSDF rays that hit non-emissive blockers."""
    draws = []
    hit_probability = F(0)
    for light, q_light in enumerate(light_pdf):
        q_bsdf = bsdf_pdf[light]
        probability = q_light if kind == "light" else q_bsdf * source_visible[light]
        if not probability:
            continue
        a = light_count * q_light
        b = bsdf_count * q_bsdf
        weight = target[light] / (a + b)
        correction = (a + b) / a if kind == "light" else F(1)
        draws.append((Candidate(light, weight, correction), probability))
        hit_probability += probability
    if hit_probability < 1:
        draws.append((None, 1 - hit_probability))
    assert sum(probability for _, probability in draws) == 1
    return draws


def reservoir_outcomes(candidates):
    """Integrate the random comparison u * new_weight_sum < weight exactly."""
    states = {None: F(1)}
    weight_sum = F(0)
    for candidate in candidates:
        if candidate is None or candidate.weight == 0:
            continue
        weight_sum += candidate.weight
        replacement_probability = candidate.weight / weight_sum
        next_states = {}
        for selected, probability in states.items():
            keep = probability * (1 - replacement_probability)
            replace = probability * replacement_probability
            if keep:
                next_states[selected] = next_states.get(selected, F(0)) + keep
            if replace:
                next_states[candidate] = next_states.get(candidate, F(0)) + replace
        states = next_states
    assert sum(states.values()) == 1
    return states, weight_sum


def expected_receiver(light_pdf=(F(1, 2), F(1, 2)),
                      bsdf_pdf=(F(1, 2), F(1, 2)),
                      target=(F(1), F(1)), source_visible=(1, 0),
                      receiver_contribution=(F(1), F(1)),
                      light_count=1, bsdf_count=1, estimator=None):
    estimator = estimator or ESTIMATOR
    light_draws = candidate_draws("light", light_pdf, bsdf_pdf, target,
                                  source_visible, light_count, bsdf_count)
    bsdf_draws = candidate_draws("bsdf", light_pdf, bsdf_pdf, target,
                                 source_visible, light_count, bsdf_count)
    expectation = F(0)
    total_probability = F(0)
    proposal_draws = [light_draws] * light_count + [bsdf_draws] * bsdf_count
    for draws in product(*proposal_draws):
        candidates, probabilities = zip(*draws)
        draw_probability = F(1)
        for probability in probabilities:
            draw_probability *= probability
        outcomes, weight_sum = reservoir_outcomes(candidates)
        for selected, reservoir_probability in outcomes.items():
            probability = draw_probability * reservoir_probability
            total_probability += probability
            if selected is None:
                continue
            light = selected.light
            contribution_weight = weight_sum / target[light]
            if estimator == "legacy":
                contribution_weight *= source_visible[light]
            elif estimator == "corrected":
                if not source_visible[light]:
                    contribution_weight *= selected.occluded_correction
            elif estimator != "unshadowed":
                raise ValueError(estimator)
            expectation += probability * receiver_contribution[light] * contribution_weight
    assert total_probability == 1
    return expectation


class VisibilityMISRegression(unittest.TestCase):
    def test_mixed_candidates_keep_energy_from_source_occluded_light(self):
        self.assertEqual(expected_receiver(), F(2))

    def test_default_ten_light_one_bsdf_candidates_keep_energy(self):
        self.assertEqual(expected_receiver(light_count=10), F(2))

    def test_light_only_candidates_cover_occluded_source(self):
        self.assertEqual(expected_receiver(bsdf_count=0), F(2))

    def test_all_visible_source_is_unchanged(self):
        self.assertEqual(expected_receiver(source_visible=(1, 1)), F(2))

    def test_receiver_visibility_still_removes_blocked_light(self):
        self.assertEqual(expected_receiver(receiver_contribution=(F(1), F(0))), F(1))

    def test_unequal_proposals_and_target_preserve_receiver_integral(self):
        self.assertEqual(expected_receiver(
            light_pdf=(F(3, 4), F(1, 4)), bsdf_pdf=(F(1, 4), F(3, 4)),
            target=(F(2), F(5)), receiver_contribution=(F(2), F(3)),
            light_count=2, bsdf_count=2), F(5))

    def test_counterexample_distinguishes_both_incomplete_fixes(self):
        # Independent hand-derived integrals for the two-light fixture.
        self.assertEqual(expected_receiver(estimator="legacy"), F(1))
        self.assertEqual(expected_receiver(estimator="unshadowed"), F(3, 2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--estimator", choices=("legacy", "unshadowed", "corrected"),
                        default="corrected")
    arguments, unittest_arguments = parser.parse_known_args()
    ESTIMATOR = arguments.estimator
    unittest.main(argv=[__file__, *unittest_arguments], verbosity=2)
