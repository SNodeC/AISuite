#!/usr/bin/env python3
"""Exercise the current Codex public-header and logging policy guards."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Mapping

sys.dont_write_bytecode = True


DIAGNOSTIC_PATTERN = re.compile(r"\b(CodexPolicy[A-Za-z0-9]+)\b")
SEMANTIC_ENVIRONMENT_KEYS = (
    "AISUITE_CODEX_SEMANTIC_LOGGER_AUTHORITY_FILE",
    "AISUITE_CODEX_SEMANTIC_LOGGER_CLASSIFICATIONS_FILE",
)


def policy_environment(
    overrides: Mapping[str, str] | None = None,
) -> dict[str, str]:
    environment = os.environ.copy()
    for key in SEMANTIC_ENVIRONMENT_KEYS:
        environment.pop(key, None)
    if overrides is not None:
        environment.update(overrides)
    return environment


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class CodexPolicyMutationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.repo_root = OPTIONS.repo_root.resolve()
        cls.public_header_policy = OPTIONS.public_header_policy.resolve()
        cls.logging_policy = OPTIONS.logging_policy.resolve()
        cls.semantic_logger_policy = OPTIONS.semantic_logger_policy.resolve()
        cls._assert_clean_executable(cls.public_header_policy)
        cls._assert_clean_executable(cls.logging_policy)
        cls._assert_clean_executable(cls.semantic_logger_policy)

    @classmethod
    def _assert_clean_executable(cls, executable: Path) -> None:
        completed = subprocess.run(
            [str(executable), "--repo-root", str(cls.repo_root)],
            check=False,
            capture_output=True,
            text=True,
            env=policy_environment(),
        )
        if completed.returncode != 0:
            raise AssertionError(
                "unmodified policy authority failed: "
                f"{executable.name}: {completed.stderr or completed.stdout}"
            )

    def assert_guard_failure(
        self,
        command: list[str],
        expected_code: str,
        *,
        environment: Mapping[str, str] | None = None,
    ) -> None:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            env=policy_environment(environment),
        )
        self.assertNotEqual(
            0,
            completed.returncode,
            f"planted mutation was accepted: {command}",
        )
        diagnostics = set(
            DIAGNOSTIC_PATTERN.findall(
                completed.stdout + "\n" + completed.stderr
            )
        )
        self.assertEqual(
            {expected_code},
            diagnostics,
            "mutation failed through an earlier or unrelated guard: "
            f"stdout={completed.stdout!r}, stderr={completed.stderr!r}",
        )

        clean = subprocess.run(
            [
                command[0],
                "--repo-root",
                str(self.repo_root),
            ],
            check=False,
            capture_output=True,
            text=True,
            env=policy_environment(),
        )
        self.assertEqual(
            0,
            clean.returncode,
            "unmodified policy authority failed after isolated mutation: "
            f"{clean.stderr or clean.stdout}",
        )

    def temporary_codex_tree(self) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory(
            prefix="aisuite-codex-policy-mutation-"
        )
        root = Path(temporary.name)
        destination = root / "src/ai/openai/codex"
        destination.parent.mkdir(parents=True)
        shutil.copytree(
            self.repo_root / "src/ai/openai/codex",
            destination,
        )
        policy_data = root / "tests/policy/codex"
        policy_data.mkdir(parents=True)
        for name in (
            "CodexSemanticLoggerAuthority.tsv",
            "CodexSemanticLoggerClassifications.tsv",
        ):
            shutil.copy2(
                self.repo_root / "tests/policy/codex" / name,
                policy_data / name,
            )
        return temporary

    def run_tree_policy(
        self,
        executable: Path,
        temporary_root: Path,
        expected_code: str,
        *,
        environment: Mapping[str, str] | None = None,
    ) -> None:
        self.assert_guard_failure(
            [str(executable), "--repo-root", str(temporary_root)],
            expected_code,
            environment=environment,
        )

    def test_public_header_inventory_removal_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            cmake = root / "src/ai/openai/codex/CMakeLists.txt"
            source = cmake.read_text(encoding="utf-8")
            marker = "    typed/Accounts.h\n"
            self.assertEqual(2, source.count(marker))
            before = digest(cmake)
            cmake.write_text(source.replace(marker, "", 1), encoding="utf-8")
            self.assertNotEqual(before, digest(cmake))
            self.run_tree_policy(
                self.public_header_policy,
                root,
                "CodexPolicyPublicHeaderInventoryMismatch",
            )

    def test_frontend_service_header_substitution_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            frontend = root / "src/ai/openai/codex/frontend"
            cmake = frontend / "CMakeLists.txt"
            source = cmake.read_text(encoding="utf-8")
            marker = "    FrontendService.h Codec.h"
            self.assertEqual(1, source.count(marker))
            shutil.copy2(
                frontend / "FrontendService.h",
                frontend / "BackendAdapter.h",
            )
            cmake.write_text(
                source.replace(
                    marker,
                    "    BackendAdapter.h Codec.h",
                    1,
                ),
                encoding="utf-8",
            )
            self.run_tree_policy(
                self.public_header_policy,
                root,
                "CodexPolicyPublicHeaderInventoryMismatch",
            )

    def test_public_backend_adapter_alias_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            header = root / "src/ai/openai/codex/frontend/FrontendService.h"
            source = header.read_text(encoding="utf-8")
            marker = "} // namespace ai::openai::codex::frontend"
            self.assertEqual(1, source.count(marker))
            header.write_text(
                source.replace(
                    marker,
                    "    using BackendAdapter = FrontendService;\n\n" + marker,
                    1,
                ),
                encoding="utf-8",
            )
            self.run_tree_policy(
                self.public_header_policy,
                root,
                "CodexPolicyPublicHeaderInventoryMismatch",
            )

    def test_pragma_once_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            header = root / "src/ai/openai/codex/typed/Accounts.h"
            source = header.read_text(encoding="utf-8")
            header.write_text(
                source.replace(
                    "#ifndef AI_OPENAI_CODEX_TYPED_ACCOUNTS_H",
                    "#pragma once\n\n#ifndef AI_OPENAI_CODEX_TYPED_ACCOUNTS_H",
                    1,
                ),
                encoding="utf-8",
            )
            self.run_tree_policy(
                self.public_header_policy,
                root,
                "CodexPolicyHeaderGuardMismatch",
            )

    def test_broken_guard_pair_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            header = root / "src/ai/openai/codex/typed/Accounts.h"
            source = header.read_text(encoding="utf-8")
            marker = "#define AI_OPENAI_CODEX_TYPED_ACCOUNTS_H"
            self.assertEqual(1, source.count(marker))
            header.write_text(
                source.replace(
                    marker,
                    "#define AI_OPENAI_CODEX_TYPED_ACCOUNTS_H_BROKEN",
                    1,
                ),
                encoding="utf-8",
            )
            self.run_tree_policy(
                self.public_header_policy,
                root,
                "CodexPolicyHeaderGuardMismatch",
            )

    def test_forbidden_logging_member_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            header = root / "src/ai/openai/codex/backend/BackendState.h"
            source = header.read_text(encoding="utf-8")
            header.write_text(
                source
                + "\nstruct CodexPolicyMutationState {\n"
                + "    bool lifecycleTerminalLogged = false;\n"
                + "};\n",
                encoding="utf-8",
            )
            self.run_tree_policy(
                self.logging_policy,
                root,
                "CodexPolicyLoggingApiSurfaceMismatch",
            )

    @staticmethod
    def semantic_rows(path: Path) -> list[str]:
        return [
            line
            for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#") and not line.startswith("path\t")
        ]

    @staticmethod
    def write_semantic_rows(
        source: Path,
        destination: Path,
        rows: list[str],
    ) -> None:
        metadata = [
            line
            for line in source.read_text(encoding="utf-8").splitlines()
            if not line or line.startswith("#") or line.startswith("path\t")
        ]
        destination.write_text(
            "\n".join([*metadata, *rows]) + "\n",
            encoding="utf-8",
        )

    def test_unclassified_semantic_logger_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            source_path = root / "src/ai/openai/codex/backend/Snapshot.cpp"
            source_path.write_text(
                source_path.read_text(encoding="utf-8")
                + "\nvoid codexPolicyMutationOnly() {\n"
                + "    semantic::policyMutationLog();\n"
                + "}\n",
                encoding="utf-8",
            )
            self.run_tree_policy(
                self.semantic_logger_policy,
                root,
                "CodexPolicySemanticLoggerUnclassified",
                environment={
                    "AISUITE_CODEX_SEMANTIC_LOGGER_AUTHORITY_FILE": str(
                        root
                        / "tests/policy/codex/CodexSemanticLoggerAuthority.tsv"
                    ),
                    "AISUITE_CODEX_SEMANTIC_LOGGER_CLASSIFICATIONS_FILE": str(
                        root
                        / "tests/policy/codex/"
                        "CodexSemanticLoggerClassifications.tsv"
                    ),
                },
            )

    def test_missing_semantic_classification_is_rejected(self) -> None:
        source = (
            self.repo_root
            / "tests/policy/codex/CodexSemanticLoggerClassifications.tsv"
        )
        rows = self.semantic_rows(source)
        with tempfile.TemporaryDirectory(
            prefix="aisuite-codex-classification-mutation-"
        ) as directory:
            mutated = Path(directory) / source.name
            self.write_semantic_rows(source, mutated, rows[:-1])
            self.assert_guard_failure(
                [
                    str(self.semantic_logger_policy),
                    "--repo-root",
                    str(self.repo_root),
                ],
                "CodexPolicySemanticLoggerClassificationMismatch",
                environment={
                    "AISUITE_CODEX_SEMANTIC_LOGGER_CLASSIFICATIONS_FILE": str(
                        mutated
                    )
                },
            )

    def test_semantic_authority_count_change_is_rejected(self) -> None:
        source = (
            self.repo_root
            / "tests/policy/codex/CodexSemanticLoggerAuthority.tsv"
        )
        rows = self.semantic_rows(source)
        with tempfile.TemporaryDirectory(
            prefix="aisuite-codex-authority-count-mutation-"
        ) as directory:
            mutated = Path(directory) / source.name
            self.write_semantic_rows(source, mutated, rows[:-1])
            self.assert_guard_failure(
                [
                    str(self.semantic_logger_policy),
                    "--repo-root",
                    str(self.repo_root),
                ],
                "CodexPolicySemanticLoggerAuthorityMismatch",
                environment={
                    "AISUITE_CODEX_SEMANTIC_LOGGER_AUTHORITY_FILE": str(mutated)
                },
            )

    def test_semantic_authority_expression_change_is_rejected(self) -> None:
        source = (
            self.repo_root
            / "tests/policy/codex/CodexSemanticLoggerAuthority.tsv"
        )
        with tempfile.TemporaryDirectory(
            prefix="aisuite-codex-authority-expression-mutation-"
        ) as directory:
            mutated = Path(directory) / source.name
            text = source.read_text(encoding="utf-8")
            original = "turn {}: thread={} turn={}"
            replacement = "turn completed: thread={} turn={}"
            self.assertEqual(1, text.count(original))
            mutated.write_text(
                text.replace(original, replacement, 1),
                encoding="utf-8",
            )
            self.assert_guard_failure(
                [
                    str(self.semantic_logger_policy),
                    "--repo-root",
                    str(self.repo_root),
                ],
                "CodexPolicySemanticLoggerAuthorityMismatch",
                environment={
                    "AISUITE_CODEX_SEMANTIC_LOGGER_AUTHORITY_FILE": str(mutated)
                },
            )


def parser() -> argparse.ArgumentParser:
    argument_parser = argparse.ArgumentParser()
    argument_parser.add_argument("--repo-root", required=True, type=Path)
    argument_parser.add_argument(
        "--public-header-policy", required=True, type=Path
    )
    argument_parser.add_argument("--logging-policy", required=True, type=Path)
    argument_parser.add_argument(
        "--semantic-logger-policy", required=True, type=Path
    )
    return argument_parser


if __name__ == "__main__":
    OPTIONS, remaining = parser().parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
