"""Keep the Web scene surface consolidated without widening hot-path reads."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def test_scene_v8_has_only_the_consolidated_names():
    source = (ROOT / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    names = re.findall(r'scene->SetValue\("([^"]+)", CefV8Value::CreateFunction', source)
    assert len(names) == 27
    registered = re.findall(r'(\w+)->SetValue\("[^"]+", CefV8Value::CreateFunction', source)
    assert len(registered) == 86
    assert len(set(registered)) == 13
    for name in ('readScene', 'updateActor', 'updateCamera', 'readActorRig',
                 'readActive', 'readTransforms', 'setActorTransform', 'setActorIkChains'):
        assert name in names
    for name in ('readTree', 'readActorIndex', 'readActorTree', 'readCameras',
                 'setActorProperties', 'renameActor', 'setCameraProperties', 'renameCamera',
                 'readActorSkeleton', 'readActorIkChains'):
        assert name not in names


def test_scene_query_uses_explicit_owned_projection():
    source = (ROOT / 'src/systems/ui/cef/cef_scene_operations.h').read_text(encoding='utf-8-sig')
    assert 'enum class ActorProjection' in source
    assert 'ActorProjection actors' in source
    assert 'bool cameras = false' in source
    binding = (ROOT / 'src/systems/ui/cef/cef_v8_binding.cpp').read_text(encoding='utf-8-sig')
    assert 'bool read_scene_query(' in binding
    assert 'scene_query_value(' in binding
