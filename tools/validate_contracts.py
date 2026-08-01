from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_VERSION = 1
BASELINE_COMMIT = "457e853a789e16eb104a0bfb43d2485d9e1ac0c8"
MEDIA_SOURCE_COMMIT = "83916cffc996496a51d3bf8b3ac346a70fe425ef"


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
    requirements = read_text("docs/WINDOWS_10_PORT_REQUIREMENTS.zh-CN.md")
    requirement_counts = [
        int(value)
        for value in re.findall(r"支持当前\s+(\d+)\s+种混合模式", requirements)
    ]
    require_equal(
        "requirements blend count declarations",
        requirement_counts,
        [blend["count"]],
    )
    if "`normal`，即 source-over 基线" not in requirements:
        raise ContractError(
            "requirements must state that normal is the source-over baseline"
        )


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
    "enum",
    "items",
    "minimum",
    "minItems",
    "not",
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
    if "enum" in schema:
        values = schema["enum"]
        if not isinstance(values, list) or not values:
            raise ContractError(f"{location}/enum: must be a non-empty array")
        canonical = [canonical_json_value(value) for value in values]
        if len(canonical) != len(set(canonical)):
            raise ContractError(f"{location}/enum: values must be unique")
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
    if "not" in schema:
        validate_schema_node(schema["not"], root_schema, f"{location}/not")


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
    if "not" in schema:
        try:
            validate_instance(value, schema["not"], root_schema, location)
        except ContractError:
            pass
        else:
            raise ContractError(f"{location}: value matched a forbidden schema")
    if "type" in schema:
        declared = schema["type"]
        expected_types = declared if isinstance(declared, list) else [declared]
        if not any(schema_type_matches(value, item) for item in expected_types):
            raise ContractError(
                f"{location}: expected type {expected_types}, "
                f"got {type(value).__name__}"
            )
    if "enum" in schema and not any(
        canonical_json_value(value) == canonical_json_value(candidate)
        for candidate in schema["enum"]
    ):
        raise ContractError(f"{location}: value is outside the declared enum")
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


def expect_failure_containing(
    label: str,
    expected_message: str,
    operation: Any,
) -> None:
    try:
        operation()
    except ContractError as error:
        if expected_message not in str(error):
            raise ContractError(
                f"{label}: expected error containing {expected_message!r}, "
                f"got {str(error)!r}"
            ) from error
        return
    raise ContractError(f"{label}: invalid fixture unexpectedly passed")


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
    expect_failure(
        "enum membership",
        lambda: validate_instance(
            "future",
            {"type": "string", "enum": ["known"]},
            {"type": "string", "enum": ["known"]},
            "self-check",
        ),
    )
    expect_failure(
        "not schema",
        lambda: validate_instance(
            {"future": True},
            {"not": {"required": ["future"]}},
            {"not": {"required": ["future"]}},
            "self-check",
        ),
    )
    swift_sample = """
// func contractProbe() { document.save(.saveAsOperation) }
func contractProbe() {
    let ignored = "document.save(.saveAsOperation)"
    /* VideoProject.writeProjectPackage */
    realCall()
}
"""
    swift_body = swift_function_code_body(swift_sample, "contractProbe", "self-check")
    if "realCall()" not in swift_body:
        raise ContractError("Swift function scanner dropped executable code")
    for ignored_token in ["document.save", ".saveAsOperation", "writeProjectPackage"]:
        if ignored_token in swift_body:
            raise ContractError(
                f"Swift function scanner retained comment/string token {ignored_token!r}"
            )


def swift_braced_body(source: str, opening: int, label: str) -> str:
    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state == "string":
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line-comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block-comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
        index += 1
    raise ContractError(f"{label} has no closing brace")


def swift_code_mask(source: str) -> str:
    result: list[str] = []
    state = "code"
    escaped = False
    index = 0
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                result.extend("  ")
                state = "line-comment"
                index += 1
            elif char == "/" and next_char == "*":
                result.extend("  ")
                state = "block-comment"
                index += 1
            elif char == '"':
                result.append(" ")
                state = "string"
            else:
                result.append(char)
        elif state == "line-comment":
            if char == "\n":
                result.append("\n")
                state = "code"
            else:
                result.append(" ")
        elif state == "block-comment":
            result.append("\n" if char == "\n" else " ")
            if char == "*" and next_char == "/":
                result.append(" ")
                state = "code"
                index += 1
        else:
            result.append("\n" if char == "\n" else " ")
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                state = "code"
        index += 1
    return "".join(result)


def swift_type_body(relative_path: str, kind: str, name: str) -> str:
    source = read_text(relative_path)
    match = re.search(rf"\b{kind}\s+{re.escape(name)}\b[^{{]*\{{", source)
    if not match:
        raise ContractError(f"{relative_path}: {kind} {name} was not found")
    opening = source.find("{", match.start())
    return swift_braced_body(source, opening, f"{relative_path}: {kind} {name}")


def swift_function_code_body(source: str, name: str, label: str) -> str:
    code = swift_code_mask(source)
    match = re.search(rf"\bfunc\s+{re.escape(name)}\s*\([^{{]*\{{", code)
    if not match:
        raise ContractError(f"{label}: func {name} was not found")
    opening = code.find("{", match.start())
    return swift_braced_body(code, opening, f"{label}: func {name}")


def swift_function_body(relative_path: str, name: str) -> str:
    return swift_function_code_body(read_text(relative_path), name, relative_path)


def swift_top_level_lines(body: str) -> list[str]:
    result: list[str] = []
    depth = 0
    for line in body.splitlines():
        if depth == 0:
            result.append(line)
        code = line.split("//", 1)[0]
        depth += code.count("{") - code.count("}")
    return result


def swift_stored_field_signatures(
    relative_path: str,
    name: str,
) -> list[dict[str, str]]:
    body = swift_type_body(relative_path, "struct", name)
    fields: list[dict[str, str]] = []
    for line in swift_top_level_lines(body):
        match = re.match(r"^\s*(let|var)\s+(\w+)\s*:\s*(.+)$", line)
        if match and "{" not in line:
            swift_type = re.split(r"\s+=\s+", match.group(3), maxsplit=1)[0]
            fields.append(
                {
                    "name": match.group(2),
                    "declaration": match.group(1),
                    "swiftType": swift_type.strip(),
                }
            )
    if not fields:
        raise ContractError(f"{relative_path}: no stored fields found for {name}")
    return fields


def swift_stored_fields(relative_path: str, name: str) -> list[str]:
    return [
        field["name"]
        for field in swift_stored_field_signatures(relative_path, name)
    ]


def swift_type_body_sha256(relative_path: str, kind: str, name: str) -> str:
    body = swift_type_body(relative_path, kind, name)
    normalized = "\n".join(line.rstrip() for line in body.strip().splitlines())
    return hashlib.sha256(normalized.encode("utf-8")).hexdigest()


def swift_enum_case_signatures(relative_path: str, name: str) -> list[str]:
    body = swift_type_body(relative_path, "enum", name)
    signatures: list[str] = []
    for line in swift_top_level_lines(body):
        match = re.match(r"^\s*case\s+(.+)$", line)
        if match:
            signatures.extend(
                declaration.strip()
                for declaration in match.group(1).split(",")
                if declaration.strip()
            )
    if not signatures:
        raise ContractError(f"{relative_path}: no enum cases found for {name}")
    return signatures


def swift_enum_cases(relative_path: str, name: str) -> list[str]:
    return [
        signature.split("(", 1)[0].split("=", 1)[0].strip()
        for signature in swift_enum_case_signatures(relative_path, name)
    ]


