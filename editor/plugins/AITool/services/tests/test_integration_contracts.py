from __future__ import annotations

import ast
from dataclasses import dataclass
from pathlib import Path
import unittest
from typing import Protocol

import editor.plugins.AITool.services.integration_contracts as integration_contracts_module
from editor.plugins.AITool.services.integration_contracts import (
    BlockedResult,
    InterfaceChangeDecision,
    InterfaceChangeRequest,
    MissingRequirement,
    PublicEnumManifest,
    PublicProtocolManifest,
    SkeletonContractManifest,
    SkeletonNodeStatus,
    SkeletonStatusReport,
    dto_manifest,
    protocol_manifest,
)
from editor.plugins.AITool.services.schema_versions import SKELETON_CONTRACT_VERSION


class ExamplePort(Protocol):
    def read(self, key: str, limit: int = 1) -> dict[str, str]: ...


class ExamplePortWithPrivateChange(Protocol):
    def read(self, key: str, limit: int = 1) -> dict[str, str]: ...

    def _private_helper(self) -> None: ...


class ExamplePortWithSignatureChange(Protocol):
    def read(self, key: str, limit: int = 2) -> dict[str, str]: ...


@dataclass(frozen=True)
class ExampleDto:
    key: str
    count: int = 1


class IntegrationContractTests(unittest.TestCase):
    @staticmethod
    def _requirement() -> MissingRequirement:
        return MissingRequirement(
            requirement_id="engine.capability_manifest",
            owner_domain="engine",
            description="Engine capability manifest is unavailable.",
        )

    @staticmethod
    def _manifest(protocol=ExamplePort) -> SkeletonContractManifest:
        protocol_shape = protocol_manifest(protocol)
        return SkeletonContractManifest(
            contract_version=SKELETON_CONTRACT_VERSION,
            schema_versions=(("skeleton", SKELETON_CONTRACT_VERSION),),
            public_protocols=(PublicProtocolManifest("ExamplePort", protocol_shape.methods),),
            public_dtos=(dto_manifest(ExampleDto),),
            public_enums=(PublicEnumManifest("Status", ("completed", "blocked")),),
            skeleton_nodes=(("source_node", "ExamplePort.read"), ("result_node", "ExampleDto")),
            skeleton_edges=(("source_node", "result_node"),),
        )

    def test_missing_requirement_rejects_invalid_identifier_and_owner(self) -> None:
        with self.assertRaises(ValueError):
            MissingRequirement("Needs Engine", "engine", "missing")
        with self.assertRaises(ValueError):
            MissingRequirement("engine.capability", "unknown", "missing")
        with self.assertRaises(ValueError):
            MissingRequirement("engine.capability", "engine", "")

    def test_integration_contract_module_only_imports_stdlib_and_schema_versions(self) -> None:
        path = Path(integration_contracts_module.__file__)
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        allowed = {"__future__", "dataclasses", "datetime", "hashlib", "inspect", "json", "re", "typing", "schema_versions"}
        imported: set[str] = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported.update(alias.name.split(".", 1)[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom):
                imported.add(str(node.module or "").split(".", 1)[0])
        self.assertEqual(imported - allowed, set())

    def test_blocked_result_requires_structured_requirement_and_action(self) -> None:
        with self.assertRaises(ValueError):
            BlockedResult(
                node_id="engine_capability_port",
                status="pending_runtime_verification",
                error_code="engine.capability_unavailable",
                summary="Engine is unavailable.",
                missing_requirements=(),
                owner_domain="engine",
                retryable=True,
                next_action="Retry after Engine integration.",
                evidence_refs=(),
            )

    def test_blocked_result_rejects_omitted_required_field(self) -> None:
        with self.assertRaises(TypeError):
            BlockedResult(  # type: ignore[call-arg]
                node_id="engine_capability_port",
                status="blocked",
                error_code="engine.capability_unavailable",
                summary="Engine is unavailable.",
                missing_requirements=(self._requirement(),),
                owner_domain="engine",
                retryable=True,
                evidence_refs=(),
            )
        with self.assertRaises(ValueError):
            BlockedResult(
                node_id="engine_capability_port",
                status="blocked",
                error_code="engine.capability_unavailable",
                summary="Engine is unavailable.",
                missing_requirements=(self._requirement(),),
                owner_domain="engine",
                retryable=True,
                next_action="",
                evidence_refs=(),
            )

    def test_node_status_rejects_invalid_status_and_priority(self) -> None:
        with self.assertRaises(ValueError):
            SkeletonNodeStatus("source_node", "ExamplePort.read", "unknown", "", "integration", 1, ())
        with self.assertRaises(ValueError):
            SkeletonNodeStatus("source_node", "ExamplePort.read", "completed", "", "integration", 0, ())

    def test_manifest_hash_is_deterministic_and_signature_sensitive(self) -> None:
        manifest = self._manifest()
        self.assertEqual(manifest.contract_hash(), self._manifest().contract_hash())
        changed = self._manifest(ExamplePortWithSignatureChange)
        self.assertNotEqual(manifest.contract_hash(), changed.contract_hash())

    def test_private_protocol_change_does_not_change_public_method_shape(self) -> None:
        self.assertEqual(
            self._manifest(ExamplePort).contract_hash(),
            self._manifest(ExamplePortWithPrivateChange).contract_hash(),
        )

    def test_report_uses_fixed_utc_clock_without_affecting_contract_hash(self) -> None:
        contract_hash = self._manifest().contract_hash()
        node = SkeletonNodeStatus(
            "source_node",
            "ExamplePort.read",
            "completed",
            "",
            "integration",
            1,
            ("test:source",),
        )
        first = SkeletonStatusReport(
            SKELETON_CONTRACT_VERSION,
            contract_hash,
            (node,),
            "completed",
            "2026-07-18T00:00:00Z",
        )
        second = SkeletonStatusReport(
            SKELETON_CONTRACT_VERSION,
            contract_hash,
            (node,),
            "completed",
            "2026-07-18T00:01:00Z",
        )
        self.assertEqual(first.contract_hash, second.contract_hash)

    def test_accepted_interface_change_requires_revalidation(self) -> None:
        with self.assertRaises(ValueError):
            InterfaceChangeDecision(
                decision="accepted",
                reason="Public method must change.",
                changed_interfaces=(),
                new_contract_version="r3-skeleton-week1-v2",
                new_contract_hash=self._manifest().contract_hash(),
                affected_nodes=("source_node",),
                required_revalidation=(),
                evidence_refs=("test:change",),
            )

    def test_interface_change_request_preserves_contract_identity(self) -> None:
        contract_hash = self._manifest().contract_hash()
        request = InterfaceChangeRequest(
            request_id="interface.change-001",
            node_id="source_node",
            detected_by_task_id="b1.node-fill",
            current_contract_version=SKELETON_CONTRACT_VERSION,
            current_contract_hash=contract_hash,
            reason_code="interface.signature_insufficient",
            required_change="Add an explicit capability result.",
            affected_interfaces=("ExamplePort.read",),
            blocked_dependents=("result_node",),
            evidence_refs=("test:interface-change",),
        )
        self.assertEqual(request.current_contract_hash, contract_hash)


if __name__ == "__main__":
    unittest.main()
