"""PBRT graphics-state appearance lowering to Corona Principled."""
from pathlib import Path
import math
from .appearance import convert_material, expression, image, rgb
from .texture_cache import checker_texture
from runtime.archive import transforms as tr


class Appearance:
    def __init__(self, builder, root):
        self.builder, self.root = builder, root
        self.textures, self.materials = {}, {}

    def get(self, props, name, default, stack=()):
        if name not in props:
            return default
        values = props[name]
        kind = getattr(props, "types", {}).get(name, "")
        if kind == "texture":
            return self.texture(values[0], stack)
        if kind == "bool":
            if values[0] not in {"true", "false"}:
                raise ValueError("Invalid PBRT boolean")
            return values[0] == "true"
        if kind == "string":
            return values[0]
        try:
            result = [float(x) for x in values]
        except ValueError:
            if kind == "spectrum":
                return values[0]
            raise
        if not all(math.isfinite(x) for x in result):
            raise ValueError("Nonfinite PBRT appearance parameter")
        if kind == "blackbody":
            self.builder.warning("SPECTRAL_APPROXIMATION", "Blackbody emission approximated as neutral RGB")
            return [1.,1.,1.]
        if kind in {"rgb", "color"}:
            return rgb(result)
        return result[0] if len(result) == 1 else result

    def texture(self, name, stack=()):
        if name in stack or len(stack) >= 32:
            raise ValueError(f"Recursive PBRT texture: {name}")
        if name not in self.textures:
            raise ValueError(f"Unknown PBRT texture: {name}")
        category, kind, props = self.textures[name]
        get = lambda k,d: self.get(props,k,d,(*stack,name))
        if kind == "imagemap":
            filename = self.root / get("filename", "")
            # Corona's shared import policy treats ordinary 8-bit color maps
            # consistently across formats. PBRT's native default is PNG-only
            # sRGB; retain explicit author intent and scalar/data-map defaults.
            suffix=filename.suffix.lower()
            color_ldr=category in {"spectrum","color","rgb"} and suffix in {".png",".tga",".jpg",".jpeg",".bmp",".qoi"}
            encoding = get("encoding", "srgb" if suffix==".png" or color_ldr else "linear").lower()
            if color_ldr and suffix!=".png" and "encoding" not in props and "gamma" not in props:
                self.builder.warning("COLOR_ENCODING_NORMALIZED", "Unspecified 8-bit color map uses shared sRGB import policy rather than PBRT's non-PNG linear default")
            if "gamma" in props:
                encoding = "srgb" if get("gamma", True) else "linear"
            filtering = get("filter", "bilinear")
            if filtering == "point":
                filtering = "nearest"
            # PBRT flips t after UVMapping; decoded images use top-down rows.
            tex = image(filename, encoding=encoding, uv=[get("uscale",1),0,get("udelta",0), 0,-get("vscale",1),1-get("vdelta",0), 0,0,1],
                        wrap=get("wrap", "repeat"), filter=filtering)
            return expression("multiply", tex, get("scale",1))
        if kind == "constant":
            return get("value", 1.)
        if kind in {"scale", "mix"}:
            return expression("multiply" if kind == "scale" else "mix", get("tex1",1.),get("tex2",1.),
                              get("amount",.5) if kind == "mix" else None)
        if kind == "checkerboard":
            a, b = get("tex1",0.), get("tex2",1.)
            uv = [get("uscale",1),0,get("udelta",0),0,get("vscale",1),get("vdelta",0),0,0,1]
            if get("dimension",2) != 2 or get("mapping","uv") != "uv":
                self.builder.warning("TEXTURE_BAKE_DEFERRED", "Non-UV or 3D checker retains the existing approximate 2D expression")
                return dict(kind="checkerboard",a=a,b=b,frequency=1,uv=uv)
            return checker_texture(a,b,uv,frequency=1,directory=self.builder.cache_directory,
                                   warn=self.builder.warning)
        self.builder.warning("TEXTURE_APPROXIMATION", f"Texture {kind} approximated by its constant value")
        return get("value", .5)

    def material(self, kind, props):
        get = lambda k,d: self.get(props,k,d)
        children = ()
        if kind == "mix":
            names = props.get("materials", [])
            if len(names) == 2:
                children = [self.materials[n] for n in names]
        result = convert_material(kind,get,self.builder.warning,source="pbrt4",children=children)
        if "normalmap" in props:
            result["normal"] = image(self.root/get("normalmap",""),encoding="linear")
        return result

    def emission(self, props):
        get = lambda k,d: self.get(props,k,d)
        if "power" in props:
            self.builder.warning("LIGHT_APPROXIMATION", "Area power normalization deferred; using authored radiance and scale")
        radiance = image(self.root/get("filename",""),encoding="linear") if "filename" in props else get("L",[1.]*3)
        return dict(emission=expression("multiply", radiance,get("scale",1.)),
                    emission_two_sided=get("twosided",False))

    def light(self, kind, props, matrix, name):
        get = lambda k,d: self.get(props,k,d)
        if kind == "infinite":
            fn = get("filename", "")
            color = image(self.root/fn, encoding="linear") if fn else get("L", [1.]*3)
            self.builder.lighting["environments"].append(dict(color=expression("multiply",color,get("scale",1.)),
                matrix=matrix, projection="equal_area", convention="pbrt"))
            return
        if kind in {"distant", "spot", "point"}:
            origin, target = get("from",[0,0,0]),get("to",[0,0,1])
            direction = [b-a for a,b in zip(origin,target)]
            up = [0,1,0] if abs(direction[1]) < .99*math.sqrt(sum(x*x for x in direction)) else [1,0,0]
            local = tr.look_at(origin,target,up)
            color = expression("multiply",get("L" if kind=="distant" else "I",[1.]*3),get("scale",1.))
            if "power" in props or "illuminance" in props:
                self.builder.warning("LIGHT_APPROXIMATION", "Photometric normalization deferred; using source L/I and scale")
            self.builder.light("directional" if kind=="distant" else kind, tr.multiply(matrix,local),color,name=name,
                               outer=get("coneangle",30.),inner=max(0.,get("coneangle",30.)-get("conedeltaangle",5.)))
        else:
            self.builder.warning("LIGHT_UNSUPPORTED",f"Unsupported PBRT light {kind}")
