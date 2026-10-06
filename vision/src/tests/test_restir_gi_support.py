"""Exact finite-domain model of GI selected-sample support normalization.

This enumerates proposal draws and reservoir replacement probabilities. It is
a CPU mathematical regression, not GPU code or proof of full-path GI accuracy.
The domains model a static scene with direction-independent secondary radiance
and identity shifts. Production visibility, BSDF evaluation, and Jacobians
still need renderer integration tests.

Run with Python's standard library only. --estimator constant reproduces the
missing-support dark bias; --estimator drop-zero reproduces the bright bias
from omitting a donor merely because its realized sample has zero weight.
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
    sample: int | None
    contribution_weight: F
    confidence: F
    weight_sum: F


@dataclass(frozen=True)
class Source:
    support: frozenset[int]
    outcomes: tuple[tuple[Reservoir, F], ...]


def proposal(probabilities, target, confidence=F(1)):
    """A null outcome consumes confidence without supplying a candidate."""
    outcomes = []
    support = frozenset(i for i, probability in enumerate(probabilities) if probability)
    for sample in support:
        probability = probabilities[sample]
        weight = 1 / probability
        outcomes.append((Reservoir(sample, weight, confidence,
                                   confidence * target[sample] * weight), probability))
    null_probability = 1 - sum(probabilities)
    assert null_probability >= 0
    if null_probability:
        outcomes.append((Reservoir(None, F(0), confidence, F(0)), null_probability))
    assert sum(probability for _, probability in outcomes) == 1
    return Source(support, tuple(outcomes))


def reservoir_selections(weights):
    """Integrate every u * accumulated_weight < candidate_weight comparison."""
    states = {None: F(1)}
    total = F(0)
    for candidate, weight in enumerate(weights):
        if not weight:
            continue
        total += weight
        replacement = weight / total
        updated = {}
        for selected, probability in states.items():
            keep = probability * (1 - replacement)
            take = probability * replacement
            if keep:
                updated[selected] = updated.get(selected, F(0)) + keep
            if take:
                updated[candidate] = updated.get(candidate, F(0)) + take
        states = updated
    assert sum(states.values()) == 1
    return states, total


def combine(sources, target, estimator=None, canonicalize=True,
            copy_canonical_sum=False):
    estimator = estimator or ESTIMATOR
    outcomes = []
    for draws in product(*(source.outcomes for source in sources)):
        reservoirs, probabilities = zip(*draws)
        draw_probability = F(1)
        for probability in probabilities:
            draw_probability *= probability
        confidence = sum(reservoir.confidence for reservoir in reservoirs)
        weights = [reservoir.confidence * target[reservoir.sample] * reservoir.contribution_weight
                   if reservoir.sample is not None else F(0) for reservoir in reservoirs]
        if copy_canonical_sum:
            weights[0] = reservoirs[0].weight_sum
        selections, total = reservoir_selections(weights)
        for selected, selection_probability in selections.items():
            probability = draw_probability * selection_probability
            if selected is None:
                outcomes.append((Reservoir(None, F(0), confidence, F(0)), probability))
                continue
            sample = reservoirs[selected].sample
            if estimator == "constant":
                normalization = confidence
            elif estimator in ("corrected", "drop-zero"):
                normalization = sum(
                    reservoir.confidence
                    for index, (source, reservoir) in enumerate(zip(sources, reservoirs))
                    if sample in source.support and
                    (estimator != "drop-zero" or index == 0 or weights[index] > 0))
            else:
                raise ValueError(estimator)
            weight = total / (normalization * target[sample])
            stored_sum = confidence * target[sample] * weight if canonicalize else total
            outcomes.append((Reservoir(sample, weight, confidence, stored_sum), probability))
    assert sum(probability for _, probability in outcomes) == 1
    return Source(frozenset().union(*(source.support for source in sources)), tuple(outcomes))


def expectation(source, target):
    return sum(probability * target[reservoir.sample] * reservoir.contribution_weight
               for reservoir, probability in source.outcomes if reservoir.sample is not None)


class GISupportRegression(unittest.TestCase):
    def test_source_occluded_region_is_not_divided_by_unsupported_donor(self):
        target = (F(1), F(1))
        sources = (proposal((F(1, 2), F(1, 2)), target),
                   proposal((F(1), F(0)), target))
        self.assertEqual(expectation(combine(sources, target), target), F(2))

    def test_unequal_history_confidence_preserves_integral(self):
        target = (F(1), F(1))
        sources = (proposal((F(1, 2), F(1, 2)), target),
                   proposal((F(1), F(0)), target, F(5)))
        self.assertEqual(expectation(combine(sources, target), target), F(2))

    def test_null_realization_does_not_remove_source_support(self):
        target = (F(1),)
        source = proposal((F(1, 2),), target)
        self.assertEqual(expectation(combine((source, source), target), target), F(1))

    def test_zero_radiance_realization_does_not_remove_source_support(self):
        target = (F(1), F(0))
        source = proposal((F(1, 2), F(1, 2)), target)
        self.assertEqual(expectation(combine((source, source), target), target), F(1))

    def test_canonicalized_weight_sum_survives_next_spatial_merge(self):
        target = (F(1), F(1))
        current = proposal((F(1, 2), F(1, 2)), target)
        history = proposal((F(1), F(0)), target, F(5))
        temporal = combine((current, history), target)
        for reservoir, _ in temporal.outcomes:
            self.assertEqual(reservoir.confidence, F(6))
            self.assertEqual(reservoir.weight_sum,
                             reservoir.confidence * target[reservoir.sample] *
                             reservoir.contribution_weight)
        spatial = combine((temporal, current), target, copy_canonical_sum=True)
        self.assertEqual(expectation(spatial, target), F(2))

    def test_all_zero_stream_keeps_confidence_and_zero_output(self):
        target = (F(0),)
        source = proposal((F(1, 2),), target, F(3))
        result = combine((source, source), target)
        for reservoir, _ in result.outcomes:
            self.assertIsNone(reservoir.sample)
            self.assertEqual(reservoir.confidence, F(6))
            self.assertEqual(reservoir.contribution_weight, F(0))
            self.assertEqual(reservoir.weight_sum, F(0))

    def test_hand_derived_counterexamples_distinguish_incomplete_fixes(self):
        target = (F(1), F(1))
        current = proposal((F(1, 2), F(1, 2)), target)
        history = proposal((F(1), F(0)), target, F(5))
        self.assertEqual(expectation(combine((current, history), target, "constant"), target), F(7, 6))
        temporal = combine((current, history), target, "corrected", canonicalize=False)
        spatial = combine((temporal, current), target, "corrected", copy_canonical_sum=True)
        self.assertEqual(expectation(spatial, target), F(9, 7))
        null_source = proposal((F(1, 2),), (F(1),))
        self.assertEqual(expectation(combine((null_source, null_source), (F(1),), "drop-zero"),
                                     (F(1),)), F(5, 4))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--estimator", choices=("constant", "drop-zero", "corrected"),
                        default="corrected")
    arguments, unittest_arguments = parser.parse_known_args()
    ESTIMATOR = arguments.estimator
    unittest.main(argv=[__file__, *unittest_arguments], verbosity=2)
