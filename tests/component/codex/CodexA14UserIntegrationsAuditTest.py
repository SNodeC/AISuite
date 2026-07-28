#!/usr/bin/env python3
"""Determinism and planted-failure guards for the PR-A freeze audit."""

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
        "aisuite_codex_a14_user_integrations", path
    )
    if specification is None or specification.loader is None:
        raise AssertionError(f"unable to import audit tool: {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


class CodexA14UserIntegrationsAuditTest(unittest.TestCase):
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

    def test_scope_closure_and_stage_arithmetic_are_exact(self) -> None:
        self.assertEqual(33, self.plan["scope"]["identity_count"])
        self.assertEqual(
            {
                "client_requests": 23,
                "server_notifications": 6,
                "server_requests": 0,
                "tagged_union_alternatives": 4,
            },
            self.plan["scope"]["taxonomy"],
        )
        self.assertEqual(
            {"Concrete": 20, "Unit": 3},
            self.plan["scope"]["result_contracts"],
        )
        counts = self.plan["schema_closure"]["counts"]
        self.assertEqual(52, counts["seed_definitions"])
        self.assertEqual(118, counts["reachable_named_definitions"])
        self.assertEqual(411, counts["schema_paths"])
        self.assertEqual(0, counts["closed_objects"])
        self.assertEqual(0, counts["opaque_json_paths"])
        self.assertEqual(
            [8, 18, 25, 33],
            [stage["native"]["Complete"] for stage in self.plan["stages"]],
        )

    def test_predecessor_and_append_indices_are_exact(self) -> None:
        predecessor = self.start["predecessor_variants"]
        self.assertEqual(51, len(predecessor["CanonicalServerNotification"]))
        self.assertEqual(53, len(predecessor["Event"]))
        self.assertTrue(predecessor["matches_frozen_mapping"])
        append = self.plan["notification_append"]
        self.assertEqual(
            {"CanonicalServerNotification": 57, "Event": 59},
            append["final_sizes"],
        )
        self.assertEqual(
            [51, 52, 53, 54, 55, 56],
            [row["canonical_index"] for row in append["mapping"]],
        )
        self.assertEqual(
            [53, 54, 55, 56, 57, 58],
            [row["event_index"] for row in append["mapping"]],
        )

    def test_identity_scope_contract_and_stage_mutations(self) -> None:
        cases = (
            (
                lambda start, plan: plan["scope"]["identities"].pop(),
                "UserIntegrationIdentitySetMismatch",
            ),
            (
                lambda start, plan: plan["scope"]["identities"].append(
                    {
                        "protocol_surface_key": {
                            "category": "server_request",
                            "domain": "ServerRequest",
                            "discriminator_field": "method",
                            "name": "mcpServer/elicitation/request",
                        }
                    }
                ),
                "UserIntegrationScopeLeak",
            ),
            (
                lambda start, plan: plan["scope"].__setitem__(
                    "result_contracts", {"Concrete": 21, "Unit": 2}
                ),
                "UserIntegrationResultContractMismatch",
            ),
            (
                lambda start, plan: plan["scope"]["identities"][0].__setitem__(
                    "result_type", "Unit"
                ),
                "UserIntegrationResultContractMismatch",
            ),
            (
                lambda start, plan: plan["stages"][0]["global"].__setitem__(
                    "Complete", 289
                ),
                "UserIntegrationStageArithmeticMismatch",
            ),
            (
                lambda start, plan: plan["plugin_source"].__setitem__(
                    "commit_4_reaches_plugin_source", True
                ),
                "UserIntegrationPromotionStageMismatch",
            ),
        )
        for mutate, code in cases:
            with self.subTest(code=code):
                self.assert_mutation(mutate, code)

    def test_schema_plugin_descriptor_and_fixture_mutations(self) -> None:
        cases = (
            (
                lambda start, plan: plan["schema_closure"]["counts"].__setitem__(
                    "seed_definitions", 51
                ),
                "UserIntegrationSchemaClosureMismatch",
            ),
            (
                lambda start, plan: plan["schema_closure"]["counts"].__setitem__(
                    "reachable_named_definitions", 117
                ),
                "UserIntegrationSchemaClosureMismatch",
            ),
            (
                lambda start, plan: plan["schema_closure"]["schema_paths"].pop(),
                "UserIntegrationSchemaClosureMismatch",
            ),
            (
                lambda start, plan: plan["plugin_source"].__setitem__(
                    "registry_order", ["local", "git", "npm", "remote"]
                ),
                "UserIntegrationPluginSourceOrderMismatch",
            ),
            (
                lambda start, plan: plan["plugin_source"].__setitem__(
                    "npm_build_or_runtime_dependency", True
                ),
                "UserIntegrationPluginSourceDependencyLeak",
            ),
            (
                lambda start, plan: plan["public_api"]["Plugins"]["methods"].pop(),
                "UserIntegrationDescriptorMismatch",
            ),
            (
                lambda start, plan: plan["codec_units"].pop(),
                "UserIntegrationDescriptorMismatch",
            ),
            (
                lambda start, plan: plan["fixtures"].__setitem__(
                    "required_roots", 51
                ),
                "UserIntegrationFixtureMismatch",
            ),
        )
        for mutate, code in cases:
            with self.subTest(code=code):
                self.assert_mutation(mutate, code)

    def test_package_variant_provenance_and_architecture_mutations(self) -> None:
        cases = (
            (
                lambda start, plan: start["package_boundary"].__setitem__(
                    "find_package", False
                ),
                "UserIntegrationPackageBoundaryMismatch",
            ),
            (
                lambda start, plan: start["package_boundary"].__setitem__(
                    "forbidden_source_relative_dependency", True
                ),
                "UserIntegrationCrossRepoDependencyMismatch",
            ),
            (
                lambda start, plan: start["predecessor_variants"][
                    "CanonicalServerNotification"
                ].pop(),
                "UserIntegrationNotificationBaseIndexMismatch",
            ),
            (
                lambda start, plan: start["predecessor_variants"]["Event"][0].__setitem__(
                    "type", "WrongType"
                ),
                "UserIntegrationNotificationBaseIndexMismatch",
            ),
            (
                lambda start, plan: plan["notification_append"]["mapping"][0].__setitem__(
                    "event_index", 59
                ),
                "UserIntegrationNotificationAppendIndexMismatch",
            ),
            (
                lambda start, plan: start["actual_base"].__setitem__(
                    "tree", "0" * 40
                ),
                "UserIntegrationPredecessorEvidenceDrift",
            ),
            (
                lambda start, plan: start["project"].__setitem__(
                    "codex_soversion", 2
                ),
                "UserIntegrationSOVERSIONDrift",
            ),
            (
                lambda start, plan: plan["architecture"].__setitem__(
                    "pending_operation_maps", 2
                ),
                "UserIntegrationFalseComplete",
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
