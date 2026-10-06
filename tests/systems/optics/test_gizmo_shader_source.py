from pathlib import Path


def test_gizmo_storage_image_matches_rgba16_float_overlay():
    shader = (
        Path(__file__).resolve().parents[3]
        / "assets"
        / "shaders"
        / "optics_gizmo.comp.glsl"
    ).read_text(encoding="utf-8")

    assert "layout(set = 2, binding = 0, rgba16f) uniform image2D images[];" in shader
    assert "layout(set = 2, binding = 0, rgba16) uniform image2D images[];" not in shader
