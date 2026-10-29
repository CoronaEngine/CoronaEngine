"""CPU-only parser -> native scene -> save -> parser regression."""
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(sys.argv[2]) / "editor"))
from runtime.archive.parser import parse_archive

executable = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="corona_archive_roundtrip_") as directory:
    root = Path(directory)
    def run(mode, path):
        output = root / "result.json"
        subprocess.run([executable, mode, str(path), str(output)], check=True, timeout=60)
        return json.loads(output.read_text(encoding="utf-8"))

    scene = root / "portable"
    asset = run("fixture", scene)
    original = parse_archive(str(scene))
    for iteration in range(2):
        current = parse_archive(str(scene))
        payload = root / "parsed.json"
        payload.write_text(json.dumps(current), encoding="utf-8")
        result = run("roundtrip", payload)
        assert result["save"]["ok"], result
        assert result["camera_lock_active"], result
        physics = result["environment"]["physics"]
        assert physics["gravity"] == [1., -3., 2.] and physics["floor_y"] == -4.
        assert abs(physics["fixed_dt"] - .02) < 1e-6 and physics["floor_restitution"] == .25
        assert result["scene"]["actors"][0]["camera_lock"]["enabled"]
        saved = parse_archive(str(scene))
        lock = saved["scene"]["actors"][0]["camera_lock"]
        expected_lock = original["scene"]["actors"][0]["camera_lock"]
        assert lock["enabled"] == expected_lock["enabled"], lock
        for field in ("position_offset", "rotation_offset"):
            assert all(math.isclose(a, b, abs_tol=1e-6) for a, b in zip(lock[field], expected_lock[field])), lock
        assert saved["scene"]["environment"]["physics"] == original["scene"]["environment"]["physics"]

    # Native open must reject corruption even when degraded loading is requested.
    model = scene / asset["route"]
    data = model.read_bytes()
    model.write_bytes(b"X" + data[1:])
    failed = run("open", scene)
    assert failed["status"] == "invalid_archive", failed
    assert any(d["code"] == "hash_mismatch" for d in failed["diagnostics"]), failed
    model.write_bytes(data)
    manifest = scene / "assets.manifest.json"
    data = manifest.read_bytes()
    manifest.unlink()
    assert run("open", scene)["status"] == "invalid_archive"
    manifest.write_bytes(data)

    # No Python service is hosted by this executable: reaching it proves that
    # recovery and full validation succeeded before the parser handoff.
    document = scene / "scene.ini"
    (scene / ".scene.ini.backup").write_bytes(document.read_bytes())
    (scene / ".scene-save.transaction").write_text("scene.ini\n")
    document.write_text("interrupted")
    assert run("open", document)["status"] == "service_initializing"
    assert parse_archive(str(scene))["scene"]["environment"]["physics"] == original["scene"]["environment"]["physics"]

    for name, content in (
        ("source.xml", '<scene version="3.0.0"><shape type="rectangle"/></scene>'),
        ("source.pbrt", 'WorldBegin\nShape "sphere" "float radius" [1]\n'),
    ):
        source = root / name
        source.write_text(content, encoding="utf-8")
        external = parse_archive(str(source))
        assert external["read_only"]
        payload = root / "external.json"
        payload.write_text(json.dumps(external), encoding="utf-8")
        result = run("roundtrip", payload)
        assert result["save"]["status"] == "read_only", result
        assert source.read_text(encoding="utf-8") == content
        assert not (root / "scene.ini").exists()
print("Archive roundtrip, native open validation, and external read-only checks passed")
