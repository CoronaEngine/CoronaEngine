from __future__ import annotations

from pathlib import Path
import sys

EDITOR_DIR = Path(__file__).resolve().parents[5]
AITOOL_DIR = EDITOR_DIR / "plugins" / "AITool"
for candidate in (EDITOR_DIR, AITOOL_DIR):
    if str(candidate) not in sys.path:
        sys.path.insert(0, str(candidate))

from plugins.AITool.cai_extensions.agent.scene_composer import (
    SceneComposer,
    _generated_asset_project_root,
)


def test_bedroom_inventory_fallback_uses_room_specific_furniture(monkeypatch) -> None:
    composer = SceneComposer(max_items=6)

    def fail_expand(*_args, **_kwargs):
        raise RuntimeError("offline")

    monkeypatch.setattr(composer, "_llm_expand_inventory", fail_expand)

    items = composer._ensure_minimum_scene_inventory("@长者 帮我生成一个欧式风格的卧室", [])
    names = [str(item.get("name") or "") for item in items]

    assert any("床" in name for name in names)
    assert any("柜" in name for name in names)
    assert "功能支撑物件" not in names
    assert "导视牌" not in names
    assert "储物道具" not in names


def test_generated_asset_root_uses_canonical_project_context(monkeypatch, tmp_path) -> None:
    project_root = tmp_path / "ActiveProject"
    project_root.mkdir()
    monkeypatch.setattr(
        "runtime.project_context.get_project_root",
        lambda: project_root,
    )

    assert _generated_asset_project_root() == project_root

    composer = SceneComposer(max_items=1)
    asset_dir, relative_dir = composer._generated_asset_dir()
    assert asset_dir.parent.parent.parent == project_root / "Resource"
    assert relative_dir.startswith("Resource/generated/scene_composer/")

    source = (
        Path(__file__).resolve().parents[1] / "scene_composer.py"
    ).read_text(encoding="utf-8")
    assert "Path(os.getcwd()).resolve()" not in source
