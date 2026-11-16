"""Shared helpers for the deployed-editor smoke probes.

`open_web_project`, `install_host_probe`, `verify_trace` and `wait_for` are used by every
CEF smoke: they open a project in the running editor, install the host probe, check the
trace and poll a read. They used to live in the Blockly actor smoke; the script runtime is
gone, so they live on their own.
"""
import json
import os
import sys
import threading
import time
from pathlib import Path
from urllib.request import urlopen
from urllib.parse import parse_qs, urlsplit

def open_web_project(web, path):
    """Use the production Web owner so old-window saves finish before replacement."""
    return web.evaluate("""(async()=>{
      const module=await import(document.querySelector('script[type=module]').src);
      const launcher=Object.values(module).find(value=>typeof value?.openProject==='function'
        &&!value.createWorldProject);
      if(!launcher)throw Error('Production project launcher missing');
      return launcher.openProject(""" + json.dumps(str(path)) + ");})()")

def install_host_probe(editor, trace_path, *, delay_first=True):
    """Record the interpreter actually parsing the archive and executing the file."""
    import _interpreters
    trace_lock = threading.Lock()

    def record(event, identity=None):
        row = {"event": event, "id": identity, "pid": os.getpid(),
               "interpreter": int(_interpreters.get_current()[0]), "modules": id(sys.modules),
               "prefix": sys.prefix}
        with trace_lock, Path(trace_path).open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(row) + "\n")

    def audit(event, args):
        if event == "exec" and "/scripts/files/" in args[0].co_filename.replace("\\", "/").lower():
            ctx = sys._getframe(1).f_locals["ctx"]
            record("execute", ctx.id)
        elif event == "open" and str(args[0]).replace("\\", "/").endswith("/scene.ini"):
            frame = sys._getframe(1)
            while frame is not None:
                if frame.f_code.co_name == "parse_archive":
                    record("archive")
                    break
                frame = frame.f_back

    dispatch = editor.dispatch_script_request_from_cpp
    delayed = not delay_first

    def dispatch_with_delayed_receipt(cls, message):
        nonlocal delayed
        response = dispatch(message)
        request = json.loads(message)
        if request["module"] == "project_archive" and request["function"] == "save":
            record("save", json.loads(response)["data"].get("status"))
        return response

    record("bootstrap")
    sys.addaudithook(audit)
    editor.dispatch_script_request_from_cpp = classmethod(dispatch_with_delayed_receipt)


def verify_trace(trace_path, engine_log, report):
    rows = [json.loads(line) for line in trace_path.read_text(encoding="utf-8").splitlines()]
    assert engine_log.read_text(encoding="utf-8", errors="replace").count(
        "PythonAPI: Python interpreter initialized successfully") == 1
    assert len({(r["pid"], r["interpreter"], r["modules"], r["prefix"]) for r in rows}) == 1, rows
    assert sum(r["event"] == "bootstrap" for r in rows) == 1 and any(r["event"] == "archive" for r in rows)
    runs = report["runs"]
    assert any(r["event"] == "save" and r["id"] == "success" for r in rows), rows
    executions = [run["runId"] for run in runs]
    lookups = [runs[0]["requestId"], runs[0]["requestId"], runs[1]["requestId"]]
    if "switch" in report:
        switch = report["switch"]
        executions.append(switch["runId"])
        lookups.append(switch["requestId"])
        restored = [r for r in rows if r["event"] == "restored"][-1]
        assert Path(restored["id"]["project"]).resolve() == Path(switch["from"]), restored
        assert restored["id"]["position"] == runs[0]["before"]
        assert any(r["event"] == "archive" for r in rows[rows.index(restored) + 1:])
    assert [r["id"] for r in rows if r["event"] == "execute"] == executions
    assert [r["id"] for r in rows if r["event"] == "delay"] == [runs[0]["requestId"]]
    # First run has a recovery lookup in addition to the explicit probe query.
    assert [r["id"] for r in rows if r["event"] == "lookup"] == lookups


def wait_for(read, condition, description):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        value = read()
        if condition(value):
            return value
        time.sleep(0.1)
    raise AssertionError(f"{description}: {value!r}")


