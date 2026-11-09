"""Renderer-independent, deliberately approximate Principled import values.

Expressions stay small and preserve original image paths. No asset is copied,
no source BSDF graph survives in the runtime, and no renderer bindings are used.
"""
from __future__ import annotations

import math
from pathlib import Path


def rgb(value):
    if isinstance(value, dict):
        return value
    values = list(value) if isinstance(value, (list, tuple)) else [value]
    if len(values) == 1:
        values *= 3
    if len(values) != 3:
        raise ValueError("Expected one or three color components")
    values = [float(x) for x in values]
    if not all(math.isfinite(x) for x in values):
        raise ValueError("Invalid color")
    return values


def expression(kind, a, b=None, weight=None):
    if not isinstance(a, dict) and (b is None or not isinstance(b, dict)) and not isinstance(weight, dict):
        aa = a if isinstance(a, list) else [a]
        bb = b if isinstance(b, list) else [b]
        size = max(len(aa), len(bb)) if b is not None else len(aa)
        aa = aa * size if len(aa) == 1 else aa
        bb = bb * size if len(bb) == 1 else bb
        if kind == "multiply":
            result = [x*y for x, y in zip(aa, bb)]
        elif kind == "mix":
            result = [x*(1-weight)+y*weight for x, y in zip(aa, bb)]
        elif kind == "sqrt":
            result = [math.sqrt(max(0, x)) for x in aa]
        else:
            raise ValueError(f"Unknown expression {kind}")
        return result if size > 1 else result[0]
    result = dict(kind=kind, a=a)
    if b is not None:
        result["b"] = b
    if weight is not None:
        result["weight"] = weight
    return result


def image(path, *, encoding="srgb", uv=None, wrap="repeat", filter="bilinear"):
    path = Path(path).resolve()
    if not path.is_file():
        raise ValueError(f"Texture does not exist: {path}")
    return dict(kind="image", path=str(path), encoding=encoding,
                uv=uv or [1,0,0, 0,1,0, 0,0,1], wrap=wrap, filter=filter)


def principled(**values):
    result = dict(base_color=[.5,.5,.5], metallic=0., roughness=.5, ior=1.5,
                  specular=0., transmission=0., anisotropic=0., coat=0.,
                  coat_roughness=.1, emission=[0.,0.,0.], emission_two_sided=False)
    result.update(values)
    return result


# RGB normal-incidence approximations, used only when a source names a metal.
# Arbitrary numeric eta/k are converted below; this is not a spectral renderer.
METALS = {"ag": [.972,.960,.915], "au": [1.,.766,.336], "cu": [.955,.638,.538],
          "al": [.913,.922,.924], "cr": [.550,.556,.554], "ni": [.660,.609,.526],
          "none": [1.,1.,1.]}
IORS = {"air": 1.000277, "vacuum": 1., "water": 1.333, "glass": 1.5,
        "bk7": 1.5046, "acrylic glass": 1.49, "polypropylene": 1.49,
        "diamond": 2.419, "fused quartz": 1.458, "sodium chloride": 1.544}


def ior(value):
    if isinstance(value, str) and value.lower() in IORS:
        return IORS[value.lower()]
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise ValueError("Invalid index of refraction")
    return number


def conductor_color(eta=None, k=None, name="none", warn=None):
    if eta is not None and k is not None and not isinstance(eta, (str, dict)) and not isinstance(k, (str, dict)):
        return [min(1., max(0., ((e-1)**2+c*c)/max(1e-12,(e+1)**2+c*c))) for e,c in zip(rgb(eta), rgb(k))]
    key = str(eta or name).lower().replace("metal-", "").split("-")[0]
    if key in METALS:
        return METALS[key]
    if warn:
        warn("MATERIAL_APPROXIMATION", f"Unknown conductor '{key}': neutral metal reflectance")
    return [.9,.9,.9]


