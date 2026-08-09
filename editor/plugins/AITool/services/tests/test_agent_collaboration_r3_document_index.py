from __future__ import annotations

from pathlib import Path
import unittest

from editor.plugins.AITool.services.agent_collaboration.r3_document_index import (
    R3_AGENT_CONSTRAINT_LOOP,
    R3_AUTHORITATIVE_DOCUMENTS,
    R3_STABILITY_GATE_PLAN,
    get_r3_authoritative_document,
)


REPOSITORY_ROOT = Path(__file__).resolve().parents[5]


class R3DocumentIndexTests(unittest.TestCase):
    def test_authoritative_documents_exist_and_contain_indexed_sections(self) -> None:
        for entry in R3_AUTHORITATIVE_DOCUMENTS.values():
            path = REPOSITORY_ROOT / entry.repository_path
            self.assertTrue(path.is_file(), entry.repository_path)
            content = path.read_text(encoding="utf-8")
            self.assertIn(f"# {entry.title}", content)
            for section in entry.major_sections:
                self.assertIn(section, content)

    def test_document_lookup_is_deterministic_and_rejects_unknown_ids(self) -> None:
        self.assertIs(
            get_r3_authoritative_document("r3_stability_gate_plan"),
            R3_STABILITY_GATE_PLAN,
        )
        self.assertIs(
            get_r3_authoritative_document("r3_agent_constraint_loop"),
            R3_AGENT_CONSTRAINT_LOOP,
        )
        with self.assertRaises(ValueError):
            get_r3_authoritative_document("")
        with self.assertRaises(KeyError):
            get_r3_authoritative_document("missing")


if __name__ == "__main__":
    unittest.main()