def cpp_type_body(source: str, kind: str, name: str) -> str:
    declaration = re.search(
        rf"\b{re.escape(kind)}\s+{re.escape(name)}\b[^{{]*{{",
        source,
    )
    if declaration is None:
        raise ContractError(f"C++ {kind} {name} not found")
    opening = source.find("{", declaration.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise ContractError(f"C++ {kind} {name} is not closed")


def cpp_enum_cases(source: str, name: str) -> list[str]:
    body = cpp_type_body(source, "enum class", name)
    cases = [
        value.split("=", 1)[0].strip()
        for value in body.split(",")
        if value.strip()
    ]
    if not cases:
        raise ContractError(f"C++ enum class {name} has no cases")
    return cases


def cpp_stored_fields(source: str, name: str) -> list[str]:
    body = cpp_type_body(source, "struct", name)
    fields: list[str] = []
    for line in body.splitlines():
        match = re.match(
            r"^\s*[\w:<>]+\s+(\w+)(?:\{[^;]*\}|\s*=\s*[^;]+)?;\s*$",
            line,
        )
        if match:
            fields.append(match.group(1))
    if not fields:
        raise ContractError(f"C++ struct {name} has no stored fields")
    return fields


def schema_allows_null(
    schema: dict[str, Any],
    root_schema: dict[str, Any],
) -> bool:
    if "$ref" in schema:
        target = resolve_local_ref(root_schema, schema["$ref"], "nullability")
        return schema_allows_null(target, root_schema)
    declared = schema.get("type")
    if isinstance(declared, list):
        return "null" in declared
    return declared == "null"


def schema_non_null_shape(
    schema: dict[str, Any],
    root_schema: dict[str, Any],
) -> str:
    if "$ref" in schema:
        resolve_local_ref(root_schema, schema["$ref"], "shape")
        return f"ref:{schema['$ref']}"
    if "oneOf" in schema:
        shapes = {
            schema_non_null_shape(alternative, root_schema)
            for alternative in schema["oneOf"]
        }
        if len(shapes) != 1:
            raise ContractError(f"schema has incompatible oneOf shapes: {shapes}")
        return next(iter(shapes))
    declared = schema.get("type")
    types = declared if isinstance(declared, list) else [declared]
    non_null = [value for value in types if value != "null"]
    if len(non_null) != 1:
        raise ContractError(f"schema must have one non-null type: {types}")
    shape = non_null[0]
    if shape == "array":
        return f"array<{schema_non_null_shape(schema['items'], root_schema)}>"
    if shape == "object" and isinstance(schema.get("additionalProperties"), dict):
        return (
            "map<"
            f"{schema_non_null_shape(schema['additionalProperties'], root_schema)}"
            ">"
        )
    return shape


def swift_json_shape(swift_type: str) -> str:
    references = {
        "ClipType": "#/$defs/clipType",
        "GenerationInput?": "#/$defs/nullableGenerationInput",
        "MediaFolder": "#/$defs/mediaFolder",
        "MediaImportInput?": "#/$defs/nullableMediaImportInput",
        "MediaManifestEntry": "#/$defs/entry",
        "MediaSource": "#/$defs/source",
        "UpscaleSettings?": "#/$defs/nullableUpscaleSettings",
        "[String]?": "#/$defs/nullableStringArray",
    }
    if swift_type in references:
        return f"ref:{references[swift_type]}"
    value = swift_type.removesuffix("?")
    primitives = {
        "Bool": "boolean",
        "Date": "number",
        "Double": "number",
        "Int": "integer",
        "String": "string",
    }
    if value in primitives:
        return primitives[value]
    if value.startswith("[") and value.endswith("]"):
        inner = value[1:-1]
        if ": " in inner:
            key, child = inner.split(": ", 1)
            if key != "String":
                raise ContractError(f"unsupported Swift dictionary key: {key}")
            return f"map<{swift_json_shape(child)}>"
        return f"array<{swift_json_shape(inner)}>"
    raise ContractError(f"unsupported persisted Swift type: {swift_type}")


def media_source_contract() -> None:
    schema = validate_schema_document(
        load_json("contracts/project/v1/media.schema.json"),
        "media.schema.json",
    )
    definitions = schema["$defs"]
    source_alternatives = definitions["source"]["oneOf"]
    open_objects = [
        ("MediaManifest", schema),
        ("MediaManifestEntry", definitions["entry"]),
        ("MediaSource.external", source_alternatives[0]),
        (
            "MediaSource.external payload",
            source_alternatives[0]["properties"]["external"],
        ),
        ("MediaSource.project", source_alternatives[1]),
        (
            "MediaSource.project payload",
            source_alternatives[1]["properties"]["project"],
        ),
        ("GenerationInput", definitions["nullableGenerationInput"]),
        ("MediaImportInput", definitions["nullableMediaImportInput"]),
        ("UpscaleSettings", definitions["nullableUpscaleSettings"]),
        ("MediaFolder", definitions["mediaFolder"]),
    ]
    for label, contract in open_objects:
        require_equal(
            f"{label} forward-compatible properties",
            contract.get("additionalProperties"),
            True,
        )
    require_equal(
        "MediaManifest version minimum",
        schema["properties"]["version"].get("minimum"),
        1,
    )
    require_equal(
        "MediaManifestEntry duration minimum",
        definitions["entry"]["properties"]["duration"].get("minimum"),
        0,
    )
    model_snapshot = load_json("contracts/project/v1/media-model.json")
    require_equal(
        "media model contract version",
        model_snapshot["contractVersion"],
        CONTRACT_VERSION,
    )
    require_equal(
        "media model source commit",
        model_snapshot["sourceCommit"],
        MEDIA_SOURCE_COMMIT,
    )
    field_mappings = [
        (
            "MediaManifest",
            schema,
        ),
        (
            "MediaManifestEntry",
            definitions["entry"],
        ),
        (
            "MediaImportInput",
            definitions["nullableMediaImportInput"],
        ),
        (
            "GenerationInput",
            definitions["nullableGenerationInput"],
        ),
        (
            "MediaFolder",
            definitions["mediaFolder"],
        ),
        (
            "UpscaleSettings",
            definitions["nullableUpscaleSettings"],
        ),
    ]
    for type_name, contract in field_mappings:
        type_snapshot = model_snapshot["types"][type_name]
        source = type_snapshot["source"]
        actual_fields = swift_stored_field_signatures(source, type_name)
        if type_name == "MediaManifest":
            actual_fields = [
                field for field in actual_fields
                if field["name"] != "compatibilitySnapshot"
            ]
        require_equal(
            f"{type_name} schema fields",
            set(contract["properties"]),
            {field["name"] for field in actual_fields},
        )
        require_equal(
            f"{type_name} Swift field signatures",
            actual_fields,
            type_snapshot["fields"],
        )
        require_equal(
            f"{type_name} Swift body",
            swift_type_body_sha256(source, "struct", type_name),
            type_snapshot["bodySha256"],
        )
        require_equal(
            f"{type_name} required decode fields",
            set(contract.get("required", [])),
            set(type_snapshot["requiredOnDecode"]),
        )
        for field in actual_fields:
            field_schema = contract["properties"][field["name"]]
            require_equal(
                f"{type_name}.{field['name']} nullability",
                schema_allows_null(
                    field_schema,
                    schema,
                ),
                field["swiftType"].endswith("?"),
            )
            require_equal(
                f"{type_name}.{field['name']} JSON shape",
                schema_non_null_shape(field_schema, schema),
                swift_json_shape(field["swiftType"]),
            )
    clip_type_snapshot = model_snapshot["enums"]["ClipType"]
    require_equal(
        "ClipType Swift cases",
        swift_enum_case_signatures(clip_type_snapshot["source"], "ClipType"),
        clip_type_snapshot["cases"],
    )
    require_equal(
        "ClipType media enum",
        definitions["clipType"]["enum"],
        swift_enum_cases(clip_type_snapshot["source"], "ClipType"),
    )
    media_source_snapshot = model_snapshot["enums"]["MediaSource"]
    require_equal(
        "MediaSource Swift cases",
        swift_enum_case_signatures(
            media_source_snapshot["source"],
            "MediaSource",
        ),
        media_source_snapshot["cases"],
    )
    require_equal(
        "MediaSource Swift body",
        swift_type_body_sha256(
            media_source_snapshot["source"],
            "enum",
            "MediaSource",
        ),
        media_source_snapshot["bodySha256"],
    )
    source_contract_cases = [
        alternative["required"][0]
        for alternative in source_alternatives
    ]
    require_equal(
        "MediaSource cases",
        source_contract_cases,
        swift_enum_cases(
            media_source_snapshot["source"],
            "MediaSource",
        ),
    )
    complete_fixture = load_json(
        "fixtures/contracts/projects/media-complete.palmier/media.json"
    )
    validate_media_document(
        complete_fixture,
        schema,
        "fixtures/contracts/projects/media-complete.palmier/media.json",
    )
    require_equal(
        "complete media root coverage",
        set(complete_fixture),
        set(schema["properties"]),
    )
    complete_entry = complete_fixture["entries"][0]
    require_equal(
        "complete media entry coverage",
        set(complete_entry),
        set(definitions["entry"]["properties"]),
    )
    require_equal(
        "complete generation input coverage",
        set(complete_entry["generationInput"]),
        set(definitions["nullableGenerationInput"]["properties"]),
    )
    require_equal(
        "complete upscale settings coverage",
        set(complete_entry["generationInput"]["upscaleSettings"]),
        set(definitions["nullableUpscaleSettings"]["properties"]),
    )
    require_equal(
        "complete media import coverage",
        set(complete_entry["importInput"]),
        set(definitions["nullableMediaImportInput"]["properties"]),
    )
    require_equal(
        "complete media folder coverage",
        set(complete_fixture["folders"][0]),
        set(definitions["mediaFolder"]["properties"]),
    )
    require_equal(
        "complete MediaSource case coverage",
        {
            next(iter(entry["source"]))
            for entry in complete_fixture["entries"]
        },
        set(source_contract_cases),
    )
    fixture_root = Path("fixtures/contracts/media/v1")
    fixture_manifest = load_json(fixture_root / "manifest.json")
    require_equal(
        "media fixture contract version",
        fixture_manifest["contractVersion"],
        CONTRACT_VERSION,
    )
    for entry in fixture_manifest["fixtures"]:
        document = load_json(fixture_root / entry["path"])
        if entry["valid"]:
            validate_media_document(
                document,
                schema,
                str(fixture_root / entry["path"]),
            )
        else:
            expect_failure_containing(
                f"invalid media fixture {entry['path']}",
                entry["errorContains"],
                lambda document=document, path=entry["path"]: validate_media_document(
                    document,
                    schema,
                    str(fixture_root / path),
                ),
            )


def validate_media_document(
    media: Any,
    schema: dict[str, Any],
    label: str,
) -> None:
    validate_instance(media, schema, schema, label)
    if not isinstance(media, dict):
        return
    for entry in media.get("entries", []):
        project_source = entry.get("source", {}).get("project")
        if project_source and "\\" in project_source.get("relativePath", ""):
            raise ContractError(f"{label}: project path must use '/'")


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
            validate_media_document(
                media,
                media_schema,
                f"{package}/media.json",
            )
            if not isinstance(media.get("entries", []), list):
                raise ContractError(f"{package}/media.json: entries must be an array")

    canary_contract = load_json("contracts/project/v1/canaries.json")
    require_equal(
        "canary contract version",
        canary_contract["contractVersion"],
        CONTRACT_VERSION,
    )
    if canary_contract["runtimeRoundTripStatus"] != "enforced":
        raise ContractError(
            "unknown-field runtime round trip must be enforced by a client "
            "load-edit-save test"
        )
    enforcement_test = canary_contract["enforcementTest"]
    enforcement_body = swift_function_body(
        enforcement_test,
        "declaredCanariesSurviveProductionLoadEditSave",
    )
    for token in [
        "VideoProject.readProjectPackage",
        "Self.saveAs(document, to: destination)",
        "ProjectJSONCodec.encode",
    ]:
        if token not in enforcement_body:
            raise ContractError(
                f"{enforcement_test}: missing enforcement token {token!r}"
            )
    if "VideoProject.writeProjectPackage" in enforcement_body:
        raise ContractError(
            f"{enforcement_test}: runtime canary must not use the static package writer"
        )
    save_as_body = swift_function_body(enforcement_test, "saveAs")
    for token in ["document.save(", ".saveAsOperation"]:
        if token not in save_as_body:
            raise ContractError(
                f"{enforcement_test}: saveAs helper missing token {token!r}"
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


def windows_bootstrap_contract() -> None:
    toolchain = load_json("windows/toolchain.json")
    require_equal(
        "Windows toolchain contract version",
        toolchain["contractVersion"],
        CONTRACT_VERSION,
    )
    require_equal(
        "Windows minimum target",
        toolchain["target"],
        {
            "minimumOs": "Windows 10 22H2",
            "minimumBuild": 19045,
            "architecture": "x86_64",
            "graphicsBaseline": "Direct3D 11 Feature Level 11_0",
        },
    )
    build = toolchain["build"]
    expected_build = {
        "ciRunner": "windows-2022",
        "visualStudio": "2022 17.14",
        "visualStudioReferenceBuild": "17.14.37",
        "msvcToolset": "v143 14.44",
        "windowsSdkTarget": "10.0.26100.0",
        "windowsSdkServicingReference": "10.0.26100.8038",
        "cmakeMinimum": "3.31.0",
        "cmakeCiVersion": "3.31.6",
        "cmakeUpgradeCandidate": "4.3.3",
        "ninjaReservedVersion": "1.13.2",
        "cppStandard": 20,
    }
    require_equal("Windows build toolchain", build, expected_build)

    manager = toolchain["dependencyManager"]
    require_equal("dependency manager", manager["name"], "vcpkg manifest mode")
    require_equal("vcpkg release", manager["release"], "2026.06.24")
    baseline = manager["builtinBaseline"]
    require_equal(
        "vcpkg builtin baseline",
        baseline,
        "cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3",
    )
    require_equal("vcpkg metrics", manager["metrics"], "disabled")
    require_equal(
        "vcpkg activation",
        manager["activation"],
        "Active for the FFmpeg media prototype",
    )
    manifest = load_json("vcpkg.json")
    require_equal("vcpkg manifest name", manifest["name"], "palmier-pro-windows")
    require_equal("vcpkg manifest baseline", manifest["builtin-baseline"], baseline)
    require_equal(
        "vcpkg FFmpeg dependency",
        manifest["dependencies"],
        [
            {
                "name": "ffmpeg",
                "default-features": False,
                "features": ["avcodec", "avformat", "swresample", "swscale"],
            }
        ],
    )

    dependencies = {
        entry["name"]: [entry["version"], entry["status"]]
        for entry in toolchain["prototypeDependencies"]
    }
    require_equal(
        "Windows prototype dependency locks",
        dependencies,
        {
            "Qt": ["6.10.3", "prototype-locked"],
            "FFmpeg": ["8.1.2", "prototype-locked"],
            "ONNX Runtime": ["1.28.0", "prototype-locked"],
            "Inno Setup": ["7.0.2", "prototype-locked"],
        },
    )
    qt = next(
        entry for entry in toolchain["prototypeDependencies"] if entry["name"] == "Qt"
    )
    require_equal(
        "Qt shell runtime module lock",
        qt["runtimeModules"],
        [
            "Core",
            "Gui",
            "Qml",
            "Quick",
            "QuickControls2",
            "QuickDialogs2",
            "Concurrent",
        ],
    )
    require_equal("Qt shell test module lock", qt["testModules"], ["Test"])
    require_equal(
        "Qt CI acquisition lock",
        qt["ciAcquisition"],
        {
            "installer": "aqtinstall",
            "installerVersion": "3.3.0",
            "archiveHelper": "py7zr",
            "archiveHelperVersion": "1.0.0",
            "archive": "win64_msvc2022_64",
        },
    )

    cmake = read_text("CMakeLists.txt")
    for token in [
        "cmake_minimum_required(VERSION 3.31)",
        "project(PalmierProWindowsContracts LANGUAGES CXX)",
        "if(NOT WIN32)",
        "if(NOT MSVC)",
        "if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)",
        "include(CTest)",
        "add_subdirectory(windows/contract-probe)",
    ]:
        if token not in cmake:
            raise ContractError(f"CMake bootstrap missing token {token!r}")

    presets = load_json("CMakePresets.json")
    require_equal("CMake preset schema version", presets["version"], 6)
    configure = presets["configurePresets"][0]
    require_equal("CMake generator", configure["generator"], "Visual Studio 17 2022")
    require_equal("CMake architecture", configure["architecture"], "x64")
    require_equal("CMake toolset", configure["toolset"], "v143")
    require_equal(
        "CMake SDK target",
        configure["cacheVariables"]["CMAKE_SYSTEM_VERSION"],
        "10.0.26100.0",
    )
    require_equal(
        "CMake preset minimum",
        presets["cmakeMinimumRequired"],
        {"major": 3, "minor": 31, "patch": 0},
    )
    ffmpeg_configure = next(
        preset
        for preset in presets["configurePresets"]
        if preset["name"] == "windows-msvc-x64-ffmpeg"
    )
    require_equal(
        "FFmpeg CMake preset variables",
        ffmpeg_configure["cacheVariables"],
        {
            "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
            "PALMIER_ENABLE_FFMPEG_PROTOTYPE": "ON",
            "VCPKG_APPLOCAL_DEPS": "ON",
            "VCPKG_MANIFEST_INSTALL": "ON",
            "VCPKG_TARGET_TRIPLET": "x64-windows",
        },
    )

    probe_cmake = read_text("windows/contract-probe/CMakeLists.txt")
    for token in [
        "palmier_contract_probe_core",
        "palmier_contract_probe",
        "/W4 /WX /permissive- /Zc:__cplusplus /utf-8",
        "contract_probe.repository_v1",
        "add_probe_failure(rejects_duplicate_version",
        "contract_probe.unicode_path",
        "add_probe_failure(unicode_missing_file",
        "add_probe_failure(rejects_above_unicode_range",
    ]:
        if token not in probe_cmake:
            raise ContractError(f"Windows probe CMake missing token {token!r}")

    workflow = read_text(".github/workflows/windows-contracts.yml")
    for token in [
        "runs-on: windows-2022",
        "actions/checkout@v6",
        "actions/setup-python@v6",
        "cmake version 3.31.6",
        "^17\\.14\\.",
        "^14\\.44\\.",
        "Windows Kits\\10\\Include\\10.0.26100.0",
        "cmake --preset windows-msvc-x64",
        "cmake --build --preset windows-msvc-x64-release --parallel",
        "ctest --preset windows-msvc-x64-release",
    ]:
        if token not in workflow:
            raise ContractError(f"Windows workflow missing token {token!r}")

    adr = read_text(
        "docs/windows/adr/0007-windows-toolchain-and-prototype-dependencies.md"
    )
    for version in [
        "Visual Studio 2022 17.14",
        "MSVC v143 14.44",
        "Windows SDK 10.0.26100.0",
        "CMake 3.31.6",
        "Qt 6.10.3",
        "FFmpeg 8.1.2",
        "ONNX Runtime 1.28.0",
        "Inno Setup 7.0.2",
    ]:
        if version not in adr:
            raise ContractError(f"Windows toolchain ADR missing {version!r}")

    media_cmake = read_text("windows/media-ffmpeg/CMakeLists.txt")
    media_header = read_text(
        "windows/media-ffmpeg/include/palmier/media/ffmpeg_media_reader.hpp"
    )
    media_source = read_text("windows/media-ffmpeg/ffmpeg_media_reader.cpp")
    media_workflow = read_text(".github/workflows/windows-media-prototype.yml")
    for token in [
        'FFMPEG_VERSION VERSION_EQUAL "8.1.2"',
        "media_ffmpeg.direct_api",
        "SYSTEM INTERFACE ${FFMPEG_INCLUDE_DIRS}",
        "/W4 /WX /permissive- /Zc:__cplusplus /utf-8",
    ]:
        if token not in media_cmake:
            raise ContractError(f"FFmpeg CMake missing token {token!r}")
    for token in [
        "maximumPixels",
        "maximumPacketsBeforeFrame",
        "std::stop_token cancellation",
        "rgba8",
        "AlphaMode",
        "DecodedAudioBlock",
        "FfmpegAudioFrameReader",
        "maximumAudioFramesPerBlock",
        "unsupportedInputProtocol",
        "unsupportedColorMetadata",
        "unsupportedDisplayTransform",
    ]:
        if token not in media_header:
            raise ContractError(f"FFmpeg API missing token {token!r}")
    for token in [
        "AVIOInterruptCB",
        "avcodec_send_packet",
        "avcodec_receive_frame",
        "AV_PIX_FMT_FLAG_HWACCEL",
        "AV_FRAME_FLAG_CORRUPT",
        "alpha_mode",
        "av_display_rotation_get",
        "max_pixels",
        "sws_scale",
        "swr_alloc_set_opts2",
        "swr_get_out_samples",
        "swr_convert",
        "swresample_version",
    ]:
        if token not in media_source:
            raise ContractError(f"FFmpeg source missing token {token!r}")
    for token in [
        "VCPKG_DISABLE_METRICS=1",
        "VCPKG_BINARY_SOURCES=clear;files,$binaryCache,readwrite",
        "cd61e1e26a038e82d6550a3ebbe0fbbfe7da78e3",
        "cmake --preset windows-msvc-x64-ffmpeg",
        "ctest --preset windows-msvc-x64-ffmpeg-release",
    ]:
        if token not in media_workflow:
            raise ContractError(f"FFmpeg workflow missing token {token!r}")


def windows_project_reader_contract() -> None:
    projection = load_json("contracts/project/v1/reader-projection.json")
    require_equal(
        "project reader contract version",
        projection["contractVersion"],
        CONTRACT_VERSION,
    )
    require_equal(
        "project reader source commit",
        projection["sourceCommit"],
        "5a3695cda5a37db6af6fc495187fd05f5fec4dd3",
    )
    require_equal("project reader status", projection["status"], "read-only-prototype")
    require_equal("project reader source", projection["sourceRepresentation"], "full-json-dom")
    require_equal("project writer availability", projection["writerAvailable"], False)
    require_equal("project projection completion", projection["projectionComplete"], False)
    require_equal("project root kinds", projection["rootKinds"], ["current", "legacy"])
    require_equal(
        "project ID origins",
        projection["idOrigins"],
        ["persisted", "synthesized"],
    )
    require_equal(
        "project reader diagnostic codes",
        projection["diagnosticCodes"],
        [
            "synthesizedId",
            "invalidOptionalDefaulted",
            "invalidActiveTimelineId",
            "invalidOpenTimelineId",
            "duplicateStableId",
            "unsafeFrameRange",
        ],
    )
    require_equal(
        "project reader fatal codes",
        projection["fatalErrorCodes"],
        [
            "emptyTimelines",
            "invalidGeneratedId",
            "invalidRequiredValue",
            "integerOutOfRange",
            "missingRequiredField",
            "unsupportedRequiredEnum",
            "wrongRequiredType",
        ],
    )

    model_header = read_text("core/project/include/palmier/project/project.hpp")
    reader_source = read_text("core/project/serialization/project_reader.cpp")
    json_source = read_text("core/project/serialization/json_document.cpp")
    oracle = read_text("tools/project_reader_oracle.py")
    core_cmake = read_text("core/project/CMakeLists.txt")
    probe_cmake = read_text("windows/contract-probe/CMakeLists.txt")
    for token in [
        "palmier::json::Value source_",
        "EntityIdOrigin origin",
        "ProjectDocumentDisposition disposition() const noexcept",
    ]:
        if token not in model_header:
            raise ContractError(f"project reader model missing token {token!r}")
    for token in [
        "diagnosticsCheckpoint",
        "diagnostics_.resize(diagnosticsCheckpoint)",
        "normalizedModelJson",
        'document.rootKind() == RootKind::current ? "current" : "legacy"',
    ]:
        if token not in reader_source:
            raise ContractError(f"project reader source missing token {token!r}")
    for token in [
        "maximumDepth = 256",
        "duplicate object key",
        "invalid UTF-8 leading byte",
        "number().lexeme",
    ]:
        if token not in json_source:
            raise ContractError(f"JSON DOM missing token {token!r}")
    for token in [
        "object_pairs_hook=reject_duplicate_pairs",
        "parse_float=Decimal",
        "compare_types(expected, actual)",
        "synthesized-",
        "--expected-error",
    ]:
        if token not in oracle:
            raise ContractError(f"project reader oracle missing token {token!r}")
    if "project_reader.direct_api" not in core_cmake:
        raise ContractError("project reader core CMake is missing the direct API test")
    for token in [
        "function(add_project_differential name input)",
        "current-multitimeline.palmier/project.json",
        "legacy-bare-timeline.palmier/project.json",
        "unknown-fields.palmier/project.json",
        "project-missing-ids.json",
        "project-diagnostics.json",
        "project-fps-string.json",
        "project-fps-fraction.json",
        "project-fps-overflow.json",
        "project-fps-zero.json",
        "project-fps-boolean.json",
        "project-missing-tracks.json",
        "project-tracks-wrong-type.json",
        "project-number-boundaries.json",
        "function(add_project_error_differential name input expected_error)",
        "add_project_differential(unicode_path",
        "project_reader.rejects_malformed_json",
        "project_reader.rejects_missing_file",
    ]:
        if token not in probe_cmake:
            raise ContractError(f"project reader CMake missing token {token!r}")

    adr = read_text("docs/windows/adr/0008-read-only-project-document.md")
    for token in [
        "strict, full JSON",
        "never falls back to legacy decoding",
        "injected generator",
        "No Windows writer",
    ]:
        if token not in adr:
            raise ContractError(f"project reader ADR missing token {token!r}")


def windows_render_plan_contract() -> None:
    contract = load_json("contracts/render/v1/render-plan.json")
    require_equal(
        "render plan contract version",
        contract["contractVersion"],
        CONTRACT_VERSION,
    )
    require_equal(
        "render plan source commit",
        contract["sourceCommit"],
        "b88d553231327df734758ac1f6aaf6f4a2b79b7e",
    )
    require_equal("render plan status", contract["status"], "synthetic-reference-spike")
    require_equal("render plan immutability", contract["immutable"], True)
    require_equal("render plan layer order", contract["layerOrder"], "bottom-to-top")
    require_equal(
        "render plan semantic evidence",
        contract["semanticEvidence"],
        "self-consistent-cpu-warp-only",
    )
    require_equal("Swift pixel golden availability", contract["swiftPixelGoldenAvailable"], False)
    require_equal("render plan maximum pixels", contract["canvas"]["maximumPixelCount"], 8_294_400)
    require_equal(
        "render plan entry points",
        contract["previewExportEntryPoints"],
        ["renderPreviewFrame", "renderExportFrame"],
    )
    require_equal(
        "render plan supported blend modes",
        contract["supported"]["blendModes"],
        ["normal"],
    )
    require_equal("render plan maximum layers", contract["supported"]["maximumLayerCount"], 256)
    require_equal(
        "render plan maximum composite samples",
        contract["supported"]["maximumCompositeSamples"],
        67_108_864,
    )
    require_equal(
        "render plan maximum resolved source pixels",
        contract["supported"]["maximumResolvedSourcePixelsPerRender"],
        67_108_864,
    )
    require_equal("render plan numeric precision", contract["supported"]["numericPrecision"], "float32")
    require_equal(
        "render plan rotation normalization",
        contract["supported"]["rotationNormalization"],
        "ieee-remainder-360-range-minus180-to180",
    )
    require_equal(
        "render plan exposure range",
        contract["supported"]["effects"]["color.exposure"],
        {"parameter": "ev", "minimum": -3, "maximum": 3},
    )
    require_equal(
        "render plan project compiler availability",
        contract["projectCompilerAvailable"],
        False,
    )
    require_equal(
        "decoded source adapter",
        contract["decodedSourceAdapter"],
        {
            "available": True,
            "input": "ffmpeg-decoded-rgba8",
            "output": "top-left-straight-srgb-rgba32-float",
            "displayTransform": "baked-cardinal-counterclockwise",
            "acceptedAlphaModes": ["opaque", "straight"],
            "refusedAlphaModes": ["unspecified", "premultiplied"],
            "rowStorage": "explicit-stride-exact-height",
        },
    )
    require_equal(
        "render plan backend concurrency",
        contract["rendererConcurrency"],
        "serialized-immediate-context-per-backend-instance",
    )
    require_equal(
        "render plan resolver lifetime",
        contract["resolverLifetime"],
        "immutable-source-through-render-call",
    )
    required_refusals = {
        "same-track-overlap",
        "speed-not-one",
        "variable-frame-rate",
        "source-fps-mismatch",
        "crop",
        "flip",
        "dynamic-keyframes",
        "fades",
        "non-normal-blend",
        "text",
        "nested-timeline",
        "lottie",
        "unsupported-effect",
        "hdr",
        "mixed-or-unknown-color-metadata",
        "unknown-alpha-mode",
    }
    require_equal(
        "render plan hard refusals",
        set(contract["hardRefusalsBeforeProjectIntegration"]),
        required_refusals,
    )
    require_equal(
        "render plan runtime gates",
        contract["runtimeGates"],
        {
            "windowsServer2022Warp": "required-ci",
            "ffmpegDecodedSourceAdapter": "required-ffmpeg-ci",
            "windows10Build19045": "pending-clean-vm",
            "physicalGpuVendors": "pending-hardware-matrix",
            "ffmpegDecodeEncode": "pending",
            "wasapiAudio": "pending",
        },
    )

    header = read_text("core/render/include/palmier/render/render_plan.hpp")
    plan_source = read_text("core/render/render_plan.cpp")
    cpu_source = read_text("core/render/cpu_renderer.cpp")
    warp_source = read_text("windows/render-d3d11/d3d11_warp_renderer.cpp")
    warp_cmake = read_text("windows/render-d3d11/CMakeLists.txt")
    root_cmake = read_text("CMakeLists.txt")
    for token in [
        "private:",
        "const std::vector<RenderLayer>& layers() const noexcept",
        "RenderedFrame renderPreviewFrame(",
        "RenderedFrame renderExportFrame(",
    ]:
        if token not in header:
            raise ContractError(f"render plan header missing token {token!r}")
    for token in [
        '"overlappingTrackLayers"',
        '"invalidOpacity"',
        '"invalidExposure"',
        '"canvasBudgetExceeded"',
        '"layerBudgetExceeded"',
        '"compositeBudgetExceeded"',
        '"sourceWorkBudgetExceeded"',
        "std::remainder(",
        "resolveAndValidateSourceFrames(",
        "return backend.render(plan, resolveFrame);",
        "validateSourceFrame",
    ]:
        if token not in plan_source:
            raise ContractError(f"render plan source missing token {token!r}")
    for token in [
        "srgbToLinear",
        "linearToSrgb",
        "std::exp2",
        "sourceOver",
        "pixels.assign(pixelCount, {0, 0, 0, 1})",
        "!std::isfinite(sourceU)",
    ]:
        if token not in cpu_source:
            raise ContractError(f"CPU reference renderer missing token {token!r}")
    for token in [
        "D3D_DRIVER_TYPE_WARP",
        "D3D_FEATURE_LEVEL_11_0",
        "D3DCompile(",
        "D3DCOMPILE_WARNINGS_ARE_ERRORS",
        "DXGI_FORMAT_R32G32B32A32_FLOAT",
        "D3D11_FORMAT_SUPPORT_SHADER_LOAD",
        "D3D11_FORMAT_SUPPORT_BLENDABLE",
        "D3D11_USAGE_STAGING",
        "mapped.value().RowPitch",
        "ScopedTextureMap",
        "std::scoped_lock lock(renderMutex_)",
    ]:
        if token not in warp_source:
            raise ContractError(f"D3D11 WARP renderer missing token {token!r}")
    for token in [
        "render_d3d11.warp_parity",
        "RUN_SERIAL TRUE TIMEOUT 60",
        "d3d11 d3dcompiler",
    ]:
        if token not in warp_cmake:
            raise ContractError(f"D3D11 WARP CMake missing token {token!r}")
    for token in [
        "add_subdirectory(core/render)",
        "add_subdirectory(windows/render-d3d11)",
    ]:
        if token not in root_cmake:
            raise ContractError(f"root CMake missing token {token!r}")

    adr = read_text("docs/windows/adr/0009-render-plan-and-d3d11-warp-reference.md")
    for token in [
        "does not compile a Palmier project into a render plan",
        "no application path may silently construct",
        "hard refusals, not fallback behavior",
        "does not prove Windows 10 build 19045",
        "not yet an oracle for the shipping Swift",
        "Swift-generated BGRA8 goldens are still required",
    ]:
        if token not in adr:
            raise ContractError(f"render plan ADR missing token {token!r}")


def windows_media_render_adapter_contract() -> None:
    root_cmake = read_text("CMakeLists.txt")
    adapter_cmake = read_text("windows/media-render/CMakeLists.txt")
    adapter_header = read_text(
        "windows/media-render/include/palmier/media/render_source_adapter.hpp"
    )
    adapter_source = read_text("windows/media-render/render_source_adapter.cpp")
    adapter_hooks = read_text(
        "windows/media-render/internal/render_source_adapter_testing.hpp"
    )
    adapter_tests = read_text(
        "windows/media-render/tests/render_source_adapter_tests.cpp"
    )
    media_tests = read_text(
        "windows/media-ffmpeg/tests/ffmpeg_media_reader_tests.cpp"
    )
    for token in [
        "add_subdirectory(windows/media-render)",
        "PALMIER_ENABLE_FFMPEG_PROTOTYPE",
    ]:
        if token not in root_cmake:
            raise ContractError(f"root CMake missing media-render token {token!r}")
    for token in [
        "palmier_media_render",
        "palmier_media_ffmpeg palmier_render",
        "palmier_render_d3d11",
        "media_render.adapter_parity",
        "RUN_SERIAL TRUE TIMEOUT 60",
        "/W4 /WX /permissive- /Zc:__cplusplus /utf-8",
    ]:
        if token not in adapter_cmake:
            raise ContractError(f"media-render CMake missing token {token!r}")
    for token in [
        "class RenderSourceError final",
        "render::SourceFrame makeRenderSourceFrame(",
        "std::stop_token cancellation",
    ]:
        if token not in adapter_header:
            raise ContractError(f"media-render API missing token {token!r}")
    for token in [
        "validateSourceFrameDimensions(",
        "decoded.rowBytes * sourceHeight != decoded.rgba8.size()",
        "AlphaMode::opaque",
        "AlphaMode::straight",
        '"unsupportedAlphaMode"',
        '"inconsistentOpaqueAlpha"',
        "destinationIndex(",
        "cancellation.stop_requested()",
    ]:
        if token not in adapter_source:
            raise ContractError(f"media-render source missing token {token!r}")
    for token in [
        "RenderSourceAdapterHooks",
        "allocatePixels",
        "didConvertRow",
        "willPublish",
    ]:
        if token not in adapter_hooks:
            raise ContractError(f"media-render test hooks missing token {token!r}")
    for token in [
        "cardinalRotationsAndPaddedStride",
        "refusalBoundaries",
        "deterministicCancellationBoundaries",
        "allocationAttempted",
        "unsupportedPrimaries",
        "unsupportedTransfer",
        "unsupportedMatrix",
        "unsupportedRange",
        "unspecifiedRange",
        "adapterFeedsSharedPreviewAndExport",
        "renderPreviewFrame",
        "renderExportFrame",
        "D3d11WarpRenderer",
    ]:
        if token not in adapter_tests:
            raise ContractError(f"media-render tests missing token {token!r}")
    if "makeRenderSourceFrame(frame)" not in media_tests:
        raise ContractError("real FFmpeg decode is not checked at the adapter boundary")

    adr = read_text(
        "docs/windows/adr/0012-decoded-frame-render-source-adapter.md"
    )
    for token in [
        "resolver never performs file I/O",
        "Unspecified and premultiplied alpha are hard refusals",
        "does not define a mapping for arbitrary RenderPlan",
        "proves successfully adapted real decoded frames",
    ]:
        if token not in adr:
            raise ContractError(f"media-render ADR missing token {token!r}")


def validate_presentation_video_contract(contract: dict[str, Any]) -> None:
    require_equal("presentation-video version", contract.get("version"), 1)
    require_equal(
        "presentation-video owner",
        contract.get("owner"),
        "windows/media-session",
    )
    require_equal(
        "presentation-video state owner",
        contract.get("stateOwner"),
        "one serial media worker",
    )
    require_equal(
        "presentation-video operations",
        contract.get("operations"),
        ["start", "enqueue", "dequeue", "select", "cancel"],
    )
    require_equal(
        "presentation-video outcomes",
        contract.get("outcomes"),
        ["changed", "noOp", "stale", "cancelled", "refused"],
    )
    require_equal(
        "presentation-video reasons",
        contract.get("reasons"),
        [
            "none",
            "staleGeneration",
            "stateChanged",
            "operationCancelled",
            "generationCancelled",
            "staleClock",
            "frameEarly",
            "frameCapacity",
            "byteCapacity",
        ],
    )
    require_equal(
        "presentation-video receipt fields",
        contract.get("receiptFields"),
        [
            "operation",
            "outcome",
            "reason",
            "generation",
            "revision",
            "queuedFrames",
            "queuedBytes",
        ],
    )
    require_equal(
        "presentation-video limits",
        contract.get("limits"),
        {"maximumFrames": 8, "maximumBytes": 268435456},
    )
    require_equal(
        "presentation-video errors",
        contract.get("errors"),
        [
            "invalidLimits",
            "invalidAdapter",
            "invalidAdapterResult",
            "revisionOverflow",
            "invalidGeneration",
            "invalidClock",
            "invalidClockSourceTimeBase",
            "clockPositionDiscontinuity",
            "clockArithmeticOverflow",
            "missingPresentationTimestamp",
            "invalidTimeBase",
            "changedTimeBase",
            "nonIncreasingTimestamp",
            "invalidFrameDimensions",
            "frameByteOverflow",
        ],
    )
    require_equal(
        "presentation-video invariants",
        contract.get("invariants"),
        {
            "generation": "positive and monotonically increasing",
            "presentationTimestamp": "required and strictly increasing within one generation",
            "timeBase": "positive and constant within one generation",
            "capacity": "refuse before adaptation when either frame or byte budget would be exceeded",
            "cancellation": "operation cancellation rejects one enqueue; generation cancellation clears the queue and rejects every late frame",
            "staleResult": "a result for any non-current generation or changed revision never mutates the buffer",
            "dequeue": "frames leave in accepted presentation order",
            "clock": "selection requires one generation-matched device anchor and sample, a positive source-media time base, and checked integer timeline mapping",
            "selection": "hold an early frame without mutation; otherwise atomically return the latest due frame, drop only older due frames, and advance revision once",
        },
    )
    require_equal(
        "presentation-video exclusions",
        contract.get("excluded"),
        [
            "file I/O or decode",
            "thread creation",
            "physical-device audio synchronization and drift correction",
            "seek or rate conversion",
            "interactive presentation",
            "cache eviction",
            "Windows 10 runtime certification",
        ],
    )


def windows_presentation_video_buffer_contract() -> None:
    root_cmake = read_text("CMakeLists.txt")
    session_cmake = read_text("windows/media-session/CMakeLists.txt")
    header = read_text(
        "windows/media-session/include/palmier/media/presentation_video_buffer.hpp"
    )
    source = read_text("windows/media-session/presentation_video_buffer.cpp")
    test_access = read_text(
        "windows/media-session/internal/presentation_video_buffer_testing.hpp"
    )
    tests = read_text(
        "windows/media-session/tests/presentation_video_buffer_tests.cpp"
    )
    contract = load_json("contracts/media/v1/presentation-video-buffer.json")

    if "add_subdirectory(windows/media-session)" not in root_cmake:
        raise ContractError("root CMake missing media-session target")
    for token in [
        "palmier_media_session",
        "palmier_media_render",
        "media_session.presentation_video_buffer",
        "PROPERTIES TIMEOUT 30",
        '"${CMAKE_CURRENT_SOURCE_DIR}/../media-render"',
        '"${CMAKE_CURRENT_SOURCE_DIR}"',
        "/W4 /WX /permissive- /Zc:__cplusplus /utf-8",
    ]:
        if token not in session_cmake:
            raise ContractError(f"media-session CMake missing token {token!r}")
    for token in [
        "PresentationVideoBufferLimits",
        "PresentationVideoOperation",
        "PresentationVideoOutcome",
        "PresentationVideoReason",
        "PresentationVideoReceipt",
        "PresentedVideoFrame",
        "PresentationVideoTake",
        "PresentationVideoClockPosition",
        "PresentationVideoSelection",
        "class PresentationVideoBuffer final",
        "using FrameAdapter = std::function<render::SourceFrame(",
        "using AdaptedFrameCheckpoint = std::function<void()>;",
        "PresentationVideoBuffer(const PresentationVideoBuffer&) = delete;",
        "PresentationVideoBuffer(PresentationVideoBuffer&&) = delete;",
        "maximumFrames{8}",
        "maximumBytes{256ULL * 1024ULL * 1024ULL}",
        "std::stop_token cancellation",
        "PresentationVideoSelection select(",
        "std::uint64_t revision() const noexcept",
    ]:
        if token not in header:
            raise ContractError(f"presentation-video API missing token {token!r}")
    for token in [
        "requireValidGeneration(generation)",
        "generation != generation_",
        "decoded.presentationTimestamp",
        "sameTimeBase(*timeBase_, decoded.timeBase)",
        "*decoded.presentationTimestamp <= *lastAcceptedTimestamp_",
        "frames_.size() >= limits_.maximumFrames",
        "frameBytes > limits_.maximumBytes - queuedBytes_",
        "frameAdapter_(decoded, cancellation)",
        "adaptedFrameCheckpoint_",
        "validateAdapterResult(decoded, *source, frameBytes)",
        "render::validateSourceFrame(source, \"/source\")",
        "revalidationReceipt(generation, expectedRevision)",
        "revision_ != expectedRevision",
        "requireRevisionCapacity()",
        "audio::timelineFrame(",
        "targetPresentationTimestamp(",
        "_umul128",
        "_udiv128",
        "wideProduct(",
        "UnsignedDivision divide(",
        "fractionSumCarries(",
        "cancelFactor(",
        "clock.deviceSample.devicePosition < clock.deviceAnchor.devicePosition",
        "PresentationVideoReason::frameEarly",
        "frames_[dueFrames - 1]",
        "dueFrames - 1",
        'error.code == "cancelled"',
        "frames_.clear()",
    ]:
        if token not in source:
            raise ContractError(f"presentation-video source missing token {token!r}")
    for token in [
        "validatesLimitsAndGenerations",
        "revisionOverflowPreservesState",
        "ordersAndDequeuesFrames",
        "selectsLatestDueFrameAndHoldsEarlyFrames",
        "mapsSourceOriginsAndNegativeFractionsExactly",
        "staleClockAndRevisionNeverMutateSelection",
        "invalidClockAndRevisionOverflowPreserveFrames",
        "validatesTimestampAndTimeBaseBeforeMutation",
        "enforcesCapacityBeforeAdaptation",
        "adapterFailurePreservesQueue",
        "invalidAdapterResultPreservesQueue",
        "cancellationDuringAndAfterAdaptationPreservesQueue",
        "reentrantGenerationChangeRejectsOuterCommit",
        "adapterReentrancyRejectsOuterCommit",
        "reentrantCancellationRejectsOuterCommit",
        "reentrantRevisionChangeRejectsOuterCommit",
        "rejectsStaleAndCancelledResultsBeforeValidation",
        "RenderSourceAdapterHooks",
        "stopDuringOnce",
        "stopAfterOnce",
        "static_assert(!std::is_copy_constructible_v<PresentationVideoBuffer>)",
        "PresentationVideoBufferTestAccess::setRevision",
        "PresentationVideoErrorCode::revisionOverflow",
        "PresentationVideoReason::frameCapacity",
        "PresentationVideoReason::byteCapacity",
        "PresentationVideoReason::stateChanged",
        "PresentationVideoReason::staleClock",
        "PresentationVideoReason::frameEarly",
        "sourceTimeBase = {1, 30}",
        "clockPosition(1, 3'200, 10)",
        "clockPosition(1, 1'209'600'000, 0, {1, 90'000})",
        "2'268'000'000",
        "representable target with a wide denominator overflowed",
        "wide-denominator fractional carry was lost",
        "PresentationVideoOutcome::stale",
        "PresentationVideoOutcome::cancelled",
    ]:
        if token not in tests:
            raise ContractError(f"presentation-video tests missing token {token!r}")

    for token in [
        "class PresentationVideoBufferTestAccess final",
        "return PresentationVideoBuffer(",
        "buffer.revision_ = revision",
    ]:
        if token not in test_access:
            raise ContractError(f"presentation-video test access missing token {token!r}")

    if source.count("cancellation.stop_requested()") < 2:
        raise ContractError("presentation-video source lost a cancellation checkpoint")
    def validate_header_shape(candidate: str) -> None:
        enum_contracts = {
            "PresentationVideoOperation": contract.get("operations", []),
            "PresentationVideoOutcome": contract.get("outcomes", []),
            "PresentationVideoReason": contract.get("reasons", []),
            "PresentationVideoErrorCode": contract.get("errors", []),
        }
        for enum_name, expected in enum_contracts.items():
            require_equal(
                f"presentation-video C++ {enum_name}",
                cpp_enum_cases(candidate, enum_name),
                expected,
            )
        require_equal(
            "presentation-video C++ receipt fields",
            cpp_stored_fields(candidate, "PresentationVideoReceipt"),
            contract.get("receiptFields", []),
        )
        require_equal(
            "presentation-video C++ clock fields",
            cpp_stored_fields(candidate, "PresentationVideoClockPosition"),
            contract.get("clockFields", []),
        )
        require_equal(
            "presentation-video C++ selection fields",
            cpp_stored_fields(candidate, "PresentationVideoSelection"),
            contract.get("selectionFields", []),
        )

    validate_header_shape(header)
    extra_reason = header.replace(
        "enum class PresentationVideoReason {",
        "enum class PresentationVideoReason {\n    inventedReason,",
        1,
    )
    expect_failure(
        "presentation-video C++ extra reason",
        lambda: validate_header_shape(extra_reason),
    )
    extra_receipt = header.replace(
        "struct PresentationVideoReceipt final {",
        "struct PresentationVideoReceipt final {\n    std::uint64_t inventedField;",
        1,
    )
    expect_failure(
        "presentation-video C++ extra receipt field",
        lambda: validate_header_shape(extra_receipt),
    )

    validate_presentation_video_contract(contract)
    invalid_owner = dict(contract)
    invalid_owner["stateOwner"] = "concurrent callers"
    expect_failure(
        "presentation-video state owner",
        lambda: validate_presentation_video_contract(invalid_owner),
    )
    invalid_receipts = dict(contract)
    invalid_receipts["receiptFields"] = contract["receiptFields"][:-1]
    expect_failure(
        "presentation-video receipt fields",
        lambda: validate_presentation_video_contract(invalid_receipts),
    )
    invalid_reasons = dict(contract)
    invalid_reasons["reasons"] = [
        value for value in contract["reasons"] if value != "stateChanged"
    ]
    expect_failure(
        "presentation-video state-change reason",
        lambda: validate_presentation_video_contract(invalid_reasons),
    )
    invalid_errors = dict(contract)
    invalid_errors["errors"] = [
        value for value in contract["errors"] if value != "revisionOverflow"
    ]
    expect_failure(
        "presentation-video revision overflow",
        lambda: validate_presentation_video_contract(invalid_errors),
    )

    adr = read_text("docs/windows/adr/0015-presentation-video-buffer.md")
    for token in [
        "media worker owns a `PresentationVideoBuffer`",
        "receipt without validation, allocation, or mutation",
        "strictly increasing PTS values",
        "normalized RGBA32F byte budgets",
        "single-owner and unsynchronized",
        "Every mutation advances a checked revision",
        "revalidates generation, admission state, and revision",
        "ADR 0016 composes this buffer with a stateful FFmpeg cursor",
    ]:
        if token not in adr:
            raise ContractError(f"presentation-video ADR missing token {token!r}")
    selection_adr = read_text(
        "docs/windows/adr/0019-audio-clock-video-selection.md"
    )
    for token in [
        "first atomic audio-clock selection",
        "common media origin",
        "holds that early frame",
        "newest due frame",
        "advances revision exactly once",
        "0, 1,024, and 2,048",
        "do not prove a swap chain",
    ]:
        if token not in selection_adr:
            raise ContractError(f"video-selection ADR missing token {token!r}")


def validate_ffmpeg_presentation_pipeline_contract(
    contract: dict[str, Any],
) -> None:
    require_equal("FFmpeg pipeline version", contract.get("version"), 1)
    require_equal(
        "FFmpeg pipeline owner",
        contract.get("owner"),
        "windows/media-session",
    )
    require_equal(
        "FFmpeg pipeline state owner",
        contract.get("stateOwner"),
        "one serial media executor",
    )
    require_equal(
        "FFmpeg pipeline states",
        contract.get("states"),
        ["idle", "ready", "blocked", "endOfStream", "cancelled", "failed"],
    )
    require_equal(
        "FFmpeg pipeline receipt fields",
        contract.get("fillReceiptFields"),
        [
            "generation",
            "state",
            "outcome",
            "admittedFrames",
            "hasPendingFrame",
            "queuedFrames",
            "queuedBytes",
        ],
    )
    require_equal(
        "FFmpeg pipeline limits",
        contract.get("limits"),
        {
            "maximumFrames": 8,
            "maximumBytes": 268435456,
            "maximumPixels": 8294400,
            "maximumFramesPerFill": 4,
            "maximumConfigurableFramesPerFill": 32,
        },
    )
    require_equal(
        "FFmpeg pipeline errors",
        contract.get("errors"),
        [
            "decodeLimitExceedsRenderBudget",
            "notStarted",
            "terminalState",
            "invariantViolation",
            "changedInputWithinGeneration",
            "invalidFillBudget",
        ],
    )
    require_equal(
        "FFmpeg pipeline fixture",
        contract.get("fixture"),
        {
            "name": "qtrleOpaqueThreeFrames",
            "sha256": "14290e9b2efb26f4ca1e2680b9a7589e141577cea53ceab4b5adf583a98a79e8",
            "codec": "qtrle",
            "pixelFormat": "rgb24",
            "width": 3,
            "height": 2,
            "frameCount": 3,
            "timeBase": [1, 10240],
            "presentationTimestamps": [0, 1024, 2048],
            "alphaMode": "opaque",
        },
    )
    require_equal(
        "FFmpeg pipeline invariants",
        contract.get("invariants"),
        {
            "reader": "one stateful reader owns one demuxer, software decoder, packet, frame, and reusable conversion context",
            "decoderProtocol": "receive before supply; retain a packet when send reports EAGAIN; send drain exactly once; EOF is stable",
            "presentationOrder": "publish best-effort timestamps in decoder output order without guessing or reordering",
            "backpressure": "retain at most one decoded pending frame until bounded admission succeeds",
            "fillBudget": "one fill admits at most the configured 1 to 32 frames independent of queue capacity",
            "generation": "opening a replacement succeeds before the new generation clears queued or pending state",
            "inputIdentity": "the exact input path cannot change within one generation",
            "cancellation": "fill throws a cancelled media error after cancellation terminates and clears only the current generation",
            "handoff": "atomically select one immutable normalized frame from a generation and revision current audio clock before synchronous preview or export rendering",
            "selection": "the real three-frame fixture is selected at its exact PTS through the decode pump before CPU and WARP rendering",
        },
    )
    require_equal(
        "FFmpeg pipeline exclusions",
        contract.get("excluded"),
        [
            "seek and frame-rate conversion",
            "thread creation or executor scheduling",
            "physical-device audio synchronization and drift correction",
            "interactive swap-chain presentation",
            "hardware decode",
            "1080p performance qualification",
            "Windows 10 runtime certification",
        ],
    )


def windows_ffmpeg_presentation_pipeline_contract() -> None:
    ffmpeg_header = read_text(
        "windows/media-ffmpeg/include/palmier/media/ffmpeg_media_reader.hpp"
    )
    ffmpeg_source = read_text("windows/media-ffmpeg/ffmpeg_media_reader.cpp")
    ffmpeg_tests = read_text(
        "windows/media-ffmpeg/tests/ffmpeg_media_reader_tests.cpp"
    )
    fixtures = read_text("windows/media-ffmpeg/tests/media_test_fixtures.hpp")
    pump_header = read_text(
        "windows/media-session/include/palmier/media/presentation_video_decode_pump.hpp"
    )
    pump_source = read_text(
        "windows/media-session/presentation_video_decode_pump.cpp"
    )
    pump_test_access = read_text(
        "windows/media-session/internal/presentation_video_decode_pump_testing.hpp"
    )
    pipeline_tests = read_text(
        "windows/media-session/tests/ffmpeg_presentation_pipeline_tests.cpp"
    )
    cmake = read_text("windows/media-session/CMakeLists.txt")
    contract = load_json("contracts/media/v1/ffmpeg-presentation-pipeline.json")

    for token in [
        "class FfmpegVideoFrameReader final",
        "std::optional<DecodedVideoFrame> nextFrame(",
        "FfmpegVideoFrameReader(const FfmpegVideoFrameReader&) = delete;",
        "FfmpegVideoFrameReader(FfmpegVideoFrameReader&&) = delete;",
    ]:
        if token not in ffmpeg_header:
            raise ContractError(f"FFmpeg cursor API missing token {token!r}")
    for token in [
        "class FfmpegVideoFrameReader::Impl final",
        "sws_getCachedContext(",
        "packetPending_",
        "drainPending_",
        "drainSent_",
        "receiveMustProgress_",
        "terminalError_",
        "std::rethrow_exception(terminalError_)",
        'requireNotCancelled(cancellation, "after-decoder-setup")',
        "FfmpegVideoFrameReader reader(input, limits, cancellation)",
    ]:
        if token not in ffmpeg_source:
            raise ContractError(f"FFmpeg cursor source missing token {token!r}")
    for token in [
        "decodesPresentationOrderedFrames",
        "cancellationTerminatesCursor",
        "end of stream is not stable",
        "first-frame compatibility changed",
        "1'024, 2'048",
    ]:
        if token not in ffmpeg_tests:
            raise ContractError(f"FFmpeg cursor tests missing token {token!r}")

    fixture_match = re.search(
        r"qtrleOpaqueThreeFrames\s*=((?:\s*\"[^\"]*\")+)\s*;",
        fixtures,
    )
    if fixture_match is None:
        raise ContractError("three-frame FFmpeg fixture is missing")
    fixture_base64 = "".join(re.findall(r'\"([^\"]*)\"', fixture_match.group(1)))
    try:
        fixture_bytes = base64.b64decode(fixture_base64, validate=True)
    except ValueError as error:
        raise ContractError(f"three-frame FFmpeg fixture is invalid: {error}") from error
    require_equal(
        "three-frame FFmpeg fixture SHA-256",
        hashlib.sha256(fixture_bytes).hexdigest(),
        contract["fixture"]["sha256"],
    )

    for token in [
        "PresentationVideoDecodeLimits",
        "PresentationVideoDecodeState",
        "PresentationVideoFillReceipt",
        "class PresentationVideoDecodePump final",
        "std::unique_ptr<FfmpegVideoFrameReader> reader_",
        "std::optional<DecodedVideoFrame> pendingFrame_",
        "std::filesystem::path inputIdentity_",
        "maximumFramesPerFill{4}",
        "render::maximumRenderFramePixels",
        "PresentationVideoSelection select(",
        "std::uint64_t revision() const noexcept",
    ]:
        if token not in pump_header:
            raise ContractError(f"FFmpeg pipeline API missing token {token!r}")
    for token in [
        "generation == buffer_.generation()",
        "input != inputIdentity_",
        "auto nextReader = std::make_unique<FfmpegVideoFrameReader>(",
        "startCommitCheckpoint_()",
        "before-generation-commit",
        "pendingFrame_ = reader_->nextFrame(cancellation)",
        "after-decode-frame",
        "enqueue.outcome == PresentationVideoOutcome::refused",
        "admittedFrames >= limits_.maximumFramesPerFill",
        "maximumConfigurableFramesPerFill = 32",
        "PresentationVideoDecodeState::blocked",
        "terminate(PresentationVideoDecodeState::failed)",
        "return buffer_.select(generation, expectedRevision, clock)",
    ]:
        if token not in pump_source:
            raise ContractError(f"FFmpeg pipeline source missing token {token!r}")
    for token in [
        "class PresentationVideoDecodePumpTestAccess final",
        "PresentationVideoDecodePump::StartCommitCheckpoint checkpoint",
        "return PresentationVideoDecodePump(",
    ]:
        if token not in pump_test_access:
            raise ContractError(f"FFmpeg pipeline test access missing token {token!r}")
    for token in [
        "realFramesReachSharedRenderers",
        "capacityRetainsOnePendingFrame",
        "cancellationTerminatesOnlyItsGeneration",
        "decodeFailureTerminatesGeneration",
        "replacementClearsQueuedAndPendingFrames",
        "sameGenerationRejectsChangedInput",
        "cancellationBeforeReplacementCommitPreservesGeneration",
        "fillBudgetIsIndependentOfQueueCapacity",
        "pipeline end of stream is not stable",
        "pipeline WARP first pixel differs",
        "pump.select(1, pump.revision(), clock)",
        "pipeline clock dropped an exact frame",
        "cancellation setup lacks queued and pending frames",
        "failed replacement changed the generation",
        "failed replacement cleared the queue",
        "failed replacement cleared the pending frame",
        "cancelled replacement committed a generation",
        "cancelled replacement cleared pending data",
        "fill exceeded its frame budget",
        "changed input refusal replaced media",
        "PresentationVideoDecodeErrorCode::terminalState",
        "PresentationVideoDecodeState::failed",
        "renderPreviewFrame",
        "renderExportFrame",
        "CpuRenderer",
        "D3d11WarpRenderer",
        "2e-5F",
    ]:
        if token not in pipeline_tests:
            raise ContractError(f"FFmpeg pipeline tests missing token {token!r}")
    for token in [
        "presentation_video_decode_pump.cpp",
        "palmier_media_ffmpeg",
        "PUBLIC palmier_audio_wasapi palmier_media_render palmier_media_ffmpeg",
        "palmier_ffmpeg_presentation_pipeline_tests",
        "palmier_render_d3d11",
        "media_session.ffmpeg_presentation_pipeline",
        "PROPERTIES RUN_SERIAL TRUE TIMEOUT 60",
        '"${CMAKE_CURRENT_SOURCE_DIR}"',
    ]:
        if token not in cmake:
            raise ContractError(f"FFmpeg pipeline CMake missing token {token!r}")

    def validate_header_shape(candidate: str) -> None:
        require_equal(
            "FFmpeg pipeline C++ states",
            cpp_enum_cases(candidate, "PresentationVideoDecodeState"),
            contract.get("states", []),
        )
        require_equal(
            "FFmpeg pipeline C++ receipt fields",
            cpp_stored_fields(candidate, "PresentationVideoFillReceipt"),
            contract.get("fillReceiptFields", []),
        )
        require_equal(
            "FFmpeg pipeline C++ errors",
            cpp_enum_cases(candidate, "PresentationVideoDecodeErrorCode"),
            contract.get("errors", []),
        )

    validate_header_shape(pump_header)
    invalid_state = pump_header.replace(
        "enum class PresentationVideoDecodeState {",
        "enum class PresentationVideoDecodeState {\n    inventedState,",
        1,
    )
    expect_failure(
        "FFmpeg pipeline C++ extra state",
        lambda: validate_header_shape(invalid_state),
    )
    invalid_error = pump_header.replace(
        "enum class PresentationVideoDecodeErrorCode {",
        "enum class PresentationVideoDecodeErrorCode {\n    inventedError,",
        1,
    )
    expect_failure(
        "FFmpeg pipeline C++ extra error",
        lambda: validate_header_shape(invalid_error),
    )
    validate_ffmpeg_presentation_pipeline_contract(contract)
    invalid_fixture = dict(contract)
    invalid_fixture["fixture"] = dict(contract["fixture"])
    invalid_fixture["fixture"]["frameCount"] = 2
    expect_failure(
        "FFmpeg pipeline fixture frame count",
        lambda: validate_ffmpeg_presentation_pipeline_contract(invalid_fixture),
    )

    adr = read_text("docs/windows/adr/0016-ffmpeg-presentation-pipeline.md")
    for token in [
        "retains an unsent packet",
        "at most one decoded pending frame",
        "opened before the new generation clears",
        "exact input path is immutable within a generation",
        "hard configurable ceiling of 32",
        "renderer's shared 3,840 × 2,160 pixel budget",
        "sws_getCachedContext",
        contract["fixture"]["sha256"],
        "decode → adapter → bounded buffer → audio-clock select",
        "does not prove seek",
    ]:
        if token not in adr:
            raise ContractError(f"FFmpeg pipeline ADR missing token {token!r}")


def windows_ffmpeg_wasapi_audio_pipeline_contract() -> None:
    contract = load_json("contracts/audio/v1/ffmpeg-wasapi-pipeline.json")
    session_contract = load_json("contracts/audio/v1/playback-session.json")
    pcm_header = read_text("core/audio/include/palmier/audio/pcm_format.hpp")
    ffmpeg_header = read_text(
        "windows/media-ffmpeg/include/palmier/media/ffmpeg_media_reader.hpp"
    )
    ffmpeg_source = read_text("windows/media-ffmpeg/ffmpeg_media_reader.cpp")
    ffmpeg_tests = read_text(
        "windows/media-ffmpeg/tests/ffmpeg_media_reader_tests.cpp"
    )
    fixtures = read_text("windows/media-ffmpeg/tests/media_test_fixtures.hpp")
    pump_header = read_text(
        "windows/media-session/include/palmier/media/presentation_audio_decode_pump.hpp"
    )
    pump_source = read_text(
        "windows/media-session/presentation_audio_decode_pump.cpp"
    )
    pipeline_tests = read_text(
        "windows/media-session/tests/ffmpeg_wasapi_audio_pipeline_tests.cpp"
    )
    session_header = read_text(
        "windows/media-session/include/palmier/media/audio_playback_session.hpp"
    )
    session_source = read_text(
        "windows/media-session/audio_playback_session.cpp"
    )
    session_tests = read_text(
        "windows/media-session/tests/audio_playback_session_tests.cpp"
    )
    cmake = read_text("windows/media-session/CMakeLists.txt")

    require_equal("audio pipeline version", contract.get("version"), CONTRACT_VERSION)
    require_equal(
        "audio pipeline states",
        contract.get("states"),
        ["idle", "ready", "blocked", "endOfStream", "cancelled", "failed"],
    )
    require_equal(
        "audio pipeline outcomes",
        contract.get("outcomes"),
        ["changed", "noOp", "stale", "refused"],
    )
    require_equal(
        "audio pipeline canonical format",
        contract.get("canonicalFormat"),
        {
            "sampleRate": 48000,
            "encoding": "integer",
            "channelCount": 2,
            "containerBitsPerSample": 16,
            "validBitsPerSample": 16,
            "blockAlign": 4,
            "channelMask": 3,
            "interleaved": True,
        },
    )
    require_equal(
        "audio pipeline limits",
        contract.get("limits"),
        {
            "maximumAudioFramesPerDecodedBlock": 65536,
            "maximumPumpCapacityFrames": 4194304,
            "maximumFramesPerFill": 65536,
            "maximumQueuedAudioBytes": 268435456,
            "testPumpCapacityFrames": 150,
            "testMaximumFramesPerFill": 50,
            "testWasapiQueueCapacityFrames": 250,
            "testEndpointPacketFrames": 100,
        },
    )

    for token in [
        "enum class PcmSampleEncoding",
        "containerBitsPerSample",
        "validBitsPerSample",
        "channelMask",
        "interleaved",
        "isValidPcmFormat",
        "format.channelCount > 2 && format.channelMask == 0",
        "(std::numeric_limits<std::uint16_t>::max)()",
    ]:
        if token not in pcm_header:
            raise ContractError(f"PCM format contract missing token {token!r}")
    for token in [
        "struct DecodedAudioBlock final",
        "class FfmpegAudioFrameReader final",
        "std::optional<DecodedAudioBlock> nextBlock(",
        "maximumAudioFramesPerBlock{65'536}",
        "discontinuousAudioTimestamp",
    ]:
        if token not in ffmpeg_header:
            raise ContractError(f"FFmpeg audio API missing token {token!r}")
    for token in [
        "class SoftwareFrameReader final",
        "class FfmpegAudioFrameReader::Impl final",
        "swr_alloc_set_opts2(",
        "swr_get_out_samples(",
        "swr_convert(",
        "nullptr,\n                0",
        "changed-source-format",
        "audio-pts-discontinuity",
        "sourceAnchorTimestamp_",
        "sourceFramesRead_",
        "maximumConfigurableAudioFramesPerBlock",
        "swresample_version() == LIBSWRESAMPLE_VERSION_INT",
    ]:
        if token not in ffmpeg_source:
            raise ContractError(f"FFmpeg audio source missing token {token!r}")
    for token in [
        "decodesExactCanonicalPcm",
        "resamplesAndRemixesCanonicalPcm",
        "audioCursorCancellationIsTerminal",
        "validatesAudioFailureBoundaries",
        "frames == 1'536",
        "audio EOF is not stable",
    ]:
        if token not in ffmpeg_tests:
            raise ContractError(f"FFmpeg audio tests missing token {token!r}")

    fixture_match = re.search(
        r"patternedPcmWav\s*=((?:\s*\"[^\"]*\")+)\s*;",
        fixtures,
    )
    if fixture_match is None:
        raise ContractError("patterned PCM fixture is missing")
    fixture_base64 = "".join(re.findall(r'\"([^\"]*)\"', fixture_match.group(1)))
    try:
        fixture_bytes = base64.b64decode(fixture_base64, validate=True)
    except ValueError as error:
        raise ContractError(f"patterned PCM fixture is invalid: {error}") from error
    require_equal(
        "patterned PCM fixture SHA-256",
        hashlib.sha256(fixture_bytes).hexdigest(),
        contract["fixture"]["sha256"],
    )
    require_equal("patterned PCM fixture bytes", len(fixture_bytes), 1580)

    require_equal(
        "audio pump C++ states",
        cpp_enum_cases(pump_header, "PresentationAudioDecodeState"),
        contract["states"],
    )
    require_equal(
        "audio pump C++ outcomes",
        cpp_enum_cases(pump_header, "PresentationAudioOutcome"),
        contract["outcomes"],
    )
    for token in [
        "class PresentationAudioDecodePump final",
        "std::unique_ptr<FfmpegAudioFrameReader> reader_",
        "std::optional<DecodedAudioBlock> pendingBlock_",
        "std::deque<DecodedAudioBlock> queue_",
        "pendingFrameOffset_",
        "queuedFrames_",
    ]:
        if token not in pump_header:
            raise ContractError(f"audio pump API missing token {token!r}")
    for token in [
        "auto nextReader = std::make_unique<FfmpegAudioFrameReader>(",
        "before-audio-generation-commit",
        "before-audio-block-admission",
        "PresentationAudioDecodeState::blocked",
        "pendingBlock_->startOutputSample",
        "maximumQueuedAudioBytes",
        "maximumConfigurableAudioCapacityFrames",
        "maximumConfigurableAudioFramesPerFill",
        "limits_.decode.maximumAudioFramesPerBlock",
        "terminate(PresentationAudioDecodeState::failed)",
    ]:
        if token not in pump_source:
            raise ContractError(f"audio pump source missing token {token!r}")
    for token in [
        "boundedPipelinePreservesEveryMediaSample",
        "pumpBackpressureCancellationAndReplacement",
        "backend.captured == expected",
        "backend.acquiredFrames.back() == 36",
        "pump.generation() == 7",
        "PresentationAudioDecodeState::cancelled",
        "rejectsUnboundedPumpLimits",
    ]:
        if token not in pipeline_tests:
            raise ContractError(f"audio pipeline tests missing token {token!r}")
    for token in [
        "presentation_audio_decode_pump.cpp",
        "palmier_ffmpeg_wasapi_audio_pipeline_tests",
        "PRIVATE palmier_media_session palmier_audio_wasapi",
        "media_session.ffmpeg_wasapi_audio_pipeline",
        "PROPERTIES TIMEOUT 60",
    ]:
        if token not in cmake:
            raise ContractError(f"audio pipeline CMake missing token {token!r}")

    adr = read_text("docs/windows/adr/0017-ffmpeg-canonical-pcm-to-wasapi.md")
    for token in [
        "one immutable identity",
        "receive-before-supply codec driver",
        "drains the resampler with null input",
        "replacement reader must open before",
        "does not create threads",
        "final exact 36-frame lease",
        contract["fixture"]["sha256"],
        "does not prove audible playback",
    ]:
        if token not in adr:
            raise ContractError(f"audio pipeline ADR missing token {token!r}")

    require_equal(
        "audio playback session version",
        session_contract.get("version"),
        CONTRACT_VERSION,
    )
    require_equal(
        "audio playback session states",
        cpp_enum_cases(session_header, "AudioPlaybackState"),
        session_contract["states"],
    )
    require_equal(
        "audio playback session outcomes",
        cpp_enum_cases(session_header, "AudioPlaybackOutcome"),
        session_contract["outcomes"],
    )
    require_equal(
        "audio playback session limits",
        session_contract.get("limits"),
        {
            "maximumPendingCommands": 8,
            "maximumTerminalReceipts": 16,
            "maximumPumpCapacityFrames": 4194304,
            "maximumFramesPerFill": 65536,
            "pcmHandoffSlots": 1,
        },
    )
    for token in [
        "class AudioPlaybackSession final",
        "AudioPlaybackReceipt play(",
        "AudioPlaybackReceipt playExactGeneration(",
        "AudioPlaybackReceipt cancel(",
        "AudioPlaybackReceipt waitForTerminal(",
        "AudioPlaybackPositionReceipt position(",
        "AudioPlaybackReceipt snapshot() const",
        "AudioPlaybackClockAnchor",
        "sourcePresentationTimestamp",
        "sourceTimeBaseNumerator",
    ]:
        if token not in session_header:
            raise ContractError(f"audio playback session API missing token {token!r}")
    for token in [
        "std::jthread worker_",
        "output_->configuration(commandToken)",
        "commandValue.input,\n                commandToken",
        "prebuffer(",
        "output_->installGeneration(",
        "commandValue.expectedGeneration != nextGeneration",
        "pendingBlock_",
        "handoffCancellation_.request_stop()",
        "block.startOutputSample != expectedSourceSample",
        "sourceAnchor_ = PlaybackSourceAnchor",
        "destination->sourceTimeBase.numerator",
        "output_->markEndOfStream(",
        "output_->waitForTerminal(",
        "output_->clockPosition(expectedGeneration)",
        "clock.sample.devicePosition\n                    < current.clockAnchor.value.devicePosition",
        "started.clockSample.devicePosition",
        "started.clockSample.qpc100Nanoseconds",
        "started.clockSample.generation != generation_",
        "terminateActiveFailure",
        "commandToken.stop_requested()",
        "closeStarted_ && kind != CommandKind::close",
        "terminalHistoryCapacity = 16",
        "maximumPendingCommands = 8",
    ]:
        if token not in session_source:
            raise ContractError(f"audio playback session invariant missing token {token!r}")
    for token in [
        "playsRealPcmToOneTerminalAndAnchorsTheClock",
        "failedReplacementPreservesTheRunningGeneration",
        "successfulReplacementFlushesOldPcmAndUsesAnExactGeneration",
        "exactGenerationRestartsAnOtherwiseIdenticalRequest",
        "cancelledExactReplacementResumesTheActiveGeneration",
        "pre-cancelled exact generation was accepted",
        "cancelled exact generation poisoned the next request",
        "cancelled replacement poisoned active handoff",
        "incorrect first exact generation was accepted",
        "skipped exact generation was accepted",
        "rejectsInvalidRequestsAndPreservesNoOpState",
        "concurrentCloseJoinsTheSessionAndDeviceExactlyOnce",
        "state->captured == expected",
        "started.clockAnchor.sourcePresentationTimestamp == 0",
        "position.clockSample.devicePosition",
        "session.position(2).outcome == AudioPlaybackOutcome::refused",
        "cancelledPosition.outcome == AudioPlaybackOutcome::cancelled",
        "terminal.acceptedFrames == 1'536",
        "replaced.generation == 2",
        "session.waitForTerminal(0).outcome == AudioPlaybackOutcome::refused",
        "terminal.state == AudioPlaybackState::cancelled",
        "state->closeCalls == 1",
    ]:
        if token not in session_tests:
            raise ContractError(f"audio playback session test missing token {token!r}")
    for token in [
        "audio_playback_session.cpp",
        "palmier_audio_playback_session_tests",
        "media_session.audio_playback_session",
        "PROPERTIES TIMEOUT 30",
    ]:
        if token not in cmake:
            raise ContractError(f"audio playback session CMake missing token {token!r}")
    session_adr = read_text(
        "docs/windows/adr/0018-audio-playback-session-ownership.md"
    )
    for token in [
        "sole audio playback-generation owner",
        "opens and prebuffers a candidate reader",
        "unadmitted block is retracted",
        "first admitted source sample is rebased to output sample zero",
        "common media origin",
        "latest clock sample already cached by the device worker",
        "generation-matched clock sample",
        "1,536 accepted output frames",
        "do not prove audible output",
    ]:
        if token not in session_adr:
            raise ContractError(f"audio playback session ADR missing token {token!r}")


def windows_headless_av_playback_contract() -> None:
    contract = load_json(
        "contracts/media/v1/headless-av-playback-session.json"
    )
    header = read_text(
        "windows/media-session/include/palmier/media/"
        "headless_av_playback_session.hpp"
    )
    source = read_text(
        "windows/media-session/headless_av_playback_session.cpp"
    )
    audio_header = read_text(
        "windows/media-session/include/palmier/media/audio_playback_session.hpp"
    )
    audio_source = read_text(
        "windows/media-session/audio_playback_session.cpp"
    )
    tests = read_text(
        "windows/media-session/tests/headless_av_playback_session_tests.cpp"
    )
    cmake = read_text("windows/media-session/CMakeLists.txt")

    require_equal("headless A/V version", contract.get("version"), CONTRACT_VERSION)
    require_equal(
        "headless A/V states",
        cpp_enum_cases(header, "HeadlessAvPlaybackState"),
        contract.get("states"),
    )
    require_equal(
        "headless A/V outcomes",
        cpp_enum_cases(header, "HeadlessAvPlaybackOutcome"),
        contract.get("outcomes"),
    )
    require_equal(
        "headless A/V stages",
        cpp_enum_cases(header, "HeadlessAvPlaybackStage"),
        contract.get("stages"),
    )
    require_equal(
        "headless A/V failures",
        cpp_enum_cases(header, "HeadlessAvPlaybackFailureCode"),
        contract.get("failures"),
    )
    require_equal(
        "headless A/V receipt fields",
        cpp_stored_fields(header, "HeadlessAvPlaybackReceipt"),
        contract.get("receiptFields"),
    )
    require_equal(
        "headless A/V limits",
        contract.get("limits"),
        {
            "defaultVideoFillCallsPerTick": 2,
            "maximumVideoFillCallsPerTick": 4,
        },
    )
    require_equal(
        "headless A/V exclusions",
        contract.get("excluded"),
        [
            "timer or presenter ownership",
            "interactive swap-chain presentation",
            "physical-device audible A/V synchronization",
            "final hardware endpoint position at audio completion",
            "seek, scrub, speed change, and drift correction",
            "device recovery",
            "Windows 10 runtime certification",
        ],
    )
    for token in [
        "class HeadlessAvPlaybackAudioPort",
        "class HeadlessAvPlaybackSession final",
        "maximumVideoFillCallsPerTick{2}",
        "HeadlessAvPlaybackReceipt play(",
        "HeadlessAvPlaybackReceipt tick(",
        "HeadlessAvPlaybackReceipt cancel(",
        "HeadlessAvPlaybackReceipt snapshot() const",
        "HeadlessAvPlaybackReceipt close()",
    ]:
        if token not in header:
            raise ContractError(f"headless A/V API missing token {token!r}")
    for token in [
        "maximumConfigurableFillCallsPerTick = 4",
        "std::make_unique<PresentationVideoDecodePump>(limits_.video)",
        "candidate->start(nextGeneration, input, operationToken)",
        "candidate->fill(nextGeneration, operationToken)",
        "audio_->playExactGeneration(",
        "video_ = std::move(candidate)",
        "position = audio_->position(expectedGeneration)",
        "position.clockAnchor.sourcePresentationTimestamp",
        "video_->select(",
        "video_->revision()",
        "value.fillCalls >= limits_.maximumVideoFillCallsPerTick",
        "++value.droppedFrames",
        "fillBudgetExhausted = true",
        "closeReceipt_.has_value()",
        "std::lock_guard operationLock(operationMutex_)",
        "pendingCancellation_ = expectedGeneration",
        "closeRequested_ = true",
    ]:
        if token not in source:
            raise ContractError(f"headless A/V invariant missing token {token!r}")
    if "audio::timelineFrame(" in source or "targetPresentationTimestamp(" in source:
        raise ContractError("headless A/V coordinator duplicates clock math")
    prepare_index = source.index("candidate->start(nextGeneration, input, operationToken)")
    audio_index = source.index("audio_->playExactGeneration(")
    commit_index = source.index("video_ = std::move(candidate)")
    if not prepare_index < audio_index < commit_index:
        raise ContractError("headless A/V replacement order changed")
    for token in [
        "AudioPlaybackReceipt playExactGeneration(",
        "std::stop_token cancellation = {}",
    ]:
        if token not in audio_header:
            raise ContractError(f"audio exact-generation API missing token {token!r}")
    for token in [
        "commandValue.expectedGeneration != nextGeneration",
        "std::stop_callback commandCancellation(",
        "commandValue.cancellation",
    ]:
        if token not in audio_source:
            raise ContractError(f"audio exact-generation invariant missing token {token!r}")
    for token in [
        "playsAndTicksOneRealVideoGeneration",
        "failedReplacementPreservesTheActiveGeneration",
        "boundsCatchUpAndDeliversOnlyTheLatestFrame",
        "noSampleAndAudioTerminalsDoNotGuessVideoTime",
        "validatesRequestsCancellationAndConcurrentClose",
        "state->positionCalls == 1",
        "post-commit failure kept old generation",
        "close did not cancel active play",
        "cancelled video preparation stopped old audio",
        "play was admitted after close requested",
        "same play reached audio",
        "frame-rate replacement did not advance",
        "tick.frame->presentationTimestamp == 2'048",
        "tick.fillCalls == 2",
        "tick.droppedFrames == 2",
        "state->closeCalls == 1",
    ]:
        if token not in tests:
            raise ContractError(f"headless A/V test missing token {token!r}")
    for token in [
        "headless_av_playback_session.cpp",
        "palmier_headless_av_playback_session_tests",
        "media_session.headless_av_playback_session",
        "PROPERTIES TIMEOUT 30",
    ]:
        if token not in cmake:
            raise ContractError(f"headless A/V CMake missing token {token!r}")
    adr = read_text("docs/windows/adr/0020-headless-av-playback-coordinator.md")
    for token in [
        "single A/V generation and lifecycle owner",
        "must be called from a background coordinator executor",
        "no-fail `unique_ptr` swap",
        "failure after\nits exact generation has committed",
        "reads `AudioPlaybackSession::position` exactly once",
        "only the newest is",
        "Audio is the master terminal",
        "request it before waiting for the state mutex",
        "publishes a permanent admission gate",
        "not prove a timer cadence",
    ]:
        if token not in adr:
            raise ContractError(f"headless A/V ADR missing token {token!r}")


def windows_audio_wasapi_contract() -> None:
    root_cmake = read_text("CMakeLists.txt")
    audio_cmake = read_text("windows/audio-wasapi/CMakeLists.txt")
    clock_header = read_text(
        "windows/audio-wasapi/include/palmier/audio/audio_clock.hpp"
    )
    clock_source = read_text("windows/audio-wasapi/audio_clock.cpp")
    probe_header = read_text(
        "windows/audio-wasapi/include/palmier/audio/wasapi_environment_probe.hpp"
    )
    probe_source = read_text("windows/audio-wasapi/wasapi_environment_probe.cpp")
    native_source = read_text("windows/audio-wasapi/wasapi_native_stream.cpp")
    session_header = read_text(
        "windows/audio-wasapi/wasapi_environment_session.hpp"
    )
    environment_tests = read_text(
        "windows/audio-wasapi/tests/wasapi_environment_tests.cpp"
    )

    if "add_subdirectory(windows/audio-wasapi)" not in root_cmake:
        raise ContractError("root CMake is missing the WASAPI audio boundary")
    for token in [
        "palmier_audio_wasapi",
        "PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN",
        "/W4 /WX /permissive- /Zc:__cplusplus /utf-8",
        "audio_wasapi.clock_math",
        "audio_wasapi.environment_contract",
        "audio_wasapi.environment_probe",
        "RUN_SERIAL TRUE TIMEOUT 30",
        "ole32",
    ]:
        if token not in audio_cmake:
            raise ContractError(f"WASAPI CMake missing token {token!r}")
    for token in [
        "qpc100Nanoseconds",
        "precisionDegraded",
        "staleGeneration",
        "positionDiscontinuity",
        "arithmeticOverflow",
    ]:
        if token not in clock_header:
            raise ContractError(f"WASAPI clock API missing token {token!r}")
    for token in [
        "std::gcd",
        "_umul128",
        "_udiv128",
        "sample.generation != anchor.generation",
        "sample.devicePosition < anchor.devicePosition",
    ]:
        if token not in clock_source:
            raise ContractError(f"WASAPI clock source missing token {token!r}")
    for token in [
        "WasapiProbeStatus",
        "WasapiProbeStage",
        "clockFrequency",
        "isUnavailableWasapiResult",
        "hasValidSharedModePeriods",
    ]:
        if token not in probe_header:
            raise ContractError(f"WASAPI probe API missing token {token!r}")
    for token in [
        "COINIT_APARTMENTTHREADED",
        "std::jthread",
        "GetDefaultAudioEndpoint(",
        "eRender,",
        "eMultimedia,",
        "IAudioClient3",
        "SetClientProperties",
        "GetSharedModeEnginePeriod",
        "InitializeSharedAudioStream",
        "AUDCLNT_STREAMFLAGS_EVENTCALLBACK",
        "SetEventHandle",
        "IAudioRenderClient",
        "IAudioClock",
        "GetFrequency",
        "parseWasapiMixFormat",
        "KSDATAFORMAT_SUBTYPE_IEEE_FLOAT",
        "wValidBitsPerSample",
        "dwChannelMask",
    ]:
        if token not in probe_source and token not in native_source:
            raise ContractError(f"WASAPI native setup missing token {token!r}")
    for token in [
        "class WasapiEnvironmentSession",
        "initializeApartment",
        "initializeSharedAudioStream",
        "loadClockFrequency",
        "runWasapiEnvironmentProbe",
    ]:
        if token not in session_header:
            raise ContractError(f"WASAPI session seam missing token {token!r}")
    for forbidden in ["->Start(", ".Start("]:
        if forbidden in probe_source:
            raise ContractError(
                f"WASAPI environment probe must not start playback: {forbidden!r}"
            )
    for token in [
        "WasapiProbeStage::defaultEndpoint, E_NOTFOUND",
        "AUDCLNT_E_SERVICE_NOT_RUNNING",
        "AUDCLNT_E_DEVICE_INVALIDATED",
        "AUDCLNT_E_RESOURCES_INVALIDATED",
        "AUDCLNT_E_DEVICE_IN_USE",
        "AUDCLNT_E_ENDPOINT_CREATE_FAILED",
        "WasapiProbeStage::createEnumerator, E_NOTFOUND",
        "WasapiProbeStage::activateClient, E_ACCESSDENIED",
        "WasapiProbeStage::initializeStream, E_INVALIDARG",
        "WasapiProbeStage::activateClient, E_NOINTERFACE",
        "WasapiProbeStage::initializeCom, RPC_E_CHANGED_MODE",
    ]:
        if token not in environment_tests:
            raise ContractError(f"WASAPI environment test missing token {token!r}")
    for token in [
        "ScriptedWasapiSession",
        "executesTheNoStartSetupInOrder",
        "preservesExternalAndImplementationFailures",
        "parsesExactPcmAndExtensibleMixFormats",
        '"initialize-stream",',
        '"clock-frequency",',
        "session.initializedPeriod == 480",
        "unavailable.calls.back() == \"initialize-stream\"",
        "failed.calls.back() == \"activate-client\"",
    ]:
        if token not in environment_tests:
            raise ContractError(f"WASAPI injected test missing token {token!r}")

    adr = read_text("docs/windows/adr/0011-wasapi-clock-and-environment-probe.md")
    for token in [
        "never calls `IAudioClient::Start`",
        "dedicated STA thread",
        "one JSON line",
        "It does not prove that audio was",
        "Long-run drift must be measured",
        "Windows 10 build 19045 runtime behavior",
    ]:
        if token not in adr:
            raise ContractError(f"WASAPI ADR missing token {token!r}")


def windows_wasapi_output_contract() -> None:
    contract = load_json("contracts/audio/v1/wasapi-output.json")
    output_header = read_text(
        "windows/audio-wasapi/include/palmier/audio/wasapi_output.hpp"
    )
    backend_header = read_text("windows/audio-wasapi/wasapi_output_backend.hpp")
    output_source = read_text("windows/audio-wasapi/wasapi_output.cpp")
    native_source = read_text("windows/audio-wasapi/wasapi_native_stream.cpp")
    smoke_source = read_text("windows/audio-wasapi/wasapi_silent_output_probe.cpp")
    worker_header = read_text(
        "windows/audio-wasapi/include/palmier/audio/wasapi_output_worker.hpp"
    )
    worker_source = read_text("windows/audio-wasapi/wasapi_output_worker.cpp")
    tests = read_text("windows/audio-wasapi/tests/wasapi_output_tests.cpp")
    worker_tests = read_text(
        "windows/audio-wasapi/tests/wasapi_output_worker_tests.cpp"
    )
    audio_cmake = read_text("windows/audio-wasapi/CMakeLists.txt")

    if contract.get("version") != CONTRACT_VERSION:
        raise ContractError("WASAPI output contract version changed")
    expected_states = [
        "ready",
        "primed",
        "running",
        "stopped",
        "invalidated",
        "failed",
        "completed",
        "closed",
    ]
    if contract.get("states") != expected_states:
        raise ContractError("WASAPI output states drifted")
    expected_outcomes = [
        "changed",
        "noOp",
        "cancelled",
        "refused",
        "failed",
        "invalidated",
    ]
    if contract.get("outcomes") != expected_outcomes:
        raise ContractError("WASAPI output outcomes drifted")
    for token in expected_states + expected_outcomes + contract["receiptFields"]:
        if token not in output_header:
            raise ContractError(f"WASAPI output API missing token {token!r}")
    for token in [
        "class WasapiOutputBackend",
        "waitForRenderEvent",
        "loadCurrentPadding",
        "acquireBuffer",
        "releaseBuffer",
        "loadClockPosition",
        "WasapiOutputCheckpoints",
    ]:
        if token not in backend_header:
            raise ContractError(f"WASAPI output seam missing token {token!r}")
    for token in [
        "value.paddingFrames > config_.bufferFrames",
        "backend_.releaseBuffer(0, 0)",
        "AUDCLNT_BUFFERFLAGS_SILENT",
        "queue_.commitFrames(value.mediaFrames)",
        "value.requestedFrames = sourceEnded",
        "WasapiOutputState::completed",
        "value.lateCancellation = stopToken.stop_requested()",
        "WasapiOutputOperation::discardGeneration",
        "WasapiOutputOperation::installGeneration",
        "expectedGeneration != config_.generation",
        "config_.generation = *nextGeneration",
        "isWasapiOutputInvalidation",
    ]:
        if token not in output_source:
            raise ContractError(f"WASAPI output invariant missing token {token!r}")
    for token in [
        "WaitForMultipleObjects",
        "std::stop_callback",
        "GetCurrentPadding",
        "GetBuffer",
        "ReleaseBuffer",
        "GetPosition",
        "impl_->client->Start()",
        "impl_->client->Stop()",
        "impl_->client->Reset()",
    ]:
        if token not in native_source:
            raise ContractError(f"WASAPI native output missing token {token!r}")
    for token in [
        "runWasapiEnvironmentProbe(stream)",
        "machine.start()",
        "machine.renderOnce(2'000)",
        "machine.pause()",
        "machine.installGeneration(1, 2)",
        "machine.close()",
        '"completed-silent-cycle"',
    ]:
        if token not in smoke_source:
            raise ContractError(f"WASAPI silent smoke missing token {token!r}")
    for token in [
        "class WasapiOutputWorker",
        "WasapiWorkerPcmBlock",
        "WasapiWorkerConfiguration",
        "WasapiWorkerClockReceipt",
        "configuration(std::stop_token",
        "clockPosition(",
        "startOutputSample",
        "markEndOfStream",
        "waitForTerminal",
    ]:
        if token not in worker_header:
            raise ContractError(f"WASAPI worker API missing token {token!r}")
    for token in [
        "std::jthread worker_",
        "runWasapiEnvironmentProbe(*stream)",
        "interruptRenderLocked",
        "renderCancellation_.request_stop()",
        "expectedGeneration != machine.generation()",
        "block->pcmFormat != format",
        "block->startOutputSample != *nextOutputSample_",
        "queue.markEndOfStream()",
        "latestClockSample_ = value.clockSample",
        "latestClockSample_.reset()",
        "configuration_->outcome\n                == WasapiWorkerConfigurationOutcome::unavailable",
    ]:
        if token not in worker_source:
            raise ContractError(f"WASAPI worker invariant missing token {token!r}")
    for token in [
        "primesBeforeStartAndCommitsOnlyReleasedPcm",
        "rendersOnlyAfterAnEventAndCountsUnderrun",
        "endOfStreamWithoutMediaCompletesBeforeStart",
        "endOfStreamReleasesTheExactTailThenCompletes",
        "cancellationInsideLeaseAbandonsAndRollsBack",
        "cancellationAfterReleaseReportsCommittedEffect",
        "cancellationInterruptsTheNativeEventWait",
        "validatesPaddingBeforeSubtraction",
        "invalidationRejectsTheOldGeneration",
        "pauseResumeResetAndRepeatedCommandsAreExact",
        "exactGenerationDiscardAndInstallAreOrdered",
        "generationInstallFinishesAfterLateCancellation",
        "generationInstallFailureIsAtomic",
        "preservesPrecisionAndFailureReceipts",
        "closeStopsRunningStreamAndReleasesBackend",
    ]:
        if token not in tests:
            raise ContractError(f"WASAPI output tests missing token {token!r}")
    for token in [
        "controlPreemptsRenderWaitAndKeepsNativeOwnership",
        "orderedPcmAndEndOfStreamReachOneTerminalReceipt",
        "fullReadyQueueReturnsRetryWithoutSpinning",
        "completedDiscardDoesNotReuseTheOldTerminal",
        "generationBarrierRejectsPendingHandoffBeforeAcknowledgement",
        "cancellationRemovesAnUnadmittedHandoff",
        "setupFailureReturnsReceiptsWithoutStartingNativeOutput",
        "terminalInvalidationMakesConfigurationUnavailable",
        "concurrentCloseJoinsTheDeviceThreadExactlyOnce",
        "worker.clockPosition(1)",
        "worker.clockPosition(42).outcome == WasapiWorkerClockOutcome::noSample",
        "clock.outcome == WasapiWorkerClockOutcome::unavailable",
        "clock.hresult == AUDCLNT_E_DEVICE_INVALIDATED",
        "thread == state->constructedThread",
    ]:
        if token not in worker_tests:
            raise ContractError(f"WASAPI worker tests missing token {token!r}")
    for token in [
        "audio_wasapi.output_state",
        "audio_wasapi.output_worker",
        "audio_wasapi.output_silence_smoke",
        "PROPERTIES TIMEOUT 10",
        "RUN_SERIAL TRUE TIMEOUT 30",
    ]:
        if token not in audio_cmake:
            raise ContractError(f"WASAPI output CMake missing token {token!r}")

    adr = read_text("docs/windows/adr/0013-bounded-wasapi-output.md")
    for token in [
        "prime-before-start",
        "Cancellation after acquire",
        "proves only classification",
        "Windows 10 build 19045 behavior",
    ]:
        if token not in adr:
            raise ContractError(f"WASAPI output ADR missing token {token!r}")


def windows_qt_read_only_shell_contract() -> None:
    package_header = read_text(
        "core/project/include/palmier/project/project_package_reader.hpp"
    )
    package_source = read_text(
        "core/project/serialization/project_package_reader.cpp"
    )
    package_tests = read_text(
        "core/project/tests/project_package_reader_tests.cpp"
    )
    for token in [
        "defaultMaximumProjectJsonBytes",
        "maximumProjectJsonBytes",
        "maximumProjectJsonValues",
        "maximumProjectJsonStringBytes",
        "std::stop_token cancellation",
        "readProjectPackage(",
        "Caller must run this synchronous file operation off the UI thread",
    ]:
        if token not in package_header:
            raise ContractError(f"project package reader header missing token {token!r}")
    for token in [
        "projectJsonTooLarge",
        "projectJsonReadFailed",
        "projectJsonTooComplex",
        "hasPalmierExtension",
        "checkCancellation(options.cancellation)",
        "palmier::json::testing::parse",
        "ProjectPackageReadCheckpoint::duringParse",
    ]:
        if token not in package_source:
            raise ContractError(f"project package reader invariant missing token {token!r}")
    for token in [
        "readsRepositoryPackages",
        "validatesPackageAndLimit",
        "validatesJsonComplexityBudgets",
        "defaultBudgetAcceptsMaximumTimelineProjection",
        "validatesProjectJsonFile",
        "cancellationBoundaries",
        "cancellationDuringModelNormalization",
        "cancellationDuringJsonParse",
        "growingFileCannotCrossLimit",
    ]:
        if token not in package_tests:
            raise ContractError(f"project package reader test missing token {token!r}")

    root_cmake = read_text("CMakeLists.txt")
    presets = load_json("CMakePresets.json")
    app_cmake = read_text("windows/app/CMakeLists.txt")
    coordinator = read_text("windows/app/src/project_load_coordinator.cpp")
    projection = read_text("windows/app/src/project_projection_loader.cpp")
    timeline_model = read_text("windows/app/src/read_only_timeline_model.cpp")
    qml = read_text("windows/app/qml/Main.qml")
    theme = read_text("windows/app/qml/AppTheme.qml")
    swift_theme = read_text("Sources/PalmierPro/UI/AppTheme.swift")
    qt_tests = read_text("windows/app/tests/qt_shell_tests.cpp")
    workflow = read_text(".github/workflows/windows-qt-shell.yml")

    for token in [
        'option(PALMIER_ENABLE_QT_SHELL "Build the optional Qt/QML read-only shell" OFF)',
        "add_subdirectory(windows/app)",
    ]:
        if token not in root_cmake:
            raise ContractError(f"Qt shell root CMake missing token {token!r}")
    qt_preset = next(
        preset
        for preset in presets["configurePresets"]
        if preset["name"] == "windows-msvc-x64-qt-shell"
    )
    require_equal(
        "Qt shell preset variables",
        qt_preset["cacheVariables"],
        {
            "CMAKE_PREFIX_PATH": "$env{QT_ROOT_DIR}",
            "PALMIER_ENABLE_QT_SHELL": "ON",
        },
    )
    for token in [
        "Qt6 6.10.3 EXACT REQUIRED",
        "Qt6::Concurrent",
        "Qt6::QuickDialogs2",
        "qt_add_executable(palmier_qt_shell",
        "qt_add_qml_module(palmier_qt_shell",
        "include/palmier/windows/project_load_coordinator.hpp",
        "include/palmier/windows/read_only_timeline_model.hpp",
        "set_target_properties(palmier_qt_shell_tests PROPERTIES WIN32_EXECUTABLE FALSE)",
        'set(test_log "${CMAKE_CURRENT_BINARY_DIR}/qt-test-${test_name}.txt")',
        'COMMAND palmier_qt_shell_tests ${test_function} -v1 -o "${test_log},txt"',
        "PROPERTIES ENVIRONMENT \"QT_QPA_PLATFORM=offscreen\" TIMEOUT 30",
        "add_palmier_qt_shell_test(reader_maps_current_project readerMapsCurrentProject)",
        "add_palmier_qt_shell_test(model_publishes_track_layout modelPublishesReadOnlyTrackLayout)",
        "add_palmier_qt_shell_test(failure_preserves_model failurePreservesPreviousModel)",
        "add_palmier_qt_shell_test(stale_generation_is_rejected staleGenerationCannotReplaceNewerProject)",
        "add_palmier_qt_shell_test(consecutive_opens_keep_latest consecutiveOpensKeepOnlyLatestPendingRequest)",
        "add_palmier_qt_shell_test(cancellation_reaches_reader cancellationReachesReader)",
        "add_palmier_qt_shell_test(late_cancellation_rejects_result cancellationAfterWorkBeforeCommitRejectsResult)",
        "add_palmier_qt_shell_test(shutdown_waits_for_worker shutdownWaitsForAdmittedWorker)",
        "add_palmier_qt_shell_test(unsafe_clip_is_skipped unsafeClipIsSkippedWithoutRejectingProject)",
        "add_palmier_qt_shell_test(dense_timeline_is_rejected denseTimelineIsExplicitlyRejected)",
        "add_palmier_qt_shell_test(structured_errors_preserve_details structuredErrorsPreserveStableDetails)",
        "add_palmier_qt_shell_test(diagnostics_publish_warnings diagnosticsProduceLoadedWithWarnings)",
        "add_palmier_qt_shell_test(qml_loads_offscreen qmlLoadsOffscreen)",
        "add_palmier_qt_shell_test(qml_close_waits_for_worker qmlCloseWaitsForActiveWorker)",
        "qt_shell.module_smoke",
    ]:
        if token not in app_cmake:
            raise ContractError(f"Qt shell CMake missing token {token!r}")
    for token in [
        "std::stop_source",
        "request_stop()",
        "requestGeneration == generation_",
        "result.cancelled || cancellation.stop_requested()",
        "pendingLoad_",
        "setMaxThreadCount(1)",
        "requestShutdown()",
        "emit shutdownReady()",
        "resultDeliveryCheckpoint",
        "errorPointer",
    ]:
        if token not in coordinator:
            raise ContractError(f"Qt load coordinator missing token {token!r}")
    for token in [
        "checkCancellation(cancellation)",
        "checkedClipEnd",
        "offsetRatio",
        "extentRatio",
        "track.clipItems",
        "maximumProjectedClipsPerTrack",
        "maximumProjectedTracksPerTimeline",
        "timelineTooDense",
        "result.timelines.reserve(1)",
        "firstDiagnostic",
        "skippedUnsafeClipCount",
    ]:
        if token not in projection:
            raise ContractError(f"Qt project projection missing token {token!r}")
    for token in [
        "ReadOnlyTimelineModel::replace(ProjectProjection&& project)",
        "ClipItemsRole",
        "std::move(project)",
    ]:
        if token not in timeline_model:
            raise ContractError(f"Qt timeline model missing token {token!r}")
    for token in [
        "FolderDialog",
        "selectedFolder",
        "projectCoordinator.openFolder",
        "ListView",
        "offsetRatio",
        "extentRatio",
        "AppTheme.",
        "requestShutdown()",
        "clip: true",
    ]:
        if token not in qml:
            raise ContractError(f"Qt shell QML missing token {token!r}")
    for token in ["pragma Singleton", "QtObject", "windowBackground", "trackHeight"]:
        if token not in theme:
            raise ContractError(f"Qt shell theme missing token {token!r}")
    for swift_token, qml_token in [
        ("static let windowWidth: CGFloat = 1100", "property int windowWidth: 1100"),
        ("static let windowHeight: CGFloat = 720", "property int windowHeight: 720"),
        ("static let trackHeight: CGFloat = 76", "property int trackHeight: 76"),
        ("static let trackHeaderWidth: CGFloat = 150", "property int trackHeaderWidth: 150"),
        ("static let clipMinimumWidth: CGFloat = 18", "property int clipMinimumWidth: 18"),
    ]:
        if swift_token not in swift_theme or qml_token not in theme:
            raise ContractError(f"Qt AppTheme mirror missing {swift_token!r}")
    for token in [
        "readerMapsCurrentProject",
        "failurePreservesPreviousModel",
        "cancellationReachesReader",
        "cancellationAfterWorkBeforeCommitRejectsResult",
        "consecutiveOpensKeepOnlyLatestPendingRequest",
        "shutdownWaitsForAdmittedWorker",
        "unsafeClipIsSkippedWithoutRejectingProject",
        "denseTimelineIsExplicitlyRejected",
        "structuredErrorsPreserveStableDetails",
        "diagnosticsProduceLoadedWithWarnings",
        "staleGenerationCannotReplaceNewerProject",
        "qmlLoadsOffscreen",
        "qmlCloseWaitsForActiveWorker",
    ]:
        if token not in qt_tests:
            raise ContractError(f"Qt shell test missing token {token!r}")
    if "QTRY_VERIFY_WITH_TIMEOUT(cancellationObserved.tryAcquire()" in qt_tests:
        raise ContractError("Qt shell cancellation checks must not consume state while polling")
    if qt_tests.count("QTRY_COMPARE_WITH_TIMEOUT(cancellationObserved.available(), 1, 5000)") != 3:
        raise ContractError("Qt shell cancellation checks must poll three non-consuming observations")
    for token in [
        "aqtinstall==3.3.0",
        "py7zr==1.0.0",
        "6.10.3",
        "win64_msvc2022_64",
        "cmake version 3.31.6",
        "^17\\.14\\.",
        "^14\\.44\\.",
        "Windows Kits\\10\\Include\\10.0.26100.0",
        "cmake --preset windows-msvc-x64-qt-shell",
        "ctest --preset windows-msvc-x64-qt-shell-release",
        "$testExitCode = $LASTEXITCODE",
        'Get-ChildItem -Path "out/build/windows-msvc-x64-qt-shell" -Recurse -Filter "qt-test-*.txt"',
        "Get-Content -LiteralPath $_.FullName -Encoding UTF8",
        "exit $testExitCode",
    ]:
        if token not in workflow:
            raise ContractError(f"Qt shell workflow missing token {token!r}")
    if "install-qt-action" in workflow:
        raise ContractError("Qt shell workflow must not use the floating install action")

    adr = read_text("docs/windows/adr/0014-qt-read-only-project-shell.md")
    for token in [
        "64 MiB",
        "monotonically increasing load generation",
        "does not approve product distribution",
        "physical Windows 10 build 19045 compatibility",
    ]:
        if token not in adr:
            raise ContractError(f"Qt shell ADR missing token {token!r}")


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
        ("media schema and Swift source", media_source_contract),
        ("project fixtures and canaries", project_fixtures),
        ("Windows toolchain and compiled probe", windows_bootstrap_contract),
        ("Windows read-only project reader", windows_project_reader_contract),
        ("Windows render plan and D3D11 WARP", windows_render_plan_contract),
        ("Windows decoded-frame render adapter", windows_media_render_adapter_contract),
        ("Windows presentation video buffer", windows_presentation_video_buffer_contract),
        ("Windows FFmpeg presentation pipeline", windows_ffmpeg_presentation_pipeline_contract),
        ("Windows FFmpeg to WASAPI audio pipeline", windows_ffmpeg_wasapi_audio_pipeline_contract),
        ("Windows headless A/V playback", windows_headless_av_playback_contract),
        ("Windows WASAPI clock and environment probe", windows_audio_wasapi_contract),
        ("Windows bounded WASAPI output", windows_wasapi_output_contract),
        ("Windows Qt read-only project shell", windows_qt_read_only_shell_contract),
    ]
    for label, check in checks:
        check()
        print(f"PASS {label}")
    print("DECLARED unknown-field round trip enforced by macOS Swift tests")
    print(f"PASS {len(checks)} contract audit groups")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ContractError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
