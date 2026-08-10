from __future__ import annotations

import pathlib
import tempfile
import unittest

from script_runtime.blockly.ai_node_graph_contract import load_contract_catalog
from script_runtime.blockly.check_ai_contract_catalog import (
    CONTRACT_PATH,
    TOOLBOX_PATH,
    check_contract_catalog,
    contract_integrity_errors,
)


class AiContractCatalogTests(unittest.TestCase):
    def test_current_contract_matches_current_toolbox(self):
        result = check_contract_catalog(TOOLBOX_PATH, CONTRACT_PATH)
        self.assertTrue(result["success"], result["errors"])
        self.assertEqual(262, result["toolboxCount"])
        self.assertEqual(result["toolboxCount"], result["catalogCount"])
        self.assertEqual(result["catalogCount"], result["declaredCount"])

    def test_current_contract_exposes_machine_readable_capabilities_and_usage_hints(self):
        catalog = load_contract_catalog(CONTRACT_PATH)
        follow = catalog["blocks"]["camera_follow_object"]
        near = catalog["blocks"]["detect_position_near"]
        self.assertIn("camera-follow", follow.capabilities)
        self.assertIn("exact real actor", follow.ai_use)
        self.assertIn("distance-check", near.capabilities)
        self.assertIn("live position", near.ai_use)

    def test_checker_reports_missing_extra_duplicate_and_bad_count(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            toolbox = root / "toolboxConfig.js"
            contract = root / "contract.xml"
            toolbox.write_text(
                "const x = [block('alpha'), block(\"beta\"), block('alpha')];",
                encoding="utf-8",
            )
            contract.write_text(
                "<CoronaBlocksDocument><Catalog blockCount=\"4\"><Blocks>"
                "<Block type=\"alpha\"/><Block type=\"alpha\"/><Block type=\"extra\"/>"
                "</Blocks></Catalog></CoronaBlocksDocument>",
                encoding="utf-8",
            )
            result = check_contract_catalog(toolbox, contract)
        self.assertFalse(result["success"])
        self.assertEqual(["beta"], result["missing"])
        self.assertEqual(["extra"], result["extra"])
        self.assertEqual(["alpha"], result["duplicates"])
        self.assertIn("blockCount=4", " ".join(result["errors"]))

    def test_checker_reports_invalid_catalog_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            contract = pathlib.Path(temporary) / "contract.xml"
            contract.write_text(
                "<CoronaBlocksDocument documentPurpose=\"internal-ai-node-graph-contract\">"
                "<InternalAiContract><FixedTarget targetId=\"wrong\" actorBinding=\"actor\"/>"
                "</InternalAiContract>"
                "<CapabilityTaxonomy><Capability name=\"known\" keywords=\"known\">Known.</Capability>"
                "</CapabilityTaxonomy><Catalog blockCount=\"1\"><Blocks>"
                "<Block type=\"alpha\" shape=\"bad\" projectUsage=\"bad\" "
                "recommended=\"maybe\" dynamic=\"false\" outputCheck=\"Number\" "
                "capabilities=\"unknown\" aiUse=\"\">"
                "<Field name=\"MODE\" kind=\"dropdown\" defaultJson=\"not-json\"/>"
                "<Field name=\"MODE\" kind=\"dropdown\" defaultJson=\"0\"/>"
                "<Input name=\"VALUE\" kind=\"bad\"/>"
                "</Block></Blocks><GlobalWorkspaceRootTypes values=\"missing\"/>"
                "</Catalog></CoronaBlocksDocument>",
                encoding="utf-8",
            )
            errors = contract_integrity_errors(contract)
        combined = " ".join(errors)
        self.assertIn("FixedTarget.targetId", combined)
        self.assertIn("missing generation rule sections", combined)
        self.assertIn("invalid shape", combined)
        self.assertIn("invalid projectUsage", combined)
        self.assertIn("duplicate Field names", combined)
        self.assertIn("defaultJson is not valid JSON", combined)
        self.assertIn("dropdown needs optionValues", combined)
        self.assertIn("invalid input kind", combined)
        self.assertIn("unknown capabilities", combined)
        self.assertIn("aiUse must not be empty", combined)
        self.assertIn("GlobalWorkspaceRootTypes references missing block", combined)


if __name__ == "__main__":
    unittest.main()
