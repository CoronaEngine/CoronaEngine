"""Old camera renderer choices are discarded on a full scene save."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "embedded_python"))

from runtime.archive.parser import parse_archive
from runtime.archive.writer import save_scene


def test_legacy_camera_backend_round_trip(tmp_path):
    path = tmp_path / "scene.ini"
    path.write_text(
        "[format]\ntype=corona_scene_folder\nversion=1\n[camera]\ncount=3\n"
        "camera0.render_backend=native\ncamera0.vision_render_mode=restir\n"
        "camera1.render_backend=vision\ncamera1.vision_render_mode=ssat\n"
        "camera2.custom_setting=preserved\n", encoding="utf-8")
    (tmp_path / "assets.manifest.json").write_text(
        '{"format":"corona_scene_assets","version":1,"bundles":[]}', encoding="utf-8")
    scene = parse_archive(str(path))["scene"]
    cameras = scene["cameras"]
    assert all("render_backend" not in camera for camera in cameras)
    assert [camera["vision_render_mode"] for camera in cameras] == ["restir", "ssat", "path_tracing"]

    save_scene({"project_path": str(tmp_path), "scene": {
        "id": scene["route"], "active_camera_id": scene["active_camera_id"], "cameras": cameras,
        "actors": [], "sun": {"enabled": True, "direction": [0, 1, 0]},
        "physics": {}, "terrain": scene["terrain"], "script": scene["scripts"]["path"],
    }})
    text = path.read_text(encoding="utf-8")
    assert "render_backend" not in text
    assert "camera2.custom_setting = preserved" in text
    assert parse_archive(str(path))["scene"]["cameras"] == cameras


def test_camera_round_trip_and_retired_grid(tmp_path):
    path = tmp_path / "scene.ini"
    (tmp_path / "assets.manifest.json").write_text(
        '{"format":"corona_scene_assets","version":1,"bundles":[]}', encoding="utf-8")
    for extra_grid_key in ("", "custom_setting=kept\n"):
        path.write_text(
            f"[format]\ntype=corona_scene_folder\nversion=1\n[grid]\nenabled=false\n{extra_grid_key}"
            "[camera]\ncount=1\ncamera0.id=main-camera\n", encoding="utf-8")
        scene = parse_archive(str(path))["scene"]
        assert "floor_grid_enabled" not in scene["environment"]
        assert "output_mode" not in scene["cameras"][0]
        save_scene({"project_path": str(tmp_path), "scene": {
            "id": scene["route"], "active_camera_id": scene["active_camera_id"], "cameras": scene["cameras"],
            "actors": [], "sun": {"enabled": True, "direction": [0, 1, 0]},
            "physics": {}, "terrain": scene["terrain"], "script": scene["scripts"]["path"],
        }})
        text = path.read_text(encoding="utf-8")
        assert "output_mode" not in text
        assert ("[grid]" in text) == bool(extra_grid_key)
        assert ("custom_setting = kept" in text) == bool(extra_grid_key)
        assert "enabled = false" not in text
        assert parse_archive(str(path))["scene"]["cameras"] == scene["cameras"]
