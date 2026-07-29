#!/usr/bin/env python3
"""Verify the deterministic Codex A1.4 user-integrations closure."""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any, Callable

sys.dont_write_bytecode = True


REQUESTS = (
    "app/list",
    "externalAgentConfig/detect",
    "externalAgentConfig/import",
    "externalAgentConfig/import/readHistories",
    "feedback/upload",
    "hooks/list",
    "marketplace/add",
    "marketplace/remove",
    "marketplace/upgrade",
    "plugin/install",
    "plugin/installed",
    "plugin/list",
    "plugin/read",
    "plugin/share/checkout",
    "plugin/share/delete",
    "plugin/share/list",
    "plugin/share/save",
    "plugin/share/updateTargets",
    "plugin/skill/read",
    "plugin/uninstall",
    "skills/config/write",
    "skills/extraRoots/set",
    "skills/list",
)
NOTIFICATIONS = (
    "app/list/updated",
    "externalAgentConfig/import/completed",
    "externalAgentConfig/import/progress",
    "hook/completed",
    "hook/started",
    "skills/changed",
)
PLUGIN_SOURCE_ORDER = ("git", "local", "npm", "remote")
APPENDED_TYPES = (
    "AppListUpdatedNotification",
    "ExternalAgentConfigImportCompletedNotification",
    "ExternalAgentConfigImportProgressNotification",
    "HookCompletedNotification",
    "HookStartedNotification",
    "SkillsChangedNotification",
)


