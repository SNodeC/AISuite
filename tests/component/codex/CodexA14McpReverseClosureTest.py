#!/usr/bin/env python3
"""Verify deterministic Codex A1.4b MCP/reverse closure evidence."""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import sys
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any, Callable

sys.dont_write_bytecode = True


def load_tool(path: Path) -> ModuleType:
    sys.path.insert(0, str(path.parent))
    specification = importlib.util.spec_from_file_location(
        "app_server_a1_4_mcp_reverse_closure_under_test",
        path,
    )
    if specification is None or specification.loader is None:
        raise AssertionError(f"unable to import closure tool: {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def tool_arguments(tool: ModuleType) -> argparse.Namespace:
    arguments = tool.parser().parse_args(
        [
            "check",
            "--repo-root",
            str(OPTIONS.repo_root),
            "--output",
            str(OPTIONS.report),
        ]
    )
    for name, value in vars(arguments).items():
        if isinstance(value, Path):
            setattr(arguments, name, value.resolve())
    return arguments


class CodexA14McpReverseClosureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tool = load_tool(OPTIONS.tool)
        cls.arguments = tool_arguments(cls.tool)
        cls.expected = cls.tool.build_report(
            cls.arguments,
            require_final_history=True,
        )
        cls.actual = cls.tool.load(cls.arguments.output)
        cls.tool.validate_report(cls.actual, cls.expected)
        if (
            cls.arguments.output.read_text(encoding="utf-8")
            != cls.tool.render(cls.expected)
        ):
            raise AssertionError("checked closure evidence is not canonical")

    def assert_mutation(
        self,
        mutate: Callable[[dict[str, Any]], None],
        expected_code: str,
    ) -> None:
        changed = copy.deepcopy(self.expected)
        before = json.dumps(changed, sort_keys=True, separators=(",", ":"))
        mutate(changed)
        after = json.dumps(changed, sort_keys=True, separators=(",", ":"))
        self.assertNotEqual(before, after, "planted mutation changed no input")
        diagnostics = self.tool.report_diagnostics(changed, self.expected)
        self.assertEqual(
            (expected_code,),
            tuple(row.code for row in diagnostics),
        )
        with self.assertRaises(self.tool.ClosureError) as caught:
            self.tool.validate_report(changed, self.expected)
        self.assertEqual((expected_code,), caught.exception.codes)
        self.tool.validate_report(self.expected, self.expected)

    def test_checked_report_is_exact(self) -> None:
        self.assertEqual(self.actual, self.expected)
        self.assertEqual(13, self.actual["scope"]["identity_count"])
        self.assertEqual(
            {"Complete": 326, "Partial": 3, "NotImplemented": 10, "NotApplicable": 48, "Total": 387},
            self.actual["registry"]["final_global"],
        )
        self.assertEqual(
            {"Complete": 46, "Partial": 0, "NotImplemented": 10, "Total": 56},
            self.actual["registry"]["final_native_a1_4"],
        )

    def test_scope_mutation_is_isolated(self) -> None:
        self.assert_mutation(
            lambda report: report["scope"]["complete_identities"].pop(),
            "McpReverseClosureScopeMismatch",
        )

    def test_schema_closure_mutation_is_isolated(self) -> None:
        self.assert_mutation(
            lambda report: report["schema_closure"].__setitem__(
                "schema_paths", 203
            ),
            "McpReverseClosureSchemaMismatch",
        )

    def test_registry_mutation_is_isolated(self) -> None:
        self.assert_mutation(
            lambda report: report["registry"]["final_global"].__setitem__(
                "Complete", 325
            ),
            "McpReverseClosureStatusMismatch",
        )

    def test_variant_mutation_is_isolated(self) -> None:
        self.assert_mutation(
            lambda report: report["variants"]["TypedServerRequest"][
                "appended"
            ].__setitem__("AttestationGenerateRequest", 9),
            "McpReverseClosureVariantMismatch",
        )

    def test_elicitation_ownership_mutation_is_isolated(self) -> None:
        self.assert_mutation(
            lambda report: report["elicitation_union"].__setitem__(
                "owner", "ToolRequestUserInputParams"
            ),
            "McpReverseClosureElicitationMismatch",
        )

    def test_dependency_mutation_is_isolated(self) -> None:
        self.assert_mutation(
            lambda report: report["dependency"]["normal"].__setitem__(
                "sha", "0" * 40
            ),
            "McpReverseClosureDependencyMismatch",
        )

    def test_concurrency_does_not_claim_unrecorded_execution(self) -> None:
        self.assertEqual(
            "not claimed by source-only closure; required from final-head CTest and exact-head CI",
            self.actual["concurrency"]["execution_status"],
        )
        self.assert_mutation(
            lambda report: report["concurrency"].__setitem__(
                "execution_status", "passed"
            ),
            "McpReverseClosureConcurrencyContractMismatch",
        )

    def test_commit_6_boundary_mutation_is_isolated(self) -> None:
        self.assert_mutation(
            lambda report: report["history"].__setitem__(
                "commit_6_production_correction", True
            ),
            "McpReverseClosureHistoryMismatch",
        )

    def test_soversion_mutation_is_isolated(self) -> None:
        self.assert_mutation(
            lambda report: report["project"].__setitem__(
                "codex_soversion", 2
            ),
            "McpReverseClosureSOVERSIONMismatch",
        )


def parser() -> argparse.ArgumentParser:
    repo = Path(__file__).resolve().parents[3]
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--repo-root", type=Path, default=repo)
    result.add_argument(
        "--tool",
        type=Path,
        default=repo
        / "tools/codex/app_server_a1_4_mcp_reverse_closure.py",
    )
    result.add_argument(
        "--report",
        type=Path,
        default=repo
        / "tools/codex/app-server-evidence/0.144.6/"
        "a1-4-mcp-reverse-closure-report.json",
    )
    return result


OPTIONS, REMAINING = parser().parse_known_args()
OPTIONS.repo_root = OPTIONS.repo_root.resolve()
OPTIONS.tool = OPTIONS.tool.resolve()
OPTIONS.report = OPTIONS.report.resolve()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *REMAINING])
