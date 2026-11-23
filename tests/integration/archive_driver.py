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
    from runtime.archive.project import _document, create_project, validate_project
    from runtime.bridge_errors import BridgeError

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
                        elif req["function"] == "create_project":
                            # Creation at an exact path. The production service only exposes the
                            # directory allocator (`create_project_in`), which picks the directory
                            # itself; native tests need a deterministic path, so this fixture-only
                            # channel calls the same archive function the allocator uses.
                            result = create_project(data)
                        elif req["function"] == "validate_project":
                            # Validation is reported as data, never raised: the fixture reads the
                            # diagnostics out of the reply.
                            result = validate_project(data)
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
                    else:
                        assert req["module"] == "project_archive", req
                        result = getattr(ProjectArchive, req["function"])(*req["args"])
                    reply = {"success": True, "data": result}
                except (OSError, ValueError, configparser.Error, BridgeError) as exc:
                    # A typed refusal is data for the fixture too: `success` false, the code rides
                    # along so a test can assert on it instead of on the message.
                    reply = {"success": False, "error": str(exc)}
                    if isinstance(exc, BridgeError):
                        reply["code"] = exc.code
                print(json.dumps(reply), file=proc.stdin, flush=True)
            if proc.wait(timeout=60):
                raise subprocess.CalledProcessError(proc.returncode, args)
        finally:
            if proc.poll() is None:
                proc.kill()


if __name__ == "__main__":
    # The archive services moved out of the CoronaEditor checkout into the engine's
    # embedded_python tree (commit 938a1995). Resolve them from this file's own location: the
    # driver used to rely on CORONA_EDITOR_SOURCE_DIR, which is not populated in the tests
    # preset, and CoronaEditor/editor/plugins/ProjectArchive no longer holds main.py either.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "embedded_python"))
    if len(sys.argv) > 2 and sys.argv[2]:
        sys.path.insert(0, str(Path(sys.argv[2]) / "editor"))
    run_native([sys.argv[1], *sys.argv[3:]])
