"""Project lifecycle and section edits for portable scene.ini archives."""

import configparser
import io
import json
from pathlib import Path
import tempfile
import time

from .errors import ArchiveParseError
from .parser import _resolve_source
from .portable import atomic_replace, recover_scene, validate_assets


def _document(path):
    root = Path(path).expanduser().resolve()
    if root.name == "scene.ini" and not root.is_dir():
        root = root.parent
    if (root / "scene.ini").is_symlink():
        raise ValueError("scene.ini must belong to the project")
    recover_scene(root)
    scene, config = _resolve_source(root)
    return root, scene, config


def project_info(request):
    root, _, config = _document(request["path"])
    return {"name": config.get("scene", "name", fallback=root.name),
            "mode": config.get("world", "type", fallback="creative"),
            "prompt": config.get("world", "prompt", fallback=""),
            "core_version": config.get("scene", "core_version", fallback=""),
            "create_time": config.get("scene", "create_time", fallback=""),
            "last_opened": config.get("scene", "last_opened", fallback=""),
            "project_path": str(root)}


def create_project_in(request):
    """Create a project in a fresh directory under a base directory.

    The caller supplies where and what to call it; this side owns the directory naming, because
    it also owns everything that goes inside. create_project() refuses an existing target and
    stages its writes atomically, so this only has to pick a free name and retry when another
    creator wins the race.
    """
    base = Path(request["base_dir"])
    stem = request["dir_stem"]
    display_base = request.get("display_base") or stem
    base.mkdir(parents=True, exist_ok=True)
    template = {key: value for key, value in request.items()
                if key not in {"base_dir", "dir_stem", "display_base"}}
    for index in range(1, 1000):
        target = base / f"{stem}_{index}"
        if target.exists():
            continue
        payload = {**template, "path": str(target), "name": f"{display_base}_{index}"}
        try:
            created = create_project(payload)
        except FileExistsError:
            continue
        return {**created, "name": payload["name"], "project_path": str(target),
                "base_dir": str(base)}
    raise RuntimeError(f"could not allocate a project directory for {stem} under {base}")


def create_project(request):
    target = Path(request["path"]).resolve()
    if target.exists():
        raise FileExistsError(str(target))
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".creating-", dir=target.parent) as tmp:
        staged = Path(tmp) / "project"
        (staged / "Assets").mkdir(parents=True)
        config = configparser.ConfigParser(interpolation=None)
        config.read_dict({"format": {"type": "corona_scene_folder", "version": "1"},
                          "scene": {"name": request["name"] or target.name},
                          "sun": {"sun_direction": "1.0, 1.0, 1.0", "enabled": "true"},
                          "grid": {"enabled": "true"}, "actors": {}, "scripts": {"path": ""},
                          "terrain": {"type": "", "path": ""}})
        for section in ("world", "multiplayer"):
            if section in request:
                config[section] = request[section]
        for section in config.sections():
            for key, value in config.items(section):
                config.set(section, key, value.replace("%", "%%"))
        stream = io.StringIO()
        config.write(stream)
        atomic_replace(staged / "scene.ini", stream.getvalue().encode())
        atomic_replace(staged / "assets.manifest.json", b'{"format":"corona_scene_assets","version":1,"bundles":[]}\n')
        # Keep publication atomic when Windows briefly holds the staged directory.
        for attempt in range(6):
            try:
                staged.rename(target)
                break
            except OSError as error:
                if (getattr(error, 'winerror', None) not in {5, 32, 33}
                        or target.exists() or attempt == 5):
                    raise
                time.sleep(0.025 * 2 ** attempt)
    return {"path": str(target)}


def update_metadata(request):
    from .writer import replace_sections
    _, path, original = _document(request["path"])
    fields = {k: v.strip() for k, v in request["settings"].items()
              if k in {"name", "core_version"} and isinstance(v, str) and v.strip()}
    replace_sections(path, original, {"scene": fields})
    return project_info(request)


def validate_project(request):
    path = Path(request["path"])
    result = {"ok": False, "status": "invalid", "path": str(path), "assetCount": 0,
              "totalBytes": 0, "diagnostics": [], "references": []}
    try:
        root, _, config = _document(path)
        result.update(status="portable_v1", path=str(root))
        references = set()
        diagnostics = validate_assets(root, {s: dict(config[s]) for s in config.sections()},
                                      verify_hashes=request.get("verifyHashes", True), references=references)
        manifest = json.loads((root / "assets.manifest.json").read_bytes())
        files = [f for bundle in manifest["bundles"] for f in bundle["files"]]
        result.update(ok=not diagnostics, diagnostics=diagnostics, references=sorted(references),
                      assetCount=len(files), totalBytes=sum(f["size"] for f in files))
    except ArchiveParseError as exc:
        if exc.code == "UNSUPPORTED_ARCHIVE_VERSION":
            result["status"] = "unsupported_version"
        elif result["status"] != "portable_v1" and (path / "scene.ini").exists():
            result["status"] = "malformed"
        diagnostic = exc.to_diagnostic()
        if exc.code == "UNSUPPORTED_VISION_ARCHIVE":
            diagnostic["code"] = "unsupported_vision_archive"
        result["diagnostics"] = [diagnostic]
    except (ValueError, OSError, configparser.Error) as exc:
        result["diagnostics"] = [{"code": "invalid_scene_format", "path": str(path), "message": str(exc)}]
    return result
