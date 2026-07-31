from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_VERSION = 1
BASELINE_COMMIT = "457e853a789e16eb104a0bfb43d2485d9e1ac0c8"


class ContractError(RuntimeError):
    pass


def reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def reject_constant(value: str) -> Any:
    raise ContractError(f"non-finite JSON number: {value}")


def load_json(relative_path: str | Path) -> Any:
    path = ROOT / relative_path
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_pairs,
            parse_constant=reject_constant,
        )
    except (OSError, json.JSONDecodeError, ContractError) as error:
        raise ContractError(f"{path.relative_to(ROOT)}: {error}") from error


def read_text(relative_path: str | Path) -> str:
    path = ROOT / relative_path
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise ContractError(f"{path.relative_to(ROOT)}: {error}") from error


def canonical_json_value(value: Any) -> Any:
    if value is None:
        return ("null", None)
    if isinstance(value, bool):
        return ("boolean", value)
    if isinstance(value, (int, float)):
        return ("number", value)
    if isinstance(value, str):
        return ("string", value)
    if isinstance(value, list):
        return ("array", tuple(canonical_json_value(child) for child in value))
    if isinstance(value, dict):
        return (
            "object",
            tuple(
                (key, canonical_json_value(child))
                for key, child in sorted(value.items())
            ),
        )
    if isinstance(value, set):
        children = [canonical_json_value(child) for child in value]
        return ("set", tuple(sorted(children, key=repr)))
    raise ContractError(f"unsupported comparison type: {type(value).__name__}")


def display_value(value: Any) -> str:
    if isinstance(value, set):
        value = sorted(value)
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    )


def require_equal(label: str, actual: Any, expected: Any) -> None:
    if canonical_json_value(actual) != canonical_json_value(expected):
        raise ContractError(
            f"{label} drifted\nexpected={display_value(expected)}"
            f"\nactual={display_value(actual)}"
        )


