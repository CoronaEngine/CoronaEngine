import base64
import sys
from pathlib import Path
from unittest.mock import patch


EDITOR_ROOT = Path(__file__).resolve().parents[3]
AI_TOOL_ROOT = EDITOR_ROOT / "plugins" / "AITool"
QUASAR_ROOT = AI_TOOL_ROOT / "Quasar"
for path in (EDITOR_ROOT, AI_TOOL_ROOT, QUASAR_ROOT):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))


def test_default_base64_media_file_uses_project_media_directory(tmp_path, monkeypatch):
    from plugins.AITool.services import media_storage

    (tmp_path / "cwd").mkdir()
    monkeypatch.chdir(tmp_path / "cwd")
    project_media = tmp_path / "project" / "media"

    with patch.object(media_storage, "get_project_media_dir", return_value=project_media):
        result = media_storage.base64_to_image_file(
            "data:image/png;base64," + base64.b64encode(b"png").decode("ascii")
        )

    assert Path(result).parent == project_media
    assert Path(result).is_file()
    assert not (tmp_path / "cwd" / "uploads").exists()


def test_explicit_media_output_path_remains_authoritative(tmp_path):
    from plugins.AITool.services import media_storage

    output = tmp_path / "explicit" / "input.png"
    result = media_storage.base64_to_image_file(
        base64.b64encode(b"png").decode("ascii"), str(output)
    )

    assert Path(result) == output
    assert output.is_file()
