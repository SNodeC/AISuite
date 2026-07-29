#!/usr/bin/env python3
"""Plant isolated failures in the AISuite Codex policy ownership guards."""

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any, Callable, Mapping

sys.dont_write_bytecode = True


DIAGNOSTIC_PATTERN = re.compile(r"\b(CodexPolicy[A-Za-z0-9]+)\b")
SEMANTIC_ENVIRONMENT_KEYS = (
    "AISUITE_CODEX_SEMANTIC_LOGGER_AUTHORITY_FILE",
    "AISUITE_CODEX_SEMANTIC_LOGGER_CLASSIFICATIONS_FILE",
)

MUTATION_TEST_METHODS = {
    "public-header-inventory-removal": "test_public_header_inventory_removal_is_rejected",
    "public-header-pragma-once": "test_pragma_once_is_rejected",
    "public-header-guard-pair": "test_broken_guard_pair_is_rejected",
    "logging-lifecycle-member": "test_forbidden_logging_member_is_rejected",
    "semantic-logger-unclassified": "test_unclassified_semantic_logger_is_rejected",
    "semantic-logger-classification-removal": "test_missing_semantic_classification_is_rejected",
    "semantic-logger-authority-count": "test_semantic_authority_count_change_is_rejected",
    "semantic-logger-authority-expression": "test_semantic_authority_expression_change_is_rejected",
    "ownership-owner-missing": "test_nonexistent_owner_is_rejected",
    "ctest-functional-registration-removal": "test_functional_registration_removal_is_rejected",
    "ctest-functional-disabled": "test_functional_disabled_is_rejected",
    "ctest-functional-label-exclusion": "test_functional_label_exclusion_is_rejected",
    "ci-job-filter": "test_ci_job_filter_change_is_rejected",
    "security-registration-duplicate": "test_duplicate_security_registration_is_rejected",
    "security-registration-removal": "test_missing_security_registration_is_rejected",
    "security-hierarchy-owner": "test_stale_root_security_hierarchy_is_rejected",
    "security-registration-property": "test_security_registration_property_drift_is_rejected",
    "security-cmake-working-directory-decoy": (
        "test_security_cmake_working_directory_decoy_is_rejected"
    ),
    "security-evidence-expected-working-directory-missing": (
        "test_security_expected_working_directory_missing_is_rejected"
    ),
    "security-evidence-expected-working-directory-drift": (
        "test_security_expected_working_directory_drift_is_rejected"
    ),
    "security-evidence-normalized-working-directory-missing": (
        "test_security_normalized_working_directory_missing_is_rejected"
    ),
    "security-evidence-normalized-working-directory-drift": (
        "test_security_normalized_working_directory_drift_is_rejected"
    ),
    "security-evidence-working-directory-rationale-missing": (
        "test_security_working_directory_rationale_missing_is_rejected"
    ),
    "security-evidence-working-directory-rationale-drift": (
        "test_security_working_directory_rationale_drift_is_rejected"
    ),
    "security-label-exclusion": "test_security_label_exclusion_is_rejected",
    "component-subdirectory-removal": "test_component_subdirectory_removal_is_rejected",
    "component-ctest-removal": "test_component_ctest_removal_is_rejected",
    "component-ctest-drift": "test_component_ctest_drift_is_rejected",
    "preexisting-ctest-removal": "test_preexisting_noncomponent_removal_is_rejected",
    "generated-artifacts-test-removal": "test_generated_artifacts_test_removal_is_rejected",
    "standalone-policy-file-reclassification": "test_standalone_policy_reclassification_is_rejected",
    "security-guard-reclassification": "test_security_guard_reclassification_is_rejected",
    "snodec-blob-alteration": "test_snodec_blob_alteration_is_rejected",
    "source-package-owner-removal": "test_source_package_owner_removal_is_rejected",
    "binary-package-policy-leak": "test_binary_package_policy_leak_is_rejected",
}


