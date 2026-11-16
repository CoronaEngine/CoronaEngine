from runtime.archive.arguments import require_choice, require_keys
"""File-browser operations on the active archive."""
import configparser
from io import StringIO
from pathlib import Path, PureWindowsPath
import shutil
from threading import RLock

from .portable import atomic_replace
from runtime.bridge_errors import BridgeError
from runtime.project_context import require_project_session


_file_lock = RLock()


def _entry(root, path, tree=False, level=0):
    directory = path.is_dir() and not path.is_symlink() and not path.is_junction()
    route = path.relative_to(root).as_posix()
    item = {"name": path.name, "path": "" if route == "." else route, "isDirectory": directory}
    if tree:
        item["level"] = level
        if directory:
            children = [_entry(root, child, True, level + 1) for child in path.iterdir()]
            item.update(expanded=level <= 1, children=sorted(children, key=lambda c: (not c["isDirectory"], c["name"])))
        else:
            item["size"] = path.lstat().st_size
    return item


def _filename(name):
    name = str(name).strip()
    if not name or name in (".", "..") or PureWindowsPath(name).name != name or ":" in name:
        raise ValueError("Invalid file name")
    return name


def file_manager(request):
    with _file_lock:
        return _file_manager(request)


# Refusals carry the code the Web branches on. They are raised (not returned) so the
# bridge rejects with that code; the dispatcher keeps it in front of the message, which
# is what stops every reason from flattening into SERVICE_FAILED.
_REFUSALS = (
    ("Path outside current project", "INVALID_ARGUMENT"),
    ("Cannot modify a linked project path", "INVALID_ARGUMENT"),
    ("Invalid Actor template request", "INVALID_ARGUMENT"),
    ("Invalid file name", "INVALID_ARGUMENT"),
    ("Unsupported project file operation", "INVALID_ARGUMENT"),
    ("Cannot change the archive root or scene.ini", "PERMISSION_DENIED"),
    ("Target already exists", "CONFLICT"),
    ("Stop and restore the file run", "CONFLICT"),
)


def _refusal(message):
    for prefix, code in _REFUSALS:
        if message.startswith(prefix):
            raise BridgeError(code, message)
    raise BridgeError("FILE_OPERATION_FAILED", message)


FILE_OPS = {
    "get_file_tree": {"required": ("path",), "optional": ()},
    "get_files": {"required": ("path",), "optional": ()},
    "create_folder": {"required": ("path", "name"), "optional": ()},
    "create_file": {"required": ("path", "name"), "optional": ("file_type",)},
    "delete_item": {"required": ("path",), "optional": ()},
    "rename_item": {"required": ("path", "name"), "optional": ()},
}

_SCOPE_KEYS = ("archive_session", "project_path")


def _file_manager(request):
    root, error = require_project_session(request)
    if error:
        raise BridgeError(error.get("code") or "SERVICE_FAILED",
                          error.get("message") or "Project session refused")
    try:
        op = require_choice(request, "op", FILE_OPS)
        spec = FILE_OPS[op]
        require_keys(request, required=("op",) + spec["required"],
                     optional=spec["optional"] + _SCOPE_KEYS)
        requested = root / request["path"]
        path = requested.resolve()
        if not path.is_relative_to(root):
            raise ValueError("Path outside current project")
        if op == "get_file_tree":
            entry = _entry(root, path, True, 0 if path == root else 1)
            return {**entry, "archive_session": request.get("archive_session")}
        if op == "get_files":
            return [_entry(root, child) for child in path.iterdir()]
        if path != requested.absolute():
            raise ValueError("Cannot modify a linked project path")
        if op == "create_folder":
            (path / _filename(request["name"])).mkdir(exist_ok=True)
            return True
        if op == "create_file":
            if request.get("file_type") != "actor" or not path.is_dir():
                raise ValueError("Invalid Actor template request")
            name = _filename(request["name"])
            if not name.lower().endswith(".actor"):
                name += ".actor"
            target = path / name
            stem = target.stem
            index = 0
            while target.exists() or target.is_symlink():
                index += 1
                target = path / f"{stem}_{index}.actor"
            config = configparser.ConfigParser(interpolation=None, strict=False)
            template = Path(__file__).resolve().parents[2] / "plugins/ProjectLauncher/templates/actor/demo.actor"
            with template.open(encoding="utf-8-sig") as stream:
                config.read_file(stream)
            config["base"]["name"] = target.stem
            stream = StringIO()
            config.write(stream)
            atomic_replace(target, stream.getvalue().encode())
            return {"status": "success", "path": target.relative_to(root).as_posix(),
                    "name": target.stem, "file_type": "actor"}
        if path == root or path == root / "scene.ini":
            raise ValueError("Cannot change the archive root or scene.ini")
        if op == "rename_item":
            target = path.with_name(_filename(request["name"]))
            if target.exists() or target.is_symlink():
                raise ValueError("Target already exists")
            path.rename(target)
        elif op == "delete_item":
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
        return True
    except (OSError, TypeError, ValueError) as failure:
        _refusal(str(failure))
