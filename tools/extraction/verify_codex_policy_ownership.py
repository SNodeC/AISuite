#!/usr/bin/env python3
"""Generate and verify the extracted AISuite Codex policy ownership model.

The normal ``check`` mode joins repository sources to a configured CTest model.
The package-safe mode deliberately uses only packaged, recorded authorities.
"""

from __future__ import annotations

import argparse
import ast
import copy
import hashlib
import json
import math
import os
import re
import shlex
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


AISUITE_BASE_SHA = "19de4f50be64e187761274f043091090609d27a3"
AISUITE_BASE_TREE = "c71a91e545d70d649446ca9d698d729c307a480a"
SNODEC_REPOSITORY = "https://github.com/SNodeC/snode.c"
SNODEC_DEPENDENCY_COMMIT = "77415c71a87fb7955e9a050bedaca02b65754324"
SNODEC_DEPENDENCY_TREE = "2d39c334f12c308828936656c820447bfcc38d47"
SNODEC_COMMIT = "d18b231a1d2ec2235fd6f204786b0a761cc24ff5"
SNODEC_TREE = "88a63edc985a851b2b76b0c56df19fae74ea8069"
EXPECTED_CI_FILTER = "ai|openai|codex|extraction"
EXPECTED_PATH_PLACEHOLDERS = {
    "${SOURCE_DIR}": "AISuite source root",
    "${BUILD_DIR}": "configured AISuite test build root",
    "${PYTHON3}": "configured Python 3 interpreter",
    "${CMAKE}": "configured CMake executable",
    "${CMAKE_GENERATOR}": "configured CMake generator",
    "${CXX_COMPILER}": "configured C++ compiler",
    "${CPACK}": "configured CPack executable",
    "${SNODEC_SOURCE_DIR}": "pinned SNode.C test-time source authority",
    "${SNODEC_PACKAGE_DIR}": "installed SNode.C package configuration",
    "${NLOHMANN_JSON_PACKAGE_DIR}": "installed nlohmann-json package configuration",
}

OWNERSHIP_PATH = Path("docs/extraction/codex-policy-ownership.json")
BASELINE_CTEST_PATH = Path("docs/extraction/codex-policy-baseline-ctest.json")
FINAL_CTEST_PATH = Path("docs/extraction/codex-policy-final-ctest.json")
MANIFEST_PATH = Path("docs/extraction/source-manifest.json")
FILTER_MAP_PATH = Path("docs/extraction/filter-map.json")
WORKFLOW_PATH = Path(".github/workflows/ci.yml")

SOURCE_AUTHORITIES: tuple[dict[str, str], ...] = (
    {
        "responsibility_id": "codex-public-header-policy",
        "path": "tests/policy/codex/CodexA12PublicHeaderPolicyTest.cpp",
        "blob": "73537e7d68b74afb335db8c4bd8b42d533c86814",
        "sha256": "a909d93dbd1b39c86c60e5f5ff8e0e496795861a81f3569712390ed75c4fee0b",
    },
    {
        "responsibility_id": "codex-logging-api-surface-policy",
        "path": "tests/policy/log/LoggingApiSurfacePolicyTest.cpp",
        "blob": "e9beada86e032261d5b128cf58d0efdf1f927234",
        "sha256": "85182c2a8b15e7a79b250e932ae509e6b540981b599d3f14a967dfca5a2883ac",
    },
    {
        "responsibility_id": "codex-semantic-logger-policy",
        "path": "tests/policy/log/ParameterlessSemanticLoggerPolicyTest.cpp",
        "blob": "e4fddcaf69b23549eab318cb86afee6210b2aaad",
        "sha256": "3940c05cac92cf45531d019fcc23d0821c3526f8a75cf1afd19faf6da489c49f",
    },
)

SUPPORT_AUTHORITY = {
    "path": "tests/policy/SourcePolicyTestRoot.h",
    "blob": "47ec895a1bb6d6344d3c9d6bbdbc345c72a09380",
    "sha256": "3553f889e3afdb6bd67f089b9cdea6343fe81b38e130c02a62c5e05f85c4f952",
}

FUNCTIONAL_TESTS: tuple[str, ...] = (
    "CodexPublicHeaderPolicyTest",
    "CodexPublicHeaderSelfContainmentTest",
    "CodexLoggingApiSurfacePolicyTest",
    "CodexSemanticLoggerPolicyTest",
    "CodexSyntheticSecretLeakGuardTest",
)

NEW_FUNCTIONAL_TESTS: tuple[str, ...] = FUNCTIONAL_TESTS[:4]
SECURITY_TEST = FUNCTIONAL_TESTS[4]
GENERATED_ARTIFACTS_TEST = "CodexAppServerGeneratedArtifactsGuardTest"
VERIFICATION_TESTS: tuple[str, ...] = (
    "CodexPolicyOwnershipTest",
    "CodexPolicyMutationTest",
)

FUNCTIONAL_LABELS: dict[str, tuple[str, ...]] = {
    "CodexPublicHeaderPolicyTest": (
        "policy",
        "ai",
        "openai",
        "codex",
        "extraction",
        "headers",
        "public-api",
    ),
    "CodexPublicHeaderSelfContainmentTest": (
        "policy",
        "ai",
        "openai",
        "codex",
        "extraction",
        "headers",
        "install",
        "consumer",
    ),
    "CodexLoggingApiSurfacePolicyTest": (
        "policy",
        "ai",
        "openai",
        "codex",
        "extraction",
        "logging",
        "api",
        "architecture",
    ),
    "CodexSemanticLoggerPolicyTest": (
        "policy",
        "ai",
        "openai",
        "codex",
        "extraction",
        "logging",
        "architecture",
    ),
    SECURITY_TEST: ("policy", "security", "codex", "extraction", "package"),
}

SECURITY_DEPENDENCIES: tuple[str, ...] = (
    "AISuiteInstalledConsumerTest",
    "AISuiteSourcePackageTest",
    "AISuiteBinaryPackageTest",
    GENERATED_ARTIFACTS_TEST,
    "CodexA14AuditToolTest",
)

SECURITY_EXPECTED_WORKING_DIRECTORY = "${CMAKE_BINARY_DIR}/tests"
SECURITY_NORMALIZED_CTEST_WORKING_DIRECTORY = "${BUILD_DIR}/tests"
SECURITY_WORKING_DIRECTORY_RATIONALE = (
    "Explicit property added to preserve the implicit cwd the test had when "
    "registered from tests/CMakeLists.txt; the directory move to "
    "tests/policy/security would otherwise silently change it."
)

FUNCTIONAL_COMMANDS: dict[str, list[str]] = {
    "CodexPublicHeaderPolicyTest": [
        "${BUILD_DIR}/tests/policy/codex/CodexPublicHeaderPolicyTest",
        "--repo-root",
        "${SOURCE_DIR}",
    ],
    "CodexPublicHeaderSelfContainmentTest": [
        "${CMAKE}",
        "-DAISUITE_PUBLIC_HEADER_POLICY_EXECUTABLE=${BUILD_DIR}/tests/policy/codex/CodexPublicHeaderPolicyTest",
        "-DAISUITE_SOURCE_DIR=${SOURCE_DIR}",
        "-DAISUITE_BUILD_DIR=${BUILD_DIR}",
        "-DAISUITE_CMAKE_COMMAND=${CMAKE}",
        "-DAISUITE_CMAKE_GENERATOR=${CMAKE_GENERATOR}",
        "-DAISUITE_CXX_COMPILER=${CXX_COMPILER}",
        "-DAISUITE_SNODEC_DIR=${SNODEC_PACKAGE_DIR}",
        "-DAISUITE_NLOHMANN_JSON_DIR=${NLOHMANN_JSON_PACKAGE_DIR}",
        "-P",
        "${SOURCE_DIR}/tests/policy/codex/CodexPublicHeaderSelfContainmentTest.cmake",
    ],
    "CodexLoggingApiSurfacePolicyTest": [
        "${BUILD_DIR}/tests/policy/codex/CodexLoggingApiSurfacePolicyTest",
        "--repo-root",
        "${SOURCE_DIR}",
    ],
    "CodexSemanticLoggerPolicyTest": [
        "${BUILD_DIR}/tests/policy/codex/CodexSemanticLoggerPolicyTest",
        "--repo-root",
        "${SOURCE_DIR}",
    ],
    SECURITY_TEST: [
        "${PYTHON3}",
        "${SOURCE_DIR}/tests/policy/security/CodexSyntheticSecretLeakGuardTest.py",
        "--repo-root",
        "${SOURCE_DIR}",
        "--build-root",
        "${BUILD_DIR}",
    ],
}

VERIFICATION_LABELS: dict[str, tuple[str, ...]] = {
    "CodexPolicyOwnershipTest": (
        "policy",
        "verification",
        "ownership",
        "cutover-readiness",
        "ai",
        "openai",
        "codex",
        "extraction",
    ),
    "CodexPolicyMutationTest": (
        "policy",
        "verification",
        "mutation",
        "ai",
        "openai",
        "codex",
        "extraction",
    ),
}

VERIFICATION_DEPENDENCIES: dict[str, list[str]] = {
    "CodexPolicyOwnershipTest": list(FUNCTIONAL_TESTS),
    "CodexPolicyMutationTest": ["CodexPolicyOwnershipTest"],
}

VERIFICATION_TIMEOUTS = {
    "CodexPolicyOwnershipTest": 120.0,
    "CodexPolicyMutationTest": 300.0,
}

HEADER_COMPONENTS: tuple[dict[str, Any], ...] = (
    {
        "name": "main",
        "cmake_path": "src/ai/openai/codex/CMakeLists.txt",
        "variable": "AI_OPENAI_CODEX_PUBLIC_H",
        "installed_prefix": "ai/openai/codex",
        "expected_count": 27,
    },
    {
        "name": "backend",
        "cmake_path": "src/ai/openai/codex/backend/CMakeLists.txt",
        "variable": "AI_OPENAI_CODEX_BACKEND_PUBLIC_H",
        "installed_prefix": "ai/openai/codex/backend",
        "expected_count": 7,
    },
    {
        "name": "frontend",
        "cmake_path": "src/ai/openai/codex/frontend/CMakeLists.txt",
        "variable": "AI_OPENAI_CODEX_FRONTEND_PUBLIC_H",
        "installed_prefix": "ai/openai/codex/frontend",
        "expected_count": 7,
    },
)

ORIGINAL_HEADER_GUARDS = {
    "typed/Accounts.h": "AI_OPENAI_CODEX_TYPED_ACCOUNTS_H",
    "typed/Configuration.h": "AI_OPENAI_CODEX_TYPED_CONFIGURATION_H",
    "typed/Models.h": "AI_OPENAI_CODEX_TYPED_MODELS_H",
}

MCP_REVERSE_COMPONENT_TEST_STAGES = (
    frozenset(),
    frozenset(
        {
            "CodexA14McpClientTest",
            "CodexA14McpClientWireTest",
            "CodexA14McpNotificationEventTest",
        }
    ),
    frozenset(
        {
            "CodexA14AttestationDynamicToolCodecTest",
            "CodexA14AttestationDynamicToolWireTest",
            "CodexA14McpClientTest",
            "CodexA14McpClientWireTest",
            "CodexA14McpNotificationEventTest",
        }
    ),
    frozenset(
        {
            "CodexA14AttestationDynamicToolCodecTest",
            "CodexA14AttestationDynamicToolWireTest",
            "CodexA14McpClientTest",
            "CodexA14McpClientWireTest",
            "CodexA14McpNotificationEventTest",
            "CodexA14NineRequestStdioTest",
            "CodexA14UserInputElicitationCodecTest",
        }
    ),
)

FORBIDDEN_LOGGING_IDENTIFIERS: tuple[str, ...] = (
    "lifecycleStart",
    "creationLogged",
    "lifecycleStarted",
    "lifecycleTerminalLogged",
)

SEMANTIC_LOGGER_ENTRIES: tuple[dict[str, str], ...] = (
    {
        "path": "src/ai/openai/codex/backend/Reducer.cpp",
        "logger_function": "lifecycleLog",
        "identifying_expression": "turn {}: thread={} turn={}",
        "classification": "DOMAIN_OR_PROTOCOL_SCOPE",
        "rationale": "Typed turn completion is owned by Codex thread and turn identifiers.",
    },
    {
        "path": "src/ai/openai/codex/backend/Reducer.cpp",
        "logger_function": "lifecycleLog",
        "identifying_expression": "turn failed: thread={} turn={}",
        "classification": "DOMAIN_OR_PROTOCOL_SCOPE",
        "rationale": "Typed turn failure is owned by Codex thread and turn identifiers.",
    },
    {
        "path": "src/ai/openai/codex/backend/Reducer.cpp",
        "logger_function": "lifecycleLog",
        "identifying_expression": "thread created: thread={}",
        "classification": "DOMAIN_OR_PROTOCOL_SCOPE",
        "rationale": "Typed thread creation is owned by the Codex thread identifier.",
    },
    {
        "path": "src/ai/openai/codex/backend/Reducer.cpp",
        "logger_function": "lifecycleLog",
        "identifying_expression": "turn started: thread={} turn={}",
        "classification": "DOMAIN_OR_PROTOCOL_SCOPE",
        "rationale": "Typed turn start is owned by Codex thread and turn identifiers.",
    },
)

EXPECTED_SELECTED_PATHS: tuple[str, ...] = (
    "src/ai/openai/codex",
    "src/apps/codex-backend",
    "src/apps/codex-backend-client",
    "docs/ai/openai/codex",
    "tools/codex",
    "tests/component/codex",
    "tests/installed/codex",
    "tests/CodexBinaryPackageTest.cmake",
    "tests/CodexSourcePackageTest.cmake",
    "tests/policy/security/CodexSyntheticSecretLeakGuardTest.py",
)

