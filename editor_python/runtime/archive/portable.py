"""Portable scene file transactions and asset-reference validation."""

import configparser
import hashlib
import io
import re
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
from tempfile import NamedTemporaryFile

from .errors import ArchiveParseError


def atomic_replace(path, content):
    temporary = None
    try:
        with NamedTemporaryFile(dir=path.parent, prefix=".scene-", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def write_ini_sections(path, original, updates):
    # Replace only edited sections, retaining user comments/formatting elsewhere.
    text = path.read_bytes().decode("utf-8-sig") if path.exists() else ""
    newline = "\r\n" if "\r\n" in text else "\n"
    chunks = re.split(r"(?m)^(?=[ \t]*\[)", text.replace("\r\n", "\n"))
    pending = {}
    for name in updates:
        partial = configparser.ConfigParser(interpolation=None)
        partial.optionxform = original.optionxform
        partial[name] = dict(original.items(name, raw=True))
        stream = io.StringIO()
        partial.write(stream)
        pending[name] = stream.getvalue()
    for index, chunk in enumerate(chunks):
        header = re.match(r"[ \t]*\[([^]\n]+)\]", chunk)
        if header and header[1] in pending:
            chunks[index] = pending.pop(header[1])
    output = "".join(chunks)
    if pending:
        output = (output.rstrip("\n") + "\n\n" if output else "") + "".join(pending.values())
    atomic_replace(path, output.replace("\n", newline).encode("utf-8"))


def recover_scene(root):
    """Honor an interrupted transaction left by an older editor."""
    marker = root / ".scene-save.transaction"
    if not marker.is_file():
        return
    backup = root / ".scene.ini.backup"
    try:
        if backup.is_symlink() or not backup.is_file():
            raise OSError("Scene transaction backup is missing or invalid")
        atomic_replace(root / "scene.ini", backup.read_bytes())
        marker.unlink()
        backup.unlink()
    except OSError as exc:
        raise ArchiveParseError("scene_recovery_failed", str(exc), path=str(root)) from exc


def _asset_route(route):
    return (isinstance(route, str) and route.startswith("Assets/") and "\\" not in route
            and not PureWindowsPath(route).drive and ".." not in PurePosixPath(route).parts
            and ":" not in route and "\0" not in route)


def validate_assets(root, sections, *, verify_hashes, references=None):
    manifest = root / "assets.manifest.json"
    def fail(code, message, path=manifest):
        raise ArchiveParseError(code, message, path=str(path))

    def require_route(route):
        if not _asset_route(route):
            fail("unsafe_route", f"Unsafe asset route: {route}")
        path = (root / route).resolve()
        if not path.is_relative_to(root / "Assets"):
            fail("unsafe_route", "Asset resolves outside the project Assets directory", path)
        return path

    try:
        if not manifest.resolve().is_relative_to(root):
            fail("unsafe_route", "Asset manifest is outside the project")
        document = json.loads(manifest.read_bytes())
        if (document.get("format"), document.get("version")) != ("corona_scene_assets", 1):
            fail("invalid_manifest", "Unsupported asset manifest")
        bundles = document["bundles"]
        if not isinstance(bundles, list):
            fail("invalid_manifest", "Expected an array of bundles")
        routes = {f["path"] for b in bundles for f in b["files"]}
        missing = []
        for bundle in bundles:
            main = bundle["main"]
            require_route(main)
            prefix = PurePosixPath(*PurePosixPath(main).parts[:3])
            files = {f["path"]: f for f in bundle["files"]}
            dependencies = bundle["dependencies"]
            if not isinstance(dependencies, list) or main not in files:
                fail("invalid_manifest", "Invalid bundle files or dependencies")
            for route in dependencies:
                require_route(route)
                if route not in routes:
                    fail("missing_dependency", "Unknown bundle dependency", route)
            if files.keys() - {main} - set(dependencies):
                fail("invalid_dependencies", "Unrecorded bundle dependency")
            digest = hashlib.sha256()
            complete = True
            for route, entry in sorted(files.items()):
                path = require_route(route)
                if not PurePosixPath(route).is_relative_to(prefix):
                    fail("unsafe_route", "Asset escapes its bundle", path)
                if not path.is_file():
                    complete = False
                    missing.append(ArchiveParseError("missing_asset", "Manifest asset is missing",
                                                     path=str(path), recoverable=True).to_diagnostic())
                    continue
                if path.stat().st_size != entry["size"]:
                    fail("size_mismatch", "Manifest asset size mismatch", path)
                if verify_hashes:
                    digest.update(PurePosixPath(route).relative_to(prefix).as_posix().encode() + b"\0")
                    asset_hash = hashlib.sha256()
                    with path.open("rb") as stream:
                        while chunk := stream.read(1 << 20):
                            digest.update(chunk)
                            asset_hash.update(chunk)
                    if asset_hash.hexdigest() != entry["sha256"]:
                        fail("hash_mismatch", "Manifest asset hash mismatch", path)
            if verify_hashes and complete and digest.hexdigest() != bundle["sha256"]:
                fail("bundle_hash_mismatch", "Bundle hash mismatch")
        for section, fields in sections.items():
            for key, value in fields.items():
                if section == "actors" and key.endswith(".animation.sources"):
                    values = json.loads(value)
                    if (not isinstance(values, list) or not all(isinstance(r, str) and r for r in values)
                            or len(set(values)) != len(values)):
                        fail("invalid_animation_sources", "Invalid animation sources")
                elif ((section == "actors" and key.endswith((".route", ".material.texture")))
                      or (section in {"scripts", "terrain"} and key == "path") or _asset_route(value)):
                    values = [value] if value else []
                else:
                    continue
                for route in values:
                    if not _asset_route(route) or route not in routes:
                        error = ArchiveParseError("untrusted_asset_route", f"Unlisted resource in {section}.{key}: {route}",
                                                  path=route, details={"field": f"{section}.{key}"})
                        if references is None:
                            raise error
                        missing.append(error.to_diagnostic())
                        continue
                    if references is not None:
                        references.add(route)
        return missing
    except ArchiveParseError:
        raise
    except (AttributeError, KeyError, OSError, TypeError, ValueError) as exc:
        fail("invalid_manifest", str(exc))
