"""Mitsuba 3 XML geometry reader. No renderer bindings are required."""
from __future__ import annotations

from pathlib import Path
import math
import re
import xml.etree.ElementTree as ET

from runtime.archive import transforms as tr
from .external_scene import SceneBuilder
from .geometry_cache import primitive
from .cameras import finite, perspective_camera, resolution, vertical_fov
from .mitsuba_appearance import Appearance


def read_scene(source, *, cache_directory=None):
    builder = SceneBuilder(source, "mitsuba3", cache_directory)
    defaults, locations, definitions, films = {}, {}, {}, {}

    def value(text):
        def replace(match):
            key = match.group(1)
            if key not in defaults:
                raise ValueError(f"Undefined Mitsuba parameter: ${key}")
            return defaults[key]
        return re.sub(r"\$([A-Za-z_][\w]*)", replace, text)

    def numbers(text):
        return [float(x) for x in re.split(r"[\s,]+", value(text).strip()) if x]

    def vector(node, fallback=0.0):
        if "value" in node.attrib:
            v = numbers(node.attrib["value"])
            if len(v) == 1:
                v *= 3
        else:
            v = [float(value(node.get(c, str(fallback)))) for c in "xyz"]
        if len(v) != 3:
            raise ValueError("Expected three vector components")
        return v

    def load(path, chain=()):
        path = Path(path).resolve()
        if path in chain or len(chain) >= 64:
            raise ValueError(f"Recursive Mitsuba include: {path}")
        try:
            text = path.read_text(encoding="utf-8-sig")
            if "<!DOCTYPE" in text or "<!ENTITY" in text:
                raise ValueError("XML document type/entity declarations are unsupported")
            root = ET.fromstring(text)
        except ET.ParseError as exc:
            raise ValueError(f"Invalid XML in {path}: {exc}") from exc
        if root.tag != "scene" or not root.get("version", "").startswith("3."):
            raise ValueError(f"Expected a Mitsuba 3 scene: {path}")
        result = []
        for node in root:
            if node.tag == "default":
                defaults.setdefault(node.attrib["name"], value(node.attrib["value"]))
            elif node.tag == "include":
                result.extend(load(path.parent / value(node.attrib["filename"]), (*chain, path)))
            else:
                for child in node.iter():
                    locations[id(child)] = path
                    if child.tag == "shape" and child.get("id"):
                        key = child.attrib["id"]
                        if key in definitions:
                            raise ValueError(f"Duplicate Mitsuba shape id: {key}")
                        definitions[key] = child
                    elif child.tag == "film" and child.get("id"):
                        key = child.attrib["id"]
                        if key in films:
                            raise ValueError(f"Duplicate Mitsuba film id: {key}")
                        films[key] = child
                result.append(node)
        return result

    nodes = load(source)

    def transform(node, name="to_world"):
        matrix = tr.identity()
        transforms = [x for x in node if x.tag == "transform" and x.get("name") == name]
        if len(transforms) > 1:
            raise ValueError("Duplicate to_world transform")
        for container in transforms:
            for op in container:
                if op.tag == "matrix":
                    values = numbers(op.attrib["value"])
                    if name == "to_uv" and len(values) == 9:
                        if values[6:] != [0,0,1]:
                            raise ValueError("Projective UV transforms are unsupported")
                        values = [values[0],values[1],0,values[2], values[3],values[4],0,values[5], 0,0,1,0, 0,0,0,1]
                    m = tr.affine(values)
                elif op.tag == "translate":
                    m = tr.translate(vector(op))
                elif op.tag == "scale":
                    m = tr.scale(vector(op, 1))
                elif op.tag == "rotate":
                    axis = [0,0,1] if name == "to_uv" and not any(k in op.attrib for k in ("x","y","z","value")) else vector(op)
                    m = tr.rotate(float(value(op.attrib["angle"])), axis)
                elif op.tag == "lookat":
                    m = tr.look_at(numbers(op.attrib["origin"]), numbers(op.attrib["target"]), numbers(op.attrib["up"]))
                else:
                    raise ValueError(f"Unsupported Mitsuba transform: {op.tag}")
                # Mitsuba applies XML transform operations in their listed order.
                matrix = tr.multiply(m, matrix)
        return matrix

    def camera(node):
        if value(node.get("type", "")) != "perspective":
            raise ValueError("Only perspective Mitsuba cameras are supported")
        if any(x.tag == "animation" for x in node):
            raise ValueError("Animated camera transforms are unsupported")

        def properties(element):
            result = {}
            for child in element:
                name = child.get("name")
                if name and "value" in child.attrib:
                    if name in result:
                        raise ValueError(f"Duplicate camera property: {name}")
                    result[name] = value(child.attrib["value"])
            return result

        props = properties(node)
        film_nodes = [x for x in node if x.tag == "film"]
        for ref in (x for x in node if x.tag == "ref"):
            key = value(ref.get("id", ""))
            if key in films:
                film_nodes.append(films[key])
            elif ref.get("name") == "film":
                raise ValueError(f"Unknown Mitsuba film reference: {key}")
        if len(film_nodes) > 1:
            raise ValueError("Only one film can be specified per camera")
        film_props = properties(film_nodes[0]) if film_nodes else {}
        if film_nodes and value(film_nodes[0].get("type", "")) not in {"hdrfilm", "specfilm"}:
            raise ValueError("Unsupported Mitsuba camera film")
        width, height = resolution(film_props.get("width", 768), film_props.get("height", 576))
        expected_crop = {"crop_width": width, "crop_height": height, "crop_offset_x": 0, "crop_offset_y": 0}
        if any(finite(film_props.get(key, default), key) != default for key, default in expected_crop.items()):
            raise ValueError("Camera crop is unsupported; expected the full film")
        if any(finite(props.get(key, 0), key) != 0 for key in ("principal_point_offset_x", "principal_point_offset_y")):
            raise ValueError("Camera principal point offsets are unsupported")
        if "fov" in props:
            if "focal_length" in props:
                raise ValueError("Specify either camera fov or focal_length, not both")
            fov = props["fov"]
            axis = props.get("fov_axis", "x").lower()
        else:
            focal_length = finite(str(props.get("focal_length", "50mm")).removesuffix("mm"), "focal_length")
            if focal_length <= 0:
                raise ValueError("Camera focal_length must be positive")
            fov = math.degrees(2 * math.atan(math.hypot(36, 24) / (2 * focal_length)))
            axis = "diagonal"
        return perspective_camera(
            transform(node), image_x_sign=-1,
            fov=vertical_fov(fov, axis, width, height), width=width, height=height,
            near_plane=props.get("near_clip", .01), far_plane=props.get("far_clip", 10000),
        )

    appearance = Appearance(builder,nodes,locations,value,numbers,vector,transform)

    def visit(node, parent, stack=(), prefix=""):
        kind = value(node.get("type", ""))
        name = prefix + node.get("id", "")
        path = locations[id(node)]
        matrix = tr.multiply(parent, transform(node))
        if kind == "shapegroup":
            for child in node:
                if child.tag == "shape":
                    visit(child, matrix, stack, name+"/")
            return
        if kind == "instance":
            refs = [x.get("id") for x in node if x.tag == "ref"]
            if len(refs) != 1 or refs[0] not in definitions:
                raise ValueError(f"Invalid Mitsuba instance reference: {refs}")
            key = refs[0]
            if key in stack or len(stack) >= 64 or definitions[key].get("type") != "shapegroup":
                raise ValueError(f"Invalid/recursive shape group: {key}")
            visit(definitions[key], matrix, (*stack, key), name+"/")
            return
        if kind not in {"obj", "ply", "rectangle", "cube", "sphere", "disk"}:
            builder.warning("UNSUPPORTED_SHAPE", f"Skipped unsupported Mitsuba shape '{kind}'", path, kind)
            return
        props = {x.get("name"): x for x in node if x.get("name")}
        material = appearance.shape(node)
        if material.get("boundary_only"):
            return

        def scalar(key, default):
            return float(value(props[key].attrib["value"])) if key in props else default

        def boolean(key, default=False):
            if key not in props:
                return default
            v = value(props[key].attrib["value"]).lower()
            if v not in {"true", "false"}:
                raise ValueError(f"Invalid boolean: {v}")
            return v == "true"

        if kind in {"obj", "ply"}:
            filename = value(props["filename"].attrib["value"])
            model = path.parent / filename
            if model.suffix.lower() != "."+kind:
                raise ValueError(f"Shape type and file extension differ: {model}")
        else:
            kwargs = {"directory": cache_directory}
            if kind == "sphere":
                kwargs["radius"] = scalar("radius", 1)
                if "center" in props:
                    matrix = tr.multiply(matrix, tr.translate(vector(props["center"])))
            model = primitive(kind, **kwargs)
        builder.add(model, matrix, name, kind=kind,
                    flip_uvs=boolean("flip_tex_coords", kind == "obj"),
                    flip_normals=boolean("flip_normals"), face_normals=boolean("face_normals"), material=material)

    sensors = [node for node in nodes if node.tag == "sensor"]
    if len(sensors) > 1:
        raise ValueError("Multiple source cameras are unsupported")
    if sensors:
        builder.cameras.append(camera(sensors[0]))
    for node in nodes:
        if node.tag == "shape" and node.get("type") != "shapegroup":
            visit(node, tr.identity())
        elif node.tag == "emitter":
            appearance.light(node)
    return builder.snapshot()
