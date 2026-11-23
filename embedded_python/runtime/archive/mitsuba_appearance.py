"""Mitsuba references and wrappers lowered to the shared Principled contract."""
import math
from .appearance import convert_material, expression, image, principled, rgb
from .texture_cache import checker_texture
from runtime.archive import transforms as tr


class Appearance:
    def __init__(self, builder, nodes, locations, value, numbers, vector, transform):
        self.builder, self.locations = builder, locations
        self.value, self.numbers, self.vector, self.transform = value,numbers,vector,transform
        self.refs, self.cache = {}, {}
        for node in nodes:
            for child in node.iter():
                if child.get("id") and child.tag in {"bsdf","texture","emitter","medium"}:
                    key = value(child.get("id"))
                    if key in self.refs:
                        raise ValueError(f"Duplicate Mitsuba resource id: {key}")
                    self.refs[key] = child

    def resolve(self, node):
        if node.tag == "ref":
            key = self.value(node.get("id", ""))
            if key not in self.refs:
                raise ValueError(f"Unknown Mitsuba appearance reference {key}")
            return self.refs[key]
        return node

    def children(self, node, tag):
        for child in node:
            if child.tag == tag:
                yield child
            elif child.tag == "ref":
                key = self.value(child.get("id", ""))
                if key in self.refs and self.refs[key].tag == tag:
                    yield self.refs[key]

    def get(self, node, name, default, stack=()):
        child = next((c for c in node if c.get("name") == name),None)
        if child is None:
            return default
        child = self.resolve(child)
        if child.tag == "texture":
            return self.texture(child, stack)
        value = self.value(child.get("value", ""))
        if child.tag == "boolean":
            if value not in {"true","false"}:
                raise ValueError("Invalid Mitsuba boolean")
            return value == "true"
        if child.tag == "string":
            return value
        if child.tag in {"rgb","srgb"}:
            result = rgb(self.numbers(value))
            if child.tag == "srgb":
                result = [x/12.92 if x <= .04045 else ((x+.055)/1.055)**2.4 for x in result]
            return result
        if child.tag in {"point","vector"}:
            return self.vector(child)
        if child.tag == "spectrum":
            try:
                return rgb(float(value))
            except ValueError:
                self.builder.warning("SPECTRAL_APPROXIMATION", "Sampled spectrum approximated as neutral RGB")
                return [1.]*3
        if child.tag in {"float","integer"}:
            result = float(value)
            if not math.isfinite(result):
                raise ValueError("Nonfinite appearance parameter")
            return result
        return default

    def texture(self, node, stack=()):
        node = self.resolve(node)
        if id(node) in stack or len(stack) >= 32:
            raise ValueError("Recursive Mitsuba texture")
        stack = (*stack,id(node))
        get = lambda k,d: self.get(node,k,d,stack)
        kind = self.value(node.get("type", ""))
        matrix = self.transform(node, "to_uv")
        uv = [matrix[0],matrix[1],matrix[3],matrix[4],matrix[5],matrix[7],0,0,1]
        if kind == "bitmap":
            filename = self.locations[id(node)].parent / get("filename", "")
            encoding = "linear" if get("raw",False) or filename.suffix.lower() in {".exr",".hdr",".pfm"} else "srgb"
            return image(filename, encoding=encoding,uv=uv,wrap=get("wrap_mode","repeat"),filter=get("filter_type","bilinear"))
        if kind == "checkerboard":
            return checker_texture(get("color0",[.4]*3),get("color1",[.2]*3),uv,
                                   directory=self.builder.cache_directory,warn=self.builder.warning)
        self.builder.warning("TEXTURE_APPROXIMATION",f"Unsupported texture {kind} approximated by constant")
        return [.5]*3

    def material(self, node, stack=()):
        node = self.resolve(node)
        if id(node) in stack or len(stack) >= 32:
            raise ValueError("Recursive Mitsuba BSDF")
        if id(node) in self.cache:
            return self.cache[id(node)]
        stack = (*stack,id(node))
        kind = self.value(node.get("type", ""))
        children = [self.material(c,stack) for c in self.children(node,"bsdf")]
        if kind in {"twosided","mask","bumpmap","normalmap"} and children:
            result = dict(children[0])
            if kind != "normalmap":
                self.builder.warning("MATERIAL_FEATURE_DEFERRED", f"{kind} wrapper uses existing front material; no new Vision BSDF features")
            if kind == "twosided":
                result["two_sided"] = True
            elif kind == "normalmap":
                result["normal"] = self.get(node,"normalmap",[.5,.5,1.],stack)
            elif kind == "mask":
                opacity=self.get(node,"opacity",.5,stack)
                result["opacity"] = opacity
                if not isinstance(opacity,dict):
                    values=rgb(opacity)
                    amount=max(0.,min(1.,sum(values)/3))
                    child_transmission=result.get("transmission",0.)
                    child_color=result["base_color"]
                    result["transmission"]=expression("mix",1.,child_transmission,amount)
                    result["base_color"]=expression("mix",[1.]*3,result["base_color"],amount)
                    if not isinstance(child_color,dict) and not isinstance(child_transmission,dict):
                        color=rgb(child_color)
                        transmitted=[1-amount+amount*c*child_transmission for c in color]
                        result["transmission"]=sum(transmitted)/max(sum(rgb(result["base_color"])),1e-12)
                    self.builder.warning("MATERIAL_APPROXIMATION", "Constant opacity uses existing Principled transmission; no new mask BSDF or exact null scattering")
        else:
            result = convert_material(kind,lambda k,d:self.get(node,k,d),self.builder.warning,
                                      source="mitsuba3",children=children)
        self.cache[id(node)] = result
        return result

    def shape(self, node):
        bsdfs = list(self.children(node,"bsdf"))
        material = dict(self.material(bsdfs[0])) if bsdfs else principled()
        for emitter in self.children(node,"emitter"):
            if self.value(emitter.get("type","")) != "area":
                self.builder.warning("LIGHT_UNSUPPORTED", "Only area emitters may attach to geometry")
                continue
            material.update(emission=self.get(emitter,"radiance",[1.]*3),
                            emission_two_sided=self.get(emitter,"twosided",False))
            if material.pop("boundary_only",False):
                material["base_color"] = [0.]*3
                self.builder.warning("MATERIAL_APPROXIMATION", "Emissive null surface uses opaque black Principled")
        if list(self.children(node,"medium")):
            self.builder.warning("MEDIUM_DEFERRED", "Volume transport is deferred")
        return material

    def light(self, node):
        kind = self.value(node.get("type", ""))
        get = lambda k,d:self.get(node,k,d)
        matrix = self.transform(node)
        if kind in {"envmap","constant"}:
            color = image(self.locations[id(node)].parent/get("filename",""),encoding="linear") if kind == "envmap" else get("radiance",[1.]*3)
            self.builder.lighting["environments"].append(dict(color=expression("multiply",color,get("scale",1.)),
                matrix=matrix,projection="latlong",convention="mitsuba"))
        elif kind in {"point","spot","directional"}:
            if kind == "point":
                matrix = tr.multiply(matrix,tr.translate(get("position",[0,0,0])))
            direction = get("direction",None)
            if kind == "directional" and direction is not None:
                up = [0,1,0] if abs(direction[1]) < .99*math.sqrt(sum(x*x for x in direction)) else [1,0,0]
                matrix = tr.look_at([0,0,0],direction,up)
            self.builder.light(kind,matrix,get("irradiance" if kind=="directional" else "intensity",[1.]*3),
                               name=node.get("id",kind),outer=get("cutoff_angle",20.),
                               inner=get("beam_width",get("cutoff_angle",20.)*.75))
        else:
            self.builder.warning("LIGHT_UNSUPPORTED",f"Unsupported emitter {kind}")
