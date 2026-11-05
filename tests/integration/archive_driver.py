"""Serve the real Python archive operations to native CPU tests over stdio."""

import configparser
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys


def run_native(args, **kwargs):
    from plugins.ProjectArchive.main import ProjectArchive
    from runtime.archive.portable import atomic_replace, recover_scene
    from runtime.archive.project import _document

    with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          encoding="utf-8", errors="replace", **kwargs) as proc:
        try:
            for line in proc.stdout:
                if not line.startswith("ARCHIVE_RPC "):
                    print(line, end="")
                    continue
                req = json.loads(line.removeprefix("ARCHIVE_RPC "))
                try:
                    if req["module"] == "ArchiveFixture":
                        data = req["args"][0]
                        root = Path(data["path"])
                        result = {}
                        if req["function"] == "sha256":
                            result = hashlib.sha256(root.read_bytes()).hexdigest()
                        elif req["function"] == "recover":
                            recover_scene(root)
                        elif req["function"] == "sections":
                            # Build arbitrary input, including intentionally invalid fixtures.
                            _, path, config = _document(root)
                            for name, lines in data["sections"].items():
                                config.remove_section(name)
                                if lines:
                                    config.read_string("\n".join(lines))
                            stream = io.StringIO()
                            config.write(stream)
                            atomic_replace(path, stream.getvalue().encode())
                        else:
                            raise AssertionError(req)
                    elif req["module"] == "FileScripts":
                        from script_runtime.file_runner import FileScripts
                        result = getattr(FileScripts, req["function"])(*req["args"])
                    else:
                        assert req["module"] == "ProjectArchive", req
                        result = getattr(ProjectArchive, req["function"])(*req["args"])
                    reply = {"success": True, "data": result}
                except (OSError, ValueError, configparser.Error) as exc:
                    reply = {"success": False, "error": str(exc)}
                print(json.dumps(reply), file=proc.stdin, flush=True)
            if proc.wait(timeout=60):
                raise subprocess.CalledProcessError(proc.returncode, args)
        finally:
            if proc.poll() is None:
                proc.kill()


if __name__ == "__main__":
    sys.path.insert(0, str(Path(sys.argv[2]) / "editor"))
    run_native([sys.argv[1], *sys.argv[3:]])