# This is the complete policy-closure file set whose extraction bucket is
# reviewed by this checker. Existing standalone authorities stay standalone;
# only the pre-existing security Python guard remains in imported_files.
STANDALONE_POLICY_PATHS: tuple[str, ...] = (
    "README.md",
    "docs/extraction/README.md",
    "docs/extraction/codex-policy-baseline-ctest.json",
    "docs/extraction/codex-policy-final-ctest.json",
    "docs/extraction/codex-policy-ownership.json",
    "docs/extraction/codex-policy-ownership.md",
    "docs/extraction/test-integrity.md",
    "docs/extraction/validation.md",
    "tests/AISuiteBinaryPackageTest.cmake",
    "tests/AISuiteSourcePackageTest.cmake",
    "tests/CMakeLists.txt",
    "tests/component/codex/CodexA14UserIntegrationsClosureTest.py",
    "tests/policy/CMakeLists.txt",
    "tests/policy/codex/CMakeLists.txt",
    "tests/policy/codex/CodexLoggingApiSurfacePolicyTest.cpp",
    "tests/policy/codex/CodexPolicyMutationTest.py",
    "tests/policy/codex/CodexPublicHeaderPolicyTest.cpp",
    "tests/policy/codex/CodexPublicHeaderSelfContainmentTest.cmake",
    "tests/policy/codex/CodexSemanticLoggerAuthority.tsv",
    "tests/policy/codex/CodexSemanticLoggerClassifications.tsv",
    "tests/policy/codex/CodexSemanticLoggerPolicyTest.cpp",
    "tests/policy/security/CMakeLists.txt",
    "tests/policy/support/AISuiteSourcePolicyTestRoot.h",
    "tests/policy/support/CxxSourceScanner.h",
    "tools/codex/app_server_a1_4_user_integrations_closure.py",
    "tools/extraction/verify_codex_policy_ownership.py",
)

SECURITY_IMPORTED_PROVENANCE = {
    "path": "tests/policy/security/CodexSyntheticSecretLeakGuardTest.py",
    "source_blob": "b5f5f6d3ef24b9c4f91ccec226ed6a437de31b79",
    "source_sha256": "e0225b78884c080085ecab7bff7b720fbdcc3da1a81751890967d7c2b5610e26",
    "final_sha256": "d4581f2c5f5204c73f1a4b88fc27ef8085d88e62558d1c524aed00590b700d74",
    "disposition": "standalone-adaptation",
}

SOURCE_PACKAGE_REQUIRED_PATHS: tuple[str, ...] = tuple(
    sorted(
        {
            *STANDALONE_POLICY_PATHS,
            SECURITY_IMPORTED_PROVENANCE["path"],
            ".github/workflows/ci.yml",
            "docs/extraction/filter-map.json",
            "docs/extraction/source-manifest.json",
            "src/ai/openai/codex/CMakeLists.txt",
            "src/ai/openai/codex/backend/CMakeLists.txt",
            "src/ai/openai/codex/frontend/CMakeLists.txt",
            "tests/policy/codex/CodexPublicHeaderPolicyTest.cpp",
            "tests/policy/codex/CodexPublicHeaderSelfContainmentTest.cmake",
            "tests/policy/codex/CodexLoggingApiSurfacePolicyTest.cpp",
            "tests/policy/codex/CodexSemanticLoggerPolicyTest.cpp",
        }
    )
)

BINARY_PACKAGE_FORBIDDEN_PATTERNS: tuple[str, ...] = (
    "tests/policy/",
    "tools/extraction/",
    "verify_codex_policy_ownership.py",
    "tests/AISuiteSourcePackageTest.cmake",
    "tests/AISuiteBinaryPackageTest.cmake",
    "AISuiteSourcePolicyTestRoot.h",
    "CxxSourceScanner.h",
    "CodexSemanticLoggerAuthority.tsv",
    "CodexSemanticLoggerClassifications.tsv",
    "CodexPolicyMutationTest.py",
    "codex-policy-ownership.md",
    "CodexPublicHeaderPolicyTest",
    "CodexPublicHeaderSelfContainmentTest",
    "CodexLoggingApiSurfacePolicyTest",
    "CodexSemanticLoggerPolicyTest",
    "CodexPolicyOwnershipTest",
    "CodexPolicyMutationTest",
    "codex-policy-baseline-ctest.json",
    "codex-policy-final-ctest.json",
    "codex-policy-ownership.json",
    "docs/extraction/",
)

MUTATION_COVERAGE: tuple[tuple[str, str], ...] = (
    ("public-header-inventory-removal", "CodexPolicyPublicHeaderInventoryMismatch"),
    ("public-header-pragma-once", "CodexPolicyHeaderGuardMismatch"),
    ("public-header-guard-pair", "CodexPolicyHeaderGuardMismatch"),
    ("logging-lifecycle-member", "CodexPolicyLoggingApiSurfaceMismatch"),
    ("semantic-logger-unclassified", "CodexPolicySemanticLoggerUnclassified"),
    ("semantic-logger-classification-removal", "CodexPolicySemanticLoggerClassificationMismatch"),
    ("semantic-logger-authority-count", "CodexPolicySemanticLoggerAuthorityMismatch"),
    ("semantic-logger-authority-expression", "CodexPolicySemanticLoggerAuthorityMismatch"),
    ("ownership-owner-missing", "CodexPolicyOwnershipMappingMismatch"),
    ("ctest-functional-registration-removal", "CodexPolicyTestNotRegistered"),
    ("ctest-functional-disabled", "CodexPolicyTestDisabled"),
    ("ctest-functional-label-exclusion", "CodexPolicyTestExcludedFromFocusedCI"),
    ("ci-job-filter", "CodexPolicyCIFilterMismatch"),
    ("security-registration-duplicate", "CodexPolicyDuplicateTestRegistration"),
    ("security-registration-removal", "CodexPolicyExistingSecurityGuardNotRegistered"),
    ("security-hierarchy-owner", "CodexPolicyHierarchyRegistrationMismatch"),
    ("security-registration-property", "CodexPolicyExistingSecurityGuardDrift"),
    (
        "security-cmake-working-directory-decoy",
        "CodexPolicyExistingSecurityGuardDrift",
    ),
    (
        "security-evidence-expected-working-directory-missing",
        "CodexPolicyOwnershipMappingMismatch",
    ),
    (
        "security-evidence-expected-working-directory-drift",
        "CodexPolicyOwnershipMappingMismatch",
    ),
    (
        "security-evidence-normalized-working-directory-missing",
        "CodexPolicyOwnershipMappingMismatch",
    ),
    (
        "security-evidence-normalized-working-directory-drift",
        "CodexPolicyOwnershipMappingMismatch",
    ),
    (
        "security-evidence-working-directory-rationale-missing",
        "CodexPolicyOwnershipMappingMismatch",
    ),
    (
        "security-evidence-working-directory-rationale-drift",
        "CodexPolicyOwnershipMappingMismatch",
    ),
    ("security-label-exclusion", "CodexPolicyTestExcludedFromFocusedCI"),
    ("component-subdirectory-removal", "CodexPolicyPreexistingComponentTestMissing"),
    ("component-ctest-removal", "CodexPolicyPreexistingComponentTestMissing"),
    ("component-ctest-drift", "CodexPolicyPreexistingComponentTestDrift"),
    ("preexisting-ctest-removal", "CodexPolicyPreexistingCTestRemoval"),
    ("generated-artifacts-test-removal", "CodexPolicyPreexistingComponentTestMissing"),
    ("standalone-policy-file-reclassification", "CodexPolicyManifestClassificationMismatch"),
    ("security-guard-reclassification", "CodexPolicyManifestClassificationMismatch"),
    ("snodec-blob-alteration", "CodexPolicySourceAuthorityMismatch"),
    ("snodec-clean-dependency-alteration", "CodexPolicySourceAuthorityMismatch"),
    ("snodec-cutover-false", "CodexPolicyCutoverReadinessMismatch"),
    ("source-package-owner-removal", "CodexPolicySourcePackageMismatch"),
    ("binary-package-policy-leak", "CodexPolicyBinaryPackageLeak"),
)


class PolicyVerificationError(RuntimeError):
    """A stable, machine-readable policy verification failure."""

    def __init__(self, code: str, detail: str):
        self.code = code
        self.detail = detail
        super().__init__(f"{code}: {detail}")


def fail(code: str, detail: str) -> None:
    raise PolicyVerificationError(code, detail)


def canonical_json(value: Any) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def canonical_sha256(value: Any) -> str:
    return sha256_bytes(canonical_json(value).encode("utf-8"))


def load_json(path: Path, *, canonical: bool = False, code: str = "CodexPolicyOwnershipMappingMismatch") -> Any:
    try:
        raw = path.read_text(encoding="utf-8")
        value = json.loads(raw)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(code, f"unable to load {path}: {error}")
    if canonical and raw != canonical_json(value):
        fail(code, f"{path} is not canonical sorted JSON")
    return value


def _replace_path(value: str, source_root: Path, build_root: Path | None) -> str:
    source = source_root.resolve().as_posix()
    build = build_root.resolve().as_posix() if build_root is not None else None
    result = value
    if build:
        result = result.replace(build, "${BUILD_DIR}")
    result = result.replace(source, "${SOURCE_DIR}")
    if result in ("/usr/bin/python3", "/usr/local/bin/python3") or re.fullmatch(
        r"/[^ ]*/python3(?:\.\d+)?", result
    ):
        return "${PYTHON3}"
    if result in ("/usr/bin/cmake", "/usr/local/bin/cmake") or re.fullmatch(
        r"/[^ ]*/cmake", result
    ):
        return "${CMAKE}"
    return result


def _normalize_command(
    command: Sequence[Any],
    source_root: Path,
    build_root: Path | None,
) -> list[str]:
    result: list[str] = []
    previous = ""
    variable_placeholders = {
        "-DAISUITE_CMAKE_COMMAND=": "${CMAKE}",
        "-DAISUITE_CMAKE_GENERATOR=": "${CMAKE_GENERATOR}",
        "-DAISUITE_CXX_COMPILER=": "${CXX_COMPILER}",
        "-DAISUITE_SNODEC_DIR=": "${SNODEC_PACKAGE_DIR}",
        "-DAISUITE_NLOHMANN_JSON_DIR=": "${NLOHMANN_JSON_PACKAGE_DIR}",
        "-DCMAKE_CPACK_COMMAND=": "${CPACK}",
        "-DSNODEC_SOURCE_REPOSITORY=": "${SNODEC_SOURCE_DIR}",
    }
    for raw_item in command:
        item = str(raw_item)
        if previous == "--snodec-root":
            normalized = "${SNODEC_SOURCE_DIR}"
        else:
            normalized = _replace_path(item, source_root, build_root)
            for prefix, placeholder in variable_placeholders.items():
                if normalized.startswith(prefix):
                    normalized = prefix + placeholder
                    break
        result.append(normalized)
        previous = item
    return result


def _infer_build_root(raw: Mapping[str, Any], source_root: Path) -> Path | None:
    candidates: dict[str, int] = {}
    source = source_root.resolve()
    for test in raw.get("tests", []):
        values: list[str] = [str(item) for item in test.get("command", [])]
        for prop in test.get("properties", []):
            value = prop.get("value")
            if isinstance(value, str):
                values.append(value)
            elif isinstance(value, list):
                values.extend(str(item) for item in value)
        for value in values:
            try:
                path = Path(value.split("=", 1)[-1])
                relative = path.resolve().relative_to(source)
            except (OSError, ValueError):
                continue
            if relative.parts and relative.parts[0].startswith("build"):
                candidates[relative.parts[0]] = candidates.get(relative.parts[0], 0) + 1
    if not candidates:
        return None
    name = max(sorted(candidates), key=lambda candidate: candidates[candidate])
    return source / name


def _backtrace_chain(
    graph: Mapping[str, Any],
    index: int | None,
    source_root: Path,
    build_root: Path | None,
) -> list[dict[str, Any]]:
    if index is None:
        return []
    nodes = graph.get("nodes", [])
    files = graph.get("files", [])
    commands = graph.get("commands", [])
    result: list[dict[str, Any]] = []
    seen: set[int] = set()
    current: int | None = index
    while current is not None:
        if current in seen or current < 0 or current >= len(nodes):
            fail("CodexPolicyOwnershipMappingMismatch", f"malformed CTest backtrace index {current}")
        seen.add(current)
        node = nodes[current]
        row: dict[str, Any] = {}
        file_index = node.get("file")
        if isinstance(file_index, int) and 0 <= file_index < len(files):
            row["file"] = _replace_path(str(files[file_index]), source_root, build_root)
        command_index = node.get("command")
        if isinstance(command_index, int) and 0 <= command_index < len(commands):
            row["command"] = str(commands[command_index])
        if isinstance(node.get("line"), int):
            row["line"] = int(node["line"])
        if row and (not result or row != result[-1]):
            result.append(row)
        parent = node.get("parent")
        current = int(parent) if isinstance(parent, int) else None
    return result


