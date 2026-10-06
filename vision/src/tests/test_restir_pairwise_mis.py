"""Exact finite-domain DI reuse regression; no renderer or third-party packages.

Enumerates every initial proposal draw, initial reservoir replacement, and reuse
replacement using rational probabilities. This checks the mathematical protocol,
not GPU source wiring. Renderer A/B tests must additionally check that wiring.
The peak bounds apply to these fixtures, not arbitrary production illumination.

Run normally for the corrected estimator. --estimator legacy reproduces the old
code; wrong-denominator, source-target, and wrong-temporal-surface isolate faults.
Pairwise weights follow the course notes Eq. 7.5:
https://intro-to-restir.cwyman.org/presentations/2023ReSTIR_Course_Notes.pdf
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from fractions import Fraction as F
from itertools import product
import unittest


ESTIMATOR = "corrected"


@dataclass(frozen=True)
class Reservoir:
    light: int
    p_hat: F
    W: F


def divide(numerator, denominator):
    return numerator / denominator if denominator else F(0)


def stream(candidates):
    """Enumerate each u * accumulated_weight < incoming_weight decision."""
    states = {None: F(1)}
    weight_sum = F(0)
    for sample, weight in candidates:
        if not weight:
            continue
        assert weight > 0 and sample is not None and sample.p_hat > 0
        weight_sum += weight
        replacement = weight / weight_sum
        updated = {}
        for selected, probability in states.items():
            for result, chance in ((selected, 1 - replacement), (sample, replacement)):
                if chance:
                    updated[result] = updated.get(result, F(0)) + probability * chance
        states = updated
    assert sum(states.values()) == 1
    outcomes = {}
    for sample, probability in states.items():
        result = (None if sample is None else
                  Reservoir(sample.light, sample.p_hat, weight_sum / sample.p_hat))
        outcomes[result] = outcomes.get(result, F(0)) + probability
    return outcomes


def initial_reservoirs(target, proposal, count=2):
    outcomes = {}
    for lights in product(range(len(target)), repeat=count):
        probability = F(1)
        candidates = []
        for light in lights:
            probability *= proposal[light]
            sample = Reservoir(light, target[light], F(0))
            candidates.append((sample, target[light] / (count * proposal[light])))
        for reservoir, selection_probability in stream(candidates).items():
            outcomes[reservoir] = outcomes.get(reservoir, F(0)) + probability * selection_probability
    assert sum(outcomes.values()) == 1
    return outcomes


def pairwise(reservoirs, targets, estimator):
    canonical, *neighbors = reservoirs
    target, *sources = targets
    count = len(neighbors)
    if not count:
        return {canonical: F(1)}
    candidates = []
    for sample, source in zip(neighbors, sources):
        if sample is None:
            continue
        light = sample.light
        pc, pn = target[light], source[light]
        denominator = pn + count * pc if estimator in ("legacy", "wrong-denominator") else pc + count * pn
        mi = divide(pn, denominator)
        p_hat = sample.p_hat if estimator in ("legacy", "source-target") else pc
        candidates.append((Reservoir(light, p_hat, sample.W), mi * p_hat * sample.W))
    if canonical is not None:
        light = canonical.light
        pc = target[light]
        mc = sum(divide(pc, count * (pc + count * source[light])) for source in sources)
        candidates.append((Reservoir(light, pc, canonical.W), mc * pc * canonical.W))
    return stream(candidates)


def temporal(reservoirs, targets, estimator, confidence=(F(1), F(5))):
    current, previous = reservoirs
    target, source = targets
    # Old code evaluates both cross-targets at the current receiver with an
    # old view direction. This fixture keeps wo fixed but changes the source
    # surface/material, so that erroneous source proxy equals the current one.
    proxy = target if estimator in ("legacy", "wrong-temporal-surface") else source
    cc, cp = confidence
    candidates = []
    for index, sample in enumerate((current, previous)):
        if sample is None:
            continue
        light = sample.light
        denominator = cc * target[light] + cp * proxy[light]
        numerator = cc * target[light] if index == 0 else cp * proxy[light]
        mi = divide(numerator, denominator)
        p_hat = target[light]
        candidates.append((Reservoir(light, p_hat, sample.W), mi * p_hat * sample.W))
    return stream(candidates)


def enumerate_reuse(targets, contribution, *, kind="pairwise", proposals=None,
                    estimator=None, confidence=(F(1), F(5))):
    estimator = estimator or ESTIMATOR
    if proposals is None:
        proposals = [(F(1, 2), F(1, 2))] * len(targets)
    distributions = [initial_reservoirs(target, proposal)
                     for target, proposal in zip(targets, proposals)]
    mean, peak, mass = F(0), F(0), F(0)
    for draws in product(*(distribution.items() for distribution in distributions)):
        reservoirs, probabilities = zip(*draws)
        probability = F(1)
        for chance in probabilities:
            probability *= chance
        outputs = (pairwise(reservoirs, targets, estimator) if kind == "pairwise" else
                   temporal(reservoirs, targets, estimator, confidence))
        for selected, selection_probability in outputs.items():
            mass += probability * selection_probability
            if selected is not None:
                value = contribution[selected.light] * selected.W
                mean += probability * selection_probability * value
                peak = max(peak, value)
    assert mass == 1
    return mean, peak


class PairwiseMISRegression(unittest.TestCase):
    def test_two_heterogeneous_neighbors_preserve_integral(self):
        mean, _ = enumerate_reuse(((F(2), F(5)), (F(1), F(11)), (F(7), F(1))),
                                 (F(3), F(7)))
        self.assertEqual(mean, F(10))

    def test_three_neighbors_and_unequal_proposals_preserve_integral(self):
        mean, _ = enumerate_reuse(
            ((F(2), F(5)), (F(1), F(11)), (F(7), F(1)), (F(3), F(4))),
            (F(3), F(7)), proposals=((F(1, 3), F(2, 3)), (F(3, 4), F(1, 4)),
                                    (F(2, 5), F(3, 5)), (F(1, 2), F(1, 2))))
        self.assertEqual(mean, F(10))

    def test_no_neighbors_preserves_canonical_estimator(self):
        self.assertEqual(enumerate_reuse(((F(2), F(5)),), (F(3), F(7)))[0], F(10))

    def test_current_target_avoids_tiny_source_denominator_peak(self):
        tiny = F(1, 10**9)
        mean, peak = enumerate_reuse(((F(1), F(1)), (tiny, F(1)), (F(1), tiny)),
                                    (F(1), F(1)))
        self.assertEqual(mean, F(2))
        # Each of two neighbor weights <= 2, canonical <= 2; output p_hat=1.
        self.assertLessEqual(peak, F(6))

    def test_pairwise_handles_missing_neighbor_support(self):
        mean, _ = enumerate_reuse(((F(1), F(1)), (F(0), F(1)), (F(1), F(0))),
                                 (F(1), F(1)))
        self.assertEqual(mean, F(2))

    def test_temporal_heterogeneous_source_and_confidence_preserve_integral(self):
        mean, _ = enumerate_reuse(((F(2), F(5)), (F(1), F(11))), (F(3), F(7)),
                                 kind="temporal")
        self.assertEqual(mean, F(10))

    def test_temporal_uses_actual_source_surface_to_bound_fixture_peak(self):
        mean, peak = enumerate_reuse(((F(1), F(1)), (F(1, 10**9), F(1))),
                                    (F(1), F(1)), kind="temporal")
        self.assertEqual(mean, F(2))
        # Current contribution <= 2; source contribution <= C_prev * 2 = 10.
        self.assertLessEqual(peak, F(12))

    def test_negative_controls_detect_independent_faults(self):
        targets = ((F(1), F(1)), (F(1, 10**9), F(1)), (F(1), F(1, 10**9)))
        wrong_mean, _ = enumerate_reuse(targets, (F(1), F(1)), estimator="wrong-denominator")
        self.assertNotEqual(wrong_mean, F(2))
        source_mean, source_peak = enumerate_reuse(targets, (F(1), F(1)), estimator="source-target")
        self.assertEqual(source_mean, F(2))
        self.assertGreater(source_peak, F(10**6))
        temporal_mean, temporal_peak = enumerate_reuse(targets[:2], (F(1), F(1)),
                                                       kind="temporal", estimator="wrong-temporal-surface")
        self.assertEqual(temporal_mean, F(2))
        self.assertGreater(temporal_peak, F(10**6))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--estimator", default="corrected",
                        choices=("corrected", "legacy", "wrong-denominator",
                                 "source-target", "wrong-temporal-surface"))
    arguments, unittest_arguments = parser.parse_known_args()
    ESTIMATOR = arguments.estimator
    unittest.main(argv=[__file__, *unittest_arguments], verbosity=2)
