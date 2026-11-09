"""Guard the source-of-truth and the production Web/Python split."""
import json
from pathlib import Path
import xml.etree.ElementTree as ET

EDITOR = Path(__file__).resolve().parents[3]
WEB = EDITOR / "Frontend/src/services"


def test_web_catalog_matches_the_canonical_xml():
    policy = json.loads((WEB / "aiPolicy.json").read_text(encoding="utf-8"))
    root = ET.parse(WEB / "aiBlockContract.xml").getroot()
    blocks = root.findall("./Catalog/Blocks/Block")
    assert len(blocks) == len(policy["catalog"]) == 259
    for block in blocks:
        spec = policy["catalog"][block.attrib["type"]]
        assert spec["projectUsage"] == block.attrib["projectUsage"]
        assert spec["shape"] == block.attrib["shape"]
        assert set(spec["fieldSpecs"]) == {field.attrib["name"] for field in block.findall("Field")}
        assert set(spec["inputSpecs"]) == {field.attrib["name"] for field in block.findall("Input")}
        assert spec["capabilities"] == block.attrib.get("capabilities", "").split()


def test_ai_is_web_owned_without_native_or_python_ai_routes():
    assert not (EDITOR / "plugins/AITool/main.py").exists()
    registry = (EDITOR / "runtime/registry.py").read_text(encoding="utf-8")
    assert "AITool" not in registry
    transport = (WEB / "aiModelService.js").read_text(encoding="utf-8")
    assert "editorApi" not in transport
    assert "initializeAiSession" in transport
    source = (EDITOR / "Frontend/src/views/sidebar/EditorSettings.vue").read_text(encoding="utf-8")
    assert "aiSettings.saveApiKey" in source
    assert "ai.settings." not in source
