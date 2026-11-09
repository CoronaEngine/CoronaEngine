"""Development-only reader for checking the Web AI block catalog."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any
import xml.etree.ElementTree as ET

SCHEMA_VERSION = 1
TARGET_ID = "node_graph:project:global"
VALID_NODE_TYPES = {"start", "custom", "end"}
VALID_PORT_SIDES = {"left", "right", "bottom"}
_ROOT = Path(__file__).resolve().parents[2]
_FRONTEND_ROOT = (_ROOT if (_ROOT / "src/services/aiBlockContract.xml").is_file()
                  else _ROOT / "CoronaEditor" / "editor" / "Frontend")
CONTRACT_PATH = _FRONTEND_ROOT / "src/services/aiBlockContract.xml"


@dataclass(frozen=True)
class BlockSpec:
    type: str
    shape: str
    output_check: tuple[str, ...]
    fields: dict[str, dict[str, Any]]
    inputs: dict[str, dict[str, Any]]
    dynamic: bool = False
    project_usage: str = ""
    capabilities: tuple[str, ...] = ()
    ai_use: str = ""


def _checks(value: str | None) -> tuple[str, ...]:
    return tuple(item for item in str(value or "").replace(",", " ").split() if item)


def load_contract_catalog(path: str | Path | None = None) -> dict[str, Any]:
    """Load the read-only block catalog embedded in the internal AI XML contract."""
    contract_path = Path(path) if path is not None else CONTRACT_PATH
    root = ET.parse(contract_path).getroot()
    if root.tag != "CoronaBlocksDocument":
        raise ValueError("Internal AI contract root must be CoronaBlocksDocument")
    if root.get("documentPurpose") != "internal-ai-node-graph-contract":
        raise ValueError("The configured XML is not the internal AI node-graph contract")

    block_specs: dict[str, BlockSpec] = {}
    for block in root.findall("./Catalog/Blocks/Block"):
        block_type = str(block.get("type") or "").strip()
        if not block_type:
            raise ValueError("Catalog contains a block without a type")
        if block_type in block_specs:
            raise ValueError(f"Catalog contains duplicate block type: {block_type}")

        fields: dict[str, dict[str, Any]] = {}
        for field in block.findall("./Field"):
            name = str(field.get("name") or "").strip()
            if not name:
                continue
            fields[name] = {
                "kind": str(field.get("kind") or ""),
                "option_values": tuple(str(field.get("optionValues") or "").split("|"))
                if field.get("optionValues") is not None
                else (),
            }

        inputs: dict[str, dict[str, Any]] = {}
        for input_node in block.findall("./Input"):
            name = str(input_node.get("name") or "").strip()
            if not name:
                continue
            inputs[name] = {
                "kind": str(input_node.get("kind") or ""),
                "check": _checks(input_node.get("check")),
            }

        block_specs[block_type] = BlockSpec(
            type=block_type,
            shape=str(block.get("shape") or ""),
            output_check=_checks(block.get("outputCheck")),
            fields=fields,
            inputs=inputs,
            dynamic=str(block.get("dynamic") or "false").lower() == "true",
            project_usage=str(block.get("projectUsage") or ""),
            capabilities=_checks(block.get("capabilities")),
            ai_use=str(block.get("aiUse") or "").strip(),
        )

    node_types = {
        str(item.get("type") or "").strip()
        for item in root.findall("./Catalog/NodeTypes/NodeType")
        if str(item.get("type") or "").strip()
    }
    roots_raw = root.find("./Catalog/GlobalWorkspaceRootTypes")
    global_roots = set(str(roots_raw.get("values") or "").split()) if roots_raw is not None else set()
    return {
        "blocks": block_specs,
        "node_types": node_types or set(VALID_NODE_TYPES),
        "global_workspace_root_types": global_roots,
    }