def convert_material(kind, get, warn, *, source="", children=()):
    """get(name, default) returns constants or bounded texture expressions."""
    if source == "pbrt4" and kind in {"", "none"}:
        kind = "interface"
    result = principled()
    rough = get("roughness", 0. if not kind.startswith("rough") else .1)
    if source == "mitsuba3":
        rough = get("alpha", rough)
    # PBRT v4 deliberately keeps its historical alpha=sqrt(roughness).
    # Vision Principled uses alpha=roughness^2. Convert through alpha rather
    # than passing similarly named controls straight through.
    def alpha(value):
        return expression("sqrt",value) if source == "pbrt4" and get("remaproughness",True) else value
    if source == "pbrt4":
        rough = alpha(rough)
    if source in {"mitsuba3","pbrt4"}:
        rough = expression("sqrt", rough)
    u, v = get("alpha_u" if source == "mitsuba3" else "uroughness", None), get("alpha_v" if source == "mitsuba3" else "vroughness", None)
    if u is not None and v is not None:
        u,v = alpha(u),alpha(v)
        rough = expression("mix", u, v, .5)
        rough = expression("sqrt", rough)
        if isinstance(u, (int,float)) and isinstance(v, (int,float)) and u>0 and v>0:
            rough = (u*v)**.25
            result["anisotropic"] = max(-.9, min(.9, (1-v/u)/.9))
    result["roughness"] = rough
    # PBRT reflection models evaluate either surface hemisphere. Keep this
    # surface property independent of emission sidedness and transmission.
    if source == "pbrt4" and kind in {"diffuse", "conductor", "coateddiffuse", "coatedconductor", "mirror"}:
        result["two_sided"] = True
    if kind == "principled":
        result.update(base_color=get("base_color", [.5]*3), metallic=get("metallic",0.),
                      roughness=get("roughness",.5), specular=get("specular",.5),
                      transmission=get("spec_trans",0.), anisotropic=get("anisotropic",0.),
                      coat=get("clearcoat",0.),
                      coat_roughness=expression("mix",.1,.001,get("clearcoat_gloss",1.)))
    elif kind in {"diffuse", "roughdiffuse"}:
        result["base_color"] = get("reflectance", [.5]*3)
        result["roughness"] = 1.
    elif kind in {"conductor", "roughconductor", "mirror", "coatedconductor"}:
        result.update(metallic=1., specular=1., base_color=expression("multiply",
            conductor_color(get("eta", None), get("k", None), get("material", "none"), warn),
            get("specular_reflectance", [1.]*3)))
        if kind == "coatedconductor":
            result["coat"] = 1.
    elif kind == "diffusetransmission":
        reflectance=get("reflectance",[.25]*3)
        transmittance=get("transmittance",[.25]*3)
        scale=get("scale",1.)
        if not any(isinstance(x,dict) for x in (reflectance,transmittance,scale)):
            reflected=[max(0.,min(1.,c*scale)) for c in rgb(reflectance)]
            transmitted=[max(0.,min(1.,c*scale)) for c in rgb(transmittance)]
            total=[r+t for r,t in zip(reflected,transmitted)]
            weight=sum(transmitted)/max(sum(total),1e-12)
            result.update(base_color=[min(1.,c) for c in total],transmission=weight,
                          roughness=1.,specular=0.,two_sided=True)
        else:
            # Keep spatial inputs in the existing expression graph. The fixed
            # half split is an explicitly reported approximation, not a scene override.
            result.update(base_color=expression("multiply",expression("mix",reflectance,transmittance,.5),expression("multiply",2.,scale)),
                          transmission=.5,roughness=1.,specular=0.,two_sided=True)
        warn("MATERIAL_APPROXIMATION", "Diffuse reflection/transmission energy retained; angular response approximated by rough Principled")
    elif kind in {"dielectric", "roughdielectric", "thindielectric"}:
        result.update(transmission=1., specular=1., base_color=get("specular_transmittance", get("transmittance", [1.]*3)))
        if kind == "thindielectric":
            warn("MATERIAL_APPROXIMATION", f"{kind} uses existing Principled transmission")
    elif kind in {"plastic", "roughplastic", "coateddiffuse"}:
        result.update(base_color=get("diffuse_reflectance", get("reflectance", [.5]*3)), specular=1.)
    elif kind in {"null", "interface"}:
        result["boundary_only"] = True
        warn("MEDIUM_DEFERRED", "Invisible medium boundary; volume transport is deferred")
    elif kind in {"blendbsdf", "mix"} and len(children) == 2:
        weight = get("weight", get("amount", .5))
        for key in result:
            if key not in {"emission_two_sided"}:
                result[key] = expression("mix", children[0][key], children[1][key], weight)
        warn("MATERIAL_APPROXIMATION", "BSDF blend flattened to Principled parameter blend")
    else:
        result["base_color"] = get("reflectance", get("diffuse_reflectance", [.5]*3))
        warn("MATERIAL_APPROXIMATION", f"{kind} approximated by diffuse Principled")
    eta = get("int_ior", get("eta", 1.5))
    if kind not in {"conductor", "roughconductor", "coatedconductor"}:
        if not isinstance(eta, (str, int, float)):
            warn("MATERIAL_APPROXIMATION", "Spectral/textured IOR approximated by 1.5")
            eta = 1.5
        # This is a relative IOR. Air bubbles inside water/ice legitimately
        # have eta < 1; clamping them to one erases refraction and TIR.
        result["ior"] = ior(eta)/ior(get("ext_ior", 1.))
    for key in ("displacement", "bumpmap"):
        if get(key, None) is not None:
            warn("MATERIAL_FEATURE_DEFERRED", f"{key} is not added to Vision BSDFs in this import")
    return result
