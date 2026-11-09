"""Publish imported resource bundles and their manifest as one archive operation."""
import configparser
import hashlib
import json
import os
from io import StringIO
from pathlib import Path, PurePosixPath, PureWindowsPath
import shutil
from tempfile import TemporaryDirectory
from threading import RLock
from time import sleep

from .errors import ArchiveParseError
from .portable import atomic_replace, validate_assets

_lock = RLock()
_categories = {"Models", "Actors", "Audio", "Images", "Scripts", "Terrain"}


def actor_descriptor(path, root=None):
    path = Path(path)
    ini = configparser.ConfigParser(interpolation=None, strict=False, delimiters=("=",))
    with path.open(encoding="utf-8-sig") as stream:
        ini.read_file(stream)
    sections = {name.lower(): dict(ini[name]) for name in ini.sections()}
    base = sections.get("base", {})
    model = base.get("path", "")
    if not model:
        raise ValueError(f"Actor file missing [base].path: {path}")
    model_path = Path(model)
    if not model_path.is_absolute():
        beside = path.parent / model_path
        model_path = beside if beside.is_file() or root is None else Path(root) / model_path
    return {"sections": sections, "model": str(model_path.resolve()), "name": base.get("name", ""),
            "actor_guid": base.get("actor_guid", ""), "follow_camera": base.get("follow_camera", "false"),
            "geometry": sections.get("geometry", {})}


def _contained(root, path):
    if not path.resolve().is_relative_to(root) or path.resolve() == root:
        raise ValueError(f"Path escapes its archive directory: {path}")
    return path


def _read_manifest(root, allow_missing):
    path = _contained(root, root / "assets.manifest.json")
    if not path.exists() and (allow_missing or not (root / "Assets").exists()):
        return {"format": "corona_scene_assets", "version": 1, "bundles": []}
    try:
        data = json.loads(path.read_bytes())
        if (data["format"], data["version"]) != ("corona_scene_assets", 1) or not isinstance(data["bundles"], list):
            raise ValueError("Unsupported asset manifest")
        return data
    except (OSError, KeyError, ValueError, TypeError) as exc:
        raise ArchiveParseError("invalid_manifest", str(exc), path=str(path)) from exc


def _publish(root, manifest):
    atomic_replace(_contained(root, root / "assets.manifest.json"),
                   (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode())


def _install_bundle(stage, destination):
    # A Windows reader of a staged file can briefly deny renaming its parent.
    # Retry only that atomic operation; persistent failures still roll back.
    for attempt in range(6):
        try:
            stage.rename(destination)
            return
        except OSError as error:
            if os.name != "nt" or getattr(error, "winerror", None) not in (5, 32) or attempt == 5:
                raise
            sleep(.01 * 2 ** attempt)


def _stage_bundle(root, manifest, category, sources, installed, *, extra_dependencies=()):
    if category not in _categories or not sources:
        raise ValueError("Invalid asset category or empty source list")
    root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=".asset-stage-", dir=root) as stage:
        stage = Path(stage)
        staged = []
        for source in sources:
            relative = source["relative"]
            if (not relative or "\\" in relative or PureWindowsPath(relative).anchor or ":" in relative
                    or ".." in PurePosixPath(relative).parts or relative == "."):
                raise ValueError("Unsafe resource dependency")
            target = _contained(stage, stage / relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            if "content" in source:
                target.write_bytes(source["content"])
            else:
                shutil.copyfile(source["source"], target)
            staged.append((relative, target))
        total = hashlib.sha256()
        files = {}
        for relative, target in sorted(staged):
            total.update(relative.encode() + b"\0")
            digest = hashlib.sha256()
            with target.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1 << 20), b""):
                    total.update(chunk)
                    digest.update(chunk)
            files[relative] = {"sha256": digest.hexdigest(), "size": target.stat().st_size}
        sha = total.hexdigest()
        directory = PurePosixPath("Assets", category, sha[:12])
        destination = _contained(root / "Assets", root / directory)
        main = str(directory / staged[0][0])
        entries = [{"path": str(directory / rel), **files[rel]} for rel, _ in staged]
        bundle = {"type": "model" if category == "Models" else category.lower(), "sha256": sha,
                  "main": main, "files": entries,
                  "dependencies": [e["path"] for e in entries[1:]] + list(extra_dependencies)}
        if destination.exists():
            for relative, entry in files.items():
                existing = _contained(destination.resolve(), destination / relative)
                if not existing.is_file() or _file_digest(existing) != entry["sha256"]:
                    raise ArchiveParseError("bundle_collision", "Existing bundle has different content", path=str(destination))
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            _install_bundle(stage, destination)
            installed.append(destination)
        if not any(b["main"] == main and b["sha256"] == sha for b in manifest["bundles"]):
            manifest["bundles"].append(bundle)
        return bundle