def canonicalize_ctest(
    raw: Mapping[str, Any],
    source_root: Path,
    build_root: Path | None = None,
) -> dict[str, Any]:
    """Return the deterministic CTest authority used by ownership checks."""

    if raw.get("kind") != "ctestInfo" or raw.get("version") != {"major": 1, "minor": 0}:
        fail("CodexPolicyOwnershipMappingMismatch", "unsupported CTest JSON model")
    actual_build = build_root.resolve() if build_root is not None else _infer_build_root(raw, source_root)
    graph = raw.get("backtraceGraph", {})
    tests: list[dict[str, Any]] = []
    for raw_test in raw.get("tests", []):
        properties: dict[str, Any] = {}
        for prop in raw_test.get("properties", []):
            name = str(prop.get("name", ""))
            if not name or name in properties:
                fail(
                    "CodexPolicyOwnershipMappingMismatch",
                    f"CTest test {raw_test.get('name')} has duplicate/malformed property {name!r}",
                )
            value = copy.deepcopy(prop.get("value"))
            if isinstance(value, str):
                value = _replace_path(value, source_root, actual_build)
            elif isinstance(value, list):
                value = [
                    _replace_path(str(item), source_root, actual_build)
                    if isinstance(item, str)
                    else item
                    for item in value
                ]
            if name == "LABELS" and isinstance(value, list):
                value = sorted(str(item) for item in value)
            properties[name] = value
        backtrace = _backtrace_chain(
            graph,
            raw_test.get("backtrace") if isinstance(raw_test.get("backtrace"), int) else None,
            source_root,
            actual_build,
        )
        registration_files = sorted(
            {
                str(row["file"])
                for row in backtrace
                if isinstance(row.get("file"), str)
            }
        )
        tests.append(
            {
                "name": str(raw_test.get("name", "")),
                "command": _normalize_command(
                    raw_test.get("command", []), source_root, actual_build
                ),
                "labels": list(properties.get("LABELS", [])),
                "timeout": properties.get("TIMEOUT"),
                "disabled": bool(properties.get("DISABLED", False)),
                "dependencies": list(properties.get("DEPENDS", [])),
                "properties": properties,
                "registration_backtrace": backtrace,
                "registration_files": registration_files,
            }
        )
    tests.sort(key=lambda row: (row["name"], canonical_json(row)))
    return {
        "format_version": 1,
        "ctest_json_version": {"major": 1, "minor": 0},
        "path_placeholders": dict(EXPECTED_PATH_PLACEHOLDERS),
        "test_count": len(tests),
        "tests": tests,
    }


def configured_ctest_model(root: Path, build_dir: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    result = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        cwd=root,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
    )
    if result.returncode != 0:
        fail(
            "CodexPolicyOwnershipMappingMismatch",
            result.stderr.strip() or f"CTest model command failed with {result.returncode}",
        )
    try:
        raw = json.loads(result.stdout)
    except json.JSONDecodeError as error:
        fail("CodexPolicyOwnershipMappingMismatch", f"CTest returned invalid JSON: {error}")
    return raw, canonicalize_ctest(raw, root, build_dir)


def _tests_by_name(model: Mapping[str, Any]) -> dict[str, list[dict[str, Any]]]:
    result: dict[str, list[dict[str, Any]]] = {}
    for test in model.get("tests", []):
        result.setdefault(str(test.get("name", "")), []).append(test)
    return result


def _is_component_test(test: Mapping[str, Any]) -> bool:
    prefix = "${SOURCE_DIR}/tests/component/codex/"
    return any(str(path).startswith(prefix) for path in test.get("registration_files", []))


def _is_policy_test(test: Mapping[str, Any]) -> bool:
    prefix = "${SOURCE_DIR}/tests/policy/"
    return any(str(path).startswith(prefix) for path in test.get("registration_files", []))


def _component_model(model: Mapping[str, Any]) -> list[dict[str, Any]]:
    return [copy.deepcopy(test) for test in model.get("tests", []) if _is_component_test(test)]


def _preservation_view(test: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "command": copy.deepcopy(test.get("command", [])),
        "labels": copy.deepcopy(test.get("labels", [])),
        "timeout": test.get("timeout"),
        "disabled": bool(test.get("disabled", False)),
        "dependencies": copy.deepcopy(test.get("dependencies", [])),
    }


