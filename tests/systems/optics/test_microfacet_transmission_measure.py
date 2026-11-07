"""Evaluate production microfacet formulas on the CPU, without a renderer build.

Expressions are extracted from the actual PDF, BTDF, BRDF, refraction and
half-vector functions. Only scalar/vector plumbing and positive symmetric D/G
values are supplied here. Numerical solid-angle differentiation independently
checks the PDF; transport reciprocity checks the matching BTDF measure.
"""
from dataclasses import dataclass
import math
from pathlib import Path
import re

import pytest


ROOT = Path(__file__).resolve().parents[3]


@dataclass(frozen=True)
class Vec:
    x: float
    y: float
    z: float

    def __add__(self, other):
        return Vec(self.x + other.x, self.y + other.y, self.z + other.z)

    def __sub__(self, other):
        return self + (-other)

    def __neg__(self):
        return self * -1

    def __mul__(self, scale):
        return Vec(self.x * scale, self.y * scale, self.z * scale)

    __rmul__ = __mul__

    def __truediv__(self, scale):
        return self * (1 / scale)


def dot(a, b):
    return a.x * b.x + a.y * b.y + a.z * b.z


def cross(a, b):
    return Vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x)


def normalize(vector):
    return vector / math.sqrt(dot(vector, vector))


def unit(degrees):
    angle = math.radians(degrees)
    return Vec(math.sin(angle), 0, math.cos(angle))


def function_body(source, signature):
    opening = source.index("{", source.index(signature))
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[opening + 1:end - 1]


def assignment(body, name):
    return re.search(r"\b" + re.escape(name) + r"\s*=\s*([^;]+);", body).group(1).strip()


def evaluate(expression, values):
    expression = " ".join(expression.split())
    expression = expression.replace("ocarina::", "").replace("<p>", "")
    expression = re.sub(r"(?<=[\d.])f\b", "", expression)
    if "?" in expression:
        condition, choices = expression.split("?", 1)
        yes, no = choices.split(":", 1)
        expression = f"({yes}) if ({condition}) else ({no})"
    functions = dict(__builtins__={}, dot=dot, abs_dot=lambda a, b: abs(dot(a, b)),
                     normalize=normalize, sqr=lambda x: x * x, rcp=lambda x: 1 / x,
                     abs=abs, max=max, safe_sqrt=lambda x: math.sqrt(max(x, 0)),
                     select=lambda condition, yes, no: yes if condition else no,
                     cos_theta=lambda v: v.z, bsdf_D=lambda *args: 2.0,
                     bsdf_G=lambda *args: 0.75, Radiance=0, Importance=1)
    return eval(expression, functions, values)


@pytest.fixture(scope="module")
def formulas():
    header = (ROOT / "vision/src/base/scattering/microfacet.h").read_text(encoding="utf-8-sig")
    source = (ROOT / "vision/src/base/scattering/microfacet.cpp").read_text(encoding="utf-8-sig")
    optics = (ROOT / "vision/src/math/optics.h").read_text(encoding="utf-8-sig")
    return dict(
        pdf=function_body(header, "PDF_wi_transmission(const oc_float<p> &PDF_wh"),
        btdf=function_body(source, "oc_float<p> BTDF_div_ft("),
        brdf=function_body(header, "oc_float<p> BRDF_div_fr("),
        refract=function_body(optics, "oc_bool<p> refract("),
        half=function_body(source, "GGXMicrofacet::TSpectrum GGXMicrofacet::BTDF(const Float3 &wo, const Float3 &wi,"),
    )


def refracted(formulas, wo, normal, eta):
    values = dict(wi=wo, n=normal, eta=eta)
    for name in ("cos_theta_i", "sin_theta_i_2", "sin_theta_t_2", "cos_theta_t"):
        values[name] = evaluate(assignment(formulas["refract"], name), values)
    assert values["sin_theta_t_2"] < 1, "fixture must not be total internal reflection"
    expression = re.search(r"\*wt\s*=\s*([^;]+);", formulas["refract"]).group(1)
    return evaluate(expression, values)


def half_vector(formulas, wo, wi, eta):
    return evaluate(assignment(formulas["half"], "wh"), dict(wo=wo, wi=wi, eta=eta))


