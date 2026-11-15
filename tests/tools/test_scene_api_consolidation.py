"""Keep the Web scene surface consolidated without widening hot-path reads.

The scene names now live in `editor_operation_table.h`; this guard pins which of them
are still published and which retired spellings must not come back.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from native_surface import camel_case

ROOT = Path(__file__).resolve().parents[2]
TABLE = ROOT / "src/systems/ui/cef/editor_operation_table.h"

TABLE_ROW = re.compile(
    r'\{"(\w+)",\s*"(\w+)",\s*(SceneMethod::\w+|std::nullopt),\s*'
    r'(true|false),\s*(true|false),\s*(\d+),'
)


def _web_scene_names() -> set[str]:
    text = TABLE.read_text(encoding="utf-8-sig")
    start = text.index("kEditorOperations[] = {")
    end = text.index("};", start)
    names = set()
    for group, name, _method, web, _script, _arity in TABLE_ROW.findall(text[start:end]):
        if web == "true" and group == "scene":
            names.add(camel_case(name))
    return names


def test_scene_v8_has_only_the_consolidated_names():
    names = _web_scene_names()
    assert len(names) == 25, f"expected 25 published scene operations, found {len(names)}"
    for name in ('readScene', 'updateActor', 'updateCamera', 'readActorRig',
                 'readActive', 'readTransforms', 'setActorTransform', 'setActorIkChains'):
        assert name in names
    for name in ('readTree', 'readActorIndex', 'readActorTree', 'readCameras',
                 'setActorProperties', 'renameActor', 'setCameraProperties', 'renameCamera',
                 'readActorSkeleton', 'readActorIkChains'):
        assert name not in names


def test_scene_query_uses_explicit_owned_projection():
    source = (ROOT / "src/systems/ui/cef/cef_scene_operations.h").read_text(encoding="utf-8-sig")
    assert 'enum class ActorProjection' in source
    assert 'ActorProjection actors' in source
    assert 'bool cameras = false' in source
    binding = (ROOT / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8-sig")
    assert 'bool read_scene_query(' in binding
    assert 'scene_query_value(' in binding
