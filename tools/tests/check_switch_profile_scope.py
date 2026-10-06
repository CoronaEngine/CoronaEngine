"""Exercise the real C++ scope emitter and consume its output with the report parser."""
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from profile_process import run_bounded
from switch_profile_report import MARKER, parse_log


def check(executable, output):
    expected = [
        ("B", "PT_to_ReSTIR"), ("B", "nested"),
        ("B", "early_return"), ("E", "early_return"),
        ("B", "unwind"), ("E", "unwind"), ("E", "nested"),
        ("B", "sibling"), ("E", "sibling"), ("E", "PT_to_ReSTIR"),
    ]
    for enabled in ("1", "0"):
        env = dict(os.environ, CORONA_SWITCH_PROFILE=enabled)
        result = run_bounded([str(executable)], cwd=executable.parent, env=env,
                             output=output / enabled, timeout=10)
        assert result["status"] == "success", result
        assert result["exit_code"] == 0 and result["cleanup_ok"], result
        text = Path(result["stdout"]).read_text(encoding="utf-8")
        events = [json.loads(line[len(MARKER):]) for line in text.splitlines()
                  if line.startswith(MARKER)]
        assert [(e["ph"], e["name"]) for e in events] == (expected if enabled == "1" else [])
        if enabled == "1":
            report = parse_log(text)
            assert not report["warnings"], report["warnings"]
            phase, = report["phases"]
            assert [c["name"] for c in phase["children"]] == ["nested", "sibling"]
            assert abs(sum(phase["breakdown_ms"].values()) - phase["duration_ms"]) < 0.01
    print("Scope normal exit, early return, exception unwinding, nesting and disabled mode passed")


if __name__ == "__main__":
    check(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
