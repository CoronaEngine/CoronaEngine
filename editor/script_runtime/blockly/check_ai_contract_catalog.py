"""Verify the internal-AI XML contract and its Blockly Catalog.

The checker keeps the XML type surface synchronized with the toolbox and also catches
contract mistakes that can make DeepSeek emit malformed Blockly serialization.
"""

from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

REPO_ROOT = Path(__file__).resolve().parents[3]
TOOLBOX_PATH = REPO_ROOT / "editor" / "Frontend" / "src" / "blockly" / "configs" / "toolboxConfig.js"
CONTRACT_PATH = REPO_ROOT / "docs" / "CoronaBlocksDocument.internal-ai-contract.xml"
BLOCK_CALL_PATTERN = re.compile(r"\bblock\(\s*(['\"])([^'\"]+)\1")
VALID_SHAPES = {"output", "statement", "hat", "definition", "terminal"}
VALID_PROJECT_USAGE = {"project-safe", "actor-context"}
VALID_BOOLEAN_ATTRIBUTES = {"true", "false"}
VALID_FIELD_KINDS = {"number", "string", "dropdown", "field", "boolean"}
VALID_INPUT_KINDS = {"value", "statement"}
REQUIRED_RULE_SECTIONS = {
    "GenerationRules",
    "ProjectObjectBindingRules",
    "NodeLifecycleRootRules",
    "EdgeConditionRules",
    "CapabilityTaxonomy",
    "DynamicActorRelationRules",
    "MacroEdgeSerializationRules",
    "ContractRepairRules",
    "BlockSelectionPriority",
    "SafeCompositionPatterns",
    "GenerationScopingRules",
    "ResponseLanguageRules",
}


def toolbox_block_types(path: str | Path = TOOLBOX_PATH) -> set[str]:
    text = Path(path).read_text(encoding="utf-8")
    return {match.group(2).strip() for match in BLOCK_CALL_PATTERN.finditer(text) if match.group(2).strip()}


def contract_catalog(path: str | Path = CONTRACT_PATH) -> tuple[list[str], int | None]:
    root = ET.parse(Path(path)).getroot()
    if root.tag != "CoronaBlocksDocument":
        raise ValueError("Contract root must be CoronaBlocksDocument")
    catalog = root.find("./Catalog")
    if catalog is None:
        raise ValueError("Contract is missing Catalog")
    declared_raw = str(catalog.get("blockCount") or "").strip()
    declared = int(declared_raw) if declared_raw else None
    block_types = [str(item.get("type") or "").strip() for item in catalog.findall("./Blocks/Block")]
    if any(not item for item in block_types):
        raise ValueError("Catalog contains a Block without type")
    return block_types, declared


