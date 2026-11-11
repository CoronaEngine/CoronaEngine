"""Serialize launcher settings and recent-project updates to the host's ini file."""
import configparser
import json
import sys
from pathlib import Path
from threading import RLock

from runtime.archive.portable import write_ini_sections

_lock = RLock()

def default_ini_path():
    """Where the editor keeps CoronaEditor.ini.

    The native rule is "the executable's directory"; an embedded interpreter can answer a
    different sys.executable than a packaged one, so the file tree is checked as a fallback.
    A caller that knows the path passes it in and none of this runs.
    """
    candidates = [
        Path(sys.executable).parent / "CoronaEditor.ini",
        Path(__file__).resolve().parents[3] / "CoronaEditor.ini",
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return candidates[0]


def editor_settings(request):
    path = Path(request["ini"]) if request.get("ini") else default_ini_path()
    with _lock:
        config = configparser.ConfigParser(interpolation=None, strict=False)
        config.read(path, encoding="utf-8-sig")
        section = request["section"]
        # Native ini lookups historically ignored section and option case.
        section = next((s for s in config if s.casefold() == section.casefold()), section)
        if request.get("recent_projects") or "recent" in request:
            try:
                history = json.loads(config.get(section, "recent_projects", fallback="[]"))
            except ValueError:
                history = []
            history = [p for p in history if isinstance(p, str)] if isinstance(history, list) else []
            if request.get("recent_projects"):
                return {"projects": history}
        if "key" in request:
            return {"value": config.get(section, request["key"], fallback=request.get("fallback", ""))}
        if not config.has_section(section):
            config.add_section(section)
        touched = [section]
        updates = dict(request.get("updates", {}))
        if "recent" in request:
            newest = request["recent"]
            rest = [p for p in history if Path(p) != Path(newest)]
            general = next((s for s in config if s.lower() == "general"), "General")
            if not config.has_section(general):
                config.add_section(general)
            config.set(general, "last_project", newest)
            touched.append(general)
            updates["recent_projects"] = json.dumps([newest, *rest[:9]], ensure_ascii=False)
        for key, value in updates.items():
            config.set(section, key, value)
        write_ini_sections(path, config, touched)
        return {"ok": True}
