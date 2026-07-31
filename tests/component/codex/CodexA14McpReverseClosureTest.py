#!/usr/bin/env python3
"""Verify deterministic Codex A1.4b MCP/reverse closure evidence."""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
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


class GitFixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.commit_number = 0
        self.git("init", "-q", "--initial-branch=main")
        (self.root / "fixture.txt").write_text("base\n", encoding="utf-8")
        self.git("add", "fixture.txt")
        tree = self.git("write-tree")
        self.base = self.commit(tree, (), "Fixture base")
        self.set_head(self.base)
        self.base_tree = self.tree(self.base)

    def git(self, *arguments: str, env: dict[str, str] | None = None) -> str:
        return subprocess.run(
            ("git", *arguments),
            cwd=self.root,
            check=True,
            text=True,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout.strip()

    def commit(
        self,
        tree: str,
        parents: tuple[str, ...],
        subject: str,
    ) -> str:
        self.commit_number += 1
        timestamp = 946684800 + self.commit_number
        environment = {
            **os.environ,
            "GIT_AUTHOR_NAME": "AISuite closure fixture",
            "GIT_AUTHOR_EMAIL": "closure@example.invalid",
            "GIT_COMMITTER_NAME": "AISuite closure fixture",
            "GIT_COMMITTER_EMAIL": "closure@example.invalid",
            "GIT_AUTHOR_DATE": f"{timestamp} +0000",
            "GIT_COMMITTER_DATE": f"{timestamp} +0000",
        }
        command = ["commit-tree", tree]
        for parent in parents:
            command.extend(("-p", parent))
        command.extend(("-m", subject))
        return self.git(*command, env=environment)

    def tree(self, revision: str) -> str:
        return self.git("rev-parse", f"{revision}^{{tree}}")

    def set_head(self, revision: str) -> None:
        self.git("update-ref", "refs/heads/main", revision)
        self.git("symbolic-ref", "HEAD", "refs/heads/main")

    def feature(
        self,
        subjects: tuple[str, ...],
        *,
        start: str | None = None,
    ) -> str:
        parent = self.base if start is None else start
        for subject in subjects:
            parent = self.commit(
                self.tree(parent),
                (parent,),
                subject,
            )
        return parent

    def merge(
        self,
        first_parent: str,
        second_parent: str,
        *,
        tree: str | None = None,
    ) -> str:
        return self.commit(
            self.tree(second_parent) if tree is None else tree,
            (first_parent, second_parent),
            "Merge A1.4b fixture",
        )

    def descendant(self, parent: str, subject: str) -> str:
        return self.commit(self.tree(parent), (parent,), subject)

    def changed_tree(self, parent: str) -> str:
        self.git("read-tree", parent)
        (self.root / "merge-only.txt").write_text(
            "merge-only change\n",
            encoding="utf-8",
        )
        self.git("add", "merge-only.txt")
        return self.git("write-tree")


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

    def fixture(self) -> GitFixture:
        temporary = tempfile.TemporaryDirectory(
            prefix="aisuite-a14b-history-"
        )
        self.addCleanup(temporary.cleanup)
        return GitFixture(Path(temporary.name))

    def resolve_fixture(
        self,
        fixture: GitFixture,
        head: str,
        *,
        base_sha: str | None = None,
        base_tree: str | None = None,
    ) -> Any:
        fixture.set_head(head)
        return self.tool.resolve_history(
            fixture.root,
            base_sha=fixture.base if base_sha is None else base_sha,
            base_tree=(
                fixture.base_tree if base_tree is None else base_tree
            ),
            subjects=tuple(self.tool.COMMIT_SUBJECTS),
            require_final=True,
        )

    def assert_history_error(
        self,
        fixture: GitFixture,
        head: str,
        expected_code: str,
    ) -> None:
        with self.assertRaises(self.tool.ClosureError) as caught:
            self.resolve_fixture(fixture, head)
        self.assertEqual((expected_code,), caught.exception.codes)

    def test_valid_unmerged_six_commit_feature_branch(self) -> None:
        fixture = self.fixture()
        feature = fixture.feature(tuple(self.tool.COMMIT_SUBJECTS))
        resolution = self.resolve_fixture(fixture, feature)
        self.assertEqual("unmerged-feature", resolution.topology_kind)
        self.assertEqual(feature, resolution.validated_feature_head)
        self.assertIsNone(resolution.merge_commit)
        self.assertEqual(6, len(resolution.validated_commits))

    def test_valid_normal_merge_commit(self) -> None:
        fixture = self.fixture()
        feature = fixture.feature(tuple(self.tool.COMMIT_SUBJECTS))
        merge = fixture.merge(fixture.base, feature)
        resolution = self.resolve_fixture(fixture, merge)
        self.assertEqual("direct-merge", resolution.topology_kind)
        self.assertEqual(feature, resolution.validated_feature_head)
        self.assertEqual(merge, resolution.merge_commit)
        self.assertEqual(
            fixture.tree(feature),
            fixture.tree(resolution.merge_commit),
        )

    def test_valid_later_descendant_of_merge(self) -> None:
        fixture = self.fixture()
        feature = fixture.feature(tuple(self.tool.COMMIT_SUBJECTS))
        merge = fixture.merge(fixture.base, feature)
        descendant = fixture.descendant(merge, "Later unrelated work")
        resolution = self.resolve_fixture(fixture, descendant)
        self.assertEqual("later-descendant", resolution.topology_kind)
        self.assertEqual(feature, resolution.validated_feature_head)
        self.assertEqual(merge, resolution.merge_commit)
        self.assertEqual(6, len(resolution.validated_commits))

    def test_actual_repository_merge_topology_is_bounded(self) -> None:
        resolution = self.tool.resolve_history(
            OPTIONS.repo_root,
            require_final=True,
        )
        self.assertEqual(
            "78db77030adeb08b3a785a5f36a48a77624196c2",
            resolution.validated_feature_head,
        )
        self.assertEqual(
            "a60b81de74c34d572f4ba58cda8c03383d36944b",
            resolution.merge_commit,
        )
        self.assertEqual(6, len(resolution.validated_commits))

    def test_reversed_merge_parent_order_is_rejected(self) -> None:
        fixture = self.fixture()
        feature = fixture.feature(tuple(self.tool.COMMIT_SUBJECTS))
        merge = fixture.merge(feature, fixture.base)
        self.assert_history_error(
            fixture,
            merge,
            "McpReverseClosureMergeParentMismatch",
        )

    def test_wrong_first_merge_parent_is_rejected(self) -> None:
        fixture = self.fixture()
        feature = fixture.feature(tuple(self.tool.COMMIT_SUBJECTS))
        wrong_first = fixture.descendant(
            fixture.base,
            "Wrong first merge parent",
        )
        merge = fixture.merge(wrong_first, feature)
        self.assert_history_error(
            fixture,
            merge,
            "McpReverseClosureMergeParentMismatch",
        )

    def test_wrong_second_parent_history_is_rejected(self) -> None:
        fixture = self.fixture()
        subjects = list(self.tool.COMMIT_SUBJECTS)
        subjects[-1] = "Wrong sixth feature subject"
        feature = fixture.feature(tuple(subjects))
        merge = fixture.merge(fixture.base, feature)
        self.assert_history_error(
            fixture,
            merge,
            "McpReverseClosureHistoryMismatch",
        )

    def test_merge_tree_change_is_rejected(self) -> None:
        fixture = self.fixture()
        feature = fixture.feature(tuple(self.tool.COMMIT_SUBJECTS))
        merge = fixture.merge(
            fixture.base,
            feature,
            tree=fixture.changed_tree(feature),
        )
        self.assert_history_error(
            fixture,
            merge,
            "McpReverseClosureMergeTreeMismatch",
        )

    def test_five_commit_bounded_range_is_rejected(self) -> None:
        fixture = self.fixture()
        feature = fixture.feature(tuple(self.tool.COMMIT_SUBJECTS[:5]))
        self.assert_history_error(
            fixture,
            feature,
            "McpReverseClosureHistoryMismatch",
        )

    def test_seven_commit_bounded_range_is_rejected(self) -> None:
        fixture = self.fixture()
        subjects = (*self.tool.COMMIT_SUBJECTS, "Inserted functional commit")
        feature = fixture.feature(tuple(subjects))
        self.assert_history_error(
            fixture,
            feature,
            "McpReverseClosureHistoryMismatch",
        )

    def test_subject_order_is_rejected(self) -> None:
        fixture = self.fixture()
        subjects = list(self.tool.COMMIT_SUBJECTS)
        subjects[2], subjects[3] = subjects[3], subjects[2]
        feature = fixture.feature(tuple(subjects))
        self.assert_history_error(
            fixture,
            feature,
            "McpReverseClosureHistoryMismatch",
        )

    def test_subject_text_is_rejected(self) -> None:
        fixture = self.fixture()
        subjects = list(self.tool.COMMIT_SUBJECTS)
        subjects[2] += " rewritten"
        feature = fixture.feature(tuple(subjects))
        self.assert_history_error(
            fixture,
            feature,
            "McpReverseClosureHistoryMismatch",
        )

    def test_unrelated_similar_merge_is_rejected(self) -> None:
        fixture = self.fixture()
        unrelated_base = fixture.descendant(
            fixture.base,
            "Unrelated intervening commit",
        )
        similar = fixture.feature(
            tuple(self.tool.COMMIT_SUBJECTS),
            start=unrelated_base,
        )
        merge = fixture.merge(unrelated_base, similar)
        self.assert_history_error(
            fixture,
            merge,
            "McpReverseClosureTopologyMismatch",
        )

    def test_squash_topology_is_rejected(self) -> None:
        fixture = self.fixture()
        squash = fixture.descendant(
            fixture.base,
            "Squashed A1.4b implementation",
        )
        self.assert_history_error(
            fixture,
            squash,
            "McpReverseClosureHistoryMismatch",
        )

    def test_wrong_base_tree_is_rejected(self) -> None:
        fixture = self.fixture()
        feature = fixture.feature(tuple(self.tool.COMMIT_SUBJECTS))
        fixture.set_head(feature)
        with self.assertRaises(self.tool.ClosureError) as caught:
            self.tool.resolve_history(
                fixture.root,
                base_sha=fixture.base,
                base_tree="0" * 40,
                subjects=tuple(self.tool.COMMIT_SUBJECTS),
                require_final=True,
            )
        self.assertEqual(
            ("McpReverseClosureBaseMismatch",),
            caught.exception.codes,
        )

    def test_later_subjects_are_outside_the_bounded_range(self) -> None:
        fixture = self.fixture()
        feature = fixture.feature(tuple(self.tool.COMMIT_SUBJECTS))
        merge = fixture.merge(fixture.base, feature)
        later = fixture.descendant(
            merge,
            "Adopt the cleaned SNode.C dependency",
        )
        later = fixture.descendant(
            later,
            "Unrelated future AISuite work",
        )
        resolution = self.resolve_fixture(fixture, later)
        self.assertEqual("later-descendant", resolution.topology_kind)
        self.assertEqual(
            tuple(self.tool.COMMIT_SUBJECTS),
            tuple(
                commit.subject for commit in resolution.validated_commits
            ),
        )

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
