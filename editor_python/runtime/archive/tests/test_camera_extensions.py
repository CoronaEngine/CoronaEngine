import json
from runtime.archive.parser import parse_archive
from runtime.archive.writer import save_scene


def test_extensions_follow_camera_id_through_reorder_rename_and_delete(tmp_path):
    path = tmp_path / "scene.ini"
    path.write_text("[format]\ntype=corona_scene_folder\nversion=1\n[scene]\nname=Main\n"
                    "[camera]\ncount=2\nactive_id=a\nvendor_version=7\n"
                    "camera0.id=a\ncamera0.name=A\ncamera0.vendor.future=only-a\n"
                    "camera1.id=b\ncamera1.name=B\ncamera1.vendor.future=only-b\n")
    (tmp_path / "assets.manifest.json").write_text(json.dumps(
        {"format": "corona_scene_assets", "version": 1, "bundles": []}))
    state = parse_archive(str(tmp_path))["scene"]
    state.update(id="scene.ini", physics={}, sun={"enabled": True, "direction": [1, 1, 1]},
                 grid={"enabled": True}, script="")
    a, b = state["cameras"]
    a["name"] = "Renamed"
    state["cameras"] = [b, a]

    def save_and_read():
        assert save_scene({"project_path": str(tmp_path), "scene": state})["ok"]
        return parse_archive(str(tmp_path))["extensions"]["ini_sections"]["camera"]

    section = save_and_read()
    assert section["camera0.vendor.future"] == "only-b"
    assert section["camera1.vendor.future"] == "only-a" and section["camera1.name"] == "Renamed"
    state["cameras"] = [a]
    section = save_and_read()
    assert section["vendor_version"] == "7" and section["camera0.vendor.future"] == "only-a"
    assert "only-b" not in section.values() and "camera1.id" not in section
