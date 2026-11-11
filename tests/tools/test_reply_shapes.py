"""Pin the rule: a reply that is Python's answer is forwarded, not mirrored.

The launcher and the settings replies carried five structs and two converters that re-declared
shapes the archive service had already produced. This guard keeps them from coming back, and
states which converters are *supposed* to stay hand-written - the ones whose producer is C++
engine state, where the converter is the only place the shape exists.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
OPERATIONS = ROOT / "src/systems/ui/cef/cef_operations.h"
BINDING = ROOT / "src/systems/ui/cef/cef_v8_binding.cpp"

FORWARDED_REPLIES = ["ProjectLauncherReply", "ProjectSettingsReply"]


def _struct(name: str) -> str:
    text = OPERATIONS.read_text(encoding="utf-8-sig")
    match = re.search(r"struct " + name + r" \{.*?\n\};", text, re.S)
    assert match, f"{name} is gone from cef_operations.h"
    return match.group(0)


def test_python_owned_replies_carry_json_not_mirror_fields():
    for name in FORWARDED_REPLIES:
        body = _struct(name)
        assert "nlohmann::json value" in body, (
            f"{name} carries Python's answer; it must forward it as JSON instead of mirroring "
            f"every field")
        mirrors = re.findall(r"std::optional<\w+Snapshot>|std::vector<\w+Snapshot>", body)
        assert not mirrors, f"{name} grew mirror fields again: {mirrors}"


def test_the_forwarders_are_one_line():
    text = BINDING.read_text(encoding="utf-8-sig")
    for converter, reply in (("project_launcher_value", "ProjectLauncherReply"),
                             ("project_settings_value", "ProjectSettingsReply")):
        start = text.index(f"CefRefPtr<CefV8Value> {converter}(")
        end = text.index("\n}", start)
        body = text[start:end]
        setters = body.count("SetValue(")
        assert setters == 0, (
            f"{converter} rebuilds the shape field by field again; {reply} is Python's answer and "
            f"should be forwarded with json_to_v8")
        assert "json_to_v8" in body, f"{converter} must forward the JSON value"


def test_engine_produced_converters_stay_typed():
    """The other direction: a shape that only C++ produces must not become a JSON blob."""
    text = BINDING.read_text(encoding="utf-8-sig")
    for converter in ("cameras_value", "tree_actors_value", "snapshot_value"):
        start = text.index(f"CefRefPtr<CefV8Value> {converter}(")
        end = text.index("\n}", start)
        assert "SetValue(" in text[start:end], (
            f"{converter} projects live engine state; its converter is where that shape lives")
