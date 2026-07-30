#!/usr/bin/env python3
"""Determinism and focused mutation guards for the A1.4b freeze audit."""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import sys
import unittest
from pathlib import Path
from types import ModuleType
from typing import Callable

sys.dont_write_bytecode = True


def load_tool(path: Path) -> ModuleType:
    sys.path.insert(0, str(path.resolve().parent))
    specification = importlib.util.spec_from_file_location(
        "aisuite_codex_a14_mcp_reverse", path
    )
    if specification is None or specification.loader is None:
        raise AssertionError(f"unable to import audit tool: {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


class CodexA14McpReverseAuditTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tool = load_tool(OPTIONS.tool)
        cls.arguments = cls.tool.parser().parse_args(
            [
                "check",
                "--repo-root",
                str(OPTIONS.repo_root),
                "--start-state",
                str(OPTIONS.start_state),
                "--batch-plan",
                str(OPTIONS.batch_plan),
            ]
        )
        for name, value in vars(cls.arguments).items():
            if isinstance(value, Path):
                setattr(cls.arguments, name, value.resolve())
        cls.start, cls.plan = cls.tool.build_reports(cls.arguments)

    def assert_mutation(
        self,
        mutate: Callable[[dict[str, object], dict[str, object]], None],
        expected_code: str,
    ) -> None:
        start = copy.deepcopy(self.start)
        plan = copy.deepcopy(self.plan)
        before = json.dumps((start, plan), sort_keys=True)
        mutate(start, plan)
        after = json.dumps((start, plan), sort_keys=True)
        self.assertNotEqual(before, after, "planted mutation changed no input")
        diagnostics = self.tool.report_diagnostics(start, plan)
        codes = {row.code for row in diagnostics}
        self.assertIn(expected_code, codes)
        with self.assertRaises(self.tool.AuditError) as raised:
            self.tool.validate_reports(start, plan)
        self.assertIn(expected_code, raised.exception.codes)
        self.tool.validate_reports(self.start, self.plan)

    def test_checked_evidence_is_current_and_deterministic(self) -> None:
        second_start, second_plan = self.tool.build_reports(self.arguments)
        self.assertEqual(self.start, second_start)
        self.assertEqual(self.plan, second_plan)
        self.assertEqual(
            self.start,
            json.loads(OPTIONS.start_state.read_text(encoding="utf-8")),
        )
        self.assertEqual(
            self.plan,
            json.loads(OPTIONS.batch_plan.read_text(encoding="utf-8")),
        )
        self.tool.validate_reports(self.start, self.plan)

    def test_exact_scope_closure_and_arithmetic(self) -> None:
        self.assertEqual(13, self.plan["scope"]["identity_count"])
        self.assertEqual(
            {
                "client_requests": 4,
                "server_notifications": 2,
                "server_requests": 4,
                "tagged_union_alternatives": 3,
            },
            self.plan["scope"]["taxonomy"],
        )
        counts = self.plan["schema_closure"]["counts"]
        self.assertEqual(18, counts["seed_definitions"])
        self.assertEqual(55, counts["reachable_named_definitions"])
        self.assertEqual(204, counts["schema_paths"])
        self.assertEqual({"legacy": 34, "v2": 21}, counts["definition_namespaces"])
        self.assertEqual(87, counts["required_properties"])
        self.assertEqual(92, counts["optional_properties"])
        self.assertEqual(97, counts["nullable_paths"])
        self.assertEqual(3, counts["default_bearing_paths"])
        self.assertEqual(48, counts["object_nodes"])
        self.assertEqual(33, counts["open_objects"])
        self.assertEqual(12, counts["closed_objects"])
        self.assertEqual(3, counts["schema_valued_additional_properties"])
        self.assertEqual(24, counts["opaque_json_paths"])
        self.assertEqual(53, counts["sensitive_paths"])
        self.assertEqual(
            [
                {"Complete": 39, "NotImplemented": 16, "Partial": 1},
                {"Complete": 41, "NotImplemented": 14, "Partial": 1},
                {"Complete": 46, "NotImplemented": 10, "Partial": 0},
            ],
            [stage["native_a1_4"] for stage in self.plan["stages"]],
        )

    def test_api_and_variant_freeze_is_application_facing(self) -> None:
        facade = self.plan["public_api"]["facade"]
        methods = facade["methods"]
        self.assertEqual(4, len(methods))
        self.assertTrue(any("startOauthLogin" in method for method in methods))
        self.assertTrue(any("readResource" in method for method in methods))
        self.assertTrue(any("callTool" in method for method in methods))
        self.assertTrue(any("listServers" in method for method in methods))
        self.assertEqual([], facade["raw_method_escape_hatches"])
        reverse = self.plan["reverse_request_api"]
        self.assertEqual(5, len(reverse["respond_methods"]))
        self.assertEqual(4, len(reverse["reject_methods"]))
        self.assertTrue(
            any(
                "ToolRequestUserInputResponse" in method
                for method in reverse["respond_methods"]
            )
        )
        self.assertTrue(
            any(
                "std::vector<UserInputAnswer>" in method
                for method in reverse["respond_methods"]
            )
        )
        request_variant = self.plan["request_variant"]
        self.assertEqual(2, request_variant["preserved_indices"]["UserInputRequest"])
        self.assertEqual([8, 9, 10], [row["index"] for row in request_variant["appends"]])
        notification = self.plan["notification_append"]
        self.assertEqual(
            [57, 58],
            [row["canonical_index"] for row in notification["mapping"]],
        )
        self.assertEqual(
            [59, 60],
            [row["event_index"] for row in notification["mapping"]],
        )

    def test_identity_scope_mutations(self) -> None:
        cases = (
            (
                lambda start, plan: plan["scope"]["identities"].pop(),
                "McpReverseIdentitySetMismatch",
            ),
            (
                lambda start, plan: plan["scope"]["identities"].append(
                    {
                        "protocol_surface_key": {
                            "category": "server_notification",
                            "domain": "ServerNotification",
                            "discriminator_field": "method",
                            "name": "warning",
                        }
                    }
                ),
                "McpReversePrCScopeLeak",
            ),
            (
                lambda start, plan: plan["scope"]["identities"].append(
                    {
                        "protocol_surface_key": {
                            "category": "client_request",
                            "domain": "ClientRequest",
                            "discriminator_field": "method",
                            "name": "initialize",
                        }
                    }
                ),
                "McpReverseInheritedScopeLeak",
            ),
            (
                lambda start, plan: next(
                    row
                    for row in plan["scope"]["identities"]
                    if row["protocol_surface_key"]["name"] == "form"
                )["protocol_surface_key"].__setitem__(
                    "domain", "ToolRequestUserInputParams"
                ),
                "McpReverseElicitationOwnershipMismatch",
            ),
        )
        for mutate, code in cases:
            with self.subTest(code=code):
                self.assert_mutation(mutate, code)

    def test_index_closure_status_and_stage_mutations(self) -> None:
        cases = (
            (
                lambda start, plan: plan["notification_append"]["mapping"].__setitem__(
                    slice(0, 2),
                    list(reversed(plan["notification_append"]["mapping"])),
                ),
                "McpReverseNotificationAppendIndexMismatch",
            ),
            (
                lambda start, plan: plan["request_variant"]["appends"][0].__setitem__(
                    "index", 9
                ),
                "McpReverseRequestAppendIndexMismatch",
            ),
            (
                lambda start, plan: plan["schema_closure"]["counts"].__setitem__(
                    "schema_paths", 203
                ),
                "McpReverseSchemaClosureMismatch",
            ),
            (
                lambda start, plan: start["registry_start"]["scope_statuses"][
                    0
                ].__setitem__("status", "Complete"),
                "McpReverseFalseComplete",
            ),
            (
                lambda start, plan: plan["scope"]["identities"][0].__setitem__(
                    "promotion_commit", 6
                ),
                "McpReversePromotionStageMismatch",
            ),
            (
                lambda start, plan: start["project"].__setitem__(
                    "codex_soversion", 2
                ),
                "McpReverseSOVERSIONDrift",
            ),
        )
        for mutate, code in cases:
            with self.subTest(code=code):
                self.assert_mutation(mutate, code)


def parse_options() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--start-state", required=True, type=Path)
    parser.add_argument("--batch-plan", required=True, type=Path)
    return parser.parse_args()


OPTIONS = parse_options()

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