def _component_stable_view(test: Mapping[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(dict(test))
    backtrace = result.get("registration_backtrace")
    if isinstance(backtrace, list):
        for frame in backtrace:
            if isinstance(frame, dict):
                frame.pop("line", None)
    return result


def _cmake_set_values(text: str, variable: str) -> list[str]:
    match = re.search(rf"\bset\s*\(\s*{re.escape(variable)}\b(.*?)\)", text, re.DOTALL)
    if match is None:
        fail("CodexPolicyPublicHeaderInventoryMismatch", f"missing CMake authority {variable}")
    body = re.sub(r"#[^\n]*", "", match.group(1))
    return [token.strip('"') for token in re.findall(r'"[^"]*"|[^\s]+', body) if token.strip('"').endswith(".h")]


def derive_public_header_inventory(root: Path) -> dict[str, Any]:
    components: list[dict[str, Any]] = []
    all_headers: list[str] = []
    for authority in HEADER_COMPONENTS:
        path = root / str(authority["cmake_path"])
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            fail("CodexPolicyPublicHeaderInventoryMismatch", f"unable to read {path}: {error}")
        values = _cmake_set_values(text, str(authority["variable"]))
        prefix = str(authority["installed_prefix"])
        base = "ai/openai/codex"
        headers = [
            f"{prefix}/{value}" if prefix != base else f"{base}/{value}"
            for value in values
        ]
        if len(values) != int(authority["expected_count"]):
            fail(
                "CodexPolicyPublicHeaderInventoryMismatch",
                f"{authority['name']} CMake public inventory has {len(values)} headers",
            )
        if len(set(headers)) != len(headers):
            fail("CodexPolicyPublicHeaderInventoryMismatch", f"duplicate {authority['name']} public header")
        if any("/detail/" in f"/{header}/" for header in headers):
            fail("CodexPolicyPublicHeaderInventoryMismatch", "private detail header is public")
        components.append(
            {
                "name": authority["name"],
                "cmake_path": authority["cmake_path"],
                "cmake_variable": authority["variable"],
                "count": len(headers),
                "headers": sorted(headers),
            }
        )
        all_headers.extend(headers)
    if len(all_headers) != 41 or len(set(all_headers)) != 41:
        fail(
            "CodexPolicyPublicHeaderInventoryMismatch",
            f"expected 41 unique Codex public headers, got {len(all_headers)}/{len(set(all_headers))}",
        )
    return {
        "components": components,
        "counts": {"main": 27, "backend": 7, "frontend": 7, "total": 41},
        "headers": sorted(all_headers),
        "original_a1_2_guards": ORIGINAL_HEADER_GUARDS,
        "inventory_authority": (
            "The three CMake public-header variables, their install(FILES) declarations, "
            "the staged installed prefix, and binary-package inventory are joined by the "
            "functional header tests."
        ),
    }


def _semantic_entries() -> list[dict[str, str]]:
    return [dict(row) for row in SEMANTIC_LOGGER_ENTRIES]


def _mutation_rows() -> list[dict[str, str]]:
    return [{"mutation_id": mutation, "diagnostic": diagnostic} for mutation, diagnostic in MUTATION_COVERAGE]


def _responsibilities() -> list[dict[str, Any]]:
    authorities = {row["responsibility_id"]: row for row in SOURCE_AUTHORITIES}
    return [
        {
            "responsibility_id": "codex-public-header-policy",
            "active": True,
            "source_authority": {
                "path": authorities["codex-public-header-policy"]["path"],
                "blob": authorities["codex-public-header-policy"]["blob"],
            },
            "aisuite_owner": {
                "owner_id": "aisuite-codex-public-header-policy",
                "implementation_files": [
                    "tests/policy/codex/CodexPublicHeaderPolicyTest.cpp",
                    "tests/policy/codex/CodexPublicHeaderSelfContainmentTest.cmake",
                ],
                "support_files": [
                    "tests/policy/support/AISuiteSourcePolicyTestRoot.h",
                    "tests/policy/support/CxxSourceScanner.h",
                ],
                "registration_owner": "tests/policy/codex/CMakeLists.txt",
            },
            "registered_ctests": [
                "CodexPublicHeaderPolicyTest",
                "CodexPublicHeaderSelfContainmentTest",
            ],
            "focused_ci_included": True,
            "planted_failure_ids": [
                "public-header-inventory-removal",
                "public-header-pragma-once",
                "public-header-guard-pair",
            ],
            "manifest_bucket": "standalone_files",
            "source_package_required": True,
            "binary_package_excluded": True,
        },
        {
            "responsibility_id": "codex-logging-api-surface-policy",
            "active": True,
            "source_authority": {
                "path": authorities["codex-logging-api-surface-policy"]["path"],
                "blob": authorities["codex-logging-api-surface-policy"]["blob"],
            },
            "aisuite_owner": {
                "owner_id": "aisuite-codex-logging-api-surface-policy",
                "implementation_files": [
                    "tests/policy/codex/CodexLoggingApiSurfacePolicyTest.cpp",
                ],
                "support_files": [
                    "tests/policy/support/AISuiteSourcePolicyTestRoot.h",
                    "tests/policy/support/CxxSourceScanner.h",
                ],
                "registration_owner": "tests/policy/codex/CMakeLists.txt",
            },
            "registered_ctests": ["CodexLoggingApiSurfacePolicyTest"],
            "focused_ci_included": True,
            "planted_failure_ids": ["logging-lifecycle-member"],
            "manifest_bucket": "standalone_files",
            "source_package_required": True,
            "binary_package_excluded": True,
        },
        {
            "responsibility_id": "codex-semantic-logger-policy",
            "active": True,
            "source_authority": {
                "path": authorities["codex-semantic-logger-policy"]["path"],
                "blob": authorities["codex-semantic-logger-policy"]["blob"],
            },
            "aisuite_owner": {
                "owner_id": "aisuite-codex-semantic-logger-policy",
                "implementation_files": [
                    "tests/policy/codex/CodexSemanticLoggerPolicyTest.cpp",
                    "tests/policy/codex/CodexSemanticLoggerAuthority.tsv",
                    "tests/policy/codex/CodexSemanticLoggerClassifications.tsv",
                ],
                "support_files": [
                    "tests/policy/support/AISuiteSourcePolicyTestRoot.h",
                    "tests/policy/support/CxxSourceScanner.h",
                ],
                "registration_owner": "tests/policy/codex/CMakeLists.txt",
            },
            "registered_ctests": ["CodexSemanticLoggerPolicyTest"],
            "focused_ci_included": True,
            "planted_failure_ids": [
                "semantic-logger-unclassified",
                "semantic-logger-classification-removal",
                "semantic-logger-authority-count",
                "semantic-logger-authority-expression",
            ],
            "manifest_bucket": "standalone_files",
            "source_package_required": True,
            "binary_package_excluded": True,
        },
    ]


def _functional_test_rows() -> list[dict[str, Any]]:
    return [
        {
            "test_name": name,
            "implementation_kind": (
                "preexisting-extracted-aisuite-policy" if name == SECURITY_TEST else "new-transferred-policy"
            ),
            "expected_labels": list(FUNCTIONAL_LABELS[name]),
            "expected_command": list(FUNCTIONAL_COMMANDS[name]),
            "expected_ci_filter": EXPECTED_CI_FILTER,
            "enabled": True,
        }
        for name in FUNCTIONAL_TESTS
    ]


def _verification_commands() -> dict[str, list[str]]:
    return {
        "CodexPolicyOwnershipTest": [
            "${PYTHON3}",
            "-B",
            "${SOURCE_DIR}/tools/extraction/verify_codex_policy_ownership.py",
            "check",
            "--repo-root",
            "${SOURCE_DIR}",
            "--build-dir",
            "${BUILD_DIR}",
            "--snodec-root",
            "${SNODEC_SOURCE_DIR}",
        ],
        "CodexPolicyMutationTest": [
            "${PYTHON3}",
            "-B",
            "${SOURCE_DIR}/tests/policy/codex/CodexPolicyMutationTest.py",
            "--repo-root",
            "${SOURCE_DIR}",
            "--build-dir",
            "${BUILD_DIR}",
            "--public-header-policy",
            "${BUILD_DIR}/tests/policy/codex/CodexPublicHeaderPolicyTest",
            "--logging-policy",
            "${BUILD_DIR}/tests/policy/codex/CodexLoggingApiSurfacePolicyTest",
            "--semantic-logger-policy",
            "${BUILD_DIR}/tests/policy/codex/CodexSemanticLoggerPolicyTest",
            "--ownership-tool",
            "${SOURCE_DIR}/tools/extraction/verify_codex_policy_ownership.py",
            "--baseline-model",
            "${SOURCE_DIR}/docs/extraction/codex-policy-baseline-ctest.json",
            "--final-model",
            "${SOURCE_DIR}/docs/extraction/codex-policy-final-ctest.json",
            "--ownership",
            "${SOURCE_DIR}/docs/extraction/codex-policy-ownership.json",
            "--manifest",
            "${SOURCE_DIR}/docs/extraction/source-manifest.json",
            "--workflow",
            "${SOURCE_DIR}/.github/workflows/ci.yml",
            "--source-package-test",
            "${SOURCE_DIR}/tests/AISuiteSourcePackageTest.cmake",
            "--binary-package-test",
            "${SOURCE_DIR}/tests/AISuiteBinaryPackageTest.cmake",
        ],
    }


def _verification_test_rows() -> list[dict[str, Any]]:
    commands = _verification_commands()
    return [
        {
            "test_name": name,
            "expected_labels": list(VERIFICATION_LABELS[name]),
            "expected_command": commands[name],
            "expected_dependencies": VERIFICATION_DEPENDENCIES[name],
            "expected_timeout": VERIFICATION_TIMEOUTS[name],
            "expected_ci_filter": EXPECTED_CI_FILTER,
            "enabled": True,
            "registration_owner": "tests/policy/codex/CMakeLists.txt",
        }
        for name in VERIFICATION_TESTS
    ]


def build_ownership_document(
    root: Path,
    baseline_model: Mapping[str, Any],
    final_model: Mapping[str, Any],
) -> dict[str, Any]:
    baseline_components = _component_model(baseline_model)
    final_components = _component_model(final_model)
    baseline_component_hash = canonical_sha256(baseline_components)
    final_component_hash = canonical_sha256(final_components)
    return {
        "format_version": 1,
        "generated_notice": (
            "Deterministic AISuite Codex policy-ownership and cutover-readiness "
            "evidence; this is test and extraction authority, not protocol runtime data."
        ),
        "aisuite_base": {"commit": AISUITE_BASE_SHA, "tree": AISUITE_BASE_TREE},
        "snodec_source_authority": {
            "repository": SNODEC_REPOSITORY,
            "commit": SNODEC_COMMIT,
            "tree": SNODEC_TREE,
            "policy_files": [
                {
                    "responsibility_id": row["responsibility_id"],
                    "path": row["path"],
                    "blob": row["blob"],
                    "sha256": row["sha256"],
                }
                for row in SOURCE_AUTHORITIES
            ],
            "reference_support_file": dict(SUPPORT_AUTHORITY),
        },
        "snodec_normal_dependency": {
            "repository": SNODEC_REPOSITORY,
            "commit": SNODEC_DEPENDENCY_COMMIT,
            "tree": SNODEC_DEPENDENCY_TREE,
            "role": "normal AISuite compilation and linking through one installed SNode.C prefix",
        },
        "transferred_responsibilities": _responsibilities(),
        "preexisting_aisuite_policy_tests": [
            {
                "test_name": SECURITY_TEST,
                "source_path": SECURITY_IMPORTED_PROVENANCE["path"],
                "registration_owner": "tests/policy/security/CMakeLists.txt",
                "previous_registration_owner": "tests/CMakeLists.txt",
                "ownership_kind": "preexisting-extracted-aisuite-policy",
                "counted_as_transferred_responsibility": False,
                "focused_ci_included": True,
                "expected_labels": list(FUNCTIONAL_LABELS[SECURITY_TEST]),
                "expected_working_directory": SECURITY_EXPECTED_WORKING_DIRECTORY,
                "normalized_ctest_working_directory": (
                    SECURITY_NORMALIZED_CTEST_WORKING_DIRECTORY
                ),
                "working_directory_rationale": SECURITY_WORKING_DIRECTORY_RATIONALE,
                "registration_count": 1,
                "file_behavior_changed": False,
                "imported_provenance": dict(SECURITY_IMPORTED_PROVENANCE),
            }
        ],
        "preserved_preexisting_test_suites": {
            "codex_component": {
                "registration_hierarchy": "tests/component/codex",
                "root_subdirectory_registration": "add_subdirectory(component/codex)",
                "baseline_test_count": len(baseline_components),
                "final_test_count": len(final_components),
                "baseline_model_sha256": baseline_component_hash,
                "final_model_sha256": final_component_hash,
                "generated_artifacts_guard": GENERATED_ARTIFACTS_TEST,
            },
            "all_ctest": {
                "baseline_test_count": int(baseline_model.get("test_count", 0)),
                "final_test_count": int(final_model.get("test_count", 0)),
            },
        },
        "ctest_evidence": {
            "baseline_model": BASELINE_CTEST_PATH.as_posix(),
            "baseline_model_sha256": canonical_sha256(baseline_model),
            "final_model": FINAL_CTEST_PATH.as_posix(),
            "final_model_sha256": canonical_sha256(final_model),
            "normalization": (
                "Source/build roots and configured Python/CMake executable paths are "
                "replaced with declared placeholders; tests and labels are sorted."
            ),
        },
        "functional_policy_tests": _functional_test_rows(),
        "verification_tests": _verification_test_rows(),
        "focused_ci": {
            "workflow": WORKFLOW_PATH.as_posix(),
            "jobs": ["gcc-debug", "gcc-15-debug"],
            "label_filter": EXPECTED_CI_FILTER,
            "functional_tests_selected": list(FUNCTIONAL_TESTS),
        },
        "public_header_policy": derive_public_header_inventory(root),
        "logging_api_surface_policy": {
            "scan_paths": [
                "src/ai/openai/codex/backend/BackendEvent.h",
                "src/ai/openai/codex/backend/BackendState.h",
            ],
            "forbidden_identifiers": list(FORBIDDEN_LOGGING_IDENTIFIERS),
            "token_aware": True,
        },
        "semantic_logger_policy": {
            "scan_root": "src/ai/openai/codex/",
            "source_extensions": [".cpp", ".h", ".hpp", ".ipp"],
            "accepted_entry_count": 4,
            "accepted_entries": _semantic_entries(),
        },
        "adapted_support_files": [
            {
                "source_path": SUPPORT_AUTHORITY["path"],
                "source_blob": SUPPORT_AUTHORITY["blob"],
                "source_sha256": SUPPORT_AUTHORITY["sha256"],
                "aisuite_destination": "tests/policy/support/AISuiteSourcePolicyTestRoot.h",
                "transfer_kind": "adapted-generic-root-helper",
                "license": "LGPL-3.0-or-later OR MIT",
                "manifest_bucket": "standalone_files",
            },
            {
                "source_paths": [
                    "tests/policy/log/LoggingApiSurfacePolicyTest.cpp",
                    "tests/policy/log/ParameterlessSemanticLoggerPolicyTest.cpp",
                ],
                "source_blobs": [
                    "e9beada86e032261d5b128cf58d0efdf1f927234",
                    "e4fddcaf69b23549eab318cb86afee6210b2aaad",
                ],
                "source_sha256": [
                    "85182c2a8b15e7a79b250e932ae509e6b540981b599d3f14a967dfca5a2883ac",
                    "3940c05cac92cf45531d019fcc23d0821c3526f8a75cf1afd19faf6da489c49f",
                ],
                "aisuite_destination": "tests/policy/support/CxxSourceScanner.h",
                "transfer_kind": "adapted-tokenizer-and-literal-masker",
                "license": "LGPL-3.0-or-later OR MIT",
                "manifest_bucket": "standalone_files",
            },
        ],
        "manifest_classifications": {
            "standalone_files": list(STANDALONE_POLICY_PATHS),
            "retained_imported_files": [dict(SECURITY_IMPORTED_PROVENANCE)],
            "filtered_history": {
                "selected_paths": list(EXPECTED_SELECTED_PATHS),
                "new_selected_paths_added": [],
            },
        },
        "source_package": {
            "required_paths": list(SOURCE_PACKAGE_REQUIRED_PATHS),
            "package_safe_command": (
                "python3 -B tools/extraction/verify_codex_policy_ownership.py "
                "check-package --repo-root <package-root>"
            ),
            "requires_git": False,
            "requires_network": False,
            "requires_external_snodec_checkout": False,
        },
        "binary_package": {
            "forbidden_patterns": list(BINARY_PACKAGE_FORBIDDEN_PATTERNS),
            "policy_internals_excluded": True,
        },
        "mutation_coverage": _mutation_rows(),
        "cutover_readiness": {
            "ready": True,
            "statement": (
                "AISuite Codex policy ownership is complete and normal builds use "
                "the cleaned SNode.C dependency; the historical SNode.C tree is "
                "immutable extraction provenance only."
            ),
            "snodec_cutover_performed": True,
            "normal_dependency_commit": SNODEC_DEPENDENCY_COMMIT,
            "normal_dependency_tree": SNODEC_DEPENDENCY_TREE,
            "extraction_provenance_commit": SNODEC_COMMIT,
            "extraction_provenance_tree": SNODEC_TREE,
        },
    }


@dataclass
class VerificationFixture:
    """Mutable, isolated verifier input used by planted-failure tests."""

    root: Path
    ownership: dict[str, Any]
    baseline_model: dict[str, Any]
    final_model: dict[str, Any]
    manifest: dict[str, Any]
    workflow_text: str
    root_tests_cmake: str
    policy_cmake: str
    security_cmake: str | None
    filter_map: dict[str, Any]
    source_package_paths: set[str] | None = None
    binary_package_paths: set[str] | None = None
    snodec_root: Path | None = None
    verify_owner_files: bool = True


def _read_text(path: Path, code: str) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        fail(code, f"unable to read {path}: {error}")


def _load_path_model(path: Path | None) -> set[str] | None:
    if path is None:
        return None
    text = _read_text(path, "CodexPolicyOwnershipMappingMismatch")
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        value = None
    if isinstance(value, list):
        rows = value
    elif isinstance(value, dict) and isinstance(value.get("paths"), list):
        rows = value["paths"]
    else:
        rows = [line.strip() for line in text.splitlines() if line.strip()]
    result: set[str] = set()
    for item in rows:
        path_text = str(item).replace("\\", "/").lstrip("./")
        # CPack tar listings generally carry one package-root component.
        if "/" in path_text and path_text.split("/", 1)[0].startswith("AISuite-"):
            path_text = path_text.split("/", 1)[1]
        result.add(path_text.rstrip("/"))
    return result


def package_files(root: Path) -> set[str]:
    result: set[str] = set()
    for path in root.rglob("*"):
        try:
            mode = path.lstat().st_mode
        except OSError as error:
            fail("CodexPolicySourcePackageMismatch", f"unable to inspect {path}: {error}")
        if path.is_symlink():
            fail("CodexPolicySourcePackageMismatch", f"package contains symlink {path}")
        if stat.S_ISREG(mode):
            result.add(path.relative_to(root).as_posix())
    return result


def load_verification_fixture(
    repo_root: Path,
    *,
    ownership_path: Path | None = None,
    baseline_model_path: Path | None = None,
    final_model_path: Path | None = None,
    manifest_path: Path | None = None,
    workflow_path: Path | None = None,
    source_package_model_path: Path | None = None,
    binary_package_model_path: Path | None = None,
    snodec_root: Path | None = None,
    verify_owner_files: bool = True,
    canonical_documents: bool = False,
) -> VerificationFixture:
    root = repo_root.resolve()
    ownership_file = (ownership_path or root / OWNERSHIP_PATH).resolve()
    baseline_file = (baseline_model_path or root / BASELINE_CTEST_PATH).resolve()
    final_file = (final_model_path or root / FINAL_CTEST_PATH).resolve()
    manifest_file = (manifest_path or root / MANIFEST_PATH).resolve()
    workflow_file = (workflow_path or root / WORKFLOW_PATH).resolve()
    return VerificationFixture(
        root=root,
        ownership=load_json(
            ownership_file,
            canonical=canonical_documents,
            code="CodexPolicyOwnershipMappingMismatch",
        ),
        baseline_model=load_json(
            baseline_file,
            canonical=canonical_documents,
            code="CodexPolicyOwnershipMappingMismatch",
        ),
        final_model=load_json(
            final_file,
            canonical=canonical_documents,
            code="CodexPolicyOwnershipMappingMismatch",
        ),
        manifest=load_json(
            manifest_file,
            canonical=canonical_documents,
            code="CodexPolicyManifestClassificationMismatch",
        ),
        workflow_text=_read_text(workflow_file, "CodexPolicyCIFilterMismatch"),
        root_tests_cmake=_read_text(
            root / "tests/CMakeLists.txt", "CodexPolicyHierarchyRegistrationMismatch"
        ),
        policy_cmake=_read_text(
            root / "tests/policy/CMakeLists.txt",
            "CodexPolicyHierarchyRegistrationMismatch",
        ),
        security_cmake=(
            _read_text(
                root / "tests/policy/security/CMakeLists.txt",
                "CodexPolicyHierarchyRegistrationMismatch",
            )
            if (root / "tests/policy/security/CMakeLists.txt").is_file()
            else None
        ),
        filter_map=load_json(
            root / FILTER_MAP_PATH,
            canonical=False,
            code="CodexPolicyManifestClassificationMismatch",
        ),
        source_package_paths=_load_path_model(source_package_model_path),
        binary_package_paths=_load_path_model(binary_package_model_path),
        snodec_root=snodec_root.resolve() if snodec_root is not None else None,
        verify_owner_files=verify_owner_files,
    )


def _git_text(root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        fail(
            "CodexPolicySourceAuthorityMismatch",
            result.stderr.strip() or f"git {' '.join(arguments)} failed",
        )
    return result.stdout.strip()


def _validate_source_authority(fixture: VerificationFixture) -> None:
    dependency = fixture.ownership.get("snodec_normal_dependency")
    if dependency != {
        "repository": SNODEC_REPOSITORY,
        "commit": SNODEC_DEPENDENCY_COMMIT,
        "tree": SNODEC_DEPENDENCY_TREE,
        "role": "normal AISuite compilation and linking through one installed SNode.C prefix",
    }:
        fail(
            "CodexPolicySourceAuthorityMismatch",
            "cleaned SNode.C normal dependency authority changed",
        )
    authority = fixture.ownership.get("snodec_source_authority")
    if not isinstance(authority, dict):
        fail("CodexPolicySourceAuthorityMismatch", "SNode.C source authority is absent")
    expected_header = {
        "repository": SNODEC_REPOSITORY,
        "commit": SNODEC_COMMIT,
        "tree": SNODEC_TREE,
    }
    for key, value in expected_header.items():
        if authority.get(key) != value:
            fail("CodexPolicySourceAuthorityMismatch", f"SNode.C {key} changed")
    rows = authority.get("policy_files")
    expected_rows = [
        {
            "responsibility_id": row["responsibility_id"],
            "path": row["path"],
            "blob": row["blob"],
            "sha256": row["sha256"],
        }
        for row in SOURCE_AUTHORITIES
    ]
    if rows != expected_rows or authority.get("reference_support_file") != SUPPORT_AUTHORITY:
        fail("CodexPolicySourceAuthorityMismatch", "pinned policy path/blob/hash inventory changed")
    if fixture.snodec_root is None:
        return
    root = fixture.snodec_root
    if _git_text(root, "rev-parse", "HEAD") != SNODEC_COMMIT:
        fail("CodexPolicySourceAuthorityMismatch", "SNode.C checkout commit changed")
    if _git_text(root, "rev-parse", "HEAD^{tree}") != SNODEC_TREE:
        fail("CodexPolicySourceAuthorityMismatch", "SNode.C checkout tree changed")
    if _git_text(root, "status", "--porcelain"):
        fail("CodexPolicySourceAuthorityMismatch", "SNode.C checkout is not clean")
    for row in (*SOURCE_AUTHORITIES, SUPPORT_AUTHORITY):
        if _git_text(root, "rev-parse", f"HEAD:{row['path']}") != row["blob"]:
            fail("CodexPolicySourceAuthorityMismatch", f"SNode.C blob changed: {row['path']}")
        path = root / row["path"]
        if not path.is_file() or sha256_file(path) != row["sha256"]:
            fail("CodexPolicySourceAuthorityMismatch", f"SNode.C content changed: {row['path']}")


def _owner_paths(responsibility: Mapping[str, Any]) -> list[str]:
    owner = responsibility.get("aisuite_owner")
    if not isinstance(owner, dict):
        return []
    paths: list[str] = []
    for key in ("implementation_files", "support_files"):
        value = owner.get(key)
        if isinstance(value, list):
            paths.extend(str(item) for item in value)
    if isinstance(owner.get("registration_owner"), str):
        paths.append(str(owner["registration_owner"]))
    return paths


def _validate_ownership_mapping(fixture: VerificationFixture) -> None:
    ownership = fixture.ownership
    expected_document = build_ownership_document(
        fixture.root, fixture.baseline_model, fixture.final_model
    )
    if set(ownership) != set(expected_document):
        fail(
            "CodexPolicyOwnershipMappingMismatch",
            "ownership document top-level structure changed",
        )
    if ownership.get("format_version") != 1:
        fail("CodexPolicyOwnershipMappingMismatch", "ownership format version changed")
    if ownership.get("generated_notice") != expected_document["generated_notice"]:
        fail("CodexPolicyOwnershipMappingMismatch", "ownership generated notice changed")
    if ownership.get("aisuite_base") != {
        "commit": AISUITE_BASE_SHA,
        "tree": AISUITE_BASE_TREE,
    }:
        fail("CodexPolicyOwnershipMappingMismatch", "AISuite base authority changed")
    responsibilities = ownership.get("transferred_responsibilities")
    if not isinstance(responsibilities, list) or len(responsibilities) != 3:
        fail("CodexPolicyOwnershipMultiplicityMismatch", "expected exactly three transferred responsibilities")
    expected = {row["responsibility_id"]: row for row in _responsibilities()}
    seen: set[str] = set()
    registered: list[str] = []
    owner_ids: list[str] = []
    for row in responsibilities:
        if not isinstance(row, dict):
            fail("CodexPolicyOwnershipMappingMismatch", "malformed responsibility row")
        responsibility_id = str(row.get("responsibility_id", ""))
        if responsibility_id in seen:
            fail("CodexPolicyOwnershipMultiplicityMismatch", f"duplicate responsibility {responsibility_id}")
        seen.add(responsibility_id)
        reference = expected.get(responsibility_id)
        if reference is None:
            fail("CodexPolicyOwnershipMappingMismatch", f"unknown responsibility {responsibility_id}")
        if row.get("active") is not True:
            fail("CodexPolicyOwnershipMappingMismatch", f"inactive responsibility {responsibility_id}")
        if row.get("source_authority") != reference["source_authority"]:
            fail("CodexPolicySourceAuthorityMismatch", f"source mapping changed for {responsibility_id}")
        owner = row.get("aisuite_owner")
        if not isinstance(owner, dict) or owner != reference["aisuite_owner"]:
            fail("CodexPolicyOwnershipMappingMismatch", f"owner mapping changed for {responsibility_id}")
        owner_ids.append(str(owner["owner_id"]))
        if row.get("registered_ctests") != reference["registered_ctests"]:
            fail("CodexPolicyOwnershipMappingMismatch", f"CTest mapping changed for {responsibility_id}")
        registered.extend(str(item) for item in row["registered_ctests"])
        if row.get("planted_failure_ids") != reference["planted_failure_ids"]:
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"planted-failure coverage changed for {responsibility_id}",
            )
        for field in (
            "focused_ci_included",
            "source_package_required",
            "binary_package_excluded",
        ):
            if row.get(field) is not True:
                fail("CodexPolicyCutoverReadinessMismatch", f"{field} is false for {responsibility_id}")
        if row.get("manifest_bucket") != "standalone_files":
            fail("CodexPolicyManifestClassificationMismatch", f"owner bucket changed for {responsibility_id}")
        if set(row) != set(reference):
            fail(
                "CodexPolicyOwnershipMappingMismatch",
                f"responsibility schema changed for {responsibility_id}",
            )
        if fixture.verify_owner_files:
            for path in _owner_paths(row):
                if not (fixture.root / path).is_file():
                    fail("CodexPolicyOwnershipMappingMismatch", f"mapped owner does not exist: {path}")
    if seen != set(expected):
        fail("CodexPolicyOwnershipMappingMismatch", f"missing responsibility IDs {sorted(set(expected)-seen)}")
    if len(set(owner_ids)) != 3:
        fail("CodexPolicyOwnershipMultiplicityMismatch", "responsibilities do not have unique conceptual owners")
    if sorted(registered) != sorted(NEW_FUNCTIONAL_TESTS):
        fail("CodexPolicyOwnershipMappingMismatch", "functional CTest responsibility bijection changed")
    preexisting = ownership.get("preexisting_aisuite_policy_tests")
    expected_preexisting = expected_document["preexisting_aisuite_policy_tests"]
    if preexisting != expected_preexisting:
        fail("CodexPolicyOwnershipMappingMismatch", "pre-existing security-policy ownership changed")


def _registration_count(text: str | None, test_name: str) -> int:
    if text is None:
        return 0
    text = _strip_cmake_comments(text)
    pattern = rf"\badd_test\s*\(\s*NAME\s+{re.escape(test_name)}\b"
    return len(re.findall(pattern, text, re.IGNORECASE | re.DOTALL))


def _strip_cmake_comments(text: str) -> str:
    lines: list[str] = []
    for line in text.splitlines(keepends=True):
        quoted = False
        escaped = False
        output: list[str] = []
        for character in line:
            if character == '"' and not escaped:
                quoted = not quoted
            if character == "#" and not quoted:
                output.extend(" " for _ in line[len(output) :])
                break
            output.append(character)
            escaped = character == "\\" and not escaped
            if character != "\\":
                escaped = False
        lines.append("".join(output))
    return "".join(lines)


@dataclass(frozen=True)
class _CMakeArgument:
    value: str
    kind: str


def _cmake_bracket_argument(
    text: str,
    position: int,
) -> tuple[str, int] | None:
    if position >= len(text) or text[position] != "[":
        return None
    cursor = position + 1
    while cursor < len(text) and text[cursor] == "=":
        cursor += 1
    if cursor >= len(text) or text[cursor] != "[":
        return None
    closing = "]" + text[position + 1 : cursor] + "]"
    content_start = cursor + 1
    content_end = text.find(closing, content_start)
    if content_end < 0:
        fail(
            "CodexPolicyExistingSecurityGuardDrift",
            "security CMake contains an unterminated bracket argument",
        )
    return text[content_start:content_end], content_end + len(closing)


def _cmake_arguments(text: str) -> list[_CMakeArgument]:
    """Tokenize the CMake argument forms needed by policy registration checks."""

    result: list[_CMakeArgument] = []
    position = 0
    while position < len(text):
        character = text[position]
        if character.isspace():
            position += 1
            continue
        if character == "#":
            bracket_comment = _cmake_bracket_argument(text, position + 1)
            if bracket_comment is not None:
                _content, position = bracket_comment
            else:
                newline = text.find("\n", position + 1)
                position = len(text) if newline < 0 else newline + 1
            continue
        if character == "(":
            result.append(_CMakeArgument("(", "open"))
            position += 1
            continue
        if character == ")":
            result.append(_CMakeArgument(")", "close"))
            position += 1
            continue
        bracket = _cmake_bracket_argument(text, position)
        if bracket is not None:
            value, position = bracket
            result.append(_CMakeArgument(value, "bracket"))
            continue
        if character == '"':
            position += 1
            value: list[str] = []
            while position < len(text):
                character = text[position]
                if character == '"':
                    position += 1
                    break
                if character == "\\" and position + 1 < len(text):
                    value.append(text[position : position + 2])
                    position += 2
                    continue
                value.append(character)
                position += 1
            else:
                fail(
                    "CodexPolicyExistingSecurityGuardDrift",
                    "security CMake contains an unterminated quoted argument",
                )
            result.append(_CMakeArgument("".join(value), "quoted"))
            continue
        value = []
        while position < len(text):
            character = text[position]
            if character.isspace() or character in "()#":
                break
            if character == "\\" and position + 1 < len(text):
                value.append(text[position : position + 2])
                position += 2
                continue
            value.append(character)
            position += 1
        if not value:
            fail(
                "CodexPolicyExistingSecurityGuardDrift",
                f"security CMake contains an unsupported token at offset {position}",
            )
        result.append(_CMakeArgument("".join(value), "unquoted"))
    return result


def _cmake_command_arguments(
    text: str,
    command_name: str,
) -> list[list[_CMakeArgument]]:
    tokens = _cmake_arguments(text)
    result: list[list[_CMakeArgument]] = []
    position = 0
    while position + 1 < len(tokens):
        command = tokens[position]
        opening = tokens[position + 1]
        if (
            command.kind != "unquoted"
            or opening.kind != "open"
        ):
            position += 1
            continue
        depth = 1
        arguments: list[_CMakeArgument] = []
        position += 2
        while position < len(tokens) and depth:
            token = tokens[position]
            if token.kind == "open":
                depth += 1
            elif token.kind == "close":
                depth -= 1
            elif depth == 1:
                arguments.append(token)
            position += 1
        if depth:
            fail(
                "CodexPolicyExistingSecurityGuardDrift",
                f"security CMake has an unterminated {command.value} invocation",
            )
        if command.value.casefold() == command_name.casefold():
            result.append(arguments)
    return result


def _security_cmake_property_values(
    text: str,
    property_name: str,
) -> list[_CMakeArgument]:
    values: list[_CMakeArgument] = []
    for arguments in _cmake_command_arguments(text, "set_tests_properties"):
        properties_index = next(
            (
                index
                for index, argument in enumerate(arguments)
                if argument.value.casefold() == "properties"
            ),
            None,
        )
        if properties_index is None or not any(
            argument.value == SECURITY_TEST
            for argument in arguments[:properties_index]
        ):
            continue
        properties = arguments[properties_index + 1 :]
        if len(properties) % 2:
            fail(
                "CodexPolicyExistingSecurityGuardDrift",
                f"{SECURITY_TEST} has malformed CMake property pairs",
            )
        for index in range(0, len(properties), 2):
            if properties[index].value.casefold() == property_name.casefold():
                values.append(properties[index + 1])
    return values


def _subdirectory_count(text: str, directory: str) -> int:
    text = _strip_cmake_comments(text)
    return len(
        re.findall(
            rf"\badd_subdirectory\s*\(\s*{re.escape(directory)}(?:\s|\))",
            text,
            re.IGNORECASE,
        )
    )


def _validate_hierarchy(fixture: VerificationFixture) -> None:
    root_cmake = _strip_cmake_comments(fixture.root_tests_cmake)
    component_count = _subdirectory_count(root_cmake, "component/codex")
    if component_count != 1:
        fail(
            "CodexPolicyPreexistingComponentTestMissing",
            f"root component/codex registration count is {component_count}",
        )
    policy_count = _subdirectory_count(root_cmake, "policy")
    if policy_count != 1:
        fail(
            "CodexPolicyHierarchyRegistrationMismatch",
            f"root policy registration count is {policy_count}",
        )
    if _subdirectory_count(fixture.policy_cmake, "codex") != 1 or _subdirectory_count(
        fixture.policy_cmake, "security"
    ) != 1:
        fail(
            "CodexPolicyHierarchyRegistrationMismatch",
            "tests/policy must own exactly one codex and security subdirectory",
        )
    root_security = _registration_count(fixture.root_tests_cmake, SECURITY_TEST)
    security_owner = _registration_count(fixture.security_cmake, SECURITY_TEST)
    total = root_security + security_owner
    if root_security and security_owner:
        fail("CodexPolicyDuplicateTestRegistration", f"{SECURITY_TEST} has two CMake registrations")
    if fixture.security_cmake is None and root_security:
        fail(
            "CodexPolicyHierarchyRegistrationMismatch",
            f"{SECURITY_TEST} remains owned directly by tests/CMakeLists.txt",
        )
    if total == 0:
        fail("CodexPolicyExistingSecurityGuardNotRegistered", f"{SECURITY_TEST} is unregistered")
    if root_security != 0 or security_owner != 1:
        fail(
            "CodexPolicyHierarchyRegistrationMismatch",
            f"{SECURITY_TEST} registration owner is incorrect",
        )
    policy_match = re.search(
        r"\badd_subdirectory\s*\(\s*policy(?:\s|\))",
        root_cmake,
        re.IGNORECASE,
    )
    if policy_match is None:
        fail("CodexPolicyHierarchyRegistrationMismatch", "root policy registration is absent")
    policy_position = policy_match.start()
    component_match = re.search(
        r"\badd_subdirectory\s*\(\s*component/codex(?:\s|\))",
        root_cmake,
        re.IGNORECASE,
    )
    if component_match is None or component_match.start() >= policy_position:
        fail(
            "CodexPolicyHierarchyRegistrationMismatch",
            "policy hierarchy precedes component/codex dependency registration",
        )
    for dependency in (
        "AISuiteInstalledConsumerTest",
        "AISuiteSourcePackageTest",
        "AISuiteBinaryPackageTest",
        "CodexA14AuditToolTest",
    ):
        match = re.search(
            rf"\bNAME\s+{re.escape(dependency)}\b",
            root_cmake,
            re.IGNORECASE,
        )
        if match is None or match.start() >= policy_position:
            fail(
                "CodexPolicyHierarchyRegistrationMismatch",
                f"policy hierarchy precedes dependency {dependency}",
            )


def _test_single(index: Mapping[str, list[dict[str, Any]]], name: str) -> dict[str, Any]:
    rows = index.get(name, [])
    if len(rows) > 1:
        fail("CodexPolicyDuplicateTestRegistration", f"{name} is registered {len(rows)} times")
    if not rows:
        fail("CodexPolicyTestNotRegistered", f"{name} is not registered")
    return rows[0]


def _selected_by_filter(labels: Iterable[str], expression: str) -> bool:
    try:
        pattern = re.compile(expression)
    except re.error as error:
        fail("CodexPolicyCIFilterMismatch", f"invalid focused label expression: {error}")
    return any(pattern.search(str(label)) is not None for label in labels)


def _has_skip_or_inversion(test: Mapping[str, Any]) -> str | None:
    properties = test.get("properties", {})
    for name in ("SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION", "WILL_FAIL"):
        if name in properties:
            return name
    return None


def _validate_functional_ctest(fixture: VerificationFixture) -> None:
    index = _tests_by_name(fixture.final_model)
    for name in FUNCTIONAL_TESTS:
        test = _test_single(index, name)
        if bool(test.get("disabled", False)):
            fail("CodexPolicyTestDisabled", f"{name} is disabled")
        skip_property = _has_skip_or_inversion(test)
        if skip_property is not None:
            fail("CodexPolicyTestDisabled", f"{name} uses {skip_property}")
        timeout = test.get("timeout")
        if not isinstance(timeout, (int, float)) or not math.isfinite(float(timeout)) or float(timeout) <= 0:
            fail("CodexPolicyTestDisabled", f"{name} has no finite nonzero timeout")
        expected_labels = set(FUNCTIONAL_LABELS[name])
        actual_labels = {str(item) for item in test.get("labels", [])}
        if not _selected_by_filter(actual_labels, EXPECTED_CI_FILTER):
            fail("CodexPolicyTestExcludedFromFocusedCI", f"{name} labels are excluded")
        if actual_labels != expected_labels:
            if name == SECURITY_TEST:
                fail("CodexPolicyExistingSecurityGuardDrift", f"{name} labels drifted")
            fail("CodexPolicyOwnershipMappingMismatch", f"{name} label set drifted")
        expected_owner = (
            "${SOURCE_DIR}/tests/policy/security/CMakeLists.txt"
            if name == SECURITY_TEST
            else "${SOURCE_DIR}/tests/policy/codex/CMakeLists.txt"
        )
        if test.get("registration_files") != [expected_owner]:
            fail(
                "CodexPolicyHierarchyRegistrationMismatch",
                f"{name} configured registration owner changed",
            )
        if test.get("command") != FUNCTIONAL_COMMANDS[name]:
            code = (
                "CodexPolicyExistingSecurityGuardDrift"
                if name == SECURITY_TEST
                else "CodexPolicyOwnershipMappingMismatch"
            )
            fail(code, f"{name} command/arguments drifted")
    policy_functional = sorted(
        str(test["name"])
        for test in fixture.final_model.get("tests", [])
        if _is_policy_test(test)
        and "policy" in test.get("labels", [])
        and "verification" not in test.get("labels", [])
    )
    if policy_functional != sorted(FUNCTIONAL_TESTS):
        fail(
            "CodexPolicyOwnershipMultiplicityMismatch",
            f"functional policy hierarchy is {policy_functional}",
        )


def _validate_verification_ctest(fixture: VerificationFixture) -> None:
    index = _tests_by_name(fixture.final_model)
    commands = _verification_commands()
    expected_owner = "${SOURCE_DIR}/tests/policy/codex/CMakeLists.txt"
    for name in VERIFICATION_TESTS:
        rows = index.get(name, [])
        if len(rows) != 1:
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"verification test {name} count is {len(rows)}",
            )
        test = rows[0]
        timeout = test.get("timeout")
        if (
            bool(test.get("disabled", False))
            or not isinstance(timeout, (int, float))
            or not math.isfinite(float(timeout))
            or float(timeout) != VERIFICATION_TIMEOUTS[name]
            or _has_skip_or_inversion(test) is not None
        ):
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"verification test {name} is not unconditionally enabled",
            )
        if set(test.get("labels", [])) != set(VERIFICATION_LABELS[name]):
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"verification test {name} labels changed",
            )
        if not _selected_by_filter(test.get("labels", []), EXPECTED_CI_FILTER):
            fail(
                "CodexPolicyTestExcludedFromFocusedCI",
                f"verification test {name} is excluded from focused CI",
            )
        if test.get("dependencies") != VERIFICATION_DEPENDENCIES[name]:
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"verification test {name} dependency chain changed",
            )
        if test.get("command") != commands[name]:
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"verification test {name} command/arguments changed",
            )
        if test.get("registration_files") != [expected_owner]:
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"verification test {name} registration owner changed",
            )
        properties = test.get("properties", {})
        if (
            properties.get("ENVIRONMENT") != ["PYTHONDONTWRITEBYTECODE=1"]
            or properties.get("RUN_SERIAL") is not True
            or properties.get("WORKING_DIRECTORY")
            != "${BUILD_DIR}/tests/policy/codex"
        ):
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"verification test {name} execution properties changed",
            )


