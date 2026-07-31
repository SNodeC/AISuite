#!/usr/bin/env python3
"""Validate the current A1.1--A1.3 typed protocol surface.

This test intentionally reads only current source, generated descriptors, and
the checked-in protocol schemas.  It does not inspect repository history or
milestone evidence.
"""

from __future__ import annotations

import argparse
import json
import unittest
from collections import Counter
from pathlib import Path


EXPECTED_TAXONOMY = {
    "A1_1": Counter(
        {
            "ClientRequest": 22,
            "ItemDiscriminator": 34,
            "ServerNotification": 37,
            "TaggedUnionDiscriminator": 58,
        }
    ),
    "A1_2": Counter(
        {
            "ClientRequest": 18,
            "ServerNotification": 7,
            "ServerRequest": 1,
            "TaggedUnionDiscriminator": 19,
        }
    ),
    "A1_3": Counter(
        {
            "ClientRequest": 17,
            "ServerNotification": 7,
            "ServerRequest": 5,
            "TaggedUnionDiscriminator": 39,
        }
    ),
}


def split_cpp_arguments(value: str) -> list[str]:
    arguments: list[str] = []
    current: list[str] = []
    depth = 0
    quoted = False
    escaped = False
    for character in value:
        if quoted:
            current.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = False
            continue
        if character == '"':
            quoted = True
            current.append(character)
        elif character == "(":
            depth += 1
            current.append(character)
        elif character == ")":
            depth -= 1
            current.append(character)
        elif character == "," and depth == 0:
            arguments.append("".join(current).strip())
            current.clear()
        else:
            current.append(character)
    arguments.append("".join(current).strip())
    return arguments


def generated_rows(path: Path, prefix: str) -> list[list[str]]:
    rows: list[list[str]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix):
            rows.append(split_cpp_arguments(line[len(prefix) : -1]))
    return rows


class CodexA11A13CurrentStateTest(unittest.TestCase):
    repo_root: Path

    @classmethod
    def setUpClass(cls) -> None:
        detail = cls.repo_root / "src/ai/openai/codex/detail"
        registry_rows = generated_rows(
            detail / "ProtocolSurfaceRegistryData.inc",
            "CODEX_PROTOCOL_SURFACE_ENTRY(",
        )
        cls.rows = [
            {
                "category": row[0].removeprefix("SurfaceCategory::"),
                "name": json.loads(row[3]),
                "disposition": row[6].removeprefix("RuntimeDisposition::"),
                "implementation": row[7].removeprefix(
                    "TypedImplementationStatus::"
                ),
                "target": row[12],
                "parameter": json.loads(row[13]),
                "result": json.loads(row[14]),
                "slice": row[19].removeprefix("A1Slice::"),
                "schema": row[20].removeprefix("TypedSchemaStatus::"),
                "evidence": row[21],
            }
            for row in registry_rows
        ]
        cls.descriptors = {
            "ClientRequest": {
                json.loads(row[3]): row[4]
                for row in generated_rows(
                    detail / "ClientOperationCodecDescriptors.inc",
                    "CODEX_CLIENT_OPERATION_CODEC_DESCRIPTOR(",
                )
            },
            "ServerNotification": {
                json.loads(row[3]): row[4]
                for row in generated_rows(
                    detail / "ServerNotificationCodecDescriptors.inc",
                    "CODEX_SERVER_NOTIFICATION_CODEC_DESCRIPTOR(",
                )
            },
            "ServerRequest": {
                json.loads(row[3]): row[4]
                for row in generated_rows(
                    detail / "ServerRequestCodecDescriptors.inc",
                    "CODEX_SERVER_REQUEST_CODEC_DESCRIPTOR(",
                )
            },
        }

    def test_exact_current_slice_taxonomy(self) -> None:
        for slice_name, expected in EXPECTED_TAXONOMY.items():
            current = [row for row in self.rows if row["slice"] == slice_name]
            self.assertEqual(expected, Counter(row["category"] for row in current))
            self.assertEqual(sum(expected.values()), len(current))

    def test_every_identity_is_currently_complete_and_typed(self) -> None:
        rows = [row for row in self.rows if row["slice"] in EXPECTED_TAXONOMY]
        self.assertEqual(264, len(rows))
        self.assertEqual({"Complete"}, {row["schema"] for row in rows})
        self.assertEqual({"Typed"}, {row["disposition"] for row in rows})
        self.assertEqual({"Implemented"}, {row["implementation"] for row in rows})
        complete = (
            "schemaCompletenessEvidence(" + ", ".join(["true"] * 14) + ")"
        )
        self.assertEqual({complete}, {row["evidence"] for row in rows})

    def test_runtime_descriptor_targets_match_current_registry(self) -> None:
        for row in self.rows:
            descriptors = self.descriptors.get(row["category"])
            if descriptors is None or row["slice"] not in EXPECTED_TAXONOMY:
                continue
            self.assertIn(row["name"], descriptors)
            self.assertEqual(row["target"], descriptors[row["name"]])

    def test_operation_schema_roots_exist_in_vendored_protocol(self) -> None:
        schema_root = (
            self.repo_root / "tools/codex/app-server-schema/0.144.6/stable"
        )
        definitions: set[str] = set()
        for path in schema_root.rglob("*.json"):
            document = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(document.get("title"), str):
                definitions.add(document["title"])
            definitions.update(document.get("definitions", {}))

        referenced = {
            type_name
            for row in self.rows
            if row["slice"] in EXPECTED_TAXONOMY
            and row["category"] in {"ClientRequest", "ServerRequest"}
            for type_name in (row["parameter"], row["result"])
            if type_name and type_name != "Unit"
        }
        self.assertEqual(set(), referenced - definitions)

    def test_current_product_guards_remain_registered(self) -> None:
        cmake = (
            self.repo_root / "tests/component/codex/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        required = {
            "CodexA11OperationProductionCoverageGuardTest",
            "CodexA11NotificationProductionCoverageGuardTest",
            "CodexA12AccountCodecTest",
            "CodexA13CommandWireTest",
            "CodexA13ApprovalWireTest",
            "AppServerClientRequestLifecycleTest",
            "CodexProtocolSurfaceRegistryTest",
        }
        self.assertEqual(set(), {name for name in required if name not in cmake})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    arguments, remaining = parser.parse_known_args()
    CodexA11A13CurrentStateTest.repo_root = arguments.repo_root.resolve()
    unittest.main(argv=[__file__, *remaining])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
