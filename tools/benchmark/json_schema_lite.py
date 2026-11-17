"""Minimal JSON Schema subset used to structure-check benchmark run manifests.

Supported keywords: type (string or list), required, properties, additionalProperties
(boolean or schema), items, enum, const, minimum, maximum, minLength, pattern.
Anything else in a schema is ignored, so the schema file must stay inside this subset.
"""

from __future__ import annotations

import json
import re

_TYPES = {
    "object": dict,
    "array": list,
    "string": str,
    "boolean": bool,
    "null": type(None),
}


def _type_matches(value, name: str) -> bool:
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    expected = _TYPES.get(name)
    if expected is None:
        return True
    if name == "boolean":
        return isinstance(value, bool)
    return isinstance(value, expected)


def _pointer(parts: list[str]) -> str:
    return "/" + "/".join(parts) if parts else "/"


def validate(value, schema: dict, parts: list[str] | None = None) -> list[str]:
    parts = parts or []
    errors: list[str] = []

    if not isinstance(schema, dict):
        return [f"{_pointer(parts)}: schema is not an object"]

    if "const" in schema and value != schema["const"]:
        errors.append(f"{_pointer(parts)}: expected {schema['const']!r}, got {value!r}")

    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{_pointer(parts)}: {value!r} not in {schema['enum']!r}")

    if "type" in schema:
        names = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_type_matches(value, name) for name in names):
            errors.append(f"{_pointer(parts)}: expected type {names}, got {type(value).__name__}")
            return errors

    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{_pointer(parts)}: missing required property {key!r}")
        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        for key, item in value.items():
            if key in properties:
                errors.extend(validate(item, properties[key], parts + [key]))
            elif isinstance(additional, dict):
                errors.extend(validate(item, additional, parts + [key]))
            elif additional is False:
                errors.append(f"{_pointer(parts)}: unexpected property {key!r}")

    if isinstance(value, list) and "items" in schema:
        for index, item in enumerate(value):
            errors.extend(validate(item, schema["items"], parts + [str(index)]))

    if isinstance(value, str):
        if "minLength" in schema and len(value) < schema["minLength"]:
            errors.append(f"{_pointer(parts)}: shorter than {schema['minLength']}")
        if "pattern" in schema and re.search(schema["pattern"], value) is None:
            errors.append(f"{_pointer(parts)}: does not match {schema['pattern']!r}")

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{_pointer(parts)}: {value} < minimum {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(f"{_pointer(parts)}: {value} > maximum {schema['maximum']}")

    return errors


def validate_file(path: str, schema_path: str) -> list[str]:
    with open(path, encoding="utf-8") as handle:
        document = json.load(handle)
    with open(schema_path, encoding="utf-8") as handle:
        schema = json.load(handle)
    return validate(document, schema)
