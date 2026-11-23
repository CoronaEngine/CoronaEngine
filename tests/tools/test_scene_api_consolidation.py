"""Keep the Web scene query an explicitly owned projection."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parents[2]


def test_scene_query_uses_explicit_owned_projection():
    source = (ROOT / "src/systems/ui/cef/cef_scene_operations.h").read_text(encoding="utf-8-sig")
    assert 'enum class ActorProjection' in source
    assert 'ActorProjection actors' in source
    assert 'bool cameras = false' in source
    binding = (ROOT / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8-sig")
    assert 'bool read_scene_query(' in binding
    assert 'scene_query_value(' in binding
