"""Validation helpers for node graphs produced by CoronaEngine's future internal AI.

This module deliberately accepts Python dictionaries only.  It does not read AI result
files, parse result XML, write Blockly targets, or update the project manifest.  The XML
under docs is a read-only capability catalog used to validate the JSON contract here.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import math
from pathlib import Path
from typing import Any
import xml.etree.ElementTree as ET

SCHEMA_VERSION = 1
TARGET_ID = "node_graph:project:global"
VALID_NODE_TYPES = {"start", "custom", "end"}
VALID_PORT_SIDES = {"left", "right", "bottom"}
CONTRACT_PATH = (
    Path(__file__).resolve().parents[3]
    / "docs"
    / "CoronaBlocksDocument.internal-ai-contract.xml"
)


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


def _workspace_roots(state: Any) -> list[Any]:
    if not isinstance(state, dict):
        return []
    blocks_container = state.get("blocks")
    if blocks_container is None:
        return []
    if not isinstance(blocks_container, dict):
        return []
    roots = blocks_container.get("blocks", [])
    return roots if isinstance(roots, list) else []


def _connection_child(connection: Any) -> list[Any]:
    if not isinstance(connection, dict):
        return []
    children = []
    for key in ("block", "shadow"):
        child = connection.get(key)
        if isinstance(child, dict):
            children.append(child)
    return children


def _is_finite_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def validate_generated_node_graph(payload: Any, *, catalog_path: str | Path | None = None) -> dict[str, Any]:
    """Validate an internal-AI JSON result without mutating project state.

    The return value is intentionally structured for a future AITool caller.  No
    exception caused by model output escapes this function; contract-loading errors are
    also reported as validation errors.
    """
    errors: list[str] = []
    warnings: list[str] = []
    block_count = 0

    if not isinstance(payload, dict):
        return {
            "success": False,
            "errors": ["AI result must be a JSON object, not XML text or a file path"],
            "warnings": [],
            "summary": {"nodeCount": 0, "edgeCount": 0, "blockCount": 0},
        }

    if payload.get("schemaVersion") != SCHEMA_VERSION or isinstance(payload.get("schemaVersion"), bool):
        errors.append(f"schemaVersion must be {SCHEMA_VERSION}")
    if payload.get("targetId") != TARGET_ID:
        errors.append(f"targetId must be {TARGET_ID}")

    workspace = payload.get("workspace")
    if not isinstance(workspace, dict):
        errors.append("workspace must be an object")
        workspace = {}
    if workspace.get("version") != 1 or isinstance(workspace.get("version"), bool):
        errors.append("workspace.version must be 1")

    nodes = workspace.get("nodes")
    edges = workspace.get("edges")
    if "globalVariablesWorkspace" not in workspace:
        errors.append("workspace.globalVariablesWorkspace is required")
    globals_workspace = workspace.get("globalVariablesWorkspace", {})
    if not isinstance(nodes, list):
        errors.append("workspace.nodes must be an array")
        nodes = []
    if not isinstance(edges, list):
        errors.append("workspace.edges must be an array")
        edges = []
    if not isinstance(globals_workspace, dict):
        errors.append("workspace.globalVariablesWorkspace must be an object")
        globals_workspace = {}

    try:
        catalog = load_contract_catalog(catalog_path)
    except Exception as exc:
        errors.append(f"Unable to load internal AI block catalog: {exc}")
        catalog = {"blocks": {}, "node_types": set(VALID_NODE_TYPES), "global_workspace_root_types": set()}
    block_specs: dict[str, BlockSpec] = catalog["blocks"]

    def validate_block(block: Any, scope: str, ids: set[str], trail: str) -> BlockSpec | None:
        nonlocal block_count
        if not isinstance(block, dict):
            errors.append(f"{trail} must be a serialized Blockly block object")
            return None
        block_count += 1
        block_id = str(block.get("id") or "").strip()
        if not block_id:
            errors.append(f"{trail} is missing a block id")
        elif block_id in ids:
            errors.append(f"{scope} contains duplicate block id: {block_id}")
        else:
            ids.add(block_id)

        block_type = str(block.get("type") or "").strip()
        spec = block_specs.get(block_type)
        if not block_type:
            errors.append(f"{trail} is missing block type")
        elif spec is None:
            errors.append(f"{trail} uses unknown block type: {block_type}")
        elif spec.project_usage != "project-safe":
            errors.append(f"{trail} uses block type {block_type}, which requires actor context")

        fields = block.get("fields", {})
        if fields is not None and not isinstance(fields, dict):
            errors.append(f"{trail}.fields must be an object")
            fields = {}
        if spec is not None:
            for field_name, value in fields.items():
                field_spec = spec.fields.get(str(field_name))
                if field_spec is None and not spec.dynamic:
                    errors.append(f"{trail} uses unknown field {field_name!s} for {block_type}")
                    continue
                options = field_spec.get("option_values", ()) if field_spec else ()
                if options and str(value) not in options:
                    errors.append(
                        f"{trail}.{field_name} value {value!r} is not in Catalog options: {'|'.join(options)}"
                    )

        inputs = block.get("inputs", {})
        if inputs is not None and not isinstance(inputs, dict):
            errors.append(f"{trail}.inputs must be an object")
            inputs = {}
        for input_name, connection in inputs.items():
            input_spec = spec.inputs.get(str(input_name)) if spec is not None else None
            if input_spec is None and spec is not None and not spec.dynamic:
                errors.append(f"{trail} uses unknown input {input_name!s} for {block_type}")
            children = _connection_child(connection)
            if not children:
                errors.append(f"{trail}.inputs.{input_name} must contain block or shadow")
                continue
            for child_index, child in enumerate(children):
                child_spec = validate_block(child, scope, ids, f"{trail}.inputs.{input_name}[{child_index}]")
                if input_spec is None or child_spec is None:
                    continue
                kind = input_spec.get("kind")
                if kind == "value" and child_spec.shape != "output":
                    errors.append(f"{trail}.inputs.{input_name} requires an output block")
                if kind == "statement" and child_spec.shape == "output":
                    errors.append(f"{trail}.inputs.{input_name} requires a statement block")
                expected = tuple(input_spec.get("check") or ())
                actual = child_spec.output_check
                if kind == "value" and expected and actual and not set(expected).intersection(actual):
                    errors.append(
                        f"{trail}.inputs.{input_name} expects {'/'.join(expected)}, got {'/'.join(actual)}"
                    )

        next_connection = block.get("next")
        if next_connection is not None:
            children = _connection_child(next_connection)
            if len(children) != 1:
                errors.append(f"{trail}.next must contain exactly one block")
            for child_index, child in enumerate(children):
                child_spec = validate_block(child, scope, ids, f"{trail}.next[{child_index}]")
                if child_spec is not None and child_spec.shape == "output":
                    errors.append(f"{trail}.next cannot connect an output block")
        return spec

    def validate_workspace(state: Any, scope: str, *, condition: bool = False, global_pool: bool = False) -> None:
        if not isinstance(state, dict):
            errors.append(f"{scope} workspace must be an object")
            return
        blocks_container = state.get("blocks")
        if blocks_container is not None and not isinstance(blocks_container, dict):
            errors.append(f"{scope}.blocks must be an object")
            return
        if isinstance(blocks_container, dict) and not isinstance(blocks_container.get("blocks", []), list):
            errors.append(f"{scope}.blocks.blocks must be an array")
            return
        roots = _workspace_roots(state)
        ids: set[str] = set()
        root_specs: list[BlockSpec | None] = []
        for index, root in enumerate(roots):
            root_specs.append(validate_block(root, scope, ids, f"{scope}.blocks[{index}]"))
        if condition:
            if len(roots) != 1:
                errors.append(f"{scope} must contain exactly one visible top-level Boolean block")
            elif root_specs[0] is not None:
                spec = root_specs[0]
                if spec.shape != "output" or "Boolean" not in spec.output_check:
                    errors.append(f"{scope} top-level block must have Boolean output")
        if global_pool:
            allowed_roots = catalog["global_workspace_root_types"]
            for index, spec in enumerate(root_specs):
                if spec is not None and spec.type not in allowed_roots:
                    errors.append(f"{scope}.blocks[{index}] type {spec.type} is not allowed as a global pool root")

    node_ids: set[str] = set()
    start_ids: list[str] = []
    for index, node in enumerate(nodes):
        trail = f"workspace.nodes[{index}]"
        if not isinstance(node, dict):
            errors.append(f"{trail} must be an object")
            continue
        node_id = str(node.get("id") or "").strip()
        if not node_id:
            errors.append(f"{trail} is missing id")
        elif node_id in node_ids:
            errors.append(f"Duplicate node id: {node_id}")
        else:
            node_ids.add(node_id)
        node_type = str(node.get("nodeType") or "")
        if node_type not in catalog["node_types"]:
            errors.append(f"{trail}.nodeType is invalid: {node_type or '<empty>'}")
        if node_type == "start" and node_id:
            start_ids.append(node_id)
        if not _is_finite_number(node.get("x")) or not _is_finite_number(node.get("y")):
            errors.append(f"{trail} x/y must be finite numbers")
        validate_workspace(node.get("workspace", {}), f"node {node_id or index}")

    if len(start_ids) != 1:
        errors.append("Node graph must contain exactly one start node")

    edge_ids: set[str] = set()
    adjacency: dict[str, list[str]] = {node_id: [] for node_id in node_ids}
    node_by_id = {
        str(node.get("id")): node
        for node in nodes
        if isinstance(node, dict) and str(node.get("id") or "") in node_ids
    }
    for index, edge in enumerate(edges):
        trail = f"workspace.edges[{index}]"
        if not isinstance(edge, dict):
            errors.append(f"{trail} must be an object")
            continue
        edge_id = str(edge.get("id") or "").strip()
        if not edge_id:
            errors.append(f"{trail} is missing id")
        elif edge_id in edge_ids:
            errors.append(f"Duplicate edge id: {edge_id}")
        else:
            edge_ids.add(edge_id)
        endpoints: dict[str, str] = {}
        for role in ("source", "target"):
            endpoint = edge.get(role)
            if not isinstance(endpoint, dict):
                errors.append(f"{trail}.{role} must be an object")
                continue
            node_id = str(endpoint.get("nodeId") or "")
            endpoints[role] = node_id
            if node_id not in node_ids:
                errors.append(f"{trail}.{role}.nodeId references missing node: {node_id or '<empty>'}")
            if endpoint.get("side") not in VALID_PORT_SIDES:
                errors.append(f"{trail}.{role}.side must be left, right, or bottom")
            port_index = endpoint.get("index")
            if not isinstance(port_index, int) or isinstance(port_index, bool) or port_index < 0:
                errors.append(f"{trail}.{role}.index must be a non-negative integer")
        if endpoints.get("source") in adjacency and endpoints.get("target") in node_ids:
            adjacency[endpoints["source"]].append(endpoints["target"])
            source = node_by_id.get(endpoints["source"], {})
            target = node_by_id.get(endpoints["target"], {})
            if _is_finite_number(source.get("x")) and _is_finite_number(target.get("x")):
                if float(target["x"]) <= float(source["x"]):
                    warnings.append(f"Edge {edge_id or index} does not follow the recommended left-to-right layout")
        validate_workspace(edge.get("conditionWorkspace", {}), f"edge {edge_id or index} condition", condition=True)

    validate_workspace(globals_workspace, "globalVariablesWorkspace", global_pool=True)

    if len(start_ids) == 1:
        reachable: set[str] = set()
        queue: deque[str] = deque(start_ids)
        while queue:
            node_id = queue.popleft()
            if node_id in reachable:
                continue
            reachable.add(node_id)
            queue.extend(adjacency.get(node_id, []))
        unreachable = sorted(node_ids - reachable)
        if unreachable:
            warnings.append("Nodes unreachable from start: " + ", ".join(unreachable))

    return {
        "success": not errors,
        "errors": errors,
        "warnings": warnings,
        "summary": {
            "nodeCount": len(nodes),
            "edgeCount": len(edges),
            "blockCount": block_count,
        },
    }