def tool_snapshot() -> None:
    source = read_text("Sources/PalmierPro/Agent/Tools/ToolDefinitions.swift")
    enum_match = re.search(
        r"enum ToolName:[^{]+\{(?P<body>.*?)^\}",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not enum_match:
        raise ContractError("ToolName enum was not found")
    cases = re.findall(
        r"^\s*case\s+(\w+)\s*=\s*\"([^\"]+)\"",
        enum_match.group("body"),
        re.MULTILINE,
    )
    symbols = dict(cases)
    tool_names = [value for _, value in cases]

    all_match = re.search(
        r"static let all:\s*\[AgentTool\]\s*=\s*\[(?P<body>.*?)"
        r"^\s*private static func effectCatalog",
        source,
        re.MULTILINE | re.DOTALL,
    )
    if not all_match:
        raise ContractError("ToolDefinitions.all was not found")
    all_symbols = re.findall(r"name:\s*\.(\w+)", all_match.group("body"))
    try:
        common = [symbols[symbol] for symbol in all_symbols]
    except KeyError as error:
        raise ContractError(f"unknown ToolName symbol in all: {error}") from error

    snapshot = load_json("contracts/mcp/v1/tools.json")
    require_equal("MCP contract version", snapshot["contractVersion"], CONTRACT_VERSION)
    require_equal("MCP baseline commit", snapshot["baselineCommit"], BASELINE_COMMIT)
    require_equal(
        "MCP schema snapshot completion",
        snapshot["schemaSnapshotComplete"],
        False,
    )
    require_equal("ToolName values", tool_names, snapshot["toolNames"])
    require_equal("ToolName count", len(tool_names), snapshot["toolNameEnumCount"])
    require_equal("unique ToolName values", len(tool_names), len(set(tool_names)))
    require_equal(
        "MCP server count",
        len(common + [symbols["manageProject"]]),
        snapshot["mcpServerCount"],
    )
    require_equal(
        "in-app Agent count",
        len(common + [symbols["readSkill"]]),
        snapshot["inAppAgentCount"],
    )

    mcp_set = set(common + [symbols["manageProject"]])
    require_equal("MCP-only tools", snapshot["mcpOnly"], [symbols["manageProject"]])
    require_equal("in-app-only tools", snapshot["inAppOnly"], [symbols["readSkill"]])
    require_equal(
        "P1 in-app additions",
        snapshot["p1InAppAdds"],
        [symbols["readSkill"]],
    )
    alpha = snapshot["p0Alpha"]
    release_adds = snapshot["p0ReleaseAdds"]
    p1_adds = snapshot["p1McpAdds"]
    stages = {
        "P0-alpha": alpha,
        "P0-release additions": release_adds,
        "P1 MCP additions": p1_adds,
    }
    for label, stage in stages.items():
        if len(stage) != len(set(stage)):
            raise ContractError(f"{label} list contains duplicates")
    release = alpha + release_adds
    full = release + p1_adds
    if len(full) != len(set(full)):
        raise ContractError("MCP milestone lists must be mutually exclusive")
    require_equal("full staged MCP surface", set(full), mcp_set)
    if symbols["readSkill"] in mcp_set:
        raise ContractError("read_skill must not appear in the MCP server surface")


def effect_snapshot() -> None:
    registry = read_text("Sources/PalmierPro/Compositing/EffectRegistry.swift")
    effect_ids = re.findall(
        r"EffectDescriptor\(\s*id:\s*\"([^\"]+)\"",
        registry,
        re.DOTALL,
    )
    order_match = re.search(
        r"static let canonicalOrder:\s*\[String\]\s*=\s*\[(?P<body>.*?)\]",
        registry,
        re.DOTALL,
    )
    if not order_match:
        raise ContractError("EffectRegistry.canonicalOrder was not found")
    canonical_order = re.findall(r"\"([^\"]+)\"", order_match.group("body"))
    effects = load_json("contracts/effects/v1/effects.json")
    require_equal(
        "effect contract version",
        effects["contractVersion"],
        CONTRACT_VERSION,
    )
    require_equal("effect baseline commit", effects["baselineCommit"], BASELINE_COMMIT)
    require_equal(
        "effect parameter contract completion",
        effects["parameterContractComplete"],
        False,
    )
    require_equal("effect identifiers", effect_ids, effects["effectIds"])
    require_equal(
        "effect canonical order",
        canonical_order,
        effects["canonicalOrder"],
    )

    blend_source = read_text("Sources/PalmierPro/Models/BlendMode.swift")
    blend_match = re.search(
        r"enum BlendMode:[^{]+\{(?P<body>.*?)\n\s*var displayName",
        blend_source,
        re.DOTALL,
    )
    if not blend_match:
        raise ContractError("BlendMode enum was not found")
    blend_modes: list[str] = []
    for line in re.findall(r"^\s*case\s+(.+)$", blend_match.group("body"), re.MULTILINE):
        blend_modes.extend(value.strip() for value in line.split(","))
    blend = load_json("contracts/effects/v1/blend-modes.json")
    require_equal(
        "blend contract version",
        blend["contractVersion"],
        CONTRACT_VERSION,
    )
    require_equal("blend baseline commit", blend["baselineCommit"], BASELINE_COMMIT)
    require_equal("blend includes normal", blend["includesNormal"], True)
    require_equal("blend modes", blend_modes, blend["values"])
    require_equal("blend mode count", len(blend_modes), blend["count"])
    require_equal("normal blend mode position", blend_modes[0], "normal")


def json_pointer(document: Any, pointer: str) -> Any:
    if pointer == "":
        return document
    if not pointer.startswith("/"):
        raise ContractError(f"invalid JSON Pointer: {pointer}")
    current = document
    for raw_part in pointer[1:].split("/"):
        part = raw_part.replace("~1", "/").replace("~0", "~")
        if isinstance(current, list):
            try:
                current = current[int(part)]
            except (ValueError, IndexError) as error:
                raise ContractError(f"missing JSON Pointer: {pointer}") from error
        elif isinstance(current, dict) and part in current:
            current = current[part]
        else:
            raise ContractError(f"missing JSON Pointer: {pointer}")
    return current


SCHEMA_TYPES = {
    "array",
    "boolean",
    "integer",
    "null",
    "number",
    "object",
    "string",
}
SCHEMA_KEYWORDS = {
    "$defs",
    "$id",
    "$ref",
    "$schema",
    "additionalProperties",
    "exclusiveMinimum",
    "items",
    "minimum",
    "minItems",
    "oneOf",
    "properties",
    "required",
    "title",
    "type",
}


def schema_type_matches(value: Any, expected: str) -> bool:
    if expected == "null":
        return value is None
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "string":
        return isinstance(value, str)
    if expected == "array":
        return isinstance(value, list)
    if expected == "object":
        return isinstance(value, dict)
    raise ContractError(f"unsupported schema type: {expected}")


def resolve_local_ref(root_schema: dict[str, Any], ref: str, location: str) -> Any:
    if not ref.startswith("#"):
        raise ContractError(f"{location}: only local $ref values are allowed: {ref}")
    return json_pointer(root_schema, ref[1:])


def validate_schema_node(
    schema: Any,
    root_schema: dict[str, Any],
    location: str,
) -> None:
    if not isinstance(schema, dict):
        raise ContractError(f"{location}: schema node must be an object")
    unsupported = set(schema) - SCHEMA_KEYWORDS
    if unsupported:
        raise ContractError(
            f"{location}: unsupported schema keywords: {sorted(unsupported)}"
        )
    if "$ref" in schema:
        ref = schema["$ref"]
        if not isinstance(ref, str):
            raise ContractError(f"{location}/$ref: must be a string")
        target = resolve_local_ref(root_schema, ref, f"{location}/$ref")
        if not isinstance(target, dict):
            raise ContractError(f"{location}/$ref: target must be a schema object")
    if "type" in schema:
        declared = schema["type"]
        values = declared if isinstance(declared, list) else [declared]
        if (
            not values
            or any(not isinstance(value, str) for value in values)
            or not set(values).issubset(SCHEMA_TYPES)
        ):
            raise ContractError(f"{location}/type: unsupported type declaration")
    if "required" in schema:
        required = schema["required"]
        if (
            not isinstance(required, list)
            or any(not isinstance(value, str) for value in required)
            or len(required) != len(set(required))
        ):
            raise ContractError(f"{location}/required: must contain unique strings")
    for numeric_keyword in ("minimum", "exclusiveMinimum"):
        if numeric_keyword in schema and (
            not isinstance(schema[numeric_keyword], (int, float))
            or isinstance(schema[numeric_keyword], bool)
        ):
            raise ContractError(f"{location}/{numeric_keyword}: must be a number")
    if "minItems" in schema and (
        not isinstance(schema["minItems"], int)
        or isinstance(schema["minItems"], bool)
        or schema["minItems"] < 0
    ):
        raise ContractError(f"{location}/minItems: must be a non-negative integer")
    if "properties" in schema:
        properties = schema["properties"]
        if not isinstance(properties, dict):
            raise ContractError(f"{location}/properties: must be an object")
        for key, child in properties.items():
            validate_schema_node(child, root_schema, f"{location}/properties/{key}")
    if "$defs" in schema:
        definitions = schema["$defs"]
        if not isinstance(definitions, dict):
            raise ContractError(f"{location}/$defs: must be an object")
        for key, child in definitions.items():
            validate_schema_node(child, root_schema, f"{location}/$defs/{key}")
    if "items" in schema:
        validate_schema_node(schema["items"], root_schema, f"{location}/items")
    if "additionalProperties" in schema:
        additional = schema["additionalProperties"]
        if not isinstance(additional, bool):
            validate_schema_node(
                additional,
                root_schema,
                f"{location}/additionalProperties",
            )
    if "oneOf" in schema:
        alternatives = schema["oneOf"]
        if not isinstance(alternatives, list) or not alternatives:
            raise ContractError(f"{location}/oneOf: must be a non-empty array")
        for index, child in enumerate(alternatives):
            validate_schema_node(
                child,
                root_schema,
                f"{location}/oneOf/{index}",
            )


def validate_schema_document(schema: Any, label: str) -> dict[str, Any]:
    if not isinstance(schema, dict):
        raise ContractError(f"{label}: schema must be an object")
    expected_draft = "https://json-schema.org/draft/2020-12/schema"
    require_equal(f"{label} draft", schema.get("$schema"), expected_draft)
    schema_id = schema.get("$id")
    if not isinstance(schema_id, str) or not schema_id.startswith("urn:palmier:"):
        raise ContractError(f"{label}: $id must use the urn:palmier namespace")
    validate_schema_node(schema, schema, label)
    return schema


def validate_instance(
    value: Any,
    schema: dict[str, Any],
    root_schema: dict[str, Any],
    location: str,
) -> None:
    if "$ref" in schema:
        target = resolve_local_ref(root_schema, schema["$ref"], location)
        validate_instance(value, target, root_schema, location)
    if "oneOf" in schema:
        matches = 0
        for alternative in schema["oneOf"]:
            try:
                validate_instance(value, alternative, root_schema, location)
            except ContractError:
                continue
            matches += 1
        if matches != 1:
            raise ContractError(
                f"{location}: expected exactly one schema match, got {matches}"
            )
    if "type" in schema:
        declared = schema["type"]
        expected_types = declared if isinstance(declared, list) else [declared]
        if not any(schema_type_matches(value, item) for item in expected_types):
            raise ContractError(
                f"{location}: expected type {expected_types}, "
                f"got {type(value).__name__}"
            )
    if isinstance(value, dict):
        required = schema.get("required", [])
        missing = [key for key in required if key not in value]
        if missing:
            raise ContractError(f"{location}: missing required keys {missing}")
        properties = schema.get("properties", {})
        for key, child in properties.items():
            if key in value:
                validate_instance(
                    value[key],
                    child,
                    root_schema,
                    f"{location}/{key}",
                )
        additional = schema.get("additionalProperties", True)
        for key in set(value) - set(properties):
            if additional is False:
                raise ContractError(f"{location}: unexpected property {key}")
            if isinstance(additional, dict):
                validate_instance(
                    value[key],
                    additional,
                    root_schema,
                    f"{location}/{key}",
                )
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            raise ContractError(f"{location}: array is shorter than minItems")
        if "items" in schema:
            for index, child in enumerate(value):
                validate_instance(
                    child,
                    schema["items"],
                    root_schema,
                    f"{location}/{index}",
                )
    if (
        "minimum" in schema
        and isinstance(value, (int, float))
        and not isinstance(value, bool)
        and value < schema["minimum"]
    ):
        raise ContractError(f"{location}: value is below minimum")
    if (
        "exclusiveMinimum" in schema
        and isinstance(value, (int, float))
        and not isinstance(value, bool)
        and value <= schema["exclusiveMinimum"]
    ):
        raise ContractError(f"{location}: value is not above exclusiveMinimum")


def expect_failure(label: str, operation: Any) -> None:
    try:
        operation()
    except ContractError:
        return
    raise ContractError(f"{label}: negative self-check unexpectedly passed")


def validator_self_check() -> None:
    expect_failure("type-sensitive equality", lambda: require_equal("canary", True, 1))
    integer_schema = {"type": "integer"}
    expect_failure(
        "boolean is not an integer",
        lambda: validate_instance(True, integer_schema, integer_schema, "self-check"),
    )
    required_schema = {
        "type": "object",
        "required": ["value"],
        "properties": {"value": {"type": "string"}},
        "additionalProperties": False,
    }
    expect_failure(
        "required property",
        lambda: validate_instance({}, required_schema, required_schema, "self-check"),
    )
    expect_failure(
        "unknown property",
        lambda: validate_instance(
            {"value": "ok", "future": True},
            required_schema,
            required_schema,
            "self-check",
        ),
    )


def validate_timeline(timeline: Any, label: str) -> None:
    if not isinstance(timeline, dict):
        raise ContractError(f"{label}: timeline must be an object")
    for key in ("fps", "width", "height", "tracks"):
        if key not in timeline:
            raise ContractError(f"{label}: missing {key}")
    if not isinstance(timeline["tracks"], list):
        raise ContractError(f"{label}: tracks must be an array")


def project_fixtures() -> None:
    project_schema = validate_schema_document(
        load_json("contracts/project/v1/project.schema.json"),
        "project.schema.json",
    )
    media_schema = validate_schema_document(
        load_json("contracts/project/v1/media.schema.json"),
        "media.schema.json",
    )

    fixture_manifest = load_json("fixtures/contracts/projects/manifest.json")
    require_equal(
        "fixture baseline",
        fixture_manifest["baselineCommit"],
        BASELINE_COMMIT,
    )
    for entry in fixture_manifest["fixtures"]:
        package = Path("fixtures/contracts/projects") / entry["path"]
        project = load_json(package / "project.json")
        validate_instance(
            project,
            project_schema,
            project_schema,
            f"{package}/project.json",
        )
        if "timelines" in project:
            if not project["timelines"]:
                raise ContractError(f"{package}: timelines must not be empty")
            for index, timeline in enumerate(project["timelines"]):
                validate_timeline(timeline, f"{package}/timelines/{index}")
        else:
            validate_timeline(project, str(package))
        media_path = ROOT / package / "media.json"
        if media_path.exists():
            media = load_json(package / "media.json")
            validate_instance(
                media,
                media_schema,
                media_schema,
                f"{package}/media.json",
            )
            if not isinstance(media.get("entries", []), list):
                raise ContractError(f"{package}/media.json: entries must be an array")
            for media_entry in media.get("entries", []):
                project_source = media_entry.get("source", {}).get("project")
                if project_source and "\\" in project_source.get("relativePath", ""):
                    raise ContractError(
                        f"{package}/media.json: project path must use '/'"
                    )

    canary_contract = load_json("contracts/project/v1/canaries.json")
    require_equal(
        "canary contract version",
        canary_contract["contractVersion"],
        CONTRACT_VERSION,
    )
    if canary_contract["runtimeRoundTripStatus"] != "blocked":
        raise ContractError(
            "unknown-field runtime round trip must stay blocked until a client "
            "load-edit-save test passes"
        )
    package = Path(canary_contract["fixture"])
    documents: dict[str, Any] = {}
    for canary in canary_contract["canaries"]:
        filename = canary["file"]
        document = documents.setdefault(filename, load_json(package / filename))
        actual = json_pointer(document, canary["pointer"])
        require_equal(
            f"canary {filename}{canary['pointer']}",
            actual,
            canary["value"],
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate Palmier contract snapshots")
    parser.add_argument(
        "--check",
        action="store_true",
        help="check snapshots without writing files",
    )
    args = parser.parse_args()
    if not args.check:
        parser.error("--check is required; snapshot rewriting is intentionally explicit")

    checks = [
        ("validator negative self-checks", validator_self_check),
        ("MCP tool snapshot", tool_snapshot),
        ("effect and blend snapshots", effect_snapshot),
        ("project fixtures and canaries", project_fixtures),
    ]
    for label, check in checks:
        check()
        print(f"PASS {label}")
    print("BLOCKED unknown-field client load-edit-save round trip")
    print(f"PASS {len(checks)} contract audit groups")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ContractError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