def pdf(formulas, wo, wi, wh, eta, pdf_wh=7.0):
    values = dict(wo=wo, wi=wi, wh=wh, eta=eta, PDF_wh=pdf_wh)
    for name in ("denom", "dwh_dwi", "ret"):
        values[name] = evaluate(assignment(formulas["pdf"], name), values)
    return values["ret"]


def btdf(formulas, wo, wi, wh, eta, mode):
    values = dict(wo=wo, wi=wi, wh=wh, eta=eta, alpha_x=.1, alpha_y=.1, type=0, tm=mode)
    for name in ("cos_theta_i", "cos_theta_o", "numerator", "denom", "ft", "factor"):
        values[name] = evaluate(assignment(formulas["btdf"], name), values)
    # Apply the actual zero-denominator selection as well as the return value.
    selection = re.findall(r"\bft\s*=\s*([^;]+);", formulas["btdf"])[-1]
    values["ft"] = evaluate(selection, values)
    result = re.search(r"return\s+([^;]+);", formulas["btdf"]).group(1)
    return evaluate(result, values)


@pytest.fixture(params=[(eta, wo, normal) for eta in (1.5, 2 / 3, 1.33)
                        for wo, normal in ((0, 0), (20, 5), (35, 15))])
def transmission(request, formulas):
    eta, angle, normal_angle = request.param
    wo, normal = unit(angle), unit(normal_angle)
    wi = refracted(formulas, wo, normal, eta)
    wh = half_vector(formulas, wo, wi, eta)
    assert abs(dot(wh, normal)) == pytest.approx(1, abs=1e-12)
    return wo, wi, wh, eta


def test_pdf_matches_solid_angle_jacobian(formulas, transmission):
    wo, wi, wh, eta = transmission
    tangent = normalize(cross(wi, Vec(0, 1, 0)))
    bitangent = cross(wi, tangent)
    epsilon = 1e-6

    def derivative(direction):
        plus = half_vector(formulas, wo, normalize(wi + epsilon * direction), eta)
        minus = half_vector(formulas, wo, normalize(wi - epsilon * direction), eta)
        return (plus - minus) / (2 * epsilon)

    area = cross(derivative(tangent), derivative(bitangent))
    numerical_jacobian = math.sqrt(dot(area, area))
    assert pdf(formulas, wo, wi, wh, eta) == pytest.approx(7 * numerical_jacobian, rel=2e-8)


def test_btdf_transport_reciprocity(formulas, transmission):
    wo, wi, wh, eta = transmission
    reverse_wh = half_vector(formulas, wi, wo, 1 / eta)
    radiance = btdf(formulas, wo, wi, wh, eta, 0)
    importance = btdf(formulas, wo, wi, wh, eta, 1)
    reverse_radiance = btdf(formulas, wi, wo, reverse_wh, 1 / eta, 0)
    assert importance == pytest.approx(radiance * eta * eta, rel=1e-12)
    assert importance == pytest.approx(reverse_radiance, rel=1e-12)


def test_bsdf_only_throughput_preserves_radiance_scaling(formulas, transmission):
    wo, wi, wh, eta = transmission
    throughput = btdf(formulas, wo, wi, wh, eta, 0) * abs(wi.z) / pdf(formulas, wo, wi, wh, eta)
    expected = 2 * .75 * abs(dot(wo, wh)) / (7 * abs(wo.z) * eta * eta)
    assert throughput == pytest.approx(expected, rel=1e-12)


@pytest.mark.parametrize("angle", [0, 20, 55])
def test_reflection_measure_is_unchanged(formulas, angle):
    wo = unit(angle)
    wi, wh = Vec(-wo.x, -wo.y, wo.z), Vec(0, 0, 1)
    values = dict(wo=wo, wi=wi, wh=wh, alpha_x=.1, alpha_y=.1, type=0)
    for name in ("cos_theta_i", "cos_theta_o", "ret"):
        values[name] = evaluate(assignment(formulas["brdf"], name), values)
    assert values["ret"] == pytest.approx(2 * .75 / (4 * abs(wo.z * wi.z)), rel=1e-12)