def _validate_mutation_harness(fixture: VerificationFixture) -> None:
    if not fixture.verify_owner_files:
        return
    path = fixture.root / "tests/policy/codex/CodexPolicyMutationTest.py"
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    except (OSError, UnicodeError, SyntaxError) as error:
        fail("CodexPolicyCutoverReadinessMismatch", f"mutation harness is unreadable: {error}")
    mapping_value: Any = None
    methods: dict[str, ast.FunctionDef | ast.AsyncFunctionDef] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.ClassDef) and node.name == "CodexPolicyMutationTest":
            methods = {
                child.name: child
                for child in node.body
                if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef))
                and child.name.startswith("test_")
            }
        target: ast.expr | None = None
        value: ast.expr | None = None
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            value = node.value
        elif isinstance(node, ast.AnnAssign):
            target = node.target
            value = node.value
        if isinstance(target, ast.Name) and target.id == "MUTATION_TEST_METHODS" and value is not None:
            try:
                mapping_value = ast.literal_eval(value)
            except (ValueError, TypeError, SyntaxError):
                mapping_value = None
    expected = dict(MUTATION_COVERAGE)
    if (
        not isinstance(mapping_value, dict)
        or set(mapping_value) != set(expected)
        or any(not isinstance(value, str) for value in mapping_value.values())
        or len(set(mapping_value.values())) != len(expected)
        or any(not value.startswith("test_") for value in mapping_value.values())
    ):
        fail(
            "CodexPolicyCutoverReadinessMismatch",
            f"mutation harness does not map exactly {len(expected)} unique planted failures",
        )
    for mutation_id, method_name in mapping_value.items():
        method = methods.get(method_name)
        if method is None:
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"mutation {mutation_id} maps to missing test method {method_name}",
            )
        string_constants = {
            node.value
            for node in ast.walk(method)
            if isinstance(node, ast.Constant) and isinstance(node.value, str)
        }
        if expected[mutation_id] not in string_constants:
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"mutation {mutation_id} method does not assert {expected[mutation_id]}",
            )


