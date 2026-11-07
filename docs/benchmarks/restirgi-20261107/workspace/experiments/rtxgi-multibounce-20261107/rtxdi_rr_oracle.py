"""Independent analytic RR oracle, not a substitute for root-owned GPU pilots."""
import json
import math
from pathlib import Path


def enumerate_closed_lambert_path():
    # All vertices see the unit sun at cosine1. Enumerate the one surviving
    # branch and all RR termination branches exactly rather than Monte Carlo.
    beta = 0.7
    probability = 1.0
    terms = []
    termination = [0.0] * 17
    tests = []
    for depth in range(2, 17):
        terms.append(probability * beta * 0.7 / math.pi)
        if depth == 16:
            termination[depth] += probability
            break
        beta_before_bsdf = beta
        beta *= 0.7
        if depth >= 6 and beta_before_bsdf < 1:
            survival = min(0.95, beta_before_bsdf)
            tests.append((depth, survival))
            termination[depth] += probability * (1.0 - survival)
            probability *= survival
            beta /= survival
    return terms, termination, tests


def main():
    terms, termination, tests = enumerate_closed_lambert_path()
    expected = [0.7 ** depth / math.pi for depth in range(2, 17)]
    assert len(terms) == 15
    assert len(tests) == 10
    assert tests[0][0] == 6
    assert math.isclose(tests[0][1], 0.16807, abs_tol=1e-14)
    assert all(depth >= 6 and depth < 16 for depth, _ in tests)
    assert all(math.isclose(q, 0.7, abs_tol=1e-14) for _, q in tests[1:])
    assert all(math.isclose(a, b, rel_tol=1e-14) for a, b in zip(terms, expected))
    assert math.isclose(sum(termination), 1.0, abs_tol=1e-14)
    assert all(termination[depth] == 0 for depth in range(6))
    assert termination[16] > 0
    report = {
        "purpose": "Independent analytic oracle; shader compilation and GPU depth/radiance pilots remain required",
        "status": "pass",
        "first_RR_surface_depth": tests[0][0],
        "first_RR_survival": tests[0][1],
        "subsequent_RR_survival": tests[1][1],
        "maximum_surface_depth": 16,
        "maximum_RR_tests": len(tests),
        "closed_all_lit_unit_sun_pure_indirect_radiance": sum(terms),
        "directly_summed_expected_radiance": sum(expected),
        "RR_termination_depth_probability": termination,
        "per_depth_expected_contribution": terms,
    }
    output = Path(__file__).with_name("rtxdi-rr-oracle.json")
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("status", "first_RR_survival", "maximum_surface_depth", "closed_all_lit_unit_sun_pure_indirect_radiance")}))


if __name__ == "__main__":
    main()