def load_tool(path: Path) -> ModuleType:
    sys.path.insert(0, str(path.parent))
    specification = importlib.util.spec_from_file_location(
        "app_server_a1_4_user_integrations_closure_under_test",
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


def protocol_key(row: dict[str, Any]) -> tuple[str, str, str, str]:
    return (
        row["category"],
        row["domain"],
        row["discriminator_field"],
        row["name"],
    )


def mutate_first_scalar(value: Any) -> None:
    """Change exactly one deterministic leaf in a generated section."""
    if isinstance(value, dict):
        for key in sorted(value):
            child = value[key]
            if isinstance(child, bool):
                value[key] = not child
                return
            if isinstance(child, int):
                value[key] = child + 1
                return
            if isinstance(child, str):
                value[key] = f"{child}-mutated"
                return
            try:
                mutate_first_scalar(child)
            except ValueError:
                continue
            return
    elif isinstance(value, list):
        for child in value:
            if isinstance(child, dict | list):
                mutate_first_scalar(child)
                return
        if value:
            value.pop()
            return
    raise ValueError("section contains no mutable scalar")


class CodexA14UserIntegrationsClosureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tool = load_tool(OPTIONS.tool)
        cls.arguments = tool_arguments(cls.tool)
        cls.expected = cls.tool.build_report(cls.arguments)
        cls.tool.validate_report(cls.expected, cls.expected)

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
            tuple(diagnostic.code for diagnostic in diagnostics),
            "mutation failed through an earlier or unrelated guard",
        )
        with self.assertRaises(self.tool.ClosureError) as caught:
            self.tool.validate_report(changed, self.expected)
        self.assertEqual((expected_code,), caught.exception.codes)

        # Every planted failure is isolated from the checked-in valid model.
        self.tool.validate_report(self.expected, self.expected)

    def test_checked_report_is_current_and_deterministic(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                str(OPTIONS.tool),
                "check",
                "--repo-root",
                str(OPTIONS.repo_root),
                "--output",
                str(OPTIONS.report),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            0,
            completed.returncode,
            "closure check failed by code/path only: "
            f"stderr-lines={len(completed.stderr.splitlines())}",
        )
        committed = json.loads(OPTIONS.report.read_text(encoding="utf-8"))
        self.tool.validate_report(committed, self.expected)
        self.assertEqual(
            self.expected,
            self.tool.build_report(self.arguments),
            "second live closure construction was not deterministic",
        )

    def test_generation_sequence_normalizes_local_toolchain_paths(
        self,
    ) -> None:
        baseline = [
            self.tool._render_step(step, self.arguments.repo_root)
            for step in self.tool._generation_steps(self.arguments)
        ]
        alternate = copy.copy(self.arguments)
        alternate.abi_compiler = "/synthetic/toolchain/bin/g++"
        alternate.abi_library = Path(
            "/synthetic/build/libaisuite-openai-codex.so"
        )
        rendered = [
            self.tool._render_step(step, alternate.repo_root)
            for step in self.tool._generation_steps(alternate)
        ]
        self.assertEqual(
            baseline,
            rendered,
            "review evidence leaked a machine-local ABI toolchain path",
        )
        abi_step = next(
            step
            for step in rendered
            if step["name"] == "pr-a-api-abi-evidence"
        )
        self.assertIn("{abi-compiler}", abi_step["command"])
        self.assertIn("{abi-library}", abi_step["command"])

    def test_exact_scope_status_and_schema_closure(self) -> None:
        counts = self.expected["counts"]
        self.assertEqual(
            {
                "Complete": 313,
                "NotApplicable": 48,
                "NotImplemented": 22,
                "Partial": 4,
                "Total": 387,
            },
            counts["global_status"],
        )
        self.assertEqual(
            {
                "Complete": 33,
                "NotImplemented": 22,
                "Partial": 1,
                "Total": 56,
            },
            counts["native_a1_4_status"],
        )
        self.assertEqual(
            {
                "client_requests": 23,
                "server_notifications": 6,
                "server_requests": 0,
                "tagged_union_alternatives": 4,
            },
            counts["taxonomy"],
        )
        self.assertEqual(
            {"Concrete": 20, "Unit": 3},
            counts["result_contracts"],
        )
        closure = counts["schema_closure"]
        self.assertEqual(52, closure["seed_definitions"])
        self.assertEqual(118, closure["reachable_named_definitions"])
        self.assertEqual(411, closure["schema_paths"])

    def test_exact_complete_and_residual_identity_sets(self) -> None:
        expected_keys = {
            ("client_request", "ClientRequest", "method", name)
            for name in REQUESTS
        }
        expected_keys.update(
            {
                ("server_notification", "ServerNotification", "method", name)
                for name in NOTIFICATIONS
            }
        )
        expected_keys.update(
            {
                (
                    "tagged_union_discriminator",
                    "PluginSource",
                    "type",
                    name,
                )
                for name in PLUGIN_SOURCE_ORDER
            }
        )
        self.assertEqual(
            expected_keys,
            {
                protocol_key(row)
                for row in self.expected["exact_complete_identities"]
            },
        )
        self.assertEqual(
            {
                "error",
                "initialize",
                "initialized",
                "item/tool/requestUserInput",
            },
            {
                row["name"]
                for row in self.expected["residual_partial_identities"]
            },
        )
        self.assertEqual(
            22,
            len(self.expected["residual_not_implemented_identities"]),
        )

    def test_plugin_source_and_notification_indices_are_exact(self) -> None:
        plugin_source = self.expected["plugin_source"]
        self.assertEqual(
            list(PLUGIN_SOURCE_ORDER),
            plugin_source["registry_order"],
        )
        self.assertEqual(
            [
                "GitPluginSource",
                "LocalPluginSource",
                "NpmPluginSource",
                "RemotePluginSource",
                "UnknownPluginSource",
            ],
            plugin_source["public_variant_order"],
        )
        self.assertFalse(plugin_source["npm_build_or_runtime_dependency"])

        variants = self.expected["notification_variants"]
        self.assertEqual(
            {"CanonicalServerNotification": 51, "Event": 53},
            variants["predecessor_sizes"],
        )
        self.assertEqual(
            {"CanonicalServerNotification": 57, "Event": 59},
            variants["final_sizes"],
        )
        append = variants["append_mapping"]
        self.assertEqual(list(APPENDED_TYPES), [row["type"] for row in append])
        self.assertEqual(
            list(range(51, 57)),
            [row["canonical_index"] for row in append],
        )
        self.assertEqual(
            list(range(53, 59)),
            [row["event_index"] for row in append],
        )

    def test_identity_status_result_schema_and_order_mutations(self) -> None:
        cases = (
            (
                lambda report: report["exact_complete_identities"].pop(),
                "UserIntegrationIdentitySetMismatch",
            ),
            (
                lambda report: report["residual_partial_identities"].pop(),
                "UserIntegrationScopeLeak",
            ),
            (
                lambda report: report["residual_not_implemented_identities"].pop(),
                "UserIntegrationScopeLeak",
            ),
            (
                lambda report: report["counts"]["global_status"].__setitem__(
                    "Complete", 312
                ),
                "UserIntegrationFalseComplete",
            ),
            (
                lambda report: report["counts"]["native_a1_4_status"].__setitem__(
                    "Complete", 32
                ),
                "UserIntegrationFalseComplete",
            ),
            (
                lambda report: report["counts"]["taxonomy"].__setitem__(
                    "client_requests", 22
                ),
                "UserIntegrationIdentitySetMismatch",
            ),
            (
                lambda report: report["counts"]["result_contracts"].__setitem__(
                    "Concrete", 19
                ),
                "UserIntegrationResultContractMismatch",
            ),
            (
                lambda report: report["counts"]["schema_closure"].__setitem__(
                    "schema_paths", 410
                ),
                "UserIntegrationSchemaClosureMismatch",
            ),
            (
                lambda report: report["plugin_source"].__setitem__(
                    "registry_order", ["local", "git", "npm", "remote"]
                ),
                "UserIntegrationPluginSourceOrderMismatch",
            ),
            (
                lambda report: report["plugin_source"].__setitem__(
                    "npm_build_or_runtime_dependency", True
                ),
                "UserIntegrationPluginSourceDependencyLeak",
            ),
        )
        for mutate, code in cases:
            with self.subTest(code=code):
                self.assert_mutation(mutate, code)

    def test_bijection_boundary_and_architecture_mutations(self) -> None:
        cases = (
            (
                lambda report: mutate_first_scalar(report["descriptors"]),
                "UserIntegrationDescriptorMismatch",
            ),
            (
                lambda report: mutate_first_scalar(report["public_api"]),
                "UserIntegrationDescriptorMismatch",
            ),
            (
                lambda report: mutate_first_scalar(report["fixtures"]),
                "UserIntegrationFixtureMismatch",
            ),
            (
                lambda report: report["package_boundary"][
                    "installed_consumer"
                ].__setitem__("sha256", "0" * 64),
                "UserIntegrationInstalledConsumerNotInstalled",
            ),
            (
                lambda report: report["package_boundary"][
                    "cross_repo_dependency"
                ].__setitem__("source_commit", "0" * 40),
                "UserIntegrationCrossRepoDependencyMismatch",
            ),
            (
                lambda report: report["package_boundary"].__setitem__(
                    "all_seven_facades_consumed", False
                ),
                "UserIntegrationPackageBoundaryMismatch",
            ),
            (
                lambda report: mutate_first_scalar(report["architecture"]),
                "UserIntegrationFalseComplete",
            ),
        )
        for mutate, code in cases:
            with self.subTest(code=code):
                self.assert_mutation(mutate, code)

    def test_variant_provenance_soversion_and_generation_mutations(self) -> None:
        def mutate_predecessor_mapping(report: dict[str, Any]) -> None:
            report["notification_variants"]["predecessor_mapping"][
                "CanonicalServerNotification"
            ][0]["type"] = "WrongNotificationType"

        def mutate_append_mapping(report: dict[str, Any]) -> None:
            report["notification_variants"]["append_mapping"][0][
                "canonical_index"
            ] = 50

        cases = (
            (
                lambda report: report["authority"].__setitem__(
                    "codex_version", "codex-cli 0.144.7"
                ),
                "UserIntegrationPredecessorEvidenceDrift",
            ),
            (
                mutate_predecessor_mapping,
                "UserIntegrationNotificationBaseIndexMismatch",
            ),
            (
                lambda report: report["notification_variants"][
                    "final_sizes"
                ].__setitem__("Event", 58),
                "UserIntegrationNotificationAppendIndexMismatch",
            ),
            (
                mutate_append_mapping,
                "UserIntegrationNotificationAppendIndexMismatch",
            ),
            (
                lambda report: report["project"].__setitem__(
                    "codex_soversion", 2
                ),
                "UserIntegrationSOVERSIONDrift",
            ),
            (
                lambda report: report["predecessor_evidence"][
                    "predecessor_closure_reports"
                ][0].__setitem__("sha256", "0" * 64),
                "UserIntegrationPredecessorEvidenceDrift",
            ),
            (
                lambda report: report["deterministic_generation"].__setitem__(
                    "live_target_corpus_equality_required", False
                ),
                "UserIntegrationSecondPassNondeterminism",
            ),
            (
                lambda report: report["history_policy"].__setitem__(
                    "commit_6_subject", "Wrong closure subject"
                ),
                "UserIntegrationPromotionStageMismatch",
            ),
        )
        for mutate, code in cases:
            with self.subTest(code=code):
                self.assert_mutation(mutate, code)

    def test_live_closure_and_extraction_mutations_are_isolated(self) -> None:
        live = self.tool._corpus_snapshot(self.arguments)
        targets = (
            self.arguments.output,
            self.arguments.extraction_manifest,
        )
        for target in targets:
            relative = self.tool._relative_command_path(
                target, self.arguments.repo_root
            )
            with self.subTest(path=relative):
                planted = self.tool._snapshot_with_replacement(
                    live,
                    relative_path=relative,
                    replacement=target.read_bytes() + b"\n",
                )
                original_rows = self.tool._snapshot_index(
                    live, location="$.test.live"
                )
                planted_rows = self.tool._snapshot_index(
                    planted, location="$.test.planted"
                )
                self.assertEqual(
                    [relative],
                    [
                        path
                        for path in sorted(original_rows)
                        if original_rows[path] != planted_rows[path]
                    ],
                    "planted live mutation did not change exactly one copy",
                )
                with self.assertRaises(
                    self.tool.ClosureError
                ) as caught:
                    self.tool._require_identical_snapshots(live, planted)
                self.assertEqual(
                    ("UserIntegrationSecondPassNondeterminism",),
                    caught.exception.codes,
                )
                self.assertEqual(1, len(caught.exception.diagnostics))
                self.assertIn(
                    relative, caught.exception.diagnostics[0].location
                )
                self.tool._require_identical_snapshots(live, live)

    def test_extraction_proof_exclusion_mutations_are_isolated(self) -> None:
        extractor = (
            OPTIONS.repo_root / "tools/extraction/verify_extraction.py"
        )
        manifest_path = (
            OPTIONS.repo_root / "docs/extraction/source-manifest.json"
        )
        original = json.loads(manifest_path.read_text(encoding="utf-8"))
        expected_code = (
            "UserIntegrationExtractionProofExclusionMismatch"
        )
        cases = {
            "missing": lambda paths: paths.pop(),
            "extra": lambda paths: paths.append(
                "tools/codex/app-server-evidence/0.144.6/"
                "unexpected-fourth-proof.json"
            ),
            "wrong": lambda paths: paths.__setitem__(
                0,
                "tools/codex/app-server-evidence/0.144.6/"
                "wrong-generation-pre.json",
            ),
        }
        with tempfile.TemporaryDirectory(
            prefix="aisuite-a14-extraction-exclusion-"
        ) as temporary:
            for name, mutate in cases.items():
                with self.subTest(mutation=name):
                    changed = copy.deepcopy(original)
                    paths = changed[
                        "generation_proof_self_reference_exclusions"
                    ]
                    before = list(paths)
                    mutate(paths)
                    self.assertNotEqual(
                        before,
                        paths,
                        "planted extraction exception changed no input",
                    )
                    candidate = Path(temporary) / f"{name}.json"
                    candidate.write_text(
                        json.dumps(
                            changed,
                            ensure_ascii=False,
                            indent=2,
                            sort_keys=True,
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                    completed = subprocess.run(
                        [
                            sys.executable,
                            str(extractor),
                            "check",
                            "--repo-root",
                            str(OPTIONS.repo_root),
                            "--manifest",
                            str(candidate),
                        ],
                        check=False,
                        capture_output=True,
                        text=True,
                    )
                    self.assertNotEqual(0, completed.returncode)
                    self.assertTrue(
                        completed.stderr.startswith(
                            "aisuite-extraction: error: "
                            f"{expected_code}:"
                        ),
                        "exception mutation failed through an earlier "
                        f"diagnostic: {completed.stderr.strip()}",
                    )

        restored = subprocess.run(
            [
                sys.executable,
                str(extractor),
                "check",
                "--repo-root",
                str(OPTIONS.repo_root),
                "--manifest",
                str(manifest_path),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            0,
            restored.returncode,
            "unmodified extraction input did not pass afterward",
        )

    def test_api_abi_guard_rejects_an_isolated_stale_header_hash(self) -> None:
        abi_tool = (
            OPTIONS.repo_root
            / "tools/codex/app_server_a1_4_user_integrations_abi.py"
        )
        evidence_root = (
            OPTIONS.repo_root
            / "tools/codex/app-server-evidence/0.144.6"
        )
        evidence = (
            evidence_root
            / "a1-4-user-integrations-api-abi-evidence.json"
        )
        symbols = evidence_root / "a1-4-user-integrations-symbols.txt"
        original = json.loads(evidence.read_text(encoding="utf-8"))

        with tempfile.TemporaryDirectory(
            prefix="aisuite-a14-abi-mutation-"
        ) as temporary:
            mutated_path = Path(temporary) / evidence.name
            mutated = copy.deepcopy(original)
            header_hashes = mutated["layout_probe"]["header_sha256"]
            first_header = sorted(header_hashes)[0]
            header_hashes[first_header] = "0" * 64
            self.assertNotEqual(
                original["layout_probe"]["header_sha256"][first_header],
                header_hashes[first_header],
                "planted ABI mutation changed no input",
            )
            mutated_path.write_text(
                json.dumps(mutated, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            completed = subprocess.run(
                [
                    sys.executable,
                    str(abi_tool),
                    "check",
                    "--repo-root",
                    str(OPTIONS.repo_root),
                    "--output",
                    str(mutated_path),
                    "--symbols-output",
                    str(symbols),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(0, completed.returncode)
            self.assertEqual(
                (
                    "UserIntegrationAbiEvidenceMismatch: "
                    "public header hashes are stale"
                ),
                completed.stderr.strip(),
                "ABI mutation failed through an earlier or unrelated guard",
            )

        restored = subprocess.run(
            [
                sys.executable,
                str(abi_tool),
                "check",
                "--repo-root",
                str(OPTIONS.repo_root),
                "--output",
                str(evidence),
                "--symbols-output",
                str(symbols),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            0,
            restored.returncode,
            "unmodified API/ABI evidence did not pass after mutation",
        )


def parse_options() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    return parser.parse_args()


if __name__ == "__main__":
    OPTIONS = parse_options()
    OPTIONS.tool = OPTIONS.tool.resolve()
    OPTIONS.repo_root = OPTIONS.repo_root.resolve()
    OPTIONS.report = OPTIONS.report.resolve()
    unittest.main(argv=[sys.argv[0]])
