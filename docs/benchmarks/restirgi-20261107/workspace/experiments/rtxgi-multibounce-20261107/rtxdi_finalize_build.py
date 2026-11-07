from pathlib import Path
import difflib
import hashlib
import json
import struct

BASE = Path(r"D:/work/corona/CoronaEngine")
EXP = BASE / ".workspace/experiments/rtxgi-multibounce-20261107"
SOURCE = BASE / ".workspace/cache/rtxgi-multibounce-20261107/rtxdi"
OLD = BASE / ".workspace/cache/rtxgi-kitchen-bench-20261107/rtxdi"
BUILD = EXP / "rtxdi-build-msvc1444"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fnv(data):
    result = 14695981039346656037
    for value in data:
        result = ((result ^ value) * 1099511628211) & 0xffffffffffffffff
    return f"{result:016x}"


def main():
    frozen = json.loads((EXP / "rtxdi-source-freeze.json").read_text())
    for record in frozen["changed_sources"]:
        assert sha(SOURCE / record["path"]) == record["sha256"], record["path"]
    for record in frozen["sdk_files"]:
        assert sha(SOURCE / record["path"]) == record["donor_sha256"], record["path"]
        assert sha(OLD / record["path"]) == record["previous_sha256"], record["path"]

    previous = json.loads((BASE / ".workspace/experiments/rtxgi-kitchen-bench-20261107/rtxdi-source-manifest.json").read_text())
    for record in previous["changed_sources"]:
        assert sha(OLD / record["path"]) == record["snapshot_sha256"], record["path"]
    assert sha(Path(previous["binary"]["path"])) == previous["binary"]["sha256"]

    patch = []
    for record in frozen["changed_sources"]:
        rel = record["path"]
        before = (OLD / rel).read_text().splitlines(keepends=True) if (OLD / rel).exists() else []
        after = (SOURCE / rel).read_text().splitlines(keepends=True)
        patch.extend(difflib.unified_diff(before, after, fromfile="a/" + rel, tofile="b/" + rel))
    (EXP / "rtxdi-multibounce.patch").write_text("".join(patch))

    selected = []
    shader = BUILD / "bin/shaders/full-sample/dxil/LightingPasses/ShadeSecondarySurfaces.bin"
    blob = shader.read_bytes()
    assert blob[:4] == b"NVSP"
    index = 4
    while index < len(blob) - 8:
        key_size, data_size = struct.unpack_from("<II", blob, index)
        key = blob[index+8:index+8+key_size].decode()
        data = blob[index+8+key_size:index+8+key_size+data_size]
        if not data_size:
            break
        selected.append({"key": key, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "fnv64": fnv(data)})
        index += 8 + key_size + data_size
    assert len(selected) == 6

    frozen["binary"] = {"path": str(BUILD / "bin/FullSample.exe"), "sha256": sha(BUILD / "bin/FullSample.exe")}
    frozen["shader_blobs"] = [{"path": str(p.relative_to(BUILD / "bin")), "sha256": sha(p)} for p in sorted((BUILD / "bin/shaders/full-sample/dxil").rglob("*.bin"))]
    frozen["secondary_shader_permutations"] = selected
    frozen["old_sources_and_binary_preserved"] = True
    (EXP / "rtxdi-source-manifest.json").write_text(json.dumps(frozen, indent=2) + "\n")
    print(json.dumps({"binary": frozen["binary"], "sdk_unchanged": frozen["sdk_all_unchanged"], "old_sources_and_binary_preserved": True, "secondary_permutations": selected}, indent=2))


if __name__ == "__main__":
    main()
