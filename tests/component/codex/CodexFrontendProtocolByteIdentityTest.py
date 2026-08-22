#!/usr/bin/env python3
"""Guard the reviewed Frontend Protocol v1 schema-template bytes."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


FRONTEND_V1_SHA256 = "d90067b85482527e46642b85225de6717e380ade39da6e3c5f992521a408f81f"
LEGACY_METHODS = (
    "controller.acquire",
    "controller.release",
    "snapshot.get",
    "events.replay",
    "thread.start",
    "thread.resume",
    "thread.list",
    "thread.read",
    "turn.start",
    "turn.interrupt",
    "request.approval.respond",
    "request.userInput.respond",
    "request.authentication.respond",
    "request.unknown.respond",
    "request.unknown.reject",
)


def command_branches(schema: dict) -> list[dict]:
    return schema["$defs"]["Command"]["allOf"][1]["oneOf"]


def command_methods(schema: dict) -> tuple[str, ...]:
    return tuple(
        branch["properties"]["method"]["const"] for branch in command_branches(schema)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema", type=Path, required=True)
    parser.add_argument("--schema-template", type=Path)
    arguments = parser.parse_args()

    schema_template = arguments.schema_template
    if schema_template is None:
        repository = Path(__file__).resolve().parents[3]
        schema_template = (
            repository / "tools/frontend/frontend-protocol-v1.schema.template.json"
        )

    template_bytes = schema_template.read_bytes()
    digest = hashlib.sha256(template_bytes).hexdigest()
    if digest != FRONTEND_V1_SHA256:
        raise SystemExit(
            "Frontend Protocol v1 schema template bytes changed: "
            f"expected {FRONTEND_V1_SHA256}, got {digest}"
        )

    template = json.loads(template_bytes)
    generated = json.loads(arguments.schema.read_text(encoding="utf-8"))
    if command_methods(template) != LEGACY_METHODS:
        raise SystemExit("legacy Frontend Protocol v1 method contract changed")

    generated_methods = command_methods(generated)
    if len(generated_methods) != 105 or len(set(generated_methods)) != 105:
        raise SystemExit("generated Frontend Protocol v1 schema is not 105 methods")
    if generated_methods[: len(LEGACY_METHODS)] != LEGACY_METHODS:
        raise SystemExit("generated schema changed the original 15 method order")
    if command_branches(generated)[: len(LEGACY_METHODS)] != command_branches(
        template
    ):
        raise SystemExit("generated schema changed an original method schema")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