def load_ownership_tool(path: Path) -> ModuleType:
    specification = importlib.util.spec_from_file_location(
        "verify_codex_policy_ownership_under_test",
        path,
    )
    if specification is None or specification.loader is None:
        raise AssertionError(f"unable to import ownership checker: {path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def canonical(value: Any) -> str:
    def normalize(item: Any) -> Any:
        if isinstance(item, Path):
            return item.as_posix()
        if isinstance(item, Mapping):
            return {
                str(key): normalize(child)
                for key, child in sorted(item.items(), key=lambda row: str(row[0]))
            }
        if isinstance(item, (set, frozenset)):
            return sorted(normalize(child) for child in item)
        if isinstance(item, (list, tuple)):
            return [normalize(child) for child in item]
        if hasattr(item, "__dataclass_fields__"):
            return normalize(vars(item))
        return item

    return json.dumps(normalize(value), sort_keys=True, separators=(",", ":"))


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
        cls.ownership_tool_path = OPTIONS.ownership_tool.resolve()
        cls.ownership_tool = load_ownership_tool(cls.ownership_tool_path)
        cls.baseline_model = json.loads(
            OPTIONS.baseline_model.read_text(encoding="utf-8")
        )
        cls.final_model = json.loads(
            OPTIONS.final_model.read_text(encoding="utf-8")
        )
        cls.ownership = json.loads(
            OPTIONS.ownership.read_text(encoding="utf-8")
        )
        cls.manifest = json.loads(
            OPTIONS.manifest.read_text(encoding="utf-8")
        )
        cls.workflow_text = OPTIONS.workflow.read_text(encoding="utf-8")
        cls.root_cmake_text = (
            cls.repo_root / "tests/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        cls.security_cmake_text = (
            cls.repo_root / "tests/policy/security/CMakeLists.txt"
        ).read_text(encoding="utf-8")
        cls.source_package_text = OPTIONS.source_package_test.read_text(
            encoding="utf-8"
        )
        cls.binary_package_text = OPTIONS.binary_package_test.read_text(
            encoding="utf-8"
        )
        cls.fixture = cls.ownership_tool.VerificationFixture(
            root=cls.repo_root,
            ownership=copy.deepcopy(cls.ownership),
            baseline_model=copy.deepcopy(cls.baseline_model),
            final_model=copy.deepcopy(cls.final_model),
            manifest=copy.deepcopy(cls.manifest),
            workflow_text=cls.workflow_text,
            root_tests_cmake=cls.root_cmake_text,
            policy_cmake=(
                cls.repo_root / "tests/policy/CMakeLists.txt"
            ).read_text(encoding="utf-8"),
            security_cmake=cls.security_cmake_text,
            filter_map=json.loads(
                (
                    cls.repo_root / "docs/extraction/filter-map.json"
                ).read_text(encoding="utf-8")
            ),
            source_package_paths=set(
                cls.ownership_tool.SOURCE_PACKAGE_REQUIRED_PATHS
            ),
            binary_package_paths=set(),
        )

        cls._assert_clean_executable(
            cls.public_header_policy, cls.repo_root
        )
        cls._assert_clean_executable(cls.logging_policy, cls.repo_root)
        cls._assert_clean_executable(
            cls.semantic_logger_policy, cls.repo_root
        )

    @staticmethod
    def _assert_clean_executable(
        executable: Path,
        repo_root: Path,
        *,
        environment: Mapping[str, str] | None = None,
    ) -> None:
        process_environment = policy_environment(environment)
        completed = subprocess.run(
            [str(executable), "--repo-root", str(repo_root)],
            check=False,
            capture_output=True,
            text=True,
            env=process_environment,
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
        clean_command: list[str] | None = None,
        clean_environment: Mapping[str, str] | None = None,
    ) -> None:
        process_environment = policy_environment(environment)
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            env=process_environment,
        )
        try:
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
        finally:
            if clean_command is not None:
                clean_process_environment = policy_environment(
                    clean_environment
                )
                clean = subprocess.run(
                    clean_command,
                    check=False,
                    capture_output=True,
                    text=True,
                    env=clean_process_environment,
                )
                self.assertEqual(
                    0,
                    clean.returncode,
                    "unmodified authority did not pass after isolated "
                    f"mutation: {clean.stderr or clean.stdout}",
                )

    def temporary_codex_tree(self) -> tempfile.TemporaryDirectory[str]:
        temporary = tempfile.TemporaryDirectory(
            prefix="aisuite-codex-policy-mutation-"
        )
        destination = Path(temporary.name) / "src/ai/openai/codex"
        destination.parent.mkdir(parents=True)
        shutil.copytree(
            self.repo_root / "src/ai/openai/codex",
            destination,
        )
        policy_data = (
            Path(temporary.name) / "tests/policy/codex"
        )
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

    def assert_file_mutated(
        self,
        path: Path,
        before_digest: str,
    ) -> None:
        self.assertNotEqual(
            before_digest,
            digest(path),
            f"planted mutation changed no bytes in {path}",
        )

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
            clean_command=[
                str(executable),
                "--repo-root",
                str(self.repo_root),
            ],
        )

    def test_public_header_inventory_removal_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            cmake = root / "src/ai/openai/codex/CMakeLists.txt"
            before = digest(cmake)
            source = cmake.read_text(encoding="utf-8")
            marker = "    typed/Accounts.h\n"
            self.assertEqual(
                2,
                source.count(marker),
                "inventory/install authority fixture changed",
            )
            cmake.write_text(source.replace(marker, "", 1), encoding="utf-8")
            self.assertEqual(
                1,
                cmake.read_text(encoding="utf-8").count(marker),
                "mutation did not remove exactly one inventory entry",
            )
            self.assert_file_mutated(cmake, before)
            self.run_tree_policy(
                self.public_header_policy,
                root,
                "CodexPolicyPublicHeaderInventoryMismatch",
            )

    def test_pragma_once_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            header = (
                root
                / "src/ai/openai/codex/typed/Accounts.h"
            )
            before = digest(header)
            source = header.read_text(encoding="utf-8")
            self.assertNotIn("#pragma once", source)
            header.write_text(
                source.replace(
                    "#ifndef AI_OPENAI_CODEX_TYPED_ACCOUNTS_H",
                    "#pragma once\n\n"
                    "#ifndef AI_OPENAI_CODEX_TYPED_ACCOUNTS_H",
                    1,
                ),
                encoding="utf-8",
            )
            self.assertEqual(
                1,
                header.read_text(encoding="utf-8").count("#pragma once"),
            )
            self.assert_file_mutated(header, before)
            self.run_tree_policy(
                self.public_header_policy,
                root,
                "CodexPolicyHeaderGuardMismatch",
            )

    def test_broken_guard_pair_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            header = (
                root
                / "src/ai/openai/codex/typed/Accounts.h"
            )
            before = digest(header)
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
            self.assert_file_mutated(header, before)
            self.run_tree_policy(
                self.public_header_policy,
                root,
                "CodexPolicyHeaderGuardMismatch",
            )

    def test_forbidden_logging_member_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            header = (
                root
                / "src/ai/openai/codex/backend/BackendState.h"
            )
            before = digest(header)
            source = header.read_text(encoding="utf-8")
            self.assertNotIn("lifecycleTerminalLogged", source)
            header.write_text(
                source
                + "\nstruct CodexPolicyMutationState {\n"
                + "    bool lifecycleTerminalLogged = false;\n"
                + "};\n",
                encoding="utf-8",
            )
            self.assert_file_mutated(header, before)
            self.run_tree_policy(
                self.logging_policy,
                root,
                "CodexPolicyLoggingApiSurfaceMismatch",
            )

    def test_unclassified_semantic_logger_is_rejected(self) -> None:
        with self.temporary_codex_tree() as directory:
            root = Path(directory)
            source_path = (
                root
                / "src/ai/openai/codex/backend/Snapshot.cpp"
            )
            before = digest(source_path)
            source = source_path.read_text(encoding="utf-8")
            marker = "semantic::policyMutationLog()"
            self.assertNotIn(marker, source)
            source_path.write_text(
                source
                + "\nvoid codexPolicyMutationOnly() {\n"
                + f"    {marker};\n"
                + "}\n",
                encoding="utf-8",
            )
            self.assert_file_mutated(source_path, before)
            self.run_tree_policy(
                self.semantic_logger_policy,
                root,
                "CodexPolicySemanticLoggerUnclassified",
                environment={
                    "AISUITE_CODEX_SEMANTIC_LOGGER_AUTHORITY_FILE": str(
                        root
                        / "tests/policy/codex/"
                        "CodexSemanticLoggerAuthority.tsv"
                    ),
                    "AISUITE_CODEX_SEMANTIC_LOGGER_CLASSIFICATIONS_FILE": str(
                        root
                        / "tests/policy/codex/"
                        "CodexSemanticLoggerClassifications.tsv"
                    ),
                },
            )

    def semantic_table_rows(self, path: Path) -> tuple[list[str], list[str]]:
        lines = path.read_text(encoding="utf-8").splitlines()
        metadata = [
            line
            for line in lines
            if not line or line.startswith("#") or line.startswith("path\t")
        ]
        rows = [
            line
            for line in lines
            if line and not line.startswith("#") and not line.startswith("path\t")
        ]
        return metadata, rows

    def write_semantic_table(
        self,
        source: Path,
        destination: Path,
        rows: list[str],
    ) -> None:
        lines = source.read_text(encoding="utf-8").splitlines()
        prefix = [
            line
            for line in lines
            if not line or line.startswith("#") or line.startswith("path\t")
        ]
        destination.write_text(
            "\n".join([*prefix, *rows]) + "\n",
            encoding="utf-8",
        )

    def test_missing_semantic_classification_is_rejected(self) -> None:
        source = (
            self.repo_root
            / "tests/policy/codex/CodexSemanticLoggerClassifications.tsv"
        )
        _metadata, rows = self.semantic_table_rows(source)
        self.assertEqual(4, len(rows))
        with tempfile.TemporaryDirectory(
            prefix="aisuite-codex-classification-mutation-"
        ) as directory:
            mutated = Path(directory) / source.name
            self.write_semantic_table(source, mutated, rows[:-1])
            self.assertEqual(
                3,
                len(self.semantic_table_rows(mutated)[1]),
                "mutation did not remove exactly one classification",
            )
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
                clean_command=[
                    str(self.semantic_logger_policy),
                    "--repo-root",
                    str(self.repo_root),
                ],
            )

    def test_semantic_authority_count_change_is_rejected(self) -> None:
        source = (
            self.repo_root
            / "tests/policy/codex/CodexSemanticLoggerAuthority.tsv"
        )
        _metadata, rows = self.semantic_table_rows(source)
        self.assertEqual(4, len(rows))
        with tempfile.TemporaryDirectory(
            prefix="aisuite-codex-authority-count-mutation-"
        ) as directory:
            mutated = Path(directory) / source.name
            self.write_semantic_table(source, mutated, rows[:-1])
            self.assertEqual(3, len(self.semantic_table_rows(mutated)[1]))
            self.assert_guard_failure(
                [
                    str(self.semantic_logger_policy),
                    "--repo-root",
                    str(self.repo_root),
                ],
                "CodexPolicySemanticLoggerAuthorityMismatch",
                environment={
                    "AISUITE_CODEX_SEMANTIC_LOGGER_AUTHORITY_FILE": str(
                        mutated
                    )
                },
                clean_command=[
                    str(self.semantic_logger_policy),
                    "--repo-root",
                    str(self.repo_root),
                ],
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
            self.assertNotEqual(digest(source), digest(mutated))
            self.assert_guard_failure(
                [
                    str(self.semantic_logger_policy),
                    "--repo-root",
                    str(self.repo_root),
                ],
                "CodexPolicySemanticLoggerAuthorityMismatch",
                environment={
                    "AISUITE_CODEX_SEMANTIC_LOGGER_AUTHORITY_FILE": str(
                        mutated
                    )
                },
                clean_command=[
                    str(self.semantic_logger_policy),
                    "--repo-root",
                    str(self.repo_root),
                ],
            )

    def assert_fixture_mutation(
        self,
        *,
        mutate: Callable[[Any], None],
        expected_code: str,
    ) -> None:
        valid = copy.deepcopy(self.fixture)
        changed = copy.deepcopy(self.fixture)
        before = canonical(changed)
        mutate(changed)
        self.assertNotEqual(
            before,
            canonical(changed),
            "planted verification mutation changed no input",
        )
        self.assertEqual(
            [expected_code],
            self.ownership_tool.diagnostic_codes(changed),
            "mutation failed through an earlier or unrelated ownership guard",
        )
        self.assertEqual(
            [],
            self.ownership_tool.diagnostic_codes(valid),
            "unmodified verification fixture did not pass after mutation",
        )

    @staticmethod
    def fixture_test(
        fixture: Any,
        name: str,
        *,
        model_name: str = "final_model",
    ) -> dict[str, Any]:
        model = getattr(fixture, model_name)
        rows = [test for test in model["tests"] if test["name"] == name]
        if len(rows) != 1:
            raise AssertionError(
                f"expected one {model_name} row for {name}, found {len(rows)}"
            )
        return rows[0]

    @staticmethod
    def remove_fixture_test(
        fixture: Any,
        name: str,
        *,
        model_name: str = "final_model",
    ) -> None:
        model = getattr(fixture, model_name)
        before = len(model["tests"])
        model["tests"] = [
            test for test in model["tests"] if test["name"] != name
        ]
        if len(model["tests"]) != before - 1:
            raise AssertionError(f"unable to remove exactly one {name} row")
        model["test_count"] -= 1

    @staticmethod
    def security_ownership_row(fixture: Any) -> dict[str, Any]:
        rows = fixture.ownership["preexisting_aisuite_policy_tests"]
        if (
            len(rows) != 1
            or rows[0].get("test_name")
            != "CodexSyntheticSecretLeakGuardTest"
        ):
            raise AssertionError(
                "pre-existing security-policy ownership fixture changed"
            )
        return rows[0]

    @staticmethod
    def security_registration_block(text: str) -> str:
        match = re.search(
            r"(?ms)^add_test\(\n"
            r"\s+NAME CodexSyntheticSecretLeakGuardTest\n"
            r".*?^\)\n",
            text,
        )
        if match is None:
            raise AssertionError("security add_test registration fixture changed")
        return match.group(0)

    @staticmethod
    def move_manifest_row(
        fixture: Any,
        path: str,
        source_bucket: str,
        destination_bucket: str,
    ) -> None:
        source = fixture.manifest[source_bucket]
        rows = [row for row in source if row.get("path") == path]
        if len(rows) != 1:
            raise AssertionError(
                f"expected one {source_bucket} row for {path}"
            )
        fixture.manifest[source_bucket] = [
            row for row in source if row.get("path") != path
        ]
        fixture.manifest[destination_bucket].append(rows[0])

    def test_mutation_method_mapping_matches_checker_coverage(self) -> None:
        expected_ids = {
            mutation_id
            for mutation_id, _diagnostic in self.ownership_tool.MUTATION_COVERAGE
        }
        self.assertEqual(expected_ids, set(MUTATION_TEST_METHODS))
        method_names = list(MUTATION_TEST_METHODS.values())
        self.assertEqual(len(method_names), len(set(method_names)))
        for method_name in method_names:
            self.assertTrue(method_name.startswith("test_"))
            self.assertTrue(
                callable(getattr(type(self), method_name, None)),
                f"mapped mutation method is missing: {method_name}",
            )

    def test_nonexistent_owner_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            fixture.ownership["transferred_responsibilities"][0][
                "aisuite_owner"
            ]["implementation_files"][0] = (
                "tests/policy/codex/NonexistentPolicyOwner.cpp"
            )

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyOwnershipMappingMismatch",
        )

    def test_functional_registration_removal_is_rejected(self) -> None:
        self.assert_fixture_mutation(
            mutate=lambda fixture: self.remove_fixture_test(
                fixture,
                "CodexPublicHeaderPolicyTest",
            ),
            expected_code="CodexPolicyTestNotRegistered",
        )

    def test_functional_disabled_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            self.fixture_test(
                fixture,
                "CodexPublicHeaderPolicyTest",
            )["disabled"] = True

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyTestDisabled",
        )

    def test_functional_label_exclusion_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            test = self.fixture_test(
                fixture,
                "CodexPublicHeaderPolicyTest",
            )
            test["labels"] = ["policy", "headers"]
            test["properties"]["LABELS"] = ["policy", "headers"]

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyTestExcludedFromFocusedCI",
        )

    def test_ci_job_filter_change_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            expected = self.ownership_tool.EXPECTED_CI_FILTER
            if fixture.workflow_text.count(expected) != 2:
                raise AssertionError("focused CI filter fixture changed")
            fixture.workflow_text = fixture.workflow_text.replace(
                expected,
                "security-only",
                1,
            )

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyCIFilterMismatch",
        )

    def test_duplicate_security_registration_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            fixture.root_tests_cmake += (
                "\n" + self.security_registration_block(fixture.security_cmake)
            )

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyDuplicateTestRegistration",
        )

    def test_missing_security_registration_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            block = self.security_registration_block(fixture.security_cmake)
            fixture.security_cmake = fixture.security_cmake.replace(
                block,
                "",
                1,
            )

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyExistingSecurityGuardNotRegistered",
        )

    def test_stale_root_security_hierarchy_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            block = self.security_registration_block(fixture.security_cmake)
            fixture.security_cmake = None
            fixture.root_tests_cmake += "\n" + block

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyHierarchyRegistrationMismatch",
        )

    def test_security_registration_property_drift_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            test = self.fixture_test(
                fixture,
                "CodexSyntheticSecretLeakGuardTest",
            )
            test["properties"]["WORKING_DIRECTORY"] = (
                "${BUILD_DIR}/tests/policy/security"
            )

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyExistingSecurityGuardDrift",
        )

    def test_security_cmake_working_directory_decoy_is_rejected(
        self,
    ) -> None:
        def mutate(fixture: Any) -> None:
            expected = (
                "        WORKING_DIRECTORY\n"
                '        "${CMAKE_BINARY_DIR}/tests"'
            )
            decoy = (
                "        DESCRIPTION\n"
                '        [[WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"]]'
            )
            if fixture.security_cmake.count(expected) != 1:
                raise AssertionError(
                    "security working-directory CMake fixture changed"
                )
            fixture.security_cmake = fixture.security_cmake.replace(
                expected,
                decoy,
                1,
            )

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyExistingSecurityGuardDrift",
        )

    def test_security_expected_working_directory_missing_is_rejected(
        self,
    ) -> None:
        def mutate(fixture: Any) -> None:
            del self.security_ownership_row(fixture)[
                "expected_working_directory"
            ]

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyOwnershipMappingMismatch",
        )

    def test_security_expected_working_directory_drift_is_rejected(
        self,
    ) -> None:
        def mutate(fixture: Any) -> None:
            self.security_ownership_row(fixture)[
                "expected_working_directory"
            ] = "${CMAKE_BINARY_DIR}/tests/policy/security"

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyOwnershipMappingMismatch",
        )

    def test_security_normalized_working_directory_missing_is_rejected(
        self,
    ) -> None:
        def mutate(fixture: Any) -> None:
            del self.security_ownership_row(fixture)[
                "normalized_ctest_working_directory"
            ]

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyOwnershipMappingMismatch",
        )

    def test_security_normalized_working_directory_drift_is_rejected(
        self,
    ) -> None:
        def mutate(fixture: Any) -> None:
            self.security_ownership_row(fixture)[
                "normalized_ctest_working_directory"
            ] = "${BUILD_DIR}/tests/policy/security"

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyOwnershipMappingMismatch",
        )

    def test_security_working_directory_rationale_missing_is_rejected(
        self,
    ) -> None:
        def mutate(fixture: Any) -> None:
            del self.security_ownership_row(fixture)[
                "working_directory_rationale"
            ]

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyOwnershipMappingMismatch",
        )

    def test_security_working_directory_rationale_drift_is_rejected(
        self,
    ) -> None:
        def mutate(fixture: Any) -> None:
            self.security_ownership_row(fixture)[
                "working_directory_rationale"
            ] = "The working directory is unrelated to registration ownership."

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyOwnershipMappingMismatch",
        )

    def test_security_label_exclusion_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            test = self.fixture_test(
                fixture,
                "CodexSyntheticSecretLeakGuardTest",
            )
            test["labels"] = ["policy", "security", "package"]
            test["properties"]["LABELS"] = [
                "package",
                "policy",
                "security",
            ]

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyTestExcludedFromFocusedCI",
        )

    def test_component_subdirectory_removal_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            marker = "add_subdirectory(component/codex)"
            if fixture.root_tests_cmake.count(marker) != 1:
                raise AssertionError("component/codex registration fixture changed")
            fixture.root_tests_cmake = fixture.root_tests_cmake.replace(
                marker,
                "",
                1,
            )

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyPreexistingComponentTestMissing",
        )

    def test_component_ctest_removal_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            component = next(
                test
                for test in fixture.final_model["tests"]
                if any(
                    str(path).startswith(
                        "${SOURCE_DIR}/tests/component/codex/"
                    )
                    for path in test["registration_files"]
                )
                and test["name"]
                != "CodexAppServerGeneratedArtifactsGuardTest"
            )
            self.remove_fixture_test(fixture, component["name"])

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyPreexistingComponentTestMissing",
        )

    def test_component_ctest_drift_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            component = next(
                test
                for test in fixture.final_model["tests"]
                if any(
                    str(path).startswith(
                        "${SOURCE_DIR}/tests/component/codex/"
                    )
                    for path in test["registration_files"]
                )
                and test["name"]
                != "CodexAppServerGeneratedArtifactsGuardTest"
            )
            component["command"].append("--codex-policy-mutation")

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyPreexistingComponentTestDrift",
        )

    def test_preexisting_noncomponent_removal_is_rejected(self) -> None:
        self.assert_fixture_mutation(
            mutate=lambda fixture: self.remove_fixture_test(
                fixture,
                "AISuiteExtractionGuardTest",
            ),
            expected_code="CodexPolicyPreexistingCTestRemoval",
        )

    def test_generated_artifacts_test_removal_is_rejected(self) -> None:
        self.assert_fixture_mutation(
            mutate=lambda fixture: self.remove_fixture_test(
                fixture,
                "CodexAppServerGeneratedArtifactsGuardTest",
            ),
            expected_code="CodexPolicyPreexistingComponentTestMissing",
        )

    def test_standalone_policy_reclassification_is_rejected(self) -> None:
        self.assert_fixture_mutation(
            mutate=lambda fixture: self.move_manifest_row(
                fixture,
                "tests/policy/codex/CodexLoggingApiSurfacePolicyTest.cpp",
                "standalone_files",
                "imported_files",
            ),
            expected_code="CodexPolicyManifestClassificationMismatch",
        )

    def test_security_guard_reclassification_is_rejected(self) -> None:
        self.assert_fixture_mutation(
            mutate=lambda fixture: self.move_manifest_row(
                fixture,
                "tests/policy/security/CodexSyntheticSecretLeakGuardTest.py",
                "imported_files",
                "standalone_files",
            ),
            expected_code="CodexPolicyManifestClassificationMismatch",
        )

    def test_snodec_blob_alteration_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            fixture.ownership["snodec_source_authority"]["policy_files"][0][
                "blob"
            ] = "0000000000000000000000000000000000000000"

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicySourceAuthorityMismatch",
        )

    def test_source_package_owner_removal_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            fixture.source_package_paths.remove(
                "tests/policy/codex/CodexLoggingApiSurfacePolicyTest.cpp"
            )

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicySourcePackageMismatch",
        )

    def test_binary_package_policy_leak_is_rejected(self) -> None:
        def mutate(fixture: Any) -> None:
            fixture.binary_package_paths.add(
                "tests/policy/support/CxxSourceScanner.h"
            )

        self.assert_fixture_mutation(
            mutate=mutate,
            expected_code="CodexPolicyBinaryPackageLeak",
        )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--repo-root", type=Path, required=True)
    result.add_argument("--build-dir", type=Path, required=True)
    result.add_argument(
        "--public-header-policy", type=Path, required=True
    )
    result.add_argument("--logging-policy", type=Path, required=True)
    result.add_argument(
        "--semantic-logger-policy", type=Path, required=True
    )
    result.add_argument("--ownership-tool", type=Path, required=True)
    result.add_argument("--baseline-model", type=Path, required=True)
    result.add_argument("--final-model", type=Path, required=True)
    result.add_argument("--ownership", type=Path, required=True)
    result.add_argument("--manifest", type=Path, required=True)
    result.add_argument("--workflow", type=Path, required=True)
    result.add_argument("--source-package-test", type=Path, required=True)
    result.add_argument("--binary-package-test", type=Path, required=True)
    return result


if __name__ == "__main__":
    OPTIONS, remaining = parser().parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])