def _validate_security_drift(fixture: VerificationFixture) -> None:
    baseline_index = _tests_by_name(fixture.baseline_model)
    final_index = _tests_by_name(fixture.final_model)
    baseline_rows = baseline_index.get(SECURITY_TEST, [])
    final_rows = final_index.get(SECURITY_TEST, [])
    if not baseline_rows or not final_rows:
        # Functional registration validation owns the missing-final diagnostic.
        return
    if len(baseline_rows) != 1 or len(final_rows) != 1:
        fail("CodexPolicyDuplicateTestRegistration", f"{SECURITY_TEST} is not unique")
    baseline = baseline_rows[0]
    final = final_rows[0]
    for model_name, test in (("baseline", baseline), ("final", final)):
        if (
            test.get("properties", {}).get("WORKING_DIRECTORY")
            != SECURITY_NORMALIZED_CTEST_WORKING_DIRECTORY
        ):
            fail(
                "CodexPolicyExistingSecurityGuardDrift",
                f"{SECURITY_TEST} {model_name} working directory drifted",
            )
    for field in ("command", "labels", "timeout", "disabled", "dependencies"):
        if baseline.get(field) != final.get(field):
            fail("CodexPolicyExistingSecurityGuardDrift", f"{SECURITY_TEST} {field} drifted")
    for prop in ("ENVIRONMENT", "RUN_SERIAL", "TIMEOUT", "LABELS", "DEPENDS", "WORKING_DIRECTORY"):
        if baseline.get("properties", {}).get(prop) != final.get("properties", {}).get(prop):
            fail("CodexPolicyExistingSecurityGuardDrift", f"{SECURITY_TEST} {prop} drifted")
    expected_owner = "${SOURCE_DIR}/tests/policy/security/CMakeLists.txt"
    if expected_owner not in final.get("registration_files", []):
        fail("CodexPolicyHierarchyRegistrationMismatch", f"{SECURITY_TEST} configured owner changed")
    if final.get("dependencies") != list(SECURITY_DEPENDENCIES):
        fail("CodexPolicyExistingSecurityGuardDrift", f"{SECURITY_TEST} dependency order drifted")
    if final.get("properties", {}).get("ENVIRONMENT") != ["PYTHONDONTWRITEBYTECODE=1"]:
        fail("CodexPolicyExistingSecurityGuardDrift", f"{SECURITY_TEST} environment drifted")
    if final.get("properties", {}).get("RUN_SERIAL") is not True:
        fail("CodexPolicyExistingSecurityGuardDrift", f"{SECURITY_TEST} is not serial")
    if float(final.get("timeout", 0)) != 120.0:
        fail("CodexPolicyExistingSecurityGuardDrift", f"{SECURITY_TEST} timeout drifted")
    if final.get("command") != FUNCTIONAL_COMMANDS[SECURITY_TEST]:
        fail("CodexPolicyExistingSecurityGuardDrift", f"{SECURITY_TEST} command drifted")
    if fixture.security_cmake is None:
        fail(
            "CodexPolicyExistingSecurityGuardDrift",
            f"{SECURITY_TEST} working-directory registration is absent",
        )
    property_values = _security_cmake_property_values(
        fixture.security_cmake,
        "WORKING_DIRECTORY",
    )
    expected_argument = _CMakeArgument(
        SECURITY_EXPECTED_WORKING_DIRECTORY,
        "quoted",
    )
    if property_values != [expected_argument]:
        fail(
            "CodexPolicyExistingSecurityGuardDrift",
            (
                f"{SECURITY_TEST} must explicitly use "
                "WORKING_DIRECTORY "
                f'"{SECURITY_EXPECTED_WORKING_DIRECTORY}"'
            ),
        )


def _validate_live_security_working_directory(model: Mapping[str, Any]) -> None:
    """Diagnose live configured cwd drift before generic model mismatch."""

    rows = _tests_by_name(model).get(SECURITY_TEST, [])
    if len(rows) != 1:
        return
    if (
        rows[0].get("properties", {}).get("WORKING_DIRECTORY")
        != SECURITY_NORMALIZED_CTEST_WORKING_DIRECTORY
    ):
        fail(
            "CodexPolicyExistingSecurityGuardDrift",
            f"{SECURITY_TEST} live configured working directory drifted",
        )