def _file_digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _import(root, manifest, req):
    from script_runtime.file_runner import FileScripts
    # Files already in this project can be instantiated without publishing anything.
    source = Path(req.get("actor") or req["source"]).resolve()
    for bundle in manifest["bundles"]:
        if source == (root / bundle["main"]).resolve():
            return bundle
    if FileScripts.has_restore_snapshot():
        raise ArchiveParseError("run_active", "Cannot import new assets during script execution")
    before = len(manifest["bundles"])
    descriptor = actor_descriptor(source, root) if req.get("actor") else None
    installed = []
    try:
        bundle = _stage_bundle(root, manifest, req["category"], req["files"], installed)
        if descriptor:
            ini = configparser.ConfigParser(interpolation=None)
            sections = descriptor["sections"]
            sections["base"]["path"] = bundle["main"]
            ini.read_dict(sections)
            out = StringIO()
            ini.write(out)
            bundle = _stage_bundle(root, manifest, "Actors",
                                   [{"relative": source.name, "content": out.getvalue().encode()}],
                                   installed, extra_dependencies=[bundle["main"]])
        if len(manifest["bundles"]) != before or not (root / "assets.manifest.json").exists():
            _publish(root, manifest)
    except BaseException:
        # Only directories installed by this transaction can be rolled back.
        for directory in reversed(installed):
            shutil.rmtree(_contained(root / "Assets", directory))
        raise
    return bundle


def _cleanup(root, manifest, references, dry_run):
    from script_runtime.file_runner import FileScripts
    missing = validate_assets(root, {}, verify_hashes=True)
    if missing:
        return {"diagnostics": missing}
    if not dry_run and FileScripts.has_restore_snapshot():
        raise ArchiveParseError("run_active", "Cannot clean up assets during script execution")
    if (root / "scene.ini").is_file():
        from .project import validate_project
        scene = validate_project({"path": str(root)})
        if not scene["ok"]:
            return {"diagnostics": scene["diagnostics"]}
        references = set(references) | set(scene["references"])
    bundles = manifest["bundles"]
    owner = {f["path"]: b for b in bundles for f in b["files"]}
    keep = set()
    pending = list(references)
    while pending:
        bundle = owner.get(pending.pop())
        if bundle and bundle["main"] not in keep:
            keep.add(bundle["main"])
            pending.extend(bundle["dependencies"])
    unused = [b for b in bundles if b["main"] not in keep]
    files = {entry["path"]: entry for b in unused for entry in b["files"]}
    if not dry_run and unused:
        manifest["bundles"] = [b for b in bundles if b["main"] in keep]
        _publish(root, manifest)
        retained = {f["path"] for b in manifest["bundles"] for f in b["files"]}
        for route in files.keys() - retained:
            path = _contained(root, root / route)
            path.unlink(missing_ok=True)
            for parent in path.parents:
                if parent == root / "Assets":
                    break
                try:
                    parent.rmdir()
                except OSError:
                    break
    return {"removedBundles": len(unused), "removedFiles": len(files),
            "reclaimedBytes": sum(f["size"] for f in files.values()), "diagnostics": []}


def asset_operation(req):
    root = Path(req["root"]).resolve()
    with _lock:
        try:
            op = req["op"]
            if op == "descriptor":
                return actor_descriptor(req["source"], root)
            manifest = _read_manifest(root, req.get("allowMissing", False))
            if op == "ensure":
                if not (root / "assets.manifest.json").exists():
                    from script_runtime.file_runner import FileScripts
                    if FileScripts.has_restore_snapshot():
                        raise ArchiveParseError("run_active", "Cannot initialize an archive during script execution")
                    root.mkdir(parents=True, exist_ok=True)
                    _publish(root, manifest)
                return {"ok": True}
            if op == "validate":
                return {"diagnostics": validate_assets(root, {}, verify_hashes=req.get("hashes", True))}
            if op == "contains":
                return {"found": any(f["path"] == req["route"] for b in manifest["bundles"] for f in b["files"])}
            if op == "cleanup":
                return _cleanup(root, manifest, req["references"], req["dryRun"])
            if op == "import":
                return {"bundle": _import(root, manifest, req), "diagnostics": []}
            raise ValueError(f"Unknown asset operation {op}")
        except (OSError, ValueError, KeyError, TypeError, configparser.Error) as error:
            return {"diagnostics": [{"code": getattr(error, "code", "asset_import_failed"),
                                     "message": str(error), "path": str(root)}]}