def contract_integrity_errors(path: str | Path = CONTRACT_PATH) -> list[str]:
    root = ET.parse(Path(path)).getroot()
    errors: list[str] = []
    if root.tag != "CoronaBlocksDocument":
        return ["Contract root must be CoronaBlocksDocument"]
    if root.get("documentPurpose") != "internal-ai-node-graph-contract":
        errors.append("documentPurpose must be internal-ai-node-graph-contract")

    fixed_target = root.find("./InternalAiContract/FixedTarget")
    if fixed_target is None or fixed_target.get("targetId") != "node_graph:project:global":
        errors.append("FixedTarget.targetId must be node_graph:project:global")
    if fixed_target is None or fixed_target.get("actorBinding") != "none":
        errors.append("FixedTarget.actorBinding must be none for the project-global graph")

    present_sections = {child.tag for child in root}
    missing_sections = sorted(REQUIRED_RULE_SECTIONS - present_sections)
    if missing_sections:
        errors.append("missing generation rule sections: " + ", ".join(missing_sections))

    capability_nodes = root.findall("./CapabilityTaxonomy/Capability")
    capability_names = [str(item.get("name") or "").strip() for item in capability_nodes]
    duplicate_capabilities = sorted(
        name for name, count in Counter(capability_names).items() if name and count > 1
    )
    if not capability_nodes:
        errors.append("CapabilityTaxonomy must declare at least one Capability")
    if any(not name for name in capability_names):
        errors.append("CapabilityTaxonomy Capability name must not be empty")
    if duplicate_capabilities:
        errors.append("duplicate capability names: " + ", ".join(duplicate_capabilities))
    capability_set = set(capability_names)
    for capability in capability_nodes:
        name = str(capability.get("name") or "").strip() or "<missing>"
        if not str(capability.get("keywords") or "").strip():
            errors.append(f"CapabilityTaxonomy {name}: keywords must not be empty")
        if not str(capability.text or "").strip():
            errors.append(f"CapabilityTaxonomy {name}: description must not be empty")

    blocks = root.findall("./Catalog/Blocks/Block")
    block_by_type = {str(block.get("type") or "").strip(): block for block in blocks}
    for block in blocks:
        block_type = str(block.get("type") or "").strip() or "<missing>"
        shape = str(block.get("shape") or "").strip()
        usage = str(block.get("projectUsage") or "").strip()
        recommended = str(block.get("recommended") or "").strip().lower()
        dynamic = str(block.get("dynamic") or "").strip().lower()
        output_check = str(block.get("outputCheck") or "").strip()
        if shape not in VALID_SHAPES:
            errors.append(f"{block_type}: invalid shape {shape!r}")
        if usage not in VALID_PROJECT_USAGE:
            errors.append(f"{block_type}: invalid projectUsage {usage!r}")
        if recommended not in VALID_BOOLEAN_ATTRIBUTES:
            errors.append(f"{block_type}: recommended must be true or false")
        if dynamic not in VALID_BOOLEAN_ATTRIBUTES:
            errors.append(f"{block_type}: dynamic must be true or false")
        if shape != "output" and output_check:
            errors.append(f"{block_type}: only output blocks may declare outputCheck")
        block_capabilities = str(block.get("capabilities") or "").split()
        unknown_capabilities = sorted(set(block_capabilities) - capability_set)
        if unknown_capabilities:
            errors.append(
                f"{block_type}: unknown capabilities: {', '.join(unknown_capabilities)}"
            )
        if len(block_capabilities) != len(set(block_capabilities)):
            errors.append(f"{block_type}: duplicate capabilities")
        if block.get("aiUse") is not None and not str(block.get("aiUse") or "").strip():
            errors.append(f"{block_type}: aiUse must not be empty when present")

        field_names = [str(item.get("name") or "").strip() for item in block.findall("./Field")]
        input_names = [str(item.get("name") or "").strip() for item in block.findall("./Input")]
        duplicate_fields = sorted(name for name, count in Counter(field_names).items() if name and count > 1)
        duplicate_inputs = sorted(name for name, count in Counter(input_names).items() if name and count > 1)
        if any(not name for name in field_names):
            errors.append(f"{block_type}: Field name must not be empty")
        if any(not name for name in input_names):
            errors.append(f"{block_type}: Input name must not be empty")
        if duplicate_fields:
            errors.append(f"{block_type}: duplicate Field names: {', '.join(duplicate_fields)}")
        if duplicate_inputs:
            errors.append(f"{block_type}: duplicate Input names: {', '.join(duplicate_inputs)}")

        for field in block.findall("./Field"):
            field_name = str(field.get("name") or "").strip() or "<missing>"
            kind = str(field.get("kind") or "").strip()
            if kind not in VALID_FIELD_KINDS:
                errors.append(f"{block_type}.{field_name}: invalid field kind {kind!r}")
            default_json = field.get("defaultJson")
            if default_json is not None:
                try:
                    json.loads(default_json)
                except (TypeError, ValueError):
                    errors.append(f"{block_type}.{field_name}: defaultJson is not valid JSON")
            if kind == "dropdown":
                options = str(field.get("optionValues") or "").strip()
                dynamic_count = str(field.get("dynamicOptionCount") or "").strip()
                if not options and dynamic != "true" and not dynamic_count:
                    errors.append(
                        f"{block_type}.{field_name}: dropdown needs optionValues, "
                        "dynamic=true, or dynamicOptionCount"
                    )
                values = options.split("|") if options else []
                labels_raw = field.get("optionLabels")
                labels = str(labels_raw).split("|") if labels_raw is not None else []
                if values and labels and len(values) != len(labels):
                    errors.append(f"{block_type}.{field_name}: optionValues/optionLabels count mismatch")

        for input_node in block.findall("./Input"):
            input_name = str(input_node.get("name") or "").strip() or "<missing>"
            kind = str(input_node.get("kind") or "").strip()
            if kind not in VALID_INPUT_KINDS:
                errors.append(f"{block_type}.{input_name}: invalid input kind {kind!r}")

    roots_node = root.find("./Catalog/GlobalWorkspaceRootTypes")
    roots = str(roots_node.get("values") or "").split() if roots_node is not None else []
    for block_type in roots:
        block = block_by_type.get(block_type)
        if block is None:
            errors.append(f"GlobalWorkspaceRootTypes references missing block: {block_type}")
        elif block.get("projectUsage") != "project-safe":
            errors.append(f"Global workspace root must be project-safe: {block_type}")
        elif block.get("shape") not in {"statement", "definition"}:
            errors.append(f"Global workspace root has invalid shape: {block_type}")
    return errors


def check_contract_catalog(
    toolbox_path: str | Path = TOOLBOX_PATH,
    contract_path: str | Path = CONTRACT_PATH,
) -> dict[str, object]:
    toolbox_types = toolbox_block_types(toolbox_path)
    catalog_items, declared_count = contract_catalog(contract_path)
    catalog_types = set(catalog_items)
    duplicates = sorted(item for item, count in Counter(catalog_items).items() if count > 1)
    missing = sorted(toolbox_types - catalog_types)
    extra = sorted(catalog_types - toolbox_types)
    integrity_errors = contract_integrity_errors(contract_path)
    errors: list[str] = list(integrity_errors)
    if duplicates:
        errors.append("duplicate XML block types: " + ", ".join(duplicates))
    if missing:
        errors.append("missing XML block types: " + ", ".join(missing))
    if extra:
        errors.append("extra XML block types: " + ", ".join(extra))
    if declared_count != len(catalog_items):
        errors.append(
            f"Catalog blockCount={declared_count!r}, but XML contains {len(catalog_items)} Block entries"
        )
    if declared_count != len(toolbox_types):
        errors.append(
            f"Catalog blockCount={declared_count!r}, but toolbox exposes {len(toolbox_types)} unique block types"
        )
    return {
        "success": not errors,
        "errors": errors,
        "toolboxCount": len(toolbox_types),
        "catalogCount": len(catalog_items),
        "declaredCount": declared_count,
        "missing": missing,
        "extra": extra,
        "duplicates": duplicates,
        "integrityErrors": integrity_errors,
    }


def main() -> int:
    result = check_contract_catalog()
    if result["success"]:
        print(
            "AI contract catalog is synchronized: "
            f"{result['catalogCount']} XML blocks / {result['toolboxCount']} toolbox types."
        )
        return 0
    for error in result["errors"]:
        print(f"ERROR: {error}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