def _validate_preexisting_ctest(fixture: VerificationFixture) -> None:
    baseline_index = _tests_by_name(fixture.baseline_model)
    final_index = _tests_by_name(fixture.final_model)
    baseline_components = {
        str(test["name"]): test
        for test in fixture.baseline_model.get("tests", [])
        if _is_component_test(test)
    }
    final_components = {
        str(test["name"]): test
        for test in fixture.final_model.get("tests", [])
        if _is_component_test(test)
    }
    for name, baseline in baseline_components.items():
        rows = final_index.get(name, [])
        if len(rows) != 1:
            fail("CodexPolicyPreexistingComponentTestMissing", f"component test {name} count is {len(rows)}")
        final = rows[0]
        if not _is_component_test(final):
            fail(
                "CodexPolicyPreexistingComponentTestDrift",
                f"component test {name} registration ownership moved",
            )
        if bool(final.get("disabled", False)):
            fail("CodexPolicyPreexistingComponentTestDrift", f"component test {name} is disabled")
        if _component_stable_view(baseline) != _component_stable_view(final):
            fail("CodexPolicyPreexistingComponentTestDrift", f"component test {name} properties drifted")
    additions = frozenset(final_components) - frozenset(baseline_components)
    if additions not in MCP_REVERSE_COMPONENT_TEST_STAGES:
        fail(
            "CodexPolicyPreexistingComponentTestDrift",
            "configured component/codex additions are not an exact reviewed A1.4b stage",
        )
    if GENERATED_ARTIFACTS_TEST not in baseline_components or len(
        final_index.get(GENERATED_ARTIFACTS_TEST, [])
    ) != 1:
        fail(
            "CodexPolicyPreexistingComponentTestMissing",
            f"{GENERATED_ARTIFACTS_TEST} is absent",
        )
    security_rows = final_index.get(SECURITY_TEST, [])
    if security_rows and GENERATED_ARTIFACTS_TEST not in security_rows[0].get("dependencies", []):
        fail(
            "CodexPolicyExistingSecurityGuardDrift",
            f"{SECURITY_TEST} lost {GENERATED_ARTIFACTS_TEST} dependency",
        )
    for name, baseline_rows in baseline_index.items():
        if name in baseline_components:
            continue
        final_rows = final_index.get(name, [])
        if len(final_rows) != len(baseline_rows):
            fail("CodexPolicyPreexistingCTestRemoval", f"pre-existing test {name} count changed")
        if name != SECURITY_TEST:
            for baseline, final in zip(baseline_rows, final_rows, strict=True):
                if (
                    _preservation_view(baseline) != _preservation_view(final)
                    or baseline.get("properties") != final.get("properties")
                ):
                    fail(
                        "CodexPolicyPreexistingCTestRemoval",
                        f"pre-existing non-component test {name} properties drifted",
                    )
    if int(fixture.final_model.get("test_count", 0)) < int(fixture.baseline_model.get("test_count", 0)):
        fail("CodexPolicyPreexistingCTestRemoval", "final configured CTest count is below baseline")


def _workflow_jobs(text: str) -> dict[str, str]:
    jobs_match = re.search(r"(?m)^jobs:\s*$", text)
    if jobs_match is None:
        return {}
    tail = text[jobs_match.end() :]
    matches = list(re.finditer(r"(?m)^  ([A-Za-z0-9_-]+):\s*$", tail))
    result: dict[str, str] = {}
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(tail)
        result[match.group(1)] = tail[match.end() : end]
    return result


def _focused_filters(job: str) -> list[str]:
    lines = job.splitlines()
    run_blocks: list[str] = []
    index = 0
    while index < len(lines):
        match = re.match(
            r"^(\s*)(?:-\s+)?run:\s*(.*?)\s*$",
            lines[index],
        )
        if match is None or lines[index].lstrip().startswith("#"):
            index += 1
            continue
        indentation = len(match.group(1))
        value = match.group(2)
        if value in ("|", ">", "|-", ">-", "|+", ">+"):
            block: list[str] = []
            index += 1
            while index < len(lines):
                line = lines[index]
                if line.strip() and len(line) - len(line.lstrip()) <= indentation:
                    break
                block.append(line)
                index += 1
            run_blocks.append("\n".join(block))
            continue
        run_blocks.append(value)
        index += 1

    commands: list[str] = []
    for block in run_blocks:
        pending = ""
        for raw_line in block.splitlines():
            stripped = raw_line.lstrip()
            if stripped.startswith("#"):
                continue
            line = raw_line
            quoted: str | None = None
            escaped = False
            active: list[str] = []
            for character in line:
                if character in ("'", '"') and not escaped:
                    quoted = (
                        None
                        if quoted == character
                        else character
                        if quoted is None
                        else quoted
                    )
                if character == "#" and quoted is None:
                    break
                active.append(character)
                escaped = character == "\\" and not escaped
                if character != "\\":
                    escaped = False
            line = "".join(active).strip()
            if not pending and not re.match(r"^ctest(?:\s|$)", line):
                continue
            pending = f"{pending} {line}".strip()
            if pending.endswith("\\"):
                pending = pending[:-1].rstrip()
                continue
            commands.append(pending)
            pending = ""
    result: list[str] = []
    for command in commands:
        try:
            tokens = shlex.split(command)
        except ValueError:
            continue
        if not tokens or tokens[0] != "ctest":
            continue
        for index, token in enumerate(tokens):
            if token == "-L" and index + 1 < len(tokens):
                result.append(tokens[index + 1])
            elif token.startswith("-L") and len(token) > 2:
                result.append(token[2:])
    return result


def _validate_ci(fixture: VerificationFixture) -> None:
    jobs = _workflow_jobs(fixture.workflow_text)
    if set(jobs) != {"gcc-debug", "gcc-15-debug"}:
        fail("CodexPolicyCIFilterMismatch", f"CI jobs changed: {sorted(jobs)}")
    for name in ("gcc-debug", "gcc-15-debug"):
        job = jobs[name]
        required_cutover_fragments = (
            "git clone https://github.com/SNodeC/snode.c.git ../snodec",
            f"git -C ../snodec checkout {SNODEC_DEPENDENCY_COMMIT}",
            "git -C ../snodec worktree add --detach ../snodec-provenance",
            SNODEC_COMMIT,
            SNODEC_DEPENDENCY_TREE,
            SNODEC_TREE,
            "test ! -d ../snodec/src/ai",
            "cmake -S ../snodec -B ../snodec-build",
            '-DCMAKE_PREFIX_PATH="$PWD/../snodec-stage"',
            (
                '-DAISUITE_TEST_SNODEC_SOURCE_REPOSITORY='
                '"$PWD/../snodec-provenance"'
            ),
        )
        missing = [
            fragment
            for fragment in required_cutover_fragments
            if fragment not in job
        ]
        if missing:
            fail(
                "CodexPolicyCutoverReadinessMismatch",
                f"{name} omits cleaned/provenance separation: {missing[0]}",
            )
        for forbidden in (
            "cmake -S ../snodec-provenance",
            "cmake --build ../snodec-provenance",
            "-DCMAKE_PREFIX_PATH=\"$PWD/../snodec-provenance",
        ):
            if forbidden in job:
                fail(
                    "CodexPolicyCutoverReadinessMismatch",
                    f"{name} uses extraction provenance as a build dependency",
                )
        filters = _focused_filters(jobs[name])
        if EXPECTED_CI_FILTER not in filters:
            fail(
                "CodexPolicyCIFilterMismatch",
                f"{name} does not execute focused filter {EXPECTED_CI_FILTER}",
            )
        for test_name in FUNCTIONAL_TESTS:
            rows = _tests_by_name(fixture.final_model).get(test_name, [])
            if len(rows) != 1 or not _selected_by_filter(rows[0].get("labels", []), EXPECTED_CI_FILTER):
                fail("CodexPolicyTestExcludedFromFocusedCI", f"{test_name} is not selected in {name}")


def _manifest_rows(manifest: Mapping[str, Any], bucket: str) -> dict[str, dict[str, Any]]:
    rows = manifest.get(bucket)
    if not isinstance(rows, list):
        fail("CodexPolicyManifestClassificationMismatch", f"manifest {bucket} is malformed")
    result: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("path"), str):
            fail("CodexPolicyManifestClassificationMismatch", f"malformed {bucket} row")
        path = str(row["path"])
        if path in result:
            fail("CodexPolicyManifestClassificationMismatch", f"duplicate manifest path {path}")
        result[path] = row
    return result


def _validate_manifest(fixture: VerificationFixture) -> None:
    expected_section = fixture.ownership.get("manifest_classifications")
    if not isinstance(expected_section, dict):
        fail("CodexPolicyManifestClassificationMismatch", "ownership manifest mapping is absent")
    if expected_section.get("standalone_files") != list(STANDALONE_POLICY_PATHS):
        fail("CodexPolicyManifestClassificationMismatch", "standalone policy path authority changed")
    retained = expected_section.get("retained_imported_files")
    if retained != [SECURITY_IMPORTED_PROVENANCE]:
        fail("CodexPolicyManifestClassificationMismatch", "security imported provenance changed")
    filtered = expected_section.get("filtered_history")
    if filtered != {
        "selected_paths": list(EXPECTED_SELECTED_PATHS),
        "new_selected_paths_added": [],
    }:
        fail("CodexPolicyManifestClassificationMismatch", "filtered-history ownership changed")
    if fixture.filter_map.get("selected_paths") != list(EXPECTED_SELECTED_PATHS):
        fail("CodexPolicyManifestClassificationMismatch", "filter-map selected paths changed")
    manifest_filtered = fixture.manifest.get("filtered_history", {})
    if manifest_filtered.get("selected_paths") != list(EXPECTED_SELECTED_PATHS):
        fail("CodexPolicyManifestClassificationMismatch", "manifest selected paths changed")
    imported = _manifest_rows(fixture.manifest, "imported_files")
    standalone = _manifest_rows(fixture.manifest, "standalone_files")
    for path in STANDALONE_POLICY_PATHS:
        if path not in standalone or path in imported:
            fail("CodexPolicyManifestClassificationMismatch", f"{path} is not standalone")
    security_path = SECURITY_IMPORTED_PROVENANCE["path"]
    if imported.get(security_path) != SECURITY_IMPORTED_PROVENANCE or security_path in standalone:
        fail(
            "CodexPolicyManifestClassificationMismatch",
            f"{security_path} did not retain imported provenance",
        )


def _validate_source_package(fixture: VerificationFixture) -> None:
    source = fixture.ownership.get("source_package")
    expected = {
        "required_paths": list(SOURCE_PACKAGE_REQUIRED_PATHS),
        "package_safe_command": (
            "python3 -B tools/extraction/verify_codex_policy_ownership.py "
            "check-package --repo-root <package-root>"
        ),
        "requires_git": False,
        "requires_network": False,
        "requires_external_snodec_checkout": False,
    }
    if source != expected:
        fail("CodexPolicySourcePackageMismatch", "source-package requirements changed")
    paths = fixture.source_package_paths
    if paths is None:
        return
    missing = sorted(set(SOURCE_PACKAGE_REQUIRED_PATHS) - paths)
    if missing:
        fail("CodexPolicySourcePackageMismatch", f"source package missing {missing[0]}")


def _validate_binary_package(fixture: VerificationFixture) -> None:
    binary = fixture.ownership.get("binary_package")
    if binary != {
        "forbidden_patterns": list(BINARY_PACKAGE_FORBIDDEN_PATTERNS),
        "policy_internals_excluded": True,
    }:
        fail("CodexPolicyBinaryPackageLeak", "binary-package exclusion authority changed")
    if fixture.binary_package_paths is None:
        return
    for path in sorted(fixture.binary_package_paths):
        for pattern in BINARY_PACKAGE_FORBIDDEN_PATTERNS:
            if pattern in path:
                fail("CodexPolicyBinaryPackageLeak", f"binary package contains {path}")


def _validate_recorded_policy_details(fixture: VerificationFixture) -> None:
    ownership = fixture.ownership
    expected = build_ownership_document(fixture.root, fixture.baseline_model, fixture.final_model)
    for key, code in (
        ("public_header_policy", "CodexPolicyPublicHeaderInventoryMismatch"),
        ("logging_api_surface_policy", "CodexPolicyLoggingApiSurfaceMismatch"),
        ("semantic_logger_policy", "CodexPolicySemanticLoggerAuthorityMismatch"),
        ("adapted_support_files", "CodexPolicyOwnershipMappingMismatch"),
        ("functional_policy_tests", "CodexPolicyOwnershipMappingMismatch"),
        ("verification_tests", "CodexPolicyCutoverReadinessMismatch"),
        ("focused_ci", "CodexPolicyCIFilterMismatch"),
        ("preserved_preexisting_test_suites", "CodexPolicyPreexistingComponentTestDrift"),
        ("ctest_evidence", "CodexPolicyPreexistingCTestRemoval"),
        ("mutation_coverage", "CodexPolicyCutoverReadinessMismatch"),
    ):
        if ownership.get(key) != expected[key]:
            fail(code, f"recorded {key} evidence changed")
    readiness = ownership.get("cutover_readiness")
    if readiness != expected["cutover_readiness"]:
        fail("CodexPolicyCutoverReadinessMismatch", "cutover readiness is not complete")


def verify_fixture(fixture: VerificationFixture) -> None:
    """Validate one fully supplied authority fixture, raising the first code."""

    _validate_source_authority(fixture)
    _validate_ownership_mapping(fixture)
    _validate_hierarchy(fixture)
    _validate_functional_ctest(fixture)
    _validate_verification_ctest(fixture)
    _validate_mutation_harness(fixture)
    _validate_security_drift(fixture)
    _validate_preexisting_ctest(fixture)
    _validate_ci(fixture)
    _validate_manifest(fixture)
    _validate_source_package(fixture)
    _validate_binary_package(fixture)
    _validate_recorded_policy_details(fixture)


def diagnostic_codes(fixture: VerificationFixture) -> list[str]:
    """Return zero or one stable diagnostic code for a planted fixture."""

    try:
        verify_fixture(fixture)
    except PolicyVerificationError as error:
        return [error.code]
    return []


def _validate_model_shape(model: Mapping[str, Any], name: str) -> None:
    if set(model) != {
        "format_version",
        "ctest_json_version",
        "path_placeholders",
        "test_count",
        "tests",
    }:
        fail("CodexPolicyOwnershipMappingMismatch", f"{name} CTest evidence schema changed")
    if (
        model.get("format_version") != 1
        or model.get("ctest_json_version") != {"major": 1, "minor": 0}
        or model.get("path_placeholders") != EXPECTED_PATH_PLACEHOLDERS
        or not isinstance(model.get("tests"), list)
    ):
        fail("CodexPolicyOwnershipMappingMismatch", f"{name} CTest evidence is malformed")
    if model.get("test_count") != len(model["tests"]):
        fail("CodexPolicyOwnershipMappingMismatch", f"{name} CTest count is inconsistent")
    names = [str(test.get("name", "")) for test in model["tests"]]
    if names != sorted(names) or len(names) != len(set(names)):
        fail("CodexPolicyOwnershipMappingMismatch", f"{name} CTest tests are not sorted")
    expected_test_keys = {
        "name",
        "command",
        "labels",
        "timeout",
        "disabled",
        "dependencies",
        "properties",
        "registration_backtrace",
        "registration_files",
    }

    def strings(value: Any) -> Iterable[str]:
        if isinstance(value, str):
            yield value
        elif isinstance(value, list):
            for item in value:
                yield from strings(item)
        elif isinstance(value, dict):
            for item in value.values():
                yield from strings(item)

    for test in model["tests"]:
        if not isinstance(test, dict) or set(test) != expected_test_keys:
            fail("CodexPolicyOwnershipMappingMismatch", f"{name} CTest test schema changed")
        if (
            not isinstance(test["name"], str)
            or not test["name"]
            or not isinstance(test["command"], list)
            or not test["command"]
            or not all(isinstance(item, str) for item in test["command"])
            or not isinstance(test["labels"], list)
            or not all(isinstance(item, str) for item in test["labels"])
            or test["labels"] != sorted(test["labels"])
            or not isinstance(test["disabled"], bool)
            or not isinstance(test["dependencies"], list)
            or not all(isinstance(item, str) for item in test["dependencies"])
            or not isinstance(test["properties"], dict)
            or not isinstance(test["registration_backtrace"], list)
            or not test["registration_backtrace"]
            or not isinstance(test["registration_files"], list)
            or not test["registration_files"]
            or test["registration_files"] != sorted(set(test["registration_files"]))
        ):
            fail("CodexPolicyOwnershipMappingMismatch", f"{name} CTest test fields are malformed")
        timeout = test["timeout"]
        if (
            not isinstance(timeout, (int, float))
            or not math.isfinite(float(timeout))
            or float(timeout) <= 0
        ):
            fail("CodexPolicyOwnershipMappingMismatch", f"{name} CTest timeout is malformed")
        properties = test["properties"]
        if (
            not all(isinstance(key, str) for key in properties)
            or list(properties.get("LABELS", [])) != test["labels"]
            or properties.get("TIMEOUT") != timeout
            or list(properties.get("DEPENDS", [])) != test["dependencies"]
            or bool(properties.get("DISABLED", False)) != test["disabled"]
        ):
            fail("CodexPolicyOwnershipMappingMismatch", f"{name} CTest properties disagree")
        for row in test["registration_backtrace"]:
            if (
                not isinstance(row, dict)
                or not row
                or not set(row) <= {"file", "command", "line"}
                or ("file" in row and not isinstance(row["file"], str))
                or ("command" in row and not isinstance(row["command"], str))
                or ("line" in row and not isinstance(row["line"], int))
            ):
                fail(
                    "CodexPolicyOwnershipMappingMismatch",
                    f"{name} CTest registration backtrace is malformed",
                )
        for value in strings(test):
            if value.startswith("/") or "=/" in value:
                fail(
                    "CodexPolicyOwnershipMappingMismatch",
                    f"{name} CTest evidence contains an unnormalized host path",
                )


def _verify_live_commands(raw: Mapping[str, Any], root: Path, build_dir: Path) -> None:
    functional = set(FUNCTIONAL_TESTS)
    for test in raw.get("tests", []):
        if test.get("name") not in functional:
            continue
        command = [str(item) for item in test.get("command", [])]
        if not command:
            fail("CodexPolicyTestNotRegistered", f"{test.get('name')} has no command")
        candidates: list[Path] = []
        first = Path(command[0])
        if first.is_absolute():
            candidates.append(first)
        if first.name.startswith("python") and len(command) >= 2:
            candidates.append(Path(command[1]))
        if first.name == "cmake" and "-P" in command:
            index = command.index("-P")
            if index + 1 < len(command):
                candidates.append(Path(command[index + 1]))
        for candidate in candidates:
            if candidate.is_absolute() and not candidate.exists():
                fail(
                    "CodexPolicyTestNotRegistered",
                    f"{test.get('name')} command target does not exist: {candidate}",
                )


def generate_evidence(
    root: Path,
    *,
    output: Path,
    baseline_model_path: Path,
    final_model_path: Path,
    baseline_ctest: Path | None,
    final_ctest: Path | None,
    build_dir: Path | None,
    snodec_root: Path | None,
) -> dict[str, Any]:
    if baseline_ctest is not None:
        raw = load_json(
            baseline_ctest.resolve(),
            canonical=False,
            code="CodexPolicyOwnershipMappingMismatch",
        )
        baseline_model = canonicalize_ctest(raw, root)
        baseline_model_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_model_path.write_text(canonical_json(baseline_model), encoding="utf-8")
    else:
        baseline_model = load_json(
            baseline_model_path,
            canonical=True,
            code="CodexPolicyOwnershipMappingMismatch",
        )
    if final_ctest is not None:
        raw = load_json(
            final_ctest.resolve(),
            canonical=False,
            code="CodexPolicyOwnershipMappingMismatch",
        )
        final_model = canonicalize_ctest(raw, root)
        final_model_path.parent.mkdir(parents=True, exist_ok=True)
        final_model_path.write_text(canonical_json(final_model), encoding="utf-8")
    elif build_dir is not None:
        _raw, final_model = configured_ctest_model(root, build_dir.resolve())
        final_model_path.parent.mkdir(parents=True, exist_ok=True)
        final_model_path.write_text(canonical_json(final_model), encoding="utf-8")
    else:
        final_model = load_json(
            final_model_path,
            canonical=True,
            code="CodexPolicyOwnershipMappingMismatch",
        )
    _validate_model_shape(baseline_model, "baseline")
    _validate_model_shape(final_model, "final")
    document = build_ownership_document(root, baseline_model, final_model)
    first_models = (
        canonical_json(baseline_model),
        canonical_json(final_model),
    )
    second_models = (
        canonical_json(copy.deepcopy(baseline_model)),
        canonical_json(copy.deepcopy(final_model)),
    )
    second_document = build_ownership_document(
        root,
        copy.deepcopy(baseline_model),
        copy.deepcopy(final_model),
    )
    if first_models != second_models or canonical_json(document) != canonical_json(
        second_document
    ):
        fail(
            "CodexPolicySecondPassNondeterminism",
            "second ownership evidence construction differs from the first",
        )
    if snodec_root is None:
        fail(
            "CodexPolicySourceAuthorityMismatch",
            "generation requires --snodec-root pinned source authority",
        )
    fixture = VerificationFixture(
        root=root,
        ownership=document,
        baseline_model=baseline_model,
        final_model=final_model,
        manifest={},
        workflow_text=_read_text(root / WORKFLOW_PATH, "CodexPolicyCIFilterMismatch"),
        root_tests_cmake=_read_text(
            root / "tests/CMakeLists.txt",
            "CodexPolicyHierarchyRegistrationMismatch",
        ),
        policy_cmake=_read_text(
            root / "tests/policy/CMakeLists.txt",
            "CodexPolicyHierarchyRegistrationMismatch",
        ),
        security_cmake=_read_text(
            root / "tests/policy/security/CMakeLists.txt",
            "CodexPolicyHierarchyRegistrationMismatch",
        ),
        filter_map=load_json(
            root / FILTER_MAP_PATH,
            code="CodexPolicyManifestClassificationMismatch",
        ),
        snodec_root=snodec_root,
        verify_owner_files=True,
    )
    _validate_source_authority(fixture)
    _validate_ownership_mapping(fixture)
    _validate_hierarchy(fixture)
    _validate_functional_ctest(fixture)
    _validate_verification_ctest(fixture)
    _validate_mutation_harness(fixture)
    _validate_security_drift(fixture)
    _validate_preexisting_ctest(fixture)
    _validate_ci(fixture)
    _validate_source_package(fixture)
    _validate_binary_package(fixture)
    _validate_recorded_policy_details(fixture)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(canonical_json(document), encoding="utf-8")
    return document


def check_repository(
    root: Path,
    *,
    ownership_path: Path,
    baseline_model_path: Path,
    final_model_path: Path,
    manifest_path: Path,
    workflow_path: Path,
    build_dir: Path | None,
    source_package_model_path: Path | None,
    binary_package_model_path: Path | None,
    snodec_root: Path | None,
) -> dict[str, Any]:
    if build_dir is None:
        fail(
            "CodexPolicyCutoverReadinessMismatch",
            "normal check requires --build-dir and a live configured CTest model",
        )
    fixture = load_verification_fixture(
        root,
        ownership_path=ownership_path,
        baseline_model_path=baseline_model_path,
        final_model_path=final_model_path,
        manifest_path=manifest_path,
        workflow_path=workflow_path,
        source_package_model_path=source_package_model_path,
        binary_package_model_path=binary_package_model_path,
        snodec_root=snodec_root,
        canonical_documents=True,
    )
    _validate_model_shape(fixture.baseline_model, "baseline")
    _validate_model_shape(fixture.final_model, "final")
    raw, live_model = configured_ctest_model(root, build_dir.resolve())
    _verify_live_commands(raw, root, build_dir.resolve())
    _validate_live_security_working_directory(live_model)
    if live_model != fixture.final_model:
        fail(
            "CodexPolicyOwnershipMappingMismatch",
            "live configured CTest model differs from final canonical evidence",
        )
    verify_fixture(fixture)
    return fixture.ownership


def check_package(root: Path, ownership_path: Path) -> dict[str, Any]:
    """Verify ownership using only packaged filesystem authorities."""

    fixture = load_verification_fixture(
        root,
        ownership_path=ownership_path,
        canonical_documents=True,
        verify_owner_files=True,
    )
    fixture.source_package_paths = package_files(root)
    fixture.snodec_root = None
    _validate_model_shape(fixture.baseline_model, "baseline")
    _validate_model_shape(fixture.final_model, "final")
    # Diagnose a physically missing packaged owner at the package boundary
    # before repository-style owner-path checks can mask it.
    _validate_source_package(fixture)
    verify_fixture(fixture)
    binary_guard = _read_text(
        root / "tests/AISuiteBinaryPackageTest.cmake",
        "CodexPolicyBinaryPackageLeak",
    )
    for pattern in BINARY_PACKAGE_FORBIDDEN_PATTERNS:
        if pattern not in binary_guard:
            fail(
                "CodexPolicyBinaryPackageLeak",
                f"packaged binary-package guard omits {pattern}",
            )
    return fixture.ownership


def _path(value: Path | None, root: Path, default: Path) -> Path:
    if value is None:
        return (root / default).resolve()
    return value.resolve()


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument(
        "command",
        choices=("generate", "check", "check-package"),
        nargs="?",
        default="check",
    )
    result.add_argument("--repo-root", type=Path, required=True)
    result.add_argument("--output", type=Path)
    result.add_argument("--ownership", type=Path)
    result.add_argument("--baseline-model", type=Path)
    result.add_argument("--final-model", type=Path)
    result.add_argument("--baseline-ctest", type=Path)
    result.add_argument("--final-ctest", type=Path)
    result.add_argument("--build-dir", type=Path)
    result.add_argument("--manifest", type=Path)
    result.add_argument("--workflow", type=Path)
    result.add_argument("--source-package-model", type=Path)
    result.add_argument("--binary-package-model", type=Path)
    result.add_argument("--snodec-root", type=Path)
    return result


def main() -> int:
    options = parser().parse_args()
    root = options.repo_root.resolve()
    ownership_path = _path(
        options.output if options.command == "generate" else options.ownership,
        root,
        OWNERSHIP_PATH,
    )
    baseline_model_path = _path(options.baseline_model, root, BASELINE_CTEST_PATH)
    final_model_path = _path(options.final_model, root, FINAL_CTEST_PATH)
    manifest_path = _path(options.manifest, root, MANIFEST_PATH)
    workflow_path = _path(options.workflow, root, WORKFLOW_PATH)
    snodec_root = (
        options.snodec_root.resolve()
        if options.snodec_root is not None
        else (
            Path(os.environ["AISUITE_TEST_SNODEC_SOURCE_REPOSITORY"]).resolve()
            if os.environ.get("AISUITE_TEST_SNODEC_SOURCE_REPOSITORY")
            else None
        )
    )
    try:
        if options.command == "generate":
            document = generate_evidence(
                root,
                output=ownership_path,
                baseline_model_path=baseline_model_path,
                final_model_path=final_model_path,
                baseline_ctest=options.baseline_ctest,
                final_ctest=options.final_ctest,
                build_dir=options.build_dir,
                snodec_root=snodec_root,
            )
            print(
                "AISuite Codex policy ownership generated: "
                f"responsibilities={len(document['transferred_responsibilities'])}, "
                f"functional_tests={len(document['functional_policy_tests'])}"
            )
        elif options.command == "check":
            document = check_repository(
                root,
                ownership_path=ownership_path,
                baseline_model_path=baseline_model_path,
                final_model_path=final_model_path,
                manifest_path=manifest_path,
                workflow_path=workflow_path,
                build_dir=options.build_dir,
                source_package_model_path=options.source_package_model,
                binary_package_model_path=options.binary_package_model,
                snodec_root=snodec_root,
            )
            print(
                "AISuite Codex policy ownership verified: "
                f"responsibilities={len(document['transferred_responsibilities'])}, "
                "cutover_ready=true"
            )
        else:
            document = check_package(root, ownership_path)
            print(
                "Packaged AISuite Codex policy ownership verified: "
                f"responsibilities={len(document['transferred_responsibilities'])}, "
                "cutover_ready=true"
            )
    except PolicyVerificationError as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
