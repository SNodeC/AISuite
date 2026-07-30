#!/usr/bin/env python3
"""Generate and verify the Codex A1.4 user-integrations closure report.

The production ``ProtocolSurfaceRegistry`` remains the only implementation
status authority.  This tool projects deterministic review evidence from that
registry, the frozen PR-A audit, the public variants, generated descriptors,
fixtures, and installed-package guards.  The report is not a second registry.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import stat
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

sys.dont_write_bytecode = True

import app_server_a1_4_user_integrations as audit
import app_server_surface as surface


FORMAT_VERSION = 1
FINAL_GLOBAL_STATUS = {
    "Complete": 313,
    "Partial": 4,
    "NotImplemented": 22,
    "NotApplicable": 48,
    "Total": 387,
}
FINAL_NATIVE_STATUS = {
    "Complete": 33,
    "Partial": 1,
    "NotImplemented": 22,
    "Total": 56,
}
FROZEN_REPORT_SHA256 = (
    "07a7e1c1137ae24c205904c364a4b322b7ec69a609f57503ae321639172c0b1f"
)
MCP_REVERSE_INSTALLED_CONSUMER = {
    "bytes": 37727,
    "path": "tests/installed/codex/CodexTypedConsumer.cpp",
    "sha256": (
        "33899dac1d117ae66acbc5a53a9847c26e80babbfce2e4bbca6225cefce790e3"
    ),
}
SUCCESSOR_PROMOTION_FIELDS = {
    "runtime_disposition",
    "runtime_target",
    "schema_completeness",
    "typed_schema_status",
    "typed_status",
}
MCP_REVERSE_PARTIAL_KEY = (
    "server_request",
    "ServerRequest",
    "method",
    "item/tool/requestUserInput",
)
MCP_REVERSE_STAGES = (
    {
        "commit": 2,
        "identities": (),
        "global": FINAL_GLOBAL_STATUS,
        "native": FINAL_NATIVE_STATUS,
        "residual_partial": {
            "error",
            "initialize",
            "initialized",
            "item/tool/requestUserInput",
        },
    },
    {
        "commit": 3,
        "identities": (
            (
                "client_request",
                "ClientRequest",
                "method",
                "mcpServer/oauth/login",
            ),
            (
                "client_request",
                "ClientRequest",
                "method",
                "mcpServer/resource/read",
            ),
            (
                "client_request",
                "ClientRequest",
                "method",
                "mcpServer/tool/call",
            ),
            (
                "client_request",
                "ClientRequest",
                "method",
                "mcpServerStatus/list",
            ),
            (
                "server_notification",
                "ServerNotification",
                "method",
                "mcpServer/oauthLogin/completed",
            ),
            (
                "server_notification",
                "ServerNotification",
                "method",
                "mcpServer/startupStatus/updated",
            ),
        ),
        "global": {
            "Complete": 319,
            "Partial": 4,
            "NotImplemented": 16,
            "NotApplicable": 48,
            "Total": 387,
        },
        "native": {
            "Complete": 39,
            "Partial": 1,
            "NotImplemented": 16,
            "Total": 56,
        },
        "residual_partial": {
            "error",
            "initialize",
            "initialized",
            "item/tool/requestUserInput",
        },
    },
    {
        "commit": 4,
        "identities": (
            (
                "server_request",
                "ServerRequest",
                "method",
                "attestation/generate",
            ),
            (
                "server_request",
                "ServerRequest",
                "method",
                "item/tool/call",
            ),
        ),
        "global": {
            "Complete": 321,
            "Partial": 4,
            "NotImplemented": 14,
            "NotApplicable": 48,
            "Total": 387,
        },
        "native": {
            "Complete": 41,
            "Partial": 1,
            "NotImplemented": 14,
            "Total": 56,
        },
        "residual_partial": {
            "error",
            "initialize",
            "initialized",
            "item/tool/requestUserInput",
        },
    },
    {
        "commit": 5,
        "identities": (
            MCP_REVERSE_PARTIAL_KEY,
            (
                "server_request",
                "ServerRequest",
                "method",
                "mcpServer/elicitation/request",
            ),
            (
                "tagged_union_discriminator",
                "McpServerElicitationRequestParams",
                "mode",
                "form",
            ),
            (
                "tagged_union_discriminator",
                "McpServerElicitationRequestParams",
                "mode",
                "openai/form",
            ),
            (
                "tagged_union_discriminator",
                "McpServerElicitationRequestParams",
                "mode",
                "url",
            ),
        ),
        "global": {
            "Complete": 326,
            "Partial": 3,
            "NotImplemented": 10,
            "NotApplicable": 48,
            "Total": 387,
        },
        "native": {
            "Complete": 46,
            "Partial": 0,
            "NotImplemented": 10,
            "Total": 56,
        },
        "residual_partial": {
            "error",
            "initialize",
            "initialized",
        },
    },
)
MCP_REVERSE_AUDIT_RELATIVE_PATH = (
    "tools/codex/app_server_a1_4_mcp_reverse.py"
)
RESIDUAL_PARTIAL_NAMES = (
    "error",
    "initialize",
    "initialized",
    "item/tool/requestUserInput",
)
REQUIRED_COMMIT_SUBJECTS = (
    "Freeze Codex A1.4 user-integration implementation batches",
    "Complete Codex apps, external agents, and feedback",
    "Complete Codex hooks, marketplace, and skills",
    "Complete Codex plugin operations without source unions",
    "Complete Codex plugin source and catalog operations",
    "Close and verify Codex A1.4 user integrations",
)
COMMIT_5_SUBJECT = REQUIRED_COMMIT_SUBJECTS[4]
FINAL_COMMIT_SUBJECT = REQUIRED_COMMIT_SUBJECTS[5]
PR_A_MERGE_SHA = "19de4f50be64e187761274f043091090609d27a3"
PR_A_MERGE_TREE = "c71a91e545d70d649446ca9d698d729c307a480a"
PR_A_MERGE_PARENTS = (
    audit.EXPECTED_BASE_SHA,
    "7c89ecdf5247ed58acc2b2e9901cb30cb48896bd",
)
PR_A_MERGE_SUBJECT = (
    "Merge pull request #2 from SNodeC/codex/a1-4-user-integrations"
)
POLICY_OWNERSHIP_COMMIT_SUBJECTS = (
    "Complete extracted Codex source-policy coverage",
    "Verify AISuite Codex policy ownership",
)
POLICY_OWNERSHIP_COMMIT_1_SHA = (
    "ab0c734143eddcd1d5b20d29ac7f61baa25711bf"
)
POLICY_OWNERSHIP_MERGE_SUBJECT = (
    "Merge pull request #3 from "
    "SNodeC/extraction/complete-codex-policy-ownership"
)
CLEANED_SNODEC_COMMIT = "77415c71a87fb7955e9a050bedaca02b65754324"
CLEANED_SNODEC_TREE = "2d39c334f12c308828936656c820447bfcc38d47"
PROTOCOL_REGISTRY_RELATIVE_PATH = (
    "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc"
)
POLICY_OWNERSHIP_EVIDENCE_RELATIVE_PATHS = (
    "docs/extraction/codex-policy-ownership.json",
    "docs/extraction/codex-policy-baseline-ctest.json",
    "docs/extraction/codex-policy-final-ctest.json",
)
GENERATION_PROOF_FILENAMES = (
    "a1-4-user-integrations-generation-pre.json",
    "a1-4-user-integrations-generation-pass-1.json",
    "a1-4-user-integrations-generation-pass-2.json",
)
GENERATION_PROOF_RELATIVE_PATHS = tuple(
    "tools/codex/app-server-evidence/0.144.6/" + name
    for name in GENERATION_PROOF_FILENAMES
)
EXTRACTION_PROOF_EXCLUSION_DIAGNOSTIC = (
    "UserIntegrationExtractionProofExclusionMismatch"
)
EXPECTED_FIXTURE_JSON_FILES = 7_834
PRIOR_GENERATION_ATTEMPTS = (
    {
        "result": "rejected-before-proof-publication",
        "diagnostic_code": "UserIntegrationSecondPassNondeterminism",
        "first_differing_path": "docs/extraction/source-manifest.json",
        "pass_1_sha256": (
            "83efc26a1538fa4bd6912d523a04dbd307e80187a412f1d4f78e840513327e84"
        ),
        "pass_2_sha256": (
            "7e24ad01a2c88218f5bc970462a06f1f64544eed203606048e5db66330ceea78"
        ),
        "resolution": (
            "Replace the self-referential pre-proof closure snapshot with "
            "a byte-stable pending marker; ABI replay was deterministic."
        ),
    },
    {
        "result": "superseded-after-proof-assertion-edit",
        "diagnostic_code": None,
        "first_differing_path": None,
        "pass_1_sha256": (
            "4cfb517b607a440fea61ff11db09a9421c47cff6ccc572a7070b9dadf90cc061"
        ),
        "pass_2_sha256": (
            "4cfb517b607a440fea61ff11db09a9421c47cff6ccc572a7070b9dadf90cc061"
        ),
        "resolution": (
            "Rerun the complete sequence after adding the explicit final "
            "proof-publication assertion because extraction hashes the tool."
        ),
    },
    {
        "result": "rejected-after-independent-cycle-audit",
        "diagnostic_code": "UserIntegrationSecondPassNondeterminism",
        "first_differing_path": (
            "tools/codex/app-server-evidence/0.144.6/"
            "a1-4-user-integrations-closure-report.json"
        ),
        "resolution": (
            "Use an acyclic split: the target corpus includes the static "
            "closure report and extraction manifest; exactly three proof "
            "metadata documents remain outside that target and are validated "
            "by the specialized live-corpus guard."
        ),
    },
    {
        "result": "rejected-after-post-publication-write-audit",
        "diagnostic_code": None,
        "first_differing_path": None,
        "pass_1_sha256": (
            "e202073266234334452759d49adfe214c64d737ac3b85edf360df2275b749b1c"
        ),
        "pass_2_sha256": (
            "e202073266234334452759d49adfe214c64d737ac3b85edf360df2275b749b1c"
        ),
        "resolution": (
            "The passes and final live corpus were byte-identical, but "
            "finalization rewrote byte-identical closure/extraction files "
            "after proof publication. Replace those operations with a "
            "check-only closure assertion and no extraction subprocess."
        ),
    },
    {
        "result": "superseded-after-source-package-boundary-failure",
        "diagnostic_code": "UserIntegrationPackageBoundaryMismatch",
        "first_differing_path": ".qtcreator/CMakeLists.txt.user",
        "pass_1_sha256": (
            "02fc4e69539b6da4aeb18a37f043de184213a8666c61c333dc0fbae38cc94844"
        ),
        "pass_2_sha256": (
            "02fc4e69539b6da4aeb18a37f043de184213a8666c61c333dc0fbae38cc94844"
        ),
        "resolution": (
            "The generation proof passed, but CPack admitted ignored local "
            "IDE metadata and Python bytecode. Use a build-local copied CPack "
            "configuration that overrides both CPACK_SOURCE_IGNORE_FILES and "
            "CPACK_IGNORE_FILES for .qtcreator, __pycache__, and .pyc/.pyo "
            "while retaining exact packaged-file validation."
        ),
    },
    {
        "result": "rejected-before-pass-1-snapshot",
        "diagnostic_code": "StartStateSourceMismatch",
        "first_differing_path": "CMakeLists.txt",
        "resolution": (
            "The frozen A1.4 SOVERSION authority forbids changing root "
            "CMakeLists.txt in closure. Move the additional local-artifact "
            "ignore list to the source-package test's CPack invocation."
        ),
    },
)


@dataclass(frozen=True)
class GenerationStep:
    """One authoritative, deterministic generated-artifact operation."""

    name: str
    generator: str
    arguments: tuple[str, ...]
    outputs: tuple[str, ...]


@dataclass(frozen=True, order=True)
class Diagnostic:
    """Stable, mutation-testable closure diagnostic."""

    code: str
    location: str
    message: str


class ClosureError(RuntimeError):
    """A PR-A closure invariant was violated."""

    def __init__(self, diagnostics: Sequence[Diagnostic]) -> None:
        self.diagnostics = tuple(diagnostics)
        self.codes = tuple(sorted({row.code for row in diagnostics}))
        super().__init__(
            "; ".join(
                f"{row.code} at {row.location}: {row.message}"
                for row in diagnostics
            )
        )


@dataclass(frozen=True)
class PolicyHistoryCommit:
    """One commit in the testable policy-ownership history model."""

    sha: str
    tree: str
    parents: tuple[str, ...]
    subject: str


@dataclass(frozen=True)
class PolicyHistoryModel:
    """History facts needed to bound the reviewed policy-ownership range."""

    head: str
    commits: Mapping[str, PolicyHistoryCommit]
    reachable_from_head: frozenset[str]
    policy_merge_candidates: tuple[str, ...]
    src_changes_by_commit: Mapping[str, tuple[str, ...]]
    registry_blobs: Mapping[str, bytes]
    worktree_src_changes: tuple[str, ...]


@dataclass(frozen=True)
class PolicyHistoryValidation:
    """Structurally validated boundary of the policy-ownership PR."""

    state: str
    policy_commit_2: str | None
    policy_merge: str | None
    bounded_policy_end: str


Key = tuple[str, str, str, str]


def _fail(code: str, location: str, message: str) -> None:
    raise ClosureError((Diagnostic(code, location, message),))


def _require(
    condition: bool, code: str, location: str, message: str
) -> None:
    if not condition:
        _fail(code, location, message)


def _load(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        _fail(
            "UserIntegrationPredecessorEvidenceDrift",
            str(path),
            f"unable to load JSON authority: {error}",
        )
    if not isinstance(value, dict):
        _fail(
            "UserIntegrationPredecessorEvidenceDrift",
            str(path),
            "expected an object-valued JSON authority",
        )
    return value


def _render(value: Mapping[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_json(value: Any) -> str:
    return _sha256_bytes(
        json.dumps(
            value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
        ).encode("utf-8")
    )


def _record(path: Path, repo_root: Path) -> dict[str, Any]:
    data = path.read_bytes()
    return {
        "path": path.resolve().relative_to(repo_root).as_posix(),
        "bytes": len(data),
        "sha256": _sha256_bytes(data),
    }


def _display_path(path: Path, repo_root: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(repo_root).as_posix()
    except ValueError:
        return resolved.as_posix()


def _run(repo_root: Path, *arguments: str) -> str:
    result = subprocess.run(
        arguments,
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout.strip()


def _git_blob(repo_root: Path, revision: str, relative: str) -> str:
    return _run(repo_root, "git", "show", f"{revision}:{relative}")


def _git_blob_bytes(
    repo_root: Path, revision: str, relative: str
) -> bytes:
    result = subprocess.run(
        ("git", "show", f"{revision}:{relative}"),
        cwd=repo_root,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout


def _key(row: Mapping[str, Any]) -> Key:
    return (
        str(row["category"]),
        str(row["domain"]),
        str(row["discriminator_field"]),
        str(row["name"]),
    )


def _key_object(key: Key) -> dict[str, str]:
    return {
        "category": key[0],
        "domain": key[1],
        "discriminator_field": key[2],
        "name": key[3],
    }


def _expected_keys() -> tuple[Key, ...]:
    return tuple(
        sorted(
            _key(row["protocol_surface_key"])
            for row in audit._identity_rows()
        )
    )


def _index(
    rows: Iterable[Mapping[str, Any]],
    *,
    code: str,
    location: str,
) -> dict[Key, dict[str, Any]]:
    result: dict[Key, dict[str, Any]] = {}
    for raw in rows:
        row = dict(raw)
        key = _key(row)
        _require(
            key not in result,
            code,
            location,
            f"duplicate registry identity: {key}",
        )
        result[key] = row
    return result


def _status(rows: Iterable[Mapping[str, Any]]) -> dict[str, int]:
    counts = Counter(str(row["typed_schema_status"]) for row in rows)
    return {
        "Complete": counts["Complete"],
        "Partial": counts["Partial"],
        "NotImplemented": counts["NotImplemented"],
        "NotApplicable": counts["NotApplicable"],
        "Total": sum(counts.values()),
    }


def _validate_mcp_reverse_successor(
    current_rows: Sequence[Mapping[str, Any]],
    base_rows: Sequence[Mapping[str, Any]],
) -> Mapping[str, Any]:
    """Validate one exact A1.4b registry stage over the frozen PR-A result."""

    current = _index(
        current_rows,
        code="UserIntegrationSuccessorMismatch",
        location="$.successor_registry",
    )
    base = _index(
        base_rows,
        code="UserIntegrationSuccessorMismatch",
        location="$.successor_registry.base",
    )
    _require(
        set(current) == set(base) and len(current) == 387,
        "UserIntegrationSuccessorMismatch",
        "$.successor_registry",
        "A1.4b changed the frozen registry denominator",
    )

    global_status = _status(current.values())
    matching = [
        stage
        for stage in MCP_REVERSE_STAGES
        if global_status == stage["global"]
    ]
    _require(
        len(matching) == 1,
        "UserIntegrationSuccessorMismatch",
        "$.successor_registry.counts",
        "registry status is not an exact A1.4b Commit 3, 4, or 5 stage",
    )
    stage = matching[0]
    cumulative: set[Key] = set()
    for candidate in MCP_REVERSE_STAGES:
        cumulative.update(candidate["identities"])
        if candidate is stage:
            break

    changed = {key for key in current if current[key] != base[key]}
    _require(
        changed == cumulative,
        "UserIntegrationSuccessorMismatch",
        "$.successor_registry.identities",
        "A1.4b changed an identity outside its exact cumulative batch",
    )
    runtime_targets: list[str] = []
    for key in sorted(cumulative):
        before = base[key]
        after = current[key]
        changed_fields = {
            field
            for field in set(before) | set(after)
            if before.get(field) != after.get(field)
        }
        expected_fields = (
            {"schema_completeness", "typed_schema_status"}
            if key == MCP_REVERSE_PARTIAL_KEY
            else SUCCESSOR_PROMOTION_FIELDS
        )
        completeness = after.get("schema_completeness")
        runtime_target = str(after.get("runtime_target", ""))
        _require(
            changed_fields == expected_fields
            and after.get("runtime_disposition") == "Typed"
            and after.get("typed_status") == "Implemented"
            and after.get("typed_schema_status") == "Complete"
            and runtime_target not in {"", "std::monostate{}"}
            and isinstance(completeness, Mapping)
            and bool(completeness)
            and all(value is True for value in completeness.values()),
            "UserIntegrationSuccessorMismatch",
            f"$.successor_registry.identities.{key[3]}",
            "A1.4b promotion differs from its exact typed Complete transition",
        )
        runtime_targets.append(runtime_target)
    _require(
        len(runtime_targets) == len(set(runtime_targets)),
        "UserIntegrationSuccessorMismatch",
        "$.successor_registry.runtime_targets",
        "A1.4b runtime targets are missing or duplicated",
    )

    native_status_full = _status(
        row for row in current.values() if row["a1_slice"] == "A1.4"
    )
    native_status = {
        key: native_status_full[key]
        for key in ("Complete", "Partial", "NotImplemented", "Total")
    }
    residual_partial = {
        key[3]
        for key, row in current.items()
        if row["typed_schema_status"] == "Partial"
    }
    _require(
        native_status == stage["native"]
        and residual_partial == stage["residual_partial"],
        "UserIntegrationSuccessorMismatch",
        "$.successor_registry.counts",
        "A1.4b native status or residual Partial identities changed",
    )
    return stage


def _has_mcp_reverse_successor_marker(repo_root: Path) -> bool:
    """Recognize the reviewed A1.4b audit boundary before registry promotion."""

    path = repo_root / MCP_REVERSE_AUDIT_RELATIVE_PATH
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8")
    return all(
        token in text
        for token in (
            'EXPECTED_BASE_SHA = "0c3a5838359eb283aca67840325ce6019345b462"',
            "CLIENT_REQUESTS = (",
            "SERVER_NOTIFICATIONS = (",
            "SERVER_REQUESTS = (",
            'ELICITATION_MODES = ("form", "openai/form", "url")',
        )
    )


def _variant(source: str, alias: str) -> list[str]:
    match = re.search(
        rf"using\s+{re.escape(alias)}\s*=\s*std::variant<(?P<body>.*?)>;",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        _fail(
            "UserIntegrationNotificationAppendIndexMismatch",
            f"$.notification_variants.{alias}",
            f"unable to locate public variant {alias}",
        )
    alternatives = [
        re.sub(r"\s+", "", part)
        for part in match.group("body").split(",")
        if part.strip()
    ]
    _require(
        bool(alternatives)
        and all(
            re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name)
            for name in alternatives
        ),
        "UserIntegrationNotificationAppendIndexMismatch",
        f"$.notification_variants.{alias}",
        f"{alias} is not a flat named-alternative variant",
    )
    return alternatives


def _parse_descriptor_file(
    path: Path, macro: str, argument_count: int
) -> list[list[str]]:
    rows: list[list[str]] = []
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        prefix = f"{macro}("
        if not line.startswith(prefix):
            continue
        _require(
            line.endswith(")"),
            "UserIntegrationDescriptorMismatch",
            f"{path}:{line_number}",
            "malformed descriptor macro",
        )
        arguments = surface.split_cpp_arguments(line[len(prefix) : -1])
        _require(
            len(arguments) == argument_count,
            "UserIntegrationDescriptorMismatch",
            f"{path}:{line_number}",
            "descriptor argument count changed",
        )
        rows.append(arguments)
    return rows


def _descriptor_evidence(
    arguments: argparse.Namespace,
    registry: Mapping[Key, Mapping[str, Any]],
) -> dict[str, Any]:
    operation_rows = _parse_descriptor_file(
        arguments.operation_descriptors,
        "CODEX_CLIENT_OPERATION_CODEC_DESCRIPTOR",
        10,
    )
    notification_rows = _parse_descriptor_file(
        arguments.notification_descriptors,
        "CODEX_SERVER_NOTIFICATION_CODEC_DESCRIPTOR",
        7,
    )
    union_rows = _parse_descriptor_file(
        arguments.union_descriptors,
        "CODEX_INTEGRATIONS_AND_LONG_TAIL_UNION_CODEC_DESCRIPTOR",
        7,
    )
    operations = {
        json.loads(row[3]): {
            "runtime_target": row[4],
            "parameter_type": json.loads(row[6]),
            "result_type": json.loads(row[7]),
            "result_kind": row[8].removeprefix("ResultContractKind::"),
            "result_decoder": row[9],
        }
        for row in operation_rows
    }
    notifications = {
        json.loads(row[3]): {
            "runtime_target": row[4],
            "payload_type": json.loads(row[5]),
        }
        for row in notification_rows
    }
    unions = [
        {
            "name": json.loads(row[3]),
            "runtime_target": row[4],
            "shape": row[5],
            "direction": row[6],
        }
        for row in union_rows
        if json.loads(row[1]) == "PluginSource"
    ]

    for method, (parameter, result, kind) in audit.REQUEST_CONTRACTS.items():
        descriptor = operations.get(method)
        key = ("client_request", "ClientRequest", "method", method)
        _require(
            descriptor is not None
            and descriptor["runtime_target"] == registry[key]["runtime_target"]
            and descriptor["parameter_type"] == parameter
            and descriptor["result_type"] == result
            and descriptor["result_kind"] == kind,
            "UserIntegrationDescriptorMismatch",
            f"$.descriptors.operations.{method}",
            "request descriptor is absent or differs from its registry contract",
        )
    for method in audit.NOTIFICATIONS:
        descriptor = notifications.get(method)
        key = ("server_notification", "ServerNotification", "method", method)
        _require(
            descriptor is not None
            and descriptor["runtime_target"] == registry[key]["runtime_target"],
            "UserIntegrationDescriptorMismatch",
            f"$.descriptors.notifications.{method}",
            "notification descriptor is absent or differs from its registry target",
        )
    _require(
        [row["name"] for row in unions] == list(audit.PLUGIN_SOURCE_ORDER)
        and all(
            row["shape"]
            == "ConversationUnionCodecShape::InternallyTaggedObject"
            and row["direction"]
            == "ConversationUnionCodecDirection::DecodeOnly"
            for row in unions
        ),
        "UserIntegrationDescriptorMismatch",
        "$.descriptors.plugin_source",
        "PluginSource descriptor order, shape, or direction changed",
    )
    return {
        "operations": [operations[name] | {"method": name} for name in sorted(audit.REQUEST_CONTRACTS)],
        "notifications": [
            notifications[name] | {"method": name}
            for name in audit.NOTIFICATIONS
        ],
        "plugin_source": unions,
        "sources": [
            _record(path, arguments.repo_root)
            for path in (
                arguments.operation_descriptors,
                arguments.notification_descriptors,
                arguments.union_descriptors,
            )
        ],
    }


def _public_api_evidence(arguments: argparse.Namespace) -> dict[str, Any]:
    client = arguments.client_header.read_text(encoding="utf-8")
    cmake = arguments.codex_cmake.read_text(encoding="utf-8")
    facades: list[dict[str, Any]] = []
    method_total = 0
    for facade, definition in audit.PUBLIC_API.items():
        header = arguments.repo_root / "src" / definition["header"]
        text = header.read_text(encoding="utf-8")
        methods = list(definition["methods"])
        method_total += len(methods)
        missing_methods = [
            method
            for method in methods
            if re.search(
                rf"\bSubmission\s+{re.escape(method)}\s*\(", text
            )
            is None
        ]
        accessor = str(definition["accessor"])
        _require(
            not missing_methods
            and re.search(
                rf"\b{re.escape(facade)}&\s+{re.escape(accessor)}\(\)\s+noexcept",
                client,
            )
            is not None
            and re.search(
                rf"\bconst\s+{re.escape(facade)}&\s+{re.escape(accessor)}\(\)\s+const\s+noexcept",
                client,
            )
            is not None,
            "UserIntegrationDescriptorMismatch",
            f"$.public_api.{facade}",
            f"facade accessor or methods are missing: {missing_methods}",
        )
        install_token = f"typed/{header.name}"
        _require(
            install_token in cmake,
            "UserIntegrationPackageBoundaryMismatch",
            f"$.package_boundary.public_headers.{header.name}",
            "public facade header is absent from the install set",
        )
        facades.append(
            {
                "facade": facade,
                "accessor": accessor,
                "header": definition["header"],
                "methods": methods,
                "installed": True,
                "source_sha256": _sha256_bytes(header.read_bytes()),
            }
        )
    _require(
        method_total == 23,
        "UserIntegrationDescriptorMismatch",
        "$.public_api.method_count",
        "public facade method denominator changed",
    )
    return {
        "facades": facades,
        "facade_count": 7,
        "method_count": method_total,
        "client_one_pointer_pimpl": (
            client.count("std::unique_ptr<Impl> impl;") == 1
        ),
    }


def _fixture_evidence(arguments: argparse.Namespace) -> dict[str, Any]:
    index = _load(arguments.fixture_index)
    expected = set(_expected_keys())
    assigned: set[Key] = set()
    stage_records: list[dict[str, Any]] = []
    for number in range(2, 6):
        name = f"a1_4_user_integrations_commit_{number}"
        stage = index.get(name)
        _require(
            isinstance(stage, Mapping),
            "UserIntegrationFixtureMismatch",
            f"$.fixtures.{name}",
            "fixture index lacks a PR-A stage",
        )
        stage_keys: list[Key] = []
        for field in (
            "assignment_derived_operation_keys",
            "assignment_derived_notification_keys",
            "assignment_derived_union_keys",
        ):
            value = stage.get(field, [])
            _require(
                isinstance(value, list),
                "UserIntegrationFixtureMismatch",
                f"$.fixtures.{name}.{field}",
                "fixture assignment keys are not an array",
            )
            stage_keys.extend(
                _key(row) for row in value if isinstance(row, Mapping)
            )
        assigned.update(stage_keys)
        indexed = stage.get("indexed_schema_coverage")
        _require(
            isinstance(indexed, Mapping)
            and len(indexed) == len(stage_keys),
            "UserIntegrationFixtureMismatch",
            f"$.fixtures.{name}.indexed_schema_coverage",
            "stage fixture/schema coverage is not bijective",
        )
        test_names = (
            f"CodexA14UserIntegrationsCommit{number}Test",
            f"CodexA14UserIntegrationsCommit{number}WireTest",
        )
        cmake = arguments.component_cmake.read_text(encoding="utf-8")
        for test_name in test_names:
            _require(
                (arguments.component_test_root / f"{test_name}.cpp").is_file()
                and test_name in cmake,
                "UserIntegrationFixtureMismatch",
                f"$.fixtures.primary_tests.{test_name}",
                "primary test source or registration is missing",
            )
        stage_records.append(
            {
                "commit": number,
                "identity_count": len(stage_keys),
                "identity_keys": [
                    _key_object(key) for key in sorted(stage_keys)
                ],
                "primary_tests": list(test_names),
                "stage_model_sha256": _sha256_json(stage),
            }
        )
    _require(
        assigned == expected and len(assigned) == 33,
        "UserIntegrationFixtureMismatch",
        "$.fixtures.identity_bijection",
        "fixture stages do not cover the exact 33 PR-A identities",
    )
    counts = index.get("counts")
    _require(
        isinstance(counts, Mapping)
        and int(counts.get("positive", 0)) > 0
        and int(counts.get("negative", 0)) > 0,
        "UserIntegrationFixtureMismatch",
        "$.fixtures.counts",
        "positive or negative fixture corpus is empty",
    )
    return {
        "required_schema_roots": 52,
        "identity_count": 33,
        "stages": stage_records,
        "corpus_counts": dict(counts),
        "index": _record(arguments.fixture_index, arguments.repo_root),
        "coverage": _record(arguments.fixture_coverage, arguments.repo_root),
        "schema_completeness": _record(
            arguments.schema_completeness, arguments.repo_root
        ),
    }


def _package_boundary(arguments: argparse.Namespace) -> dict[str, Any]:
    consumer = arguments.installed_consumer_source.read_text(encoding="utf-8")
    script = arguments.installed_consumer_test.read_text(encoding="utf-8")
    consumer_cmake = arguments.installed_consumer_cmake.read_text(
        encoding="utf-8"
    )
    source_package = arguments.source_package_test.read_text(
        encoding="utf-8"
    )
    headers = [
        f"ai/openai/codex/typed/{name}.h"
        for name in audit.PUBLIC_API
    ]
    accessors = [
        f".{definition['accessor']}()"
        for definition in audit.PUBLIC_API.values()
    ]
    mcp_reverse_successor = _has_mcp_reverse_successor_marker(
        arguments.repo_root.resolve()
    )
    consumer_tokens = [
        *headers,
        *accessors,
        (
            "std::variant_size_v<typed::CanonicalServerNotification> == "
            f"{59 if mcp_reverse_successor else 57}"
        ),
        (
            "std::variant_size_v<typed::Event> == "
            f"{61 if mcp_reverse_successor else 59}"
        ),
        "std::variant_size_v<typed::PluginSource> == 5",
        "typed::AppListUpdatedNotification",
        "typed::ExternalAgentConfigImportCompletedNotification",
        "typed::ExternalAgentConfigImportProgressNotification",
        "typed::HookCompletedNotification",
        "typed::HookStartedNotification",
        "typed::SkillsChangedNotification",
    ]
    missing_consumer = [token for token in consumer_tokens if token not in consumer]
    _require(
        not missing_consumer,
        "UserIntegrationInstalledConsumerNotInstalled",
        "$.package_boundary.installed_consumer",
        f"installed consumer lacks PR-A API evidence: {missing_consumer}",
    )
    strict_tokens = (
        CLEANED_SNODEC_COMMIT,
        CLEANED_SNODEC_TREE,
        audit.EXPECTED_SNODEC_SOURCE,
        audit.EXPECTED_SNODEC_TREE,
        "set(expected_snodec_dependency_commit",
        "set(expected_snodec_provenance_commit",
        "set(aisuite_install ",
        "set(consumer_source ",
        "set(consumer_build ",
        "git_executable",
        '"${CMAKE_COMMAND}" --install "${AISUITE_BUILD_DIR}"',
        "CMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE",
        "CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=TRUE",
        "CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE",
        "AISuite_DIR",
        "snodec_DIR",
        "CMakeCache.txt",
        "compile_commands.json",
        "--verbose",
        "readelf",
        '-d "${executable}"',
        "ELF dynamic metadata",
        "ldd",
        "LD_LIBRARY_PATH=${aisuite_install}/lib:${snodec_install}/lib",
        "--unset=CMAKE_PREFIX_PATH",
        "--unset=LDFLAGS",
        "--unset=LD_LIBRARY_PATH",
        "isolated_environment",
        "SNODEC_SOURCE_REPOSITORY",
        "${snodec_install}/include/snode.c",
        "${aisuite_install}/lib",
        "${snodec_install}/lib",
        "UserIntegrationInstalledConsumerNotInstalled",
        "UserIntegrationCrossRepoDependencyMismatch",
    )
    missing_script = [token for token in strict_tokens if token not in script]
    _require(
        not missing_script,
        "UserIntegrationInstalledConsumerNotInstalled",
        "$.package_boundary.genuine_installed_consumer",
        f"installed consumer does not prove isolated installed packages: {missing_script}",
    )
    forbidden_provenance_build_tokens = (
        "set(snodec_archive ",
        "set(snodec_source ",
        "set(snodec_build ",
        '"${git_executable}" -C "${SNODEC_SOURCE_REPOSITORY}" archive',
        '-S "${SNODEC_SOURCE_REPOSITORY}"',
        '--build "${SNODEC_SOURCE_REPOSITORY}"',
    )
    leaked_provenance_build = [
        token for token in forbidden_provenance_build_tokens
        if token in script
    ]
    _require(
        not leaked_provenance_build,
        "UserIntegrationCrossRepoDependencyMismatch",
        "$.package_boundary.extraction_provenance",
        (
            "installed consumer builds or exports historical SNode.C provenance: "
            f"{leaked_provenance_build}"
        ),
    )
    consumer_cmake_tokens = (
        "SNodeInstalledCoreConsumer.cpp",
        "AISuiteInstalledSNodeCoreConsumer",
        "snodec::core",
    )
    missing_consumer_cmake = [
        token for token in consumer_cmake_tokens
        if token not in consumer_cmake
    ]
    _require(
        not missing_consumer_cmake,
        "UserIntegrationInstalledConsumerNotInstalled",
        "$.package_boundary.snodec_public_header_consumer",
        (
            "installed consumer lacks the direct snodec::core probe: "
            f"{missing_consumer_cmake}"
        ),
    )
    source_package_tokens = (
        "GIT_CEILING_DIRECTORIES=",
        "verify_extraction.py",
        "verify_codex_policy_ownership.py",
        "app_server_a1_4_user_integrations_closure.py",
        "check-package",
        *(
            Path(path).name
            for path in POLICY_OWNERSHIP_EVIDENCE_RELATIVE_PATHS
        ),
        *GENERATION_PROOF_FILENAMES,
    )
    missing_source_package = [
        token for token in source_package_tokens
        if token not in source_package
    ]
    _require(
        not missing_source_package
        and source_package.count("check-package") >= 3,
        "UserIntegrationPackageBoundaryMismatch",
        "$.package_boundary.source_package",
        (
            "source package lacks no-history extraction/policy/closure proof: "
            f"{missing_source_package}"
        ),
    )
    root_cmake = (arguments.repo_root / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    _require(
        "find_package(snodec CONFIG REQUIRED COMPONENTS core net-un-stream-legacy)"
        in root_cmake
        and "add_subdirectory" not in "\n".join(
            line
            for line in root_cmake.splitlines()
            if "snode" in line.lower()
        ),
        "UserIntegrationCrossRepoDependencyMismatch",
        "$.package_boundary.snodec",
        "AISuite does not consume SNode.C solely through its installed package",
    )
    return {
        "public_headers": headers,
        "installed_consumer": _record(
            arguments.installed_consumer_source, arguments.repo_root
        ),
        "installed_consumer_guard": _record(
            arguments.installed_consumer_test, arguments.repo_root
        ),
        "installed_consumer_cmake": _record(
            arguments.installed_consumer_cmake, arguments.repo_root
        ),
        "source_package_guard": _record(
            arguments.source_package_test, arguments.repo_root
        ),
        "all_seven_facades_consumed": True,
        "final_variants_consumed": True,
        "direct_snodec_public_header_consumer": True,
        "package_checks_require_no_git_history": True,
        "package_registries_disabled": True,
        "cache_package_directories_verified": True,
        "compile_and_link_origins_verified": True,
        "dynamic_metadata_verified": True,
        "source_and_build_rpaths_rejected": True,
        "installed_snodec_only": True,
        "cross_repo_dependency": {
            "installed_config_mode_only": True,
            "source_relative_include_absent": True,
            "snodec_add_subdirectory_absent": True,
            "build_tree_resolution_rejected": True,
            "normal_dependency_commit": CLEANED_SNODEC_COMMIT,
            "normal_dependency_tree": CLEANED_SNODEC_TREE,
            "extraction_provenance_commit": audit.EXPECTED_SNODEC_SOURCE,
            "extraction_provenance_tree": audit.EXPECTED_SNODEC_TREE,
            "historical_checkout_read_only": True,
            "configured_dependency_install_reused": True,
        },
    }


def _api_abi_evidence(arguments: argparse.Namespace) -> dict[str, Any]:
    document = _load(arguments.abi_evidence)
    expected_authority = {
        "base_sha": audit.EXPECTED_BASE_SHA,
        "base_tree": audit.EXPECTED_BASE_TREE,
        "codex_version": audit.CODEX_VERSION,
        "implementation_sha": _history_evidence(arguments.repo_root)["sha"],
        "implementation_subject": COMMIT_5_SUBJECT,
        "implementation_tree": _history_evidence(arguments.repo_root)["tree"],
        "upstream_tag": audit.UPSTREAM_TAG,
    }
    _require(
        document.get("authority") == expected_authority,
        "UserIntegrationPredecessorEvidenceDrift",
        "$.api_abi.authority",
        "PR-A API/ABI authority changed",
    )
    layout = document.get("layout_probe")
    symbols = document.get("shared_library_symbols")
    conclusion = document.get("conclusion")
    _require(
        isinstance(layout, Mapping)
        and layout.get("variant_alternatives")
        == {
            "CanonicalServerNotification": 57,
            "Event": 59,
            "PluginSource": 5,
        }
        and layout.get("source")
        == _relative_command_path(
            arguments.abi_probe, arguments.repo_root
        )
        and layout.get("source_sha256")
        == _sha256_bytes(arguments.abi_probe.read_bytes()),
        "UserIntegrationNotificationAppendIndexMismatch",
        "$.api_abi.layout_probe",
        "ABI layout/variant probe is stale",
    )
    header_hashes = layout.get("header_sha256")
    _require(
        isinstance(header_hashes, Mapping)
        and all(
            (arguments.repo_root / str(path)).is_file()
            and digest
            == _sha256_bytes(
                (arguments.repo_root / str(path)).read_bytes()
            )
            for path, digest in header_hashes.items()
        ),
        "UserIntegrationPredecessorEvidenceDrift",
        "$.api_abi.public_headers",
        "captured public-header hashes are stale",
    )
    symbol_bytes = arguments.abi_symbols.read_bytes()
    _require(
        isinstance(symbols, Mapping)
        and symbols.get("symbol_list")
        == _relative_command_path(
            arguments.abi_symbols, arguments.repo_root
        )
        and symbols.get("symbol_list_sha256")
        == _sha256_bytes(symbol_bytes)
        and symbols.get("symbol_count")
        == len(symbol_bytes.decode("utf-8").splitlines()),
        "UserIntegrationPredecessorEvidenceDrift",
        "$.api_abi.strong_symbols",
        "strong dynamic-symbol manifest is stale",
    )
    _require(
        isinstance(conclusion, Mapping)
        and conclusion.get("soversion") == audit.EXPECTED_SOVERSION
        and conclusion.get("binary_compatible") is False
        and conclusion.get("installed_consumers_must_rebuild") is True,
        "UserIntegrationSOVERSIONDrift",
        "$.api_abi.conclusion",
        "API/ABI conclusion or SOVERSION policy changed",
    )
    return {
        "authority": expected_authority,
        "layout_probe": {
            "variant_alternatives": layout["variant_alternatives"],
            "stdout_lines": layout.get("stdout_lines"),
            "stdout_sha256": layout.get("stdout_sha256"),
            "public_header_count": len(header_hashes),
        },
        "strong_symbols": {
            "symbol_count": symbols["symbol_count"],
            "symbol_list_sha256": symbols["symbol_list_sha256"],
            "library_basename": symbols.get("library_basename"),
            "library_sha256": symbols.get("library_sha256"),
        },
        "conclusion": dict(conclusion),
        "generated_files": [
            _record(arguments.abi_evidence, arguments.repo_root),
            _record(arguments.abi_symbols, arguments.repo_root),
        ],
        "inputs": [
            _record(arguments.abi_tool, arguments.repo_root),
            _record(arguments.abi_probe, arguments.repo_root),
        ],
    }


def _history_evidence(repo_root: Path) -> dict[str, Any]:
    rows = _run(
        repo_root,
        "git",
        "log",
        "--reverse",
        "--format=%H%x09%s",
        f"{audit.EXPECTED_BASE_SHA}..HEAD",
    ).splitlines()
    matching = [
        row.split("\t", 1)[0]
        for row in rows
        if "\t" in row and row.split("\t", 1)[1] == COMMIT_5_SUBJECT
    ]
    _require(
        len(matching) == 1,
        "UserIntegrationPredecessorEvidenceDrift",
        "$.authority.implementation_head",
        "exactly one Commit-5 implementation boundary is required",
    )
    commit = matching[0]
    tree = _run(repo_root, "git", "show", "-s", "--format=%T", commit)
    ancestry = subprocess.run(
        ("git", "merge-base", "--is-ancestor", commit, "HEAD"),
        cwd=repo_root,
        check=False,
    ).returncode == 0
    _require(
        ancestry,
        "UserIntegrationPredecessorEvidenceDrift",
        "$.authority.implementation_head",
        "Commit-5 implementation boundary is not an ancestor of HEAD",
    )
    return {"sha": commit, "tree": tree, "subject": COMMIT_5_SUBJECT}


def _expected_history_policy() -> dict[str, Any]:
    return {
        "base_sha": audit.EXPECTED_BASE_SHA,
        "required_commit_count": 6,
        "required_subjects": list(REQUIRED_COMMIT_SUBJECTS),
        "commit_6_subject": FINAL_COMMIT_SUBJECT,
        "commit_6_production_implementation_forbidden": True,
        "commit_6_registry_promotion_forbidden": True,
        "merged_pr_a": {
            "sha": PR_A_MERGE_SHA,
            "tree": PR_A_MERGE_TREE,
            "parents": list(PR_A_MERGE_PARENTS),
            "subject": PR_A_MERGE_SUBJECT,
        },
        "policy_ownership_base_sha": PR_A_MERGE_SHA,
        "policy_ownership_base_tree": PR_A_MERGE_TREE,
        "policy_ownership_commit_1": {
            "sha": POLICY_OWNERSHIP_COMMIT_1_SHA,
            "subject": POLICY_OWNERSHIP_COMMIT_SUBJECTS[0],
            "parents": [PR_A_MERGE_SHA],
        },
        "required_policy_ownership_commit_count": 2,
        "required_policy_ownership_subjects": list(
            POLICY_OWNERSHIP_COMMIT_SUBJECTS
        ),
        "policy_ownership_commit_2_subject": (
            POLICY_OWNERSHIP_COMMIT_SUBJECTS[1]
        ),
        "accepted_policy_ownership_states": [
            "commit-1-construction",
            "unmerged-two-commit-branch",
            "merged-pr-3-or-later-descendant",
        ],
        "policy_ownership_merge": {
            "subject": POLICY_OWNERSHIP_MERGE_SUBJECT,
            "parent_count": 2,
            "first_parent": PR_A_MERGE_SHA,
            "second_parent": "structurally-validated-policy-commit-2",
            "tree_equals_policy_commit_2": True,
            "unique_in_head_ancestry": True,
        },
        "bounded_reviewed_policy_range": {
            "start_exclusive": PR_A_MERGE_SHA,
            "end_inclusive": "structurally-validated-policy-commit-2",
            "production_changes_forbidden": True,
            "registry_changes_forbidden": True,
        },
        "later_descendants_outside_reviewed_policy_range": True,
        "later_descendant_production_changes_may_be_reviewed_separately": True,
        "policy_ownership_production_implementation_forbidden": True,
        "policy_ownership_registry_promotion_forbidden": True,
        "final_sha_or_tree_embedded": False,
    }


def _policy_history_commit(
    model: PolicyHistoryModel,
    sha: str,
    *,
    location: str,
) -> PolicyHistoryCommit:
    commit = model.commits.get(sha)
    if commit is None or commit.sha != sha:
        _fail(
            "UserIntegrationPromotionStageMismatch",
            location,
            f"required history commit {sha} is missing or malformed",
        )
    return commit


def _require_policy_commit_content(
    model: PolicyHistoryModel,
    sha: str,
    *,
    label: str,
) -> None:
    _require(
        sha in model.src_changes_by_commit,
        "UserIntegrationPromotionStageMismatch",
        f"$.history_policy.{label}.src_evidence",
        "policy commit source-change evidence is missing",
    )
    changes = model.src_changes_by_commit[sha]
    _require(
        not changes,
        "UserIntegrationFalseComplete",
        f"$.history_policy.{label}.src",
        (
            "reviewed policy commit contains forbidden production changes: "
            f"{list(changes)}"
        ),
    )
    _require(
        PR_A_MERGE_SHA in model.registry_blobs
        and sha in model.registry_blobs,
        "UserIntegrationPromotionStageMismatch",
        f"$.history_policy.{label}.registry_evidence",
        "policy commit registry evidence is missing",
    )
    _require(
        model.registry_blobs[sha] == model.registry_blobs[PR_A_MERGE_SHA],
        "UserIntegrationFalseComplete",
        f"$.history_policy.{label}.registry",
        "reviewed policy commit changes the production protocol registry",
    )


def _validate_policy_commit_2(
    model: PolicyHistoryModel,
    sha: str,
) -> PolicyHistoryCommit:
    commit = _policy_history_commit(
        model,
        sha,
        location="$.history_policy.policy_ownership_commit_2",
    )
    _require(
        sha != POLICY_OWNERSHIP_COMMIT_1_SHA
        and commit.subject == POLICY_OWNERSHIP_COMMIT_SUBJECTS[1]
        and commit.parents == (POLICY_OWNERSHIP_COMMIT_1_SHA,),
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_ownership_commit_2",
        (
            "Commit 2 must be the direct single-parent child of unchanged "
            "Commit 1 with the exact reviewed subject"
        ),
    )
    _require(
        sha in model.reachable_from_head,
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_ownership_commit_2",
        "the structurally selected Commit 2 is not an ancestor of HEAD",
    )
    _require_policy_commit_content(
        model,
        sha,
        label="policy_ownership_commit_2",
    )
    return commit


def _validate_policy_history_model(
    model: PolicyHistoryModel,
) -> PolicyHistoryValidation:
    """Validate the exact reviewed policy range independently of later work."""

    commit_1 = _policy_history_commit(
        model,
        POLICY_OWNERSHIP_COMMIT_1_SHA,
        location="$.history_policy.policy_ownership_commit_1",
    )
    _require(
        commit_1.subject == POLICY_OWNERSHIP_COMMIT_SUBJECTS[0]
        and commit_1.parents == (PR_A_MERGE_SHA,),
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_ownership_commit_1",
        (
            "Commit 1 SHA, subject, or single-parent relationship to the "
            "policy base changed"
        ),
    )
    _require(
        POLICY_OWNERSHIP_COMMIT_1_SHA in model.reachable_from_head,
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_ownership_commit_1",
        "unchanged Commit 1 is not an ancestor of HEAD",
    )
    _require_policy_commit_content(
        model,
        POLICY_OWNERSHIP_COMMIT_1_SHA,
        label="policy_ownership_commit_1",
    )

    candidates = model.policy_merge_candidates
    _require(
        len(candidates) <= 1,
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_merge",
        "multiple reachable policy PR #3 merge candidates are ambiguous",
    )
    for candidate in candidates:
        merge = _policy_history_commit(
            model,
            candidate,
            location="$.history_policy.policy_merge",
        )
        _require(
            merge.subject == POLICY_OWNERSHIP_MERGE_SUBJECT,
            "UserIntegrationPromotionStageMismatch",
            "$.history_policy.policy_merge.subject",
            "policy merge candidate has the wrong first-line subject",
        )
        _require(
            candidate in model.reachable_from_head,
            "UserIntegrationPromotionStageMismatch",
            "$.history_policy.policy_merge.ancestry",
            "policy merge candidate is not an ancestor of HEAD",
        )

    if model.head == POLICY_OWNERSHIP_COMMIT_1_SHA:
        _require(
            not candidates,
            "UserIntegrationPromotionStageMismatch",
            "$.history_policy.policy_merge",
            "Commit-1 construction state cannot contain a policy merge",
        )
        _require(
            not model.worktree_src_changes,
            "UserIntegrationFalseComplete",
            "$.history_policy.policy_ownership_worktree",
            (
                "Commit-2 construction worktree contains forbidden "
                f"production changes: {list(model.worktree_src_changes)}"
            ),
        )
        return PolicyHistoryValidation(
            state="commit-1-construction",
            policy_commit_2=None,
            policy_merge=None,
            bounded_policy_end=POLICY_OWNERSHIP_COMMIT_1_SHA,
        )

    if not candidates:
        commit_2 = _validate_policy_commit_2(model, model.head)
        _require(
            not model.worktree_src_changes,
            "UserIntegrationFalseComplete",
            "$.history_policy.policy_ownership_worktree",
            (
                "unmerged policy branch worktree contains forbidden "
                f"production changes: {list(model.worktree_src_changes)}"
            ),
        )
        return PolicyHistoryValidation(
            state="unmerged-two-commit-branch",
            policy_commit_2=commit_2.sha,
            policy_merge=None,
            bounded_policy_end=commit_2.sha,
        )

    merge_sha = candidates[0]
    merge = _policy_history_commit(
        model,
        merge_sha,
        location="$.history_policy.policy_merge",
    )
    _require(
        len(merge.parents) == 2,
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_merge.parents",
        "the normal policy PR #3 merge must have exactly two parents",
    )
    _require(
        merge.parents[0] == PR_A_MERGE_SHA,
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_merge.parents",
        "the policy merge first parent is not the unchanged policy base",
    )
    commit_2 = _validate_policy_commit_2(model, merge.parents[1])
    _require(
        merge.tree == commit_2.tree,
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_merge.tree",
        "the policy merge introduces tree changes beyond Commit 2",
    )
    _require(
        merge_sha in model.registry_blobs,
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_merge.registry_evidence",
        "policy merge registry evidence is missing",
    )
    _require(
        model.registry_blobs[merge_sha]
        == model.registry_blobs[commit_2.sha]
        == model.registry_blobs[POLICY_OWNERSHIP_COMMIT_1_SHA]
        == model.registry_blobs[PR_A_MERGE_SHA],
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.policy_merge.registry",
        "the policy merge tree does not preserve the bounded registry",
    )
    return PolicyHistoryValidation(
        state="merged-pr-3-or-later-descendant",
        policy_commit_2=commit_2.sha,
        policy_merge=merge.sha,
        bounded_policy_end=commit_2.sha,
    )


def _live_policy_history_model(repo_root: Path) -> PolicyHistoryModel:
    rows = _run(
        repo_root,
        "git",
        "log",
        "--format=%H%x09%T%x09%P%x09%s",
        "HEAD",
    ).splitlines()
    commits: dict[str, PolicyHistoryCommit] = {}
    for row in rows:
        fields = row.split("\t", 3)
        if len(fields) != 4:
            continue
        sha, tree, parents, subject = fields
        commits[sha] = PolicyHistoryCommit(
            sha=sha,
            tree=tree,
            parents=tuple(parents.split()),
            subject=subject,
        )
    head = _run(repo_root, "git", "rev-parse", "HEAD")
    reachable = frozenset(commits)
    candidates = tuple(
        commit.sha
        for commit in commits.values()
        if commit.subject == POLICY_OWNERSHIP_MERGE_SUBJECT
    )

    relevant = {
        PR_A_MERGE_SHA,
        POLICY_OWNERSHIP_COMMIT_1_SHA,
        head,
        *candidates,
    }
    for candidate in candidates:
        relevant.update(commits[candidate].parents)

    src_changes: dict[str, tuple[str, ...]] = {}
    registry_blobs: dict[str, bytes] = {}
    for sha in sorted(relevant):
        if sha not in commits:
            continue
        src_changes[sha] = tuple(
            _run(
                repo_root,
                "git",
                "diff-tree",
                "--no-commit-id",
                "--name-only",
                "-r",
                sha,
                "--",
                "src",
            ).splitlines()
        )
        try:
            registry_blobs[sha] = _git_blob_bytes(
                repo_root,
                sha,
                PROTOCOL_REGISTRY_RELATIVE_PATH,
            )
        except subprocess.CalledProcessError:
            pass

    worktree_src = set(
        _run(
            repo_root,
            "git",
            "diff",
            "--name-only",
            "HEAD",
            "--",
            "src",
        ).splitlines()
    )
    worktree_src.update(
        _run(
            repo_root,
            "git",
            "ls-files",
            "--others",
            "--exclude-standard",
            "--",
            "src",
        ).splitlines()
    )
    return PolicyHistoryModel(
        head=head,
        commits=commits,
        reachable_from_head=reachable,
        policy_merge_candidates=candidates,
        src_changes_by_commit=src_changes,
        registry_blobs=registry_blobs,
        worktree_src_changes=tuple(sorted(worktree_src)),
    )


def _history_policy(repo_root: Path) -> dict[str, Any]:
    """Validate PR-A and the bounded policy PR across all reviewed states."""

    rows = _run(
        repo_root,
        "git",
        "log",
        "--reverse",
        "--format=%H%x09%P%x09%s",
        f"{audit.EXPECTED_BASE_SHA}..HEAD",
    ).splitlines()
    parsed = [
        tuple(row.split("\t", 2))
        for row in rows
        if row.count("\t") == 2
    ]
    subjects = tuple(row[2] for row in parsed)
    required_prefix = (*REQUIRED_COMMIT_SUBJECTS, PR_A_MERGE_SUBJECT)
    _require(
        subjects[: len(required_prefix)] == required_prefix,
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.pr_a_subjects",
        (
            "history does not preserve the exact six-commit PR-A subject "
            "sequence followed by its reviewed merge"
        ),
    )

    merge_sha, merge_parents, merge_subject = parsed[
        len(REQUIRED_COMMIT_SUBJECTS)
    ]
    _require(
        merge_sha == PR_A_MERGE_SHA
        and tuple(merge_parents.split()) == PR_A_MERGE_PARENTS
        and merge_subject == PR_A_MERGE_SUBJECT
        and _run(
            repo_root,
            "git",
            "show",
            "-s",
            "--format=%T",
            merge_sha,
        )
        == PR_A_MERGE_TREE,
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.merged_pr_a",
        "the pinned PR-A merge SHA, tree, parents, or subject changed",
    )

    commit_5 = parsed[4][0]
    commit_6 = parsed[5][0]
    commit_6_src = _run(
        repo_root,
        "git",
        "diff",
        "--name-only",
        f"{commit_5}..{commit_6}",
        "--",
        "src",
    ).splitlines()
    _require(
        not commit_6_src,
        (
            "UserIntegrationPromotionStageMismatch"
            if PROTOCOL_REGISTRY_RELATIVE_PATH in commit_6_src
            else "UserIntegrationFalseComplete"
        ),
        "$.history_policy.commit_6",
        (
            "the original PR-A Commit 6 contains production or registry "
            f"changes: {commit_6_src}"
        ),
    )
    _require(
        _git_blob_bytes(
            repo_root,
            commit_5,
            PROTOCOL_REGISTRY_RELATIVE_PATH,
        )
        == _git_blob_bytes(
            repo_root,
            commit_6,
            PROTOCOL_REGISTRY_RELATIVE_PATH,
        )
        == _git_blob_bytes(
            repo_root,
            PR_A_MERGE_SHA,
            PROTOCOL_REGISTRY_RELATIVE_PATH,
        ),
        "UserIntegrationPromotionStageMismatch",
        "$.history_policy.registry",
        "PR-A closure or merge changed the status registry",
    )

    _validate_policy_history_model(_live_policy_history_model(repo_root))
    return _expected_history_policy()


def _staged_arithmetic(
    repo_root: Path,
    base: Mapping[Key, Mapping[str, Any]],
) -> list[dict[str, Any]]:
    history = _run(
        repo_root,
        "git",
        "log",
        "--reverse",
        "--format=%H%x09%s",
        f"{audit.EXPECTED_BASE_SHA}..HEAD",
    ).splitlines()
    commits_by_subject: dict[str, list[str]] = {}
    for row in history:
        if "\t" not in row:
            continue
        commit, subject = row.split("\t", 1)
        commits_by_subject.setdefault(subject, []).append(commit)

    cumulative: set[Key] = set()
    evidence: list[dict[str, Any]] = []
    for stage in audit.STAGES:
        subject = str(stage["subject"])
        matches = commits_by_subject.get(subject, [])
        _require(
            len(matches) == 1,
            "UserIntegrationStageArithmeticMismatch",
            f"$.staged_arithmetic.commit_{stage['commit']}",
            f"expected exactly one history commit with subject {subject!r}",
        )
        commit = matches[0]
        registry_bytes = _git_blob_bytes(
            repo_root,
            commit,
            "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc",
        )
        rows = surface.parse_registry_data_text(
            registry_bytes.decode("utf-8"),
            f"{commit}:ProtocolSurfaceRegistryData.inc",
        )
        registry = _index(
            rows,
            code="UserIntegrationStageArithmeticMismatch",
            location=f"$.staged_arithmetic.commit_{stage['commit']}",
        )
        owned = {
            ("client_request", "ClientRequest", "method", name)
            for name in stage["requests"]
        }
        owned.update(
            {
                (
                    "server_notification",
                    "ServerNotification",
                    "method",
                    name,
                )
                for name in stage["notifications"]
            }
        )
        owned.update(
            {
                (
                    "tagged_union_discriminator",
                    "PluginSource",
                    "type",
                    name,
                )
                for name in stage["unions"]
            }
        )
        cumulative.update(owned)
        changed = {key for key in registry if registry[key] != base[key]}
        native_full = _status(
            row for row in registry.values() if row["a1_slice"] == "A1.4"
        )
        native = {
            key: native_full[key]
            for key in ("Complete", "Partial", "NotImplemented")
        }
        global_full = _status(registry.values())
        global_counts = {
            key: global_full[key]
            for key in (
                "Complete",
                "Partial",
                "NotImplemented",
                "NotApplicable",
            )
        }
        _require(
            changed == cumulative
            and native == stage["native"]
            and global_counts == stage["global"]
            and all(
                registry[key]["typed_schema_status"] == "Complete"
                for key in cumulative
            ),
            "UserIntegrationStageArithmeticMismatch",
            f"$.staged_arithmetic.commit_{stage['commit']}",
            "committed promotion ownership or cumulative arithmetic changed",
        )
        evidence.append(
            {
                "commit": stage["commit"],
                "sha": commit,
                "tree": _run(
                    repo_root, "git", "show", "-s", "--format=%T", commit
                ),
                "subject": subject,
                "owned_identity_count": len(owned),
                "owned_identities": [
                    _key_object(key) for key in sorted(owned)
                ],
                "cumulative_complete_identity_count": len(cumulative),
                "native_status": native,
                "global_status": global_counts,
            }
        )
    _require(
        cumulative == set(_expected_keys()),
        "UserIntegrationStageArithmeticMismatch",
        "$.staged_arithmetic",
        "Commit-5 cumulative set is not the exact PR-A scope",
    )
    return evidence


def _proof_paths(arguments: argparse.Namespace) -> set[Path]:
    return {
        arguments.generation_pre.resolve(),
        arguments.generation_pass_1.resolve(),
        arguments.generation_pass_2.resolve(),
    }


def _proof_relative_paths(arguments: argparse.Namespace) -> list[str]:
    return [
        _relative_command_path(path, arguments.repo_root)
        for path in (
            arguments.generation_pre,
            arguments.generation_pass_1,
            arguments.generation_pass_2,
        )
    ]


def _policy_ownership_evidence_paths(
    arguments: argparse.Namespace,
) -> tuple[Path, ...]:
    ownership = arguments.policy_ownership_output.resolve()
    _require(
        _relative_command_path(ownership, arguments.repo_root)
        == POLICY_OWNERSHIP_EVIDENCE_RELATIVE_PATHS[0],
        "UserIntegrationPredecessorEvidenceDrift",
        "$.generation_sequence.codex-policy-ownership.output",
        "Codex policy ownership output path changed",
    )
    return (
        ownership,
        *(
            (arguments.repo_root / relative).resolve()
            for relative in POLICY_OWNERSHIP_EVIDENCE_RELATIVE_PATHS[1:]
        ),
    )


def _validate_extraction_proof_exclusions(
    arguments: argparse.Namespace,
) -> None:
    extraction = _load(arguments.extraction_manifest)
    proof_paths = _proof_relative_paths(arguments)
    _require(
        proof_paths == list(GENERATION_PROOF_RELATIVE_PATHS),
        EXTRACTION_PROOF_EXCLUSION_DIAGNOSTIC,
        "$.deterministic_generation.proof_paths",
        "closure proof paths differ from the exact path-only exception set",
    )
    _require(
        extraction.get("generation_proof_self_reference_exclusions")
        == proof_paths,
        EXTRACTION_PROOF_EXCLUSION_DIAGNOSTIC,
        "$.extraction.generation_proof_self_reference_exclusions",
        "extraction manifest has a missing, extra, reordered, or wrong exception",
    )
    inventoried = {
        str(row.get("path"))
        for collection in ("imported_files", "standalone_files")
        for row in extraction.get(collection, [])
        if isinstance(row, Mapping)
    }
    _require(
        not (set(proof_paths) & inventoried),
        EXTRACTION_PROOF_EXCLUSION_DIAGNOSTIC,
        "$.extraction.generation_proof_self_reference_exclusions",
        "a proof metadata path is also included in the extraction hash domain",
    )
    invalid: list[str] = []
    for relative in proof_paths:
        path = arguments.repo_root / relative
        try:
            mode = path.lstat().st_mode
        except OSError:
            invalid.append(relative)
            continue
        if path.is_symlink() or not stat.S_ISREG(mode):
            invalid.append(relative)
    _require(
        not invalid,
        EXTRACTION_PROOF_EXCLUSION_DIAGNOSTIC,
        "$.extraction.generation_proof_self_reference_exclusions",
        f"proof exceptions are not regular non-symlink files: {invalid}",
    )


def _generated_paths(arguments: argparse.Namespace) -> list[Path]:
    """Return the complete generated corpus in deterministic path order.

    The three generation-proof documents are metadata about this corpus and
    are the only exclusions.  Including a proof document in the hash domain
    that the same document contains would have no finite byte representation.
    The extraction manifest records only the exact three paths.  Their bytes
    are parsed, canonically checked, and hashed by this specialized guard.
    """

    proof_paths = _proof_paths(arguments)
    paths: set[Path] = {
        path.resolve()
        for path in arguments.evidence_root.rglob("*")
        if path.is_file() and path.resolve() not in proof_paths
    }
    paths.update(
        path.resolve()
        for path in arguments.fixture_root.rglob("*.json")
    )
    paths.update(
        path.resolve()
        for path in (
            arguments.surface_manifest,
            arguments.protocol_provenance,
            arguments.registry,
            arguments.operation_descriptors,
            arguments.notification_descriptors,
            arguments.conversation_descriptors,
            arguments.accounts_models_configuration_descriptors,
            arguments.commands_filesystem_reviews_approvals_descriptors,
            arguments.union_descriptors,
            arguments.server_request_descriptors,
            arguments.thread_item_descriptors,
            arguments.response_item_descriptors,
            arguments.coverage_document,
            arguments.security_document,
            arguments.extraction_manifest,
            *_policy_ownership_evidence_paths(arguments),
            arguments.abi_tool,
            arguments.abi_probe,
        )
    )
    return sorted(
        (path for path in paths if path.is_file()),
        key=lambda path: path.relative_to(arguments.repo_root).as_posix(),
    )


def _corpus_snapshot(arguments: argparse.Namespace) -> dict[str, Any]:
    records = [
        _record(path, arguments.repo_root)
        for path in _generated_paths(arguments)
    ]
    fixture_prefix = (
        arguments.fixture_root.resolve()
        .relative_to(arguments.repo_root)
        .as_posix()
        + "/"
    )
    fixture_count = sum(
        str(row["path"]).startswith(fixture_prefix) for row in records
    )
    return {
        "file_count": len(records),
        "byte_count": sum(int(row["bytes"]) for row in records),
        "fixture_json_file_count": fixture_count,
        "manifest_sha256": _sha256_json(records),
        "files": records,
    }


def _snapshot_with_replacement(
    snapshot: Mapping[str, Any],
    *,
    relative_path: str,
    replacement: bytes,
) -> dict[str, Any]:
    """Return a valid snapshot with one live-artifact copy replaced."""

    planted = json.loads(json.dumps(snapshot, ensure_ascii=False))
    matches = [
        row
        for row in planted.get("files", [])
        if row.get("path") == relative_path
    ]
    _require(
        len(matches) == 1,
        "UserIntegrationSecondPassNondeterminism",
        f"$.deterministic_generation.files.{relative_path}",
        "live mutation target is absent or duplicated",
    )
    matches[0]["bytes"] = len(replacement)
    matches[0]["sha256"] = _sha256_bytes(replacement)
    planted["byte_count"] = sum(
        int(row["bytes"]) for row in planted["files"]
    )
    planted["manifest_sha256"] = _sha256_json(planted["files"])
    return planted


def _snapshot_summary(snapshot: Mapping[str, Any]) -> dict[str, Any]:
    return {
        key: snapshot[key]
        for key in (
            "file_count",
            "byte_count",
            "fixture_json_file_count",
            "manifest_sha256",
        )
    }


def _snapshot_index(
    snapshot: Mapping[str, Any],
    *,
    location: str,
) -> dict[str, dict[str, Any]]:
    files = snapshot.get("files")
    _require(
        isinstance(files, list),
        "UserIntegrationSecondPassNondeterminism",
        location,
        "generated-corpus manifest lacks its file array",
    )
    records: dict[str, dict[str, Any]] = {}
    previous = ""
    for index, raw in enumerate(files):
        _require(
            isinstance(raw, Mapping)
            and isinstance(raw.get("path"), str)
            and isinstance(raw.get("bytes"), int)
            and isinstance(raw.get("sha256"), str),
            "UserIntegrationSecondPassNondeterminism",
            f"{location}.files[{index}]",
            "generated-corpus record is malformed",
        )
        record = dict(raw)
        path = str(record["path"])
        _require(
            path > previous and path not in records,
            "UserIntegrationSecondPassNondeterminism",
            f"{location}.files[{index}]",
            "generated-corpus paths are not strictly sorted and unique",
        )
        previous = path
        records[path] = record
    reconstructed = {
        "file_count": len(files),
        "byte_count": sum(int(row["bytes"]) for row in files),
        "fixture_json_file_count": int(
            snapshot.get("fixture_json_file_count", -1)
        ),
        "manifest_sha256": _sha256_json(files),
    }
    _require(
        reconstructed == _snapshot_summary(snapshot),
        "UserIntegrationSecondPassNondeterminism",
        location,
        "generated-corpus summary does not match its records",
    )
    return records


def _first_snapshot_difference(
    first: Mapping[str, Any], second: Mapping[str, Any]
) -> tuple[str, str, str] | None:
    first_rows = _snapshot_index(
        first, location="$.generation_pass_1.snapshot"
    )
    second_rows = _snapshot_index(
        second, location="$.generation_pass_2.snapshot"
    )
    for path in sorted(set(first_rows) | set(second_rows)):
        left = first_rows.get(path)
        right = second_rows.get(path)
        if left != right:
            return (
                path,
                str(left["sha256"]) if left is not None else "<missing>",
                str(right["sha256"]) if right is not None else "<missing>",
            )
    if _snapshot_summary(first) != _snapshot_summary(second):
        return (
            "<manifest-summary>",
            str(first.get("manifest_sha256", "<missing>")),
            str(second.get("manifest_sha256", "<missing>")),
        )
    return None


def _require_identical_snapshots(
    first: Mapping[str, Any], second: Mapping[str, Any]
) -> None:
    difference = _first_snapshot_difference(first, second)
    if difference is not None:
        path, first_hash, second_hash = difference
        _fail(
            "UserIntegrationSecondPassNondeterminism",
            f"$.deterministic_generation.files.{path}",
            (
                "complete generator passes differ at "
                f"{path}: pass-1={first_hash}, pass-2={second_hash}"
            ),
        )


def _relative_command_path(path: Path, repo_root: Path) -> str:
    try:
        return path.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def _python_step(
    arguments: argparse.Namespace,
    name: str,
    tool: str,
    tool_arguments: Sequence[str],
    outputs: Sequence[str],
) -> GenerationStep:
    return GenerationStep(
        name=name,
        generator=tool,
        arguments=(
            sys.executable,
            "-B",
            str(arguments.repo_root / tool),
            *tuple(tool_arguments),
        ),
        outputs=tuple(outputs),
    )


def _generation_steps(
    arguments: argparse.Namespace,
) -> tuple[GenerationStep, ...]:
    """Construct the complete authoritative dependency-ordered pass."""

    root = arguments.repo_root
    evidence = arguments.evidence_root
    fixtures = arguments.fixture_root
    detail = root / "src/ai/openai/codex/detail"
    surface_tool = "tools/codex/app_server_surface.py"
    manifest = str(arguments.surface_manifest)
    schema = str(arguments.schema_root)
    evidence_text = str(evidence)
    fixture_index = str(arguments.fixture_index)
    common_surface = ("--manifest", manifest, "--evidence-root", evidence_text)
    abi_library = (
        str(arguments.abi_library)
        if arguments.abi_library is not None
        else "{abi-library}"
    )
    policy_baseline_ctest = (
        str(arguments.policy_baseline_ctest)
        if arguments.policy_baseline_ctest is not None
        else "{policy-baseline-ctest}"
    )
    policy_final_ctest = (
        str(arguments.policy_final_ctest)
        if arguments.policy_final_ctest is not None
        else "{policy-final-ctest}"
    )
    policy_snodec_root = (
        str(arguments.policy_snodec_root)
        if arguments.policy_snodec_root is not None
        else "{policy-snodec-root}"
    )

    steps: list[GenerationStep] = [
        _python_step(
            arguments,
            "surface-manifest",
            surface_tool,
            ("extract", "--schema-root", schema, "--output", manifest),
            (_relative_command_path(arguments.surface_manifest, root),),
        ),
        _python_step(
            arguments,
            "surface-provenance-verification",
            surface_tool,
            (
                "verify",
                "--schema-root",
                schema,
                "--provenance",
                str(arguments.schema_provenance),
                "--manifest",
                manifest,
            ),
            (),
        ),
        _python_step(
            arguments,
            "protocol-contracts",
            "tools/codex/app_server_contracts.py",
            (
                "--source-root",
                str(arguments.protocol_source_root),
                "--schema-root",
                schema,
                "--manifest",
                manifest,
                "--schema-provenance",
                str(arguments.schema_provenance),
                "--evidence-root",
                evidence_text,
            ),
            (
                _relative_command_path(arguments.protocol_provenance, root),
                _relative_command_path(
                    evidence / "operation-contracts.json", root
                ),
                _relative_command_path(
                    evidence / "typescript-audit.json", root
                ),
            ),
        ),
        _python_step(
            arguments,
            "schema-derived-fixtures",
            "tools/codex/app_server_fixtures.py",
            (
                "generate",
                "--repo-root",
                str(root),
                "--schema-root",
                schema,
                "--manifest",
                manifest,
                "--contracts",
                str(evidence / "operation-contracts.json"),
                "--fixture-root",
                str(fixtures),
                "--evidence-root",
                evidence_text,
            ),
            (
                _relative_command_path(fixtures, root) + "/**/*.json",
                *(
                    _relative_command_path(evidence / name, root)
                    for name in (
                        "fixture-coverage.json",
                        "schema-keywords.json",
                        "nested-reachability.json",
                        "module-slice-assignment.json",
                        "schema-completeness-evidence.json",
                    )
                ),
            ),
        ),
        _python_step(
            arguments,
            "production-registry",
            surface_tool,
            (
                "registry",
                *common_surface,
                "--output",
                str(arguments.registry),
            ),
            (_relative_command_path(arguments.registry, root),),
        ),
    ]

    descriptor_steps = (
        (
            "conversation-descriptors",
            "conversation-descriptors",
            arguments.conversation_descriptors,
            True,
        ),
        (
            "accounts-models-configuration-descriptors",
            "accounts-models-configuration-union-descriptors",
            arguments.accounts_models_configuration_descriptors,
            True,
        ),
        (
            "commands-filesystem-reviews-approvals-descriptors",
            "commands-filesystem-reviews-approvals-union-descriptors",
            arguments.commands_filesystem_reviews_approvals_descriptors,
            True,
        ),
        (
            "integrations-and-long-tail-descriptors",
            "integrations-and-long-tail-union-descriptors",
            arguments.union_descriptors,
            True,
        ),
        (
            "server-request-descriptors",
            "server-request-descriptors",
            arguments.server_request_descriptors,
            False,
        ),
        (
            "client-operation-descriptors",
            "operation-descriptors",
            arguments.operation_descriptors,
            False,
        ),
        (
            "server-notification-descriptors",
            "notification-descriptors",
            arguments.notification_descriptors,
            False,
        ),
    )
    for name, subcommand, output, needs_schema in descriptor_steps:
        command: list[str] = [subcommand, *common_surface]
        if needs_schema:
            command.extend(("--schema-root", schema))
        command.extend(("--output", str(output)))
        steps.append(
            _python_step(
                arguments,
                name,
                surface_tool,
                command,
                (_relative_command_path(output, root),),
            )
        )

    steps.extend(
        (
            _python_step(
                arguments,
                "item-descriptors",
                surface_tool,
                (
                    "item-descriptors",
                    *common_surface,
                    "--schema-root",
                    schema,
                    "--thread-output",
                    str(arguments.thread_item_descriptors),
                    "--response-output",
                    str(arguments.response_item_descriptors),
                ),
                (
                    _relative_command_path(
                        arguments.thread_item_descriptors, root
                    ),
                    _relative_command_path(
                        arguments.response_item_descriptors, root
                    ),
                ),
            ),
            _python_step(
                arguments,
                "operation-production-coverage",
                surface_tool,
                (
                    "operation-production-coverage",
                    *common_surface,
                    "--fixture-index",
                    fixture_index,
                    "--repo-root",
                    str(root),
                    "--output",
                    str(
                        evidence
                        / "a1-1-operation-production-coverage.json"
                    ),
                ),
                (
                    _relative_command_path(
                        evidence
                        / "a1-1-operation-production-coverage.json",
                        root,
                    ),
                ),
            ),
            _python_step(
                arguments,
                "notification-production-coverage",
                surface_tool,
                (
                    "notification-production-coverage",
                    *common_surface,
                    "--fixture-index",
                    fixture_index,
                    "--repo-root",
                    str(root),
                    "--output",
                    str(
                        evidence
                        / "a1-1-notification-production-coverage.json"
                    ),
                ),
                (
                    _relative_command_path(
                        evidence
                        / "a1-1-notification-production-coverage.json",
                        root,
                    ),
                ),
            ),
            _python_step(
                arguments,
                "registry-coverage-and-security-documents",
                surface_tool,
                (
                    "docs",
                    "--manifest",
                    manifest,
                    "--registry",
                    str(arguments.registry),
                    "--provenance",
                    str(arguments.schema_provenance),
                    "--coverage-output",
                    str(arguments.coverage_document),
                    "--security-output",
                    str(arguments.security_document),
                ),
                (
                    _relative_command_path(arguments.coverage_document, root),
                    _relative_command_path(arguments.security_document, root),
                ),
            ),
        )
    )

    stage_steps = (
        (
            "a1-1-audit",
            "tools/codex/app_server_a1_1.py",
            ("generate", "--repo-root", str(root)),
            (
                "a1-1-implementation-plan.json",
                "a1-1-type-closure.json",
            ),
        ),
        (
            "a1-1-closure",
            "tools/codex/app_server_a1_1_closure.py",
            ("generate", "--repo-root", str(root)),
            ("a1-1-closure-report.json",),
        ),
        (
            "a1-2-audit",
            "tools/codex/app_server_a1_2.py",
            ("generate", "--repo-root", str(root)),
            (
                "a1-2-implementation-plan.json",
                "a1-2-type-closure.json",
            ),
        ),
        (
            "a1-2-closure",
            "tools/codex/app_server_a1_2_closure.py",
            ("generate", "--repo-root", str(root)),
            ("a1-2-closure-report.json",),
        ),
        (
            "a1-3-audit",
            "tools/codex/app_server_a1_3.py",
            ("generate", "--repo-root", str(root)),
            (
                "a1-3-implementation-plan.json",
                "a1-3-type-closure.json",
            ),
        ),
        (
            "a1-3-closure",
            "tools/codex/app_server_a1_3_closure.py",
            ("generate", "--repo-root", str(root)),
            ("a1-3-closure-report.json",),
        ),
        (
            "native-a1-4-successor",
            "tools/codex/app_server_a1_4.py",
            ("generate", "--repo-root", str(root)),
            (
                "a1-4-total-partition.json",
                "a1-4-type-closure.json",
                "a1-4-implementation-plan.json",
                "a1-final-cross-slice-ledger.json",
            ),
        ),
        (
            "pr-a-audit-and-batch-plan",
            "tools/codex/app_server_a1_4_user_integrations.py",
            ("generate", "--repo-root", str(root)),
            (
                "a1-4-user-integrations-start-state.json",
                "a1-4-user-integrations-batch-plan.json",
            ),
        ),
        (
            "pr-a-api-abi-evidence",
            "tools/codex/app_server_a1_4_user_integrations_abi.py",
            (
                "generate",
                "--repo-root",
                str(root),
                "--compiler",
                arguments.abi_compiler,
                "--library",
                abi_library,
                "--output",
                str(arguments.abi_evidence),
                "--symbols-output",
                str(arguments.abi_symbols),
            ),
            (
                arguments.abi_evidence.name,
                arguments.abi_symbols.name,
            ),
        ),
        (
            "pr-a-closure",
            "tools/codex/app_server_a1_4_user_integrations_closure.py",
            (
                "generate",
                "--repo-root",
                str(root),
                "--output",
                str(arguments.output),
            ),
            ("a1-4-user-integrations-closure-report.json",),
        ),
    )
    for name, tool, command, names in stage_steps:
        steps.append(
            _python_step(
                arguments,
                name,
                tool,
                command,
                tuple(
                    _relative_command_path(evidence / item, root)
                    for item in names
                ),
            )
        )
    steps.append(
        _python_step(
            arguments,
            "codex-policy-ownership",
            "tools/extraction/verify_codex_policy_ownership.py",
            (
                "generate",
                "--repo-root",
                str(root),
                "--baseline-ctest",
                policy_baseline_ctest,
                "--final-ctest",
                policy_final_ctest,
                "--snodec-root",
                policy_snodec_root,
                "--output",
                str(arguments.policy_ownership_output),
            ),
            tuple(
                _relative_command_path(path, root)
                for path in _policy_ownership_evidence_paths(arguments)
            ),
        )
    )
    steps.append(
        _python_step(
            arguments,
            "extraction-manifest-last",
            "tools/extraction/verify_extraction.py",
            (
                "generate",
                "--repo-root",
                str(root),
                "--manifest",
                str(arguments.extraction_manifest),
            ),
            (
                _relative_command_path(arguments.extraction_manifest, root),
            ),
        )
    )
    return tuple(steps)


def _render_step(step: GenerationStep, repo_root: Path) -> dict[str, Any]:
    command: list[str] = []
    after_abi_compiler = False
    after_abi_library = False
    policy_placeholder: str | None = None
    policy_argument_placeholders = {
        "--baseline-ctest": "{policy-baseline-ctest}",
        "--final-ctest": "{policy-final-ctest}",
        "--snodec-root": "{policy-snodec-root}",
    }
    for index, argument in enumerate(step.arguments):
        if index == 0:
            command.append("{python}")
            continue
        if policy_placeholder is not None:
            command.append(policy_placeholder)
            policy_placeholder = None
            continue
        if after_abi_compiler:
            command.append("{abi-compiler}")
            after_abi_compiler = False
            continue
        if after_abi_library:
            command.append("{abi-library}")
            after_abi_library = False
            continue
        if (
            argument == "--compiler"
            and step.name == "pr-a-api-abi-evidence"
        ):
            command.append(argument)
            after_abi_compiler = True
            continue
        if argument == "--library" and step.name == "pr-a-api-abi-evidence":
            command.append(argument)
            after_abi_library = True
            continue
        if (
            step.name == "codex-policy-ownership"
            and argument in policy_argument_placeholders
        ):
            command.append(argument)
            policy_placeholder = policy_argument_placeholders[argument]
            continue
        try:
            candidate = Path(argument)
            if candidate.is_absolute():
                command.append(_relative_command_path(candidate, repo_root))
                continue
        except (OSError, ValueError):
            pass
        command.append(argument)
    return {
        "name": step.name,
        "generator": step.generator,
        "command": command,
        "outputs": list(step.outputs),
    }


def _run_generation_pass(
    arguments: argparse.Namespace,
    *,
    pass_name: str,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    steps = _generation_steps(arguments)
    _require(
        steps[-1].name == "extraction-manifest-last"
        and steps[-2].name == "codex-policy-ownership"
        and all(
            step.name != "extraction-manifest-last"
            for step in steps[:-1]
        )
        and all(
            step.name != "codex-policy-ownership"
            for step in steps[:-2]
        ),
        "UserIntegrationPredecessorEvidenceDrift",
        "$.generation_sequence",
        (
            "Codex policy ownership does not immediately precede the unique "
            "final extraction-manifest generator step"
        ),
    )
    rendered_steps: list[dict[str, Any]] = []
    for index, step in enumerate(steps, start=1):
        print(
            f"{pass_name}: [{index}/{len(steps)}] {step.name}",
            flush=True,
        )
        completed = subprocess.run(
            step.arguments,
            cwd=arguments.repo_root,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            _fail(
                "UserIntegrationPredecessorEvidenceDrift",
                f"$.generation_sequence.{step.name}",
                (
                    f"authoritative generator exited {completed.returncode}: "
                    f"{detail[-4000:]}"
                ),
            )
        rendered_steps.append(_render_step(step, arguments.repo_root))
    snapshot = _corpus_snapshot(arguments)
    _require(
        snapshot["fixture_json_file_count"] == EXPECTED_FIXTURE_JSON_FILES,
        "UserIntegrationFixtureMismatch",
        f"$.{pass_name}.fixture_json_file_count",
        (
            "full generated corpus does not contain the exact "
            f"{EXPECTED_FIXTURE_JSON_FILES} fixture JSON files"
        ),
    )
    return rendered_steps, snapshot


def _aggregate_records(
    root: Path, repo_root: Path
) -> dict[str, Any]:
    records = [
        _record(path, repo_root)
        for path in sorted(
            (path for path in root.rglob("*") if path.is_file()),
            key=lambda path: path.relative_to(repo_root).as_posix(),
        )
    ]
    return {
        "file_count": len(records),
        "byte_count": sum(int(row["bytes"]) for row in records),
        "manifest_sha256": _sha256_json(records),
    }


def _semantic_authorities(
    arguments: argparse.Namespace,
) -> dict[str, Any]:
    """Capture protocol semantics independently of generated hash ratchets."""

    surface_document = _load(arguments.surface_manifest)
    contracts = _load(
        arguments.evidence_root / "operation-contracts.json"
    )
    assignments = _load(
        arguments.evidence_root / "module-slice-assignment.json"
    )
    fixture_index = _load(arguments.fixture_index)
    extraction = _load(arguments.extraction_manifest)
    plan = _load(arguments.batch_plan)
    registry_rows = surface.parse_registry_data(arguments.registry)

    contract_projection = [
        {
            key: row.get(key)
            for key in (
                "surface_key",
                "parameter_type_identity",
                "parameter_schema",
                "result_type_identity",
                "result_schema_type_identity",
                "result_schema",
                "result_contract_kind",
            )
        }
        for row in contracts["contracts"]
    ]
    fixture_projection = [
        {
            key: row.get(key)
            for key in (
                "id",
                "file",
                "protocol_surface_key",
                "role",
                "schema",
                "expected_valid",
                "expected_diagnostic_codes",
                "a1_slice",
            )
        }
        for row in fixture_index["fixtures"]
    ]
    root_cmake = (arguments.repo_root / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    soversion = re.search(
        r"set\(AISUITE_CODEX_SOVERSION\s+(\d+)\)", root_cmake
    )
    _require(
        soversion is not None,
        "UserIntegrationSOVERSIONDrift",
        "$.semantic_authorities.codex_soversion",
        "unable to derive Codex SOVERSION",
    )
    return {
        "codex": {
            "version": audit.CODEX_VERSION,
            "upstream_tag": audit.UPSTREAM_TAG,
            "upstream_source_commit": audit.UPSTREAM_SOURCE_COMMIT,
        },
        "surface_manifest_sha256": _sha256_json(surface_document),
        "registry_rows_sha256": _sha256_json(registry_rows),
        "registry_status": _status(registry_rows),
        "operation_contract_projection_sha256": _sha256_json(
            contract_projection
        ),
        "module_slice_assignments_sha256": _sha256_json(
            assignments["assignments"]
        ),
        "fixture_protocol_projection_sha256": _sha256_json(
            fixture_projection
        ),
        "fixture_protocol_projection_count": len(fixture_projection),
        "schema_tree": _aggregate_records(
            arguments.schema_root, arguments.repo_root
        ),
        "production_tree": _aggregate_records(
            arguments.repo_root / "src", arguments.repo_root
        ),
        "schema_closure": {
            "counts": plan["schema_closure"]["counts"],
            "seed_definitions_sha256": _sha256_json(
                plan["schema_closure"].get("seed_definitions", [])
            ),
            "reachable_definitions_sha256": _sha256_json(
                plan["schema_closure"].get(
                    "reachable_definitions",
                    plan["schema_closure"].get(
                        "reachable_named_definitions", []
                    ),
                )
            ),
            "schema_paths_sha256": _sha256_json(
                plan["schema_closure"].get("schema_paths", [])
            ),
        },
        "plugin_source_order": list(audit.PLUGIN_SOURCE_ORDER),
        "operation_contracts": {
            "count": len(contract_projection),
            "unit_result_surfaces": contracts["unit_result_surfaces"],
        },
        "extraction_provenance": {
            "source": extraction["source"],
            "filtered_history": {
                key: extraction["filtered_history"][key]
                for key in (
                    "head",
                    "tree",
                    "retained_commit_count",
                    "selected_paths",
                    "commit_map_sha256",
                )
            },
        },
        "codex_soversion": int(soversion.group(1)),
    }


def _require_identical_semantics(
    first: Mapping[str, Any], second: Mapping[str, Any]
) -> None:
    if first == second:
        return
    _count, samples = _difference_paths(first, second, limit=1)
    first_path = samples[0] if samples else "/"
    _fail(
        "UserIntegrationPredecessorEvidenceDrift",
        f"$.semantic_authorities{first_path}",
        "generation changed a protocol, schema, registry, production, or pin authority",
    )


def _difference_paths(
    left: Any,
    right: Any,
    *,
    limit: int = 32,
) -> tuple[int, list[str]]:
    count = 0
    samples: list[str] = []

    def visit(first: Any, second: Any, path: str) -> None:
        nonlocal count
        if type(first) is not type(second):
            count += 1
            if len(samples) < limit:
                samples.append(path)
            return
        if isinstance(first, Mapping):
            for key in sorted(set(first) | set(second)):
                child = f"{path}/{str(key).replace('~', '~0').replace('/', '~1')}"
                if key not in first or key not in second:
                    count += 1
                    if len(samples) < limit:
                        samples.append(child)
                else:
                    visit(first[key], second[key], child)
            return
        if isinstance(first, list):
            for index in range(max(len(first), len(second))):
                child = f"{path}/{index}"
                if index >= len(first) or index >= len(second):
                    count += 1
                    if len(samples) < limit:
                        samples.append(child)
                else:
                    visit(first[index], second[index], child)
            return
        if first != second:
            count += 1
            if len(samples) < limit:
                samples.append(path)

    visit(left, right, "")
    return count, samples


def _changed_test_paths(arguments: argparse.Namespace) -> list[str]:
    tracked = _run(
        arguments.repo_root,
        "git",
        "diff",
        "--name-only",
        audit.EXPECTED_BASE_SHA,
        "--",
        "tests",
    ).splitlines()
    untracked = _run(
        arguments.repo_root,
        "git",
        "ls-files",
        "--others",
        "--exclude-standard",
        "--",
        "tests",
    ).splitlines()
    return sorted(
        {
            path
            for path in (*tracked, *untracked)
            if path.startswith("tests/")
        }
    )


def _reviewed_change_inputs(
    arguments: argparse.Namespace,
) -> dict[str, Any]:
    tracked = _run(
        arguments.repo_root,
        "git",
        "diff",
        "--name-only",
        audit.EXPECTED_BASE_SHA,
        "--",
    ).splitlines()
    untracked = _run(
        arguments.repo_root,
        "git",
        "ls-files",
        "--others",
        "--exclude-standard",
    ).splitlines()
    paths = sorted(set((*tracked, *untracked)))
    policy_evidence = {
        _relative_command_path(path, arguments.repo_root)
        for path in _policy_ownership_evidence_paths(arguments)
    }
    return {
        "base_sha": audit.EXPECTED_BASE_SHA,
        "test_sources": [
            path for path in paths if path.startswith("tests/")
        ],
        "production_sources": [
            path for path in paths if path.startswith("src/")
        ],
        "generator_and_evidence_sources": [
            path
            for path in paths
            if (
                path.startswith("tools/codex/")
                and path.endswith(".py")
                and path.count("/") == 2
            )
            or path
            in {
                "tools/extraction/verify_extraction.py",
                "tools/extraction/verify_codex_policy_ownership.py",
            }
        ],
        "documentation_and_build_guards": [
            path
            for path in paths
            if path in {"CMakeLists.txt", "README.md"}
            or (
                path.startswith(("docs/", ".github/"))
                and path
                not in (
                    {
                        "docs/extraction/source-manifest.json",
                        (
                            "docs/ai/openai/codex/"
                            "app-server-api-coverage.md"
                        ),
                        (
                            "docs/ai/openai/codex/"
                            "app-server-security-decisions.md"
                        ),
                    }
                    | policy_evidence
                )
            )
            or (
                path.startswith("tests/")
                and not path.endswith((".cpp", ".h", ".py"))
            )
        ],
    }


def _generator_for_path(path: str) -> str:
    name = Path(path).name
    if path.startswith("tools/codex/app-server-fixtures/"):
        return "tools/codex/app_server_fixtures.py generate"
    if name in {
        "fixture-coverage.json",
        "schema-keywords.json",
        "nested-reachability.json",
        "module-slice-assignment.json",
        "schema-completeness-evidence.json",
    }:
        return "tools/codex/app_server_fixtures.py generate"
    if name in {
        "operation-contracts.json",
        "typescript-audit.json",
        "PROVENANCE.json",
    } and (
        "app-server-protocol-source" in path
        or "app-server-evidence" in path
    ):
        return "tools/codex/app_server_contracts.py"
    if name.startswith("a1-1-operation-production-coverage"):
        return "tools/codex/app_server_surface.py operation-production-coverage"
    if name.startswith("a1-1-notification-production-coverage"):
        return "tools/codex/app_server_surface.py notification-production-coverage"
    if name.startswith("a1-1-"):
        return (
            "tools/codex/app_server_a1_1_closure.py"
            if "closure-report" in name
            else "tools/codex/app_server_a1_1.py"
        )
    if name.startswith("a1-2-"):
        return (
            "tools/codex/app_server_a1_2_closure.py"
            if "closure-report" in name
            else "tools/codex/app_server_a1_2.py"
        )
    if name.startswith("a1-3-"):
        return (
            "tools/codex/app_server_a1_3_closure.py"
            if "closure-report" in name
            else "tools/codex/app_server_a1_3.py"
        )
    if name.startswith("a1-4-user-integrations-"):
        return (
            "tools/codex/app_server_a1_4_user_integrations_closure.py"
            if "closure-report" in name
            else "tools/codex/app_server_a1_4_user_integrations.py"
        )
    if name.startswith("a1-4-") or name == "a1-final-cross-slice-ledger.json":
        return "tools/codex/app_server_a1_4.py"
    if path in POLICY_OWNERSHIP_EVIDENCE_RELATIVE_PATHS:
        return "tools/extraction/verify_codex_policy_ownership.py generate"
    if path == "docs/extraction/source-manifest.json":
        return "tools/extraction/verify_extraction.py generate"
    return "tools/codex/app_server_surface.py"


def _delta_classification(
    arguments: argparse.Namespace,
    before: Mapping[str, Any],
    after: Mapping[str, Any],
    semantic_before: Mapping[str, Any],
    semantic_after: Mapping[str, Any],
) -> list[dict[str, Any]]:
    _require_identical_semantics(semantic_before, semantic_after)
    before_rows = _snapshot_index(
        before, location="$.pre_generation.snapshot"
    )
    after_rows = _snapshot_index(
        after, location="$.generation_pass_1.snapshot"
    )
    changed_tests = _changed_test_paths(arguments)
    changes: list[dict[str, Any]] = []
    for path in sorted(set(before_rows) | set(after_rows)):
        old = before_rows.get(path)
        new = after_rows.get(path)
        if old == new:
            continue
        predecessor = Path(path).name.startswith(("a1-1-", "a1-2-", "a1-3-"))
        difference_count = 1
        difference_samples = ["/"]
        source_path = arguments.repo_root / path
        if (
            old is not None
            and new is not None
            and source_path.suffix == ".json"
            and source_path.is_file()
        ):
            try:
                previous_text = _git_blob_bytes(
                    arguments.repo_root, "HEAD", path
                )
                previous_json = json.loads(previous_text.decode("utf-8"))
                current_json = json.loads(
                    source_path.read_text(encoding="utf-8")
                )
                difference_count, difference_samples = _difference_paths(
                    previous_json, current_json
                )
            except (
                subprocess.CalledProcessError,
                UnicodeError,
                json.JSONDecodeError,
            ):
                pass
        content = (
            source_path.read_text(encoding="utf-8", errors="ignore")
            if source_path.is_file()
            else ""
        )
        direct_causes = [
            changed_path
            for changed_path in changed_tests
            if changed_path in content
        ]
        if path == "docs/extraction/source-manifest.json":
            semantic_classification = (
                "reviewed-extraction-final-hash-inventory-refresh"
            )
            cause = (
                "reviewed PR-A implementation, test, generator, evidence, "
                "documentation, package-guard, and CI final hashes"
            )
        elif path in POLICY_OWNERSHIP_EVIDENCE_RELATIVE_PATHS:
            semantic_classification = (
                "reviewed-codex-policy-ownership-derived-evidence"
            )
            cause = (
                "authoritative Codex policy ownership and configured CTest "
                "model regeneration"
            )
        elif path == _relative_command_path(
            arguments.output, arguments.repo_root
        ):
            semantic_classification = (
                "reviewed-pr-a-closure-proof-publication"
            )
            cause = (
                "authoritative PR-A closure and generation-proof publication"
            )
        elif predecessor:
            semantic_classification = (
                "derived-hash-or-evidence-ratchet-only"
            )
            cause = (
                "direct reviewed test-source hash/evidence ratchet"
                if direct_causes
                else "transitive reviewed test/evidence hash ratchet"
            )
        else:
            semantic_classification = "reviewed-pr-a-derived-output"
            cause = "authoritative PR-A regeneration"
        changes.append(
            {
                "path": path,
                "previous": old,
                "generated": new,
                "generator": _generator_for_path(path),
                "json_leaf_difference_count": difference_count,
                "json_pointer_samples": difference_samples,
                "predecessor_artifact": predecessor,
                "semantic_classification": semantic_classification,
                "byte_hash_ratcheting_only": predecessor,
                "semantic_identity_counts_and_contracts_changed": False,
                "direct_intentionally_modified_test_sources": direct_causes,
                "cause": cause,
            }
        )
    return changes


def _proof_hash_domain() -> dict[str, Any]:
    return {
        "description": (
            "Complete inherited and PR-A generated corpus, including "
            "all fixtures, evidence, descriptor/registry tables, "
            "generated coverage/security documents, Codex policy ownership "
            "and configured CTest evidence, the PR-A closure report, and "
            "the extraction manifest."
        ),
        "self_reference_exclusions": list(
            GENERATION_PROOF_RELATIVE_PATHS
        ),
        "exclusion_reason": (
            "The three proof metadata documents are outside the target "
            "corpus they describe. The extraction manifest records only "
            "their exact paths. The specialized closure guard validates "
            "their canonical bytes and requires the complete live target "
            "corpus, including the closure report and extraction "
            "manifest, to equal both recorded passes."
        ),
    }


def _proof_document(
    *,
    phase: str,
    snapshot: Mapping[str, Any],
    steps: Sequence[Mapping[str, Any]],
    semantic_authorities: Mapping[str, Any],
    reviewed_change_inputs: Mapping[str, Any],
    delta: Sequence[Mapping[str, Any]] | None = None,
    comparison: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    document: dict[str, Any] = {
        "format_version": 1,
        "generated_notice": (
            "Generated by tools/codex/"
            "app_server_a1_4_user_integrations_closure.py regenerate; "
            "do not edit."
        ),
        "phase": phase,
        "captured_before_first_generator": phase == "pre-generation",
        "snapshot_provenance": {
            "capture_boundary": (
                "immediately before the first generator in the finalized "
                "successful two-pass sequence"
            ),
            "is_task_start_snapshot": False,
            "scope_note": (
                "This is the pre-generation snapshot for the finalized "
                "successful sequence, not a claim about the tree before "
                "earlier diagnostic generator attempts."
            ),
            "prior_attempts": list(PRIOR_GENERATION_ATTEMPTS),
        },
        "hash_domain": _proof_hash_domain(),
        "generator_sequence": list(steps),
        "snapshot": dict(snapshot),
        "semantic_authorities": dict(semantic_authorities),
        "reviewed_change_inputs": dict(reviewed_change_inputs),
    }
    if delta is not None:
        document["delta_from_pre_generation"] = list(delta)
    if comparison is not None:
        document["comparison"] = dict(comparison)
    return document


def _write_proof(path: Path, document: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(_render(document), encoding="utf-8")


def _load_canonical_proof(path: Path) -> dict[str, Any]:
    try:
        mode = path.lstat().st_mode
        raw = path.read_bytes()
        value = json.loads(raw.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        _fail(
            "UserIntegrationPredecessorEvidenceDrift",
            str(path),
            f"unable to load generation-proof metadata: {error}",
        )
    _require(
        not path.is_symlink() and stat.S_ISREG(mode),
        "UserIntegrationPredecessorEvidenceDrift",
        str(path),
        "generation-proof metadata is not a regular non-symlink file",
    )
    _require(
        isinstance(value, dict),
        "UserIntegrationPredecessorEvidenceDrift",
        str(path),
        "generation-proof metadata is not an object",
    )
    canonical = _render(value).encode("utf-8")
    _require(
        raw == canonical
        and _sha256_bytes(raw) == _sha256_bytes(path.read_bytes()),
        "UserIntegrationPredecessorEvidenceDrift",
        str(path),
        "generation-proof bytes are noncanonical or changed during validation",
    )
    return value


def _load_generation_proof(
    arguments: argparse.Namespace,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    return (
        _load_canonical_proof(arguments.generation_pre),
        _load_canonical_proof(arguments.generation_pass_1),
        _load_canonical_proof(arguments.generation_pass_2),
    )


def _expected_generation_comparison() -> dict[str, Any]:
    return {
        "pass_1_equals_pass_2": True,
        "live_target_equals_pass_1": True,
        "live_target_equals_pass_2": True,
        "first_differing_path": None,
        "mutation_self_test": {
            "changed_exactly_one_file_copy": True,
            "diagnostic_code": "UserIntegrationSecondPassNondeterminism",
            "unmodified_passes_valid_afterward": True,
        },
        "live_artifact_mutation_self_tests": [
            {
                "path": (
                    "tools/codex/app-server-evidence/0.144.6/"
                    "a1-4-user-integrations-closure-report.json"
                ),
                "changed_exactly_one_file_copy": True,
                "diagnostic_code": (
                    "UserIntegrationSecondPassNondeterminism"
                ),
                "unmodified_live_target_valid_afterward": True,
            },
            {
                "path": "docs/extraction/source-manifest.json",
                "changed_exactly_one_file_copy": True,
                "diagnostic_code": (
                    "UserIntegrationSecondPassNondeterminism"
                ),
                "unmodified_live_target_valid_afterward": True,
            },
        ],
        "predecessor_mutation_self_test": {
            "changed_exactly_one_semantic_hash": True,
            "diagnostic_code": "UserIntegrationPredecessorEvidenceDrift",
            "unmodified_semantics_valid_afterward": True,
        },
    }


def _run_snapshot_mutation_self_test(
    arguments: argparse.Namespace,
    *,
    baseline: Mapping[str, Any],
    relative_path: str,
    artifact: Path,
    location: str,
) -> None:
    original = artifact.read_bytes()
    planted = _snapshot_with_replacement(
        baseline,
        relative_path=relative_path,
        replacement=original + b"\n",
    )
    baseline_rows = _snapshot_index(
        baseline, location=f"{location}.baseline"
    )
    planted_rows = _snapshot_index(
        planted, location=f"{location}.planted"
    )
    changed = [
        path
        for path in sorted(baseline_rows)
        if baseline_rows[path] != planted_rows[path]
    ]
    _require(
        changed == [relative_path],
        "UserIntegrationSecondPassNondeterminism",
        location,
        f"planted mutation did not change exactly {relative_path}",
    )
    try:
        _require_identical_snapshots(baseline, planted)
    except ClosureError as error:
        _require(
            error.codes == ("UserIntegrationSecondPassNondeterminism",)
            and len(error.diagnostics) == 1
            and relative_path in error.diagnostics[0].location,
            "UserIntegrationSecondPassNondeterminism",
            location,
            "planted artifact copy failed through an unrelated diagnostic",
        )
    else:
        _fail(
            "UserIntegrationSecondPassNondeterminism",
            location,
            "planted artifact copy mutation was not rejected",
        )
    _require_identical_snapshots(baseline, baseline)


def _validate_generation_proof(
    arguments: argparse.Namespace,
    *,
    package_safe: bool = False,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    _validate_extraction_proof_exclusions(arguments)
    pre, pass_1, pass_2 = _load_generation_proof(arguments)
    _require(
        pre.get("phase") == "pre-generation"
        and pass_1.get("phase") == "generation-pass-1"
        and pass_2.get("phase") == "generation-pass-2",
        "UserIntegrationPredecessorEvidenceDrift",
        "$.deterministic_generation.phase",
        "generation-proof phase labels changed",
    )
    expected_steps = [
        _render_step(step, arguments.repo_root)
        for step in _generation_steps(arguments)
    ]
    if package_safe:
        reviewed_inputs = pre.get("reviewed_change_inputs")
        _require(
            isinstance(reviewed_inputs, Mapping)
            and reviewed_inputs.get("base_sha") == audit.EXPECTED_BASE_SHA,
            "UserIntegrationPredecessorEvidenceDrift",
            "$.deterministic_generation.reviewed_change_inputs",
            "packaged proof has malformed reviewed-input provenance",
        )
    else:
        reviewed_inputs = _reviewed_change_inputs(arguments)
    expected_snapshot_provenance = {
        "capture_boundary": (
            "immediately before the first generator in the finalized "
            "successful two-pass sequence"
        ),
        "is_task_start_snapshot": False,
        "scope_note": (
            "This is the pre-generation snapshot for the finalized "
            "successful sequence, not a claim about the tree before "
            "earlier diagnostic generator attempts."
        ),
        "prior_attempts": list(PRIOR_GENERATION_ATTEMPTS),
    }
    for name, document in (
        ("pre", pre),
        ("pass_1", pass_1),
        ("pass_2", pass_2),
    ):
        expected_keys = {
            "captured_before_first_generator",
            "format_version",
            "generated_notice",
            "generator_sequence",
            "hash_domain",
            "phase",
            "reviewed_change_inputs",
            "semantic_authorities",
            "snapshot",
            "snapshot_provenance",
        }
        if name in ("pass_1", "pass_2"):
            expected_keys.add("delta_from_pre_generation")
        if name == "pass_2":
            expected_keys.add("comparison")
        _require(
            set(document) == expected_keys
            and document.get("format_version") == 1
            and document.get("generated_notice")
            == (
                "Generated by tools/codex/"
                "app_server_a1_4_user_integrations_closure.py regenerate; "
                "do not edit."
            )
            and document.get("hash_domain") == _proof_hash_domain()
            and document.get("captured_before_first_generator")
            == (name == "pre"),
            "UserIntegrationPredecessorEvidenceDrift",
            f"$.deterministic_generation.{name}",
            "generation-proof structure or exact hash domain changed",
        )
        _require(
            document.get("generator_sequence") == expected_steps,
            "UserIntegrationPredecessorEvidenceDrift",
            f"$.deterministic_generation.{name}.generator_sequence",
            "recorded authoritative generator sequence is stale",
        )
        _require(
            document.get("reviewed_change_inputs") == reviewed_inputs,
            "UserIntegrationPredecessorEvidenceDrift",
            f"$.deterministic_generation.{name}.reviewed_change_inputs",
            "reviewed source/test change inventory is stale",
        )
        _require(
            document.get("snapshot_provenance")
            == expected_snapshot_provenance,
            "UserIntegrationPredecessorEvidenceDrift",
            f"$.deterministic_generation.{name}.snapshot_provenance",
            "generation-attempt provenance is stale or misleading",
        )
        snapshot = document.get("snapshot")
        _require(
            isinstance(snapshot, Mapping),
            "UserIntegrationSecondPassNondeterminism",
            f"$.deterministic_generation.{name}.snapshot",
            "generation proof lacks a corpus snapshot",
        )
        _snapshot_index(
            snapshot,
            location=f"$.deterministic_generation.{name}.snapshot",
        )
        _require(
            snapshot.get("fixture_json_file_count")
            == EXPECTED_FIXTURE_JSON_FILES,
            "UserIntegrationFixtureMismatch",
            f"$.deterministic_generation.{name}.fixture_json_file_count",
            "generation proof does not inventory every fixture JSON file",
        )
    first_snapshot = pass_1["snapshot"]
    second_snapshot = pass_2["snapshot"]
    _require_identical_snapshots(first_snapshot, second_snapshot)
    semantic_pre = pre.get("semantic_authorities")
    semantic_first = pass_1.get("semantic_authorities")
    semantic_second = pass_2.get("semantic_authorities")
    _require(
        isinstance(semantic_pre, Mapping)
        and isinstance(semantic_first, Mapping)
        and isinstance(semantic_second, Mapping),
        "UserIntegrationPredecessorEvidenceDrift",
        "$.deterministic_generation.semantic_authorities",
        "semantic authority snapshot is malformed",
    )
    _require_identical_semantics(semantic_pre, semantic_first)
    _require_identical_semantics(semantic_first, semantic_second)
    _require_identical_semantics(
        semantic_second, _semantic_authorities(arguments)
    )
    _require(
        pass_1.get("delta_from_pre_generation")
        == pass_2.get("delta_from_pre_generation"),
        "UserIntegrationPredecessorEvidenceDrift",
        "$.deterministic_generation.delta_classification",
        "pass delta classifications differ",
    )
    delta = pass_2.get("delta_from_pre_generation", [])
    _require(
        isinstance(delta, list)
        and all(
            isinstance(row, Mapping)
            and row.get(
                "semantic_identity_counts_and_contracts_changed"
            )
            is False
            and (
                not row.get("predecessor_artifact")
                or (
                    row.get("semantic_classification")
                    == "derived-hash-or-evidence-ratchet-only"
                    and row.get("byte_hash_ratcheting_only") is True
                )
            )
            for row in delta
        ),
        "UserIntegrationPredecessorEvidenceDrift",
        "$.deterministic_generation.delta_classification",
        "predecessor delta is not classified as derived evidence only",
    )
    pass_rows = _snapshot_index(
        first_snapshot,
        location="$.deterministic_generation.pass_1.snapshot",
    )
    required_paths = {
        _relative_command_path(arguments.output, arguments.repo_root),
        _relative_command_path(
            arguments.extraction_manifest, arguments.repo_root
        ),
        _relative_command_path(arguments.abi_evidence, arguments.repo_root),
        _relative_command_path(arguments.abi_symbols, arguments.repo_root),
        _relative_command_path(arguments.abi_tool, arguments.repo_root),
        _relative_command_path(arguments.abi_probe, arguments.repo_root),
        *(
            _relative_command_path(path, arguments.repo_root)
            for path in _policy_ownership_evidence_paths(arguments)
        ),
    }
    _require(
        required_paths <= set(pass_rows),
        "UserIntegrationSecondPassNondeterminism",
        "$.deterministic_generation.hash_domain",
        (
            "full corpus omits required closure/extraction/API-ABI/policy "
            "ownership paths: "
            f"{sorted(required_paths - set(pass_rows))}"
        ),
    )

    live_snapshot = _corpus_snapshot(arguments)
    _require(
        live_snapshot["fixture_json_file_count"]
        == EXPECTED_FIXTURE_JSON_FILES,
        "UserIntegrationFixtureMismatch",
        "$.deterministic_generation.live.fixture_json_file_count",
        "live corpus does not inventory every fixture JSON file",
    )
    _require_identical_snapshots(first_snapshot, live_snapshot)
    _require_identical_snapshots(second_snapshot, live_snapshot)

    first_relative = str(second_snapshot["files"][0]["path"])
    _run_snapshot_mutation_self_test(
        arguments,
        baseline=first_snapshot,
        relative_path=first_relative,
        artifact=arguments.repo_root / first_relative,
        location="$.deterministic_generation.mutation_self_test",
    )
    _run_snapshot_mutation_self_test(
        arguments,
        baseline=live_snapshot,
        relative_path=_relative_command_path(
            arguments.output, arguments.repo_root
        ),
        artifact=arguments.output,
        location=(
            "$.deterministic_generation."
            "live_artifact_mutation_self_tests.closure_report"
        ),
    )
    _run_snapshot_mutation_self_test(
        arguments,
        baseline=live_snapshot,
        relative_path=_relative_command_path(
            arguments.extraction_manifest, arguments.repo_root
        ),
        artifact=arguments.extraction_manifest,
        location=(
            "$.deterministic_generation."
            "live_artifact_mutation_self_tests.extraction_manifest"
        ),
    )
    planted_semantics = json.loads(
        json.dumps(semantic_second, ensure_ascii=False)
    )
    planted_semantics["registry_rows_sha256"] = "0" * 64
    try:
        _require_identical_semantics(semantic_first, planted_semantics)
    except ClosureError as error:
        _require(
            error.codes == ("UserIntegrationPredecessorEvidenceDrift",),
            "UserIntegrationPredecessorEvidenceDrift",
            "$.deterministic_generation.predecessor_mutation_self_test",
            "planted semantic mutation failed through an unrelated diagnostic",
        )
    else:
        _fail(
            "UserIntegrationPredecessorEvidenceDrift",
            "$.deterministic_generation.predecessor_mutation_self_test",
            "planted predecessor semantic mutation was not rejected",
        )
    _require(
        pass_2.get("comparison") == _expected_generation_comparison(),
        "UserIntegrationSecondPassNondeterminism",
        "$.deterministic_generation.comparison",
        "second-pass equality or planted-mutation evidence is stale",
    )
    _require_identical_snapshots(first_snapshot, second_snapshot)
    _require_identical_snapshots(first_snapshot, live_snapshot)
    return pre, pass_1, pass_2


def _generation_report_section(
    arguments: argparse.Namespace,
) -> dict[str, Any]:
    proof_paths = [
        _relative_command_path(path, arguments.repo_root)
        for path in (
            arguments.generation_pre,
            arguments.generation_pass_1,
            arguments.generation_pass_2,
        )
    ]
    return {
        "scope": (
            "complete inherited and PR-A generated corpus, including every "
            "fixture, evidence artifact, generated registry/descriptor/"
            "document, Codex policy ownership/configured CTest evidence, "
            "this closure report, and the extraction manifest"
        ),
        "proof_manifests": proof_paths,
        "target_corpus_metadata_exclusions": proof_paths,
        "extraction_path_only_self_reference_exclusions": proof_paths,
        "acyclicity_contract": (
            "The three proof manifests hash the target corpus and are "
            "therefore outside that hash domain. The extraction manifest "
            "records only their exact paths. This report embeds no pass "
            "aggregate, artifact hash, or byte count. The specialized check "
            "hashes all three proof files and requires the live target corpus "
            "to equal both recorded passes."
        ),
        "currentness_established_only_by": (
            "app_server_a1_4_user_integrations_closure.py check"
        ),
        "live_target_corpus_equality_required": True,
        "second_pass_diagnostic": (
            "UserIntegrationSecondPassNondeterminism"
        ),
        "predecessor_drift_diagnostic": (
            "UserIntegrationPredecessorEvidenceDrift"
        ),
        "extraction_exclusion_diagnostic": (
            "UserIntegrationExtractionProofExclusionMismatch"
        ),
        "extraction_is_unique_final_generator_step": True,
        "policy_ownership_immediately_precedes_extraction_manifest": True,
    }


def _require_published_generation_proof(
    report: Mapping[str, Any],
) -> None:
    section = report.get("deterministic_generation")
    forbidden_keys = {
        "pre_generation",
        "pass_1",
        "pass_2",
        "manifest_sha256",
        "byte_count",
        "file_count",
        "proof_current",
    }
    nested_keys: set[str] = set()

    def collect_keys(value: Any) -> None:
        if isinstance(value, Mapping):
            for key, child in value.items():
                nested_keys.add(str(key))
                collect_keys(child)
        elif isinstance(value, list):
            for child in value:
                collect_keys(child)

    collect_keys(section)
    _require(
        isinstance(section, Mapping)
        and section.get("live_target_corpus_equality_required") is True
        and section.get(
            "policy_ownership_immediately_precedes_extraction_manifest"
        )
        is True
        and not (forbidden_keys & nested_keys),
        "UserIntegrationSecondPassNondeterminism",
        "$.deterministic_generation.proof_publication",
        "closure report embeds a self-referential pass aggregate",
    )


def _validate_package_report(arguments: argparse.Namespace) -> dict[str, Any]:
    """Validate the checked report without consulting repository history."""

    report = _load_canonical_proof(arguments.output)
    mcp_reverse_successor = _has_mcp_reverse_successor_marker(
        arguments.repo_root.resolve()
    )
    if mcp_reverse_successor:
        _require(
            _sha256_bytes(arguments.output.read_bytes())
            == FROZEN_REPORT_SHA256,
            "UserIntegrationPredecessorEvidenceDrift",
            "$.package_report.successor.frozen_report",
            "the frozen PR-A closure report changed across A1.4b",
        )
    required_keys = {
        "api_abi",
        "architecture",
        "authority",
        "counts",
        "descriptors",
        "deterministic_generation",
        "exact_complete_identities",
        "fixtures",
        "format_version",
        "generated_notice",
        "history_policy",
        "notification_variants",
        "package_boundary",
        "plugin_source",
        "predecessor_evidence",
        "project",
        "public_api",
        "residual_not_implemented_identities",
        "residual_partial_identities",
        "staged_arithmetic",
    }
    _require(
        set(report) == required_keys
        and report.get("format_version") == FORMAT_VERSION
        and report.get("generated_notice")
        == (
            "Generated by tools/codex/"
            "app_server_a1_4_user_integrations_closure.py; do not edit."
        ),
        "UserIntegrationPredecessorEvidenceDrift",
        "$.package_report",
        "packaged closure report structure changed",
    )
    authority = report.get("authority")
    _require(
        isinstance(authority, Mapping)
        and authority.get("base")
        == {
            "sha": audit.EXPECTED_BASE_SHA,
            "tree": audit.EXPECTED_BASE_TREE,
        }
        and authority.get("codex_version") == audit.CODEX_VERSION
        and authority.get("upstream_tag") == audit.UPSTREAM_TAG
        and authority.get("upstream_source_commit")
        == audit.UPSTREAM_SOURCE_COMMIT
        and isinstance(authority.get("implementation_head"), Mapping)
        and authority["implementation_head"].get("subject")
        == COMMIT_5_SUBJECT,
        "UserIntegrationPredecessorEvidenceDrift",
        "$.package_report.authority",
        "packaged closure authority changed",
    )
    _require(
        report.get("history_policy") == _expected_history_policy(),
        "UserIntegrationPromotionStageMismatch",
        "$.package_report.history_policy",
        "packaged exact six-commit policy changed",
    )
    counts = report.get("counts")
    _require(
        isinstance(counts, Mapping)
        and counts.get("global_status") == FINAL_GLOBAL_STATUS
        and counts.get("native_a1_4_status") == FINAL_NATIVE_STATUS
        and counts.get("taxonomy")
        == {
            "client_requests": 23,
            "server_notifications": 6,
            "server_requests": 0,
            "tagged_union_alternatives": 4,
        }
        and counts.get("result_contracts") == {"Concrete": 20, "Unit": 3}
        and counts.get("schema_closure") == audit.EXPECTED_CLOSURE,
        "UserIntegrationFalseComplete",
        "$.package_report.counts",
        "packaged closure arithmetic changed",
    )
    _require(
        report.get("exact_complete_identities")
        == [_key_object(key) for key in _expected_keys()],
        "UserIntegrationIdentitySetMismatch",
        "$.package_report.exact_complete_identities",
        "packaged exact PR-A identity set changed",
    )
    partial_rows = report.get("residual_partial_identities")
    _require(
        isinstance(partial_rows, list)
        and all(isinstance(row, Mapping) for row in partial_rows)
        and tuple(sorted(str(row.get("name")) for row in partial_rows))
        == RESIDUAL_PARTIAL_NAMES,
        "UserIntegrationScopeLeak",
        "$.package_report.residual_partial_identities",
        "packaged residual Partial set changed",
    )
    _require(
        isinstance(report.get("residual_not_implemented_identities"), list)
        and len(report["residual_not_implemented_identities"]) == 22,
        "UserIntegrationScopeLeak",
        "$.package_report.residual_not_implemented_identities",
        "packaged residual NotImplemented set changed",
    )
    plugin = report.get("plugin_source")
    _require(
        isinstance(plugin, Mapping)
        and plugin.get("registry_order") == list(audit.PLUGIN_SOURCE_ORDER)
        and plugin.get("public_variant_order")
        == [
            "GitPluginSource",
            "LocalPluginSource",
            "NpmPluginSource",
            "RemotePluginSource",
            "UnknownPluginSource",
        ]
        and plugin.get("npm_build_or_runtime_dependency") is False,
        "UserIntegrationPluginSourceOrderMismatch",
        "$.package_report.plugin_source",
        "packaged PluginSource policy changed",
    )
    variants = report.get("notification_variants")
    _require(
        isinstance(variants, Mapping)
        and variants.get("predecessor_sizes")
        == {"CanonicalServerNotification": 51, "Event": 53}
        and variants.get("final_sizes")
        == {"CanonicalServerNotification": 57, "Event": 59}
        and variants.get("append_mapping")
        == [
            {
                "type": type_name,
                "canonical_index": canonical_index,
                "event_index": event_index,
                "method": method,
            }
            for type_name, canonical_index, event_index, method
            in audit.APPENDS
        ],
        "UserIntegrationNotificationAppendIndexMismatch",
        "$.package_report.notification_variants",
        "packaged notification append mapping changed",
    )
    project = report.get("project")
    _require(
        isinstance(project, Mapping)
        and project.get("codex_soversion") == audit.EXPECTED_SOVERSION
        and project.get("soversion_bump_deferred_to_final_a1_closure")
        is True,
        "UserIntegrationSOVERSIONDrift",
        "$.package_report.project",
        "packaged SOVERSION policy changed",
    )
    _require(
        report.get("deterministic_generation")
        == _generation_report_section(arguments),
        "UserIntegrationSecondPassNondeterminism",
        "$.package_report.deterministic_generation",
        "packaged acyclic generation-proof contract changed",
    )
    package_boundary = report.get("package_boundary")
    expected_live_package_boundary = package_boundary
    if mcp_reverse_successor and isinstance(package_boundary, Mapping):
        expected_live_package_boundary = dict(package_boundary)
        expected_live_package_boundary["installed_consumer"] = (
            MCP_REVERSE_INSTALLED_CONSUMER
        )
        _require(
            _package_boundary(arguments) == expected_live_package_boundary,
            "UserIntegrationPackageBoundaryMismatch",
            "$.package_report.package_boundary.successor",
            (
                "the exact A1.4b installed-consumer successor boundary "
                "changed"
            ),
        )
    _require(
        isinstance(package_boundary, Mapping)
        and (
            mcp_reverse_successor
            or package_boundary.get("installed_consumer")
            == _record(
                arguments.installed_consumer_source, arguments.repo_root
            )
        )
        and package_boundary.get("installed_consumer_guard")
        == _record(arguments.installed_consumer_test, arguments.repo_root)
        and package_boundary.get("installed_consumer_cmake")
        == _record(arguments.installed_consumer_cmake, arguments.repo_root)
        and package_boundary.get("source_package_guard")
        == _record(arguments.source_package_test, arguments.repo_root)
        and package_boundary.get("direct_snodec_public_header_consumer")
        is True
        and package_boundary.get("package_checks_require_no_git_history")
        is True,
        "UserIntegrationPackageBoundaryMismatch",
        "$.package_report.package_boundary",
        "packaged installed/source-package guard evidence changed",
    )
    _require_published_generation_proof(report)
    return report


def regenerate_all(arguments: argparse.Namespace) -> None:
    """Run and prove two complete authoritative generation passes."""

    _require(
        arguments.abi_library is not None
        and arguments.abi_library.is_file(),
        "UserIntegrationPredecessorEvidenceDrift",
        "$.generation_sequence.pr-a-api-abi-evidence",
        "regenerate requires an existing --abi-library",
    )
    _require(
        arguments.policy_baseline_ctest is not None
        and arguments.policy_baseline_ctest.is_file()
        and arguments.policy_final_ctest is not None
        and arguments.policy_final_ctest.is_file()
        and arguments.policy_snodec_root is not None
        and arguments.policy_snodec_root.is_dir(),
        "UserIntegrationPredecessorEvidenceDrift",
        "$.generation_sequence.codex-policy-ownership",
        (
            "regenerate requires --policy-baseline-ctest, "
            "--policy-final-ctest, and --policy-snodec-root authorities"
        ),
    )
    expected_steps = [
        _render_step(step, arguments.repo_root)
        for step in _generation_steps(arguments)
    ]
    reviewed_inputs = _reviewed_change_inputs(arguments)
    semantic_before = _semantic_authorities(arguments)
    pre_snapshot = _corpus_snapshot(arguments)
    _require(
        pre_snapshot["fixture_json_file_count"]
        == EXPECTED_FIXTURE_JSON_FILES,
        "UserIntegrationFixtureMismatch",
        "$.pre_generation.fixture_json_file_count",
        "pre-generation corpus does not inventory all fixture JSON files",
    )
    pass_1_steps, pass_1_snapshot = _run_generation_pass(
        arguments, pass_name="generation-pass-1"
    )
    semantic_after_pass_1 = _semantic_authorities(arguments)
    delta = _delta_classification(
        arguments,
        pre_snapshot,
        pass_1_snapshot,
        semantic_before,
        semantic_after_pass_1,
    )
    pass_2_steps, pass_2_snapshot = _run_generation_pass(
        arguments, pass_name="generation-pass-2"
    )
    semantic_after_pass_2 = _semantic_authorities(arguments)
    _require_identical_semantics(
        semantic_after_pass_1, semantic_after_pass_2
    )
    _require_identical_snapshots(pass_1_snapshot, pass_2_snapshot)
    _require(
        pass_1_steps == expected_steps and pass_2_steps == expected_steps,
        "UserIntegrationPredecessorEvidenceDrift",
        "$.generation_sequence",
        "executed generator sequence differs between passes",
    )

    first_relative = str(pass_2_snapshot["files"][0]["path"])
    _run_snapshot_mutation_self_test(
        arguments,
        baseline=pass_1_snapshot,
        relative_path=first_relative,
        artifact=arguments.repo_root / first_relative,
        location="$.generation_pass_2.mutation_self_test",
    )
    _require_identical_snapshots(pass_1_snapshot, pass_2_snapshot)
    planted_semantics = json.loads(
        json.dumps(semantic_after_pass_2, ensure_ascii=False)
    )
    planted_semantics["registry_rows_sha256"] = "0" * 64
    try:
        _require_identical_semantics(
            semantic_after_pass_1, planted_semantics
        )
    except ClosureError as error:
        _require(
            error.codes == ("UserIntegrationPredecessorEvidenceDrift",),
            "UserIntegrationPredecessorEvidenceDrift",
            "$.generation_pass_2.predecessor_mutation_self_test",
            "planted semantic mutation triggered an unrelated failure",
        )
    else:
        _fail(
            "UserIntegrationPredecessorEvidenceDrift",
            "$.generation_pass_2.predecessor_mutation_self_test",
            "planted predecessor semantic mutation was accepted",
        )
    _require_identical_semantics(
        semantic_after_pass_1, semantic_after_pass_2
    )

    pre_document = _proof_document(
        phase="pre-generation",
        snapshot=pre_snapshot,
        steps=expected_steps,
        semantic_authorities=semantic_before,
        reviewed_change_inputs=reviewed_inputs,
    )
    pass_1_document = _proof_document(
        phase="generation-pass-1",
        snapshot=pass_1_snapshot,
        steps=pass_1_steps,
        semantic_authorities=semantic_after_pass_1,
        reviewed_change_inputs=reviewed_inputs,
        delta=delta,
    )
    pass_2_document = _proof_document(
        phase="generation-pass-2",
        snapshot=pass_2_snapshot,
        steps=pass_2_steps,
        semantic_authorities=semantic_after_pass_2,
        reviewed_change_inputs=reviewed_inputs,
        delta=delta,
        comparison=_expected_generation_comparison(),
    )
    _write_proof(arguments.generation_pre, pre_document)
    _write_proof(arguments.generation_pass_1, pass_1_document)
    _write_proof(arguments.generation_pass_2, pass_2_document)

    # Publishing the three out-of-corpus proof documents must not change or
    # rewrite the target corpus.  Check the already generated static closure
    # report in place; pass 2 already generated extraction uniquely last.
    report = build_report(arguments)
    _require_published_generation_proof(report)
    write_or_check(arguments.output, report, check=True)
    _validate_generation_proof(arguments)


def build_report(arguments: argparse.Namespace) -> dict[str, Any]:
    """Build PR-A evidence or validate an exact A1.4b successor stage."""

    repo_root = arguments.repo_root.resolve()
    current_rows = surface.parse_registry_data(arguments.registry)
    mcp_reverse_plan = (
        repo_root
        / "tools/codex/app-server-evidence/0.144.6/"
        "a1-4-mcp-reverse-plan.json"
    )
    if mcp_reverse_plan.is_file():
        base_rows = surface.parse_registry_data_text(
            _git_blob_bytes(
                repo_root,
                PR_A_MERGE_SHA,
                PROTOCOL_REGISTRY_RELATIVE_PATH,
            ).decode("utf-8"),
            f"{PR_A_MERGE_SHA}:ProtocolSurfaceRegistryData.inc",
        )
        _validate_mcp_reverse_successor(current_rows, base_rows)
        _require(
            arguments.output.is_file()
            and _sha256_bytes(arguments.output.read_bytes())
            == FROZEN_REPORT_SHA256,
            "UserIntegrationPredecessorEvidenceDrift",
            "$.successor_registry.frozen_report",
            "the frozen PR-A closure report changed across A1.4b",
        )
        report = _load(arguments.output)
        _require(
            report.get("counts", {}).get("global_status")
            == FINAL_GLOBAL_STATUS
            and report.get("counts", {}).get("native_a1_4_status")
            == FINAL_NATIVE_STATUS
            and len(report.get("exact_complete_identities", ())) == 33,
            "UserIntegrationPredecessorEvidenceDrift",
            "$.successor_registry.frozen_report",
            "the frozen PR-A closure semantics changed across A1.4b",
        )
        return report

    start = _load(arguments.start_state)
    plan = _load(arguments.batch_plan)
    audit.validate_reports(start, plan)

    audit_arguments = argparse.Namespace(
        repo_root=repo_root,
        predecessor_plan=arguments.predecessor_plan,
        native_closure=arguments.native_closure,
        extraction_manifest=arguments.extraction_manifest,
    )
    rebuilt_start, rebuilt_plan = audit.build_reports(audit_arguments)
    _require(
        start == rebuilt_start and plan == rebuilt_plan,
        "UserIntegrationPredecessorEvidenceDrift",
        "$.predecessor_evidence.audit",
        "checked PR-A audit evidence is not reproduced by live authorities",
    )

    base_bytes = _git_blob_bytes(
        repo_root,
        audit.EXPECTED_BASE_SHA,
        "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc",
    )
    base_text = base_bytes.decode("utf-8")
    base_rows = surface.parse_registry_data_text(
        base_text, f"{audit.EXPECTED_BASE_SHA}:ProtocolSurfaceRegistryData.inc"
    )
    current = _index(
        current_rows,
        code="UserIntegrationIdentitySetMismatch",
        location="$.exact_complete_identities",
    )
    base = _index(
        base_rows,
        code="UserIntegrationPredecessorEvidenceDrift",
        location="$.predecessor_evidence.base_registry",
    )
    expected = set(_expected_keys())
    _require(
        set(current) == set(base) and len(current) == 387,
        "UserIntegrationScopeLeak",
        "$.exact_complete_identities",
        "registry identity denominator changed",
    )
    changed = {key for key in current if current[key] != base[key]}
    _require(
        changed == expected,
        "UserIntegrationScopeLeak",
        "$.exact_complete_identities",
        "the exact set of base-to-final registry changes is not PR A",
    )
    for key in sorted(expected):
        row = current[key]
        _require(
            row["typed_schema_status"] == "Complete"
            and row["typed_status"] == "Implemented"
            and row["runtime_disposition"] == "Typed"
            and row["runtime_target"] != "std::monostate{}"
            and all(row["schema_completeness"].values()),
            "UserIntegrationFalseComplete",
            f"$.exact_complete_identities.{key[3]}",
            "Complete row lacks a typed target or complete schema evidence",
        )

    global_status = _status(current.values())
    native_rows = [row for row in current.values() if row["a1_slice"] == "A1.4"]
    native_status_full = _status(native_rows)
    native_status = {
        key: native_status_full[key]
        for key in ("Complete", "Partial", "NotImplemented", "Total")
    }
    _require(
        global_status == FINAL_GLOBAL_STATUS
        and native_status == FINAL_NATIVE_STATUS,
        "UserIntegrationFalseComplete",
        "$.counts",
        "final native or global registry arithmetic changed",
    )
    partial = sorted(
        key for key, row in current.items()
        if row["typed_schema_status"] == "Partial"
    )
    not_implemented = sorted(
        key for key, row in current.items()
        if row["a1_slice"] == "A1.4"
        and row["typed_schema_status"] == "NotImplemented"
    )
    _require(
        tuple(sorted(key[3] for key in partial)) == RESIDUAL_PARTIAL_NAMES
        and len(not_implemented) == 22,
        "UserIntegrationScopeLeak",
        "$.residual_partial_identities",
        "residual inherited/PR-B/PR-C closure changed",
    )

    canonical = _variant(
        arguments.events_header.read_text(encoding="utf-8"),
        "CanonicalServerNotification",
    )
    events = _variant(
        arguments.events_header.read_text(encoding="utf-8"), "Event"
    )
    predecessor = start["predecessor_variants"]
    predecessor_canonical = [
        str(row["type"])
        for row in predecessor["CanonicalServerNotification"]
    ]
    predecessor_events = [str(row["type"]) for row in predecessor["Event"]]
    _require(
        len(predecessor_canonical) == 51
        and len(predecessor_events) == 53
        and canonical[:51] == predecessor_canonical
        and events[:53] == predecessor_events,
        "UserIntegrationNotificationBaseIndexMismatch",
        "$.notification_variants.predecessor_mapping",
        "a predecessor notification alternative moved or changed",
    )
    append_canonical = [row[0] for row in audit.APPENDS]
    append_events = [row[0] for row in audit.APPENDS]
    _require(
        len(canonical) == 57
        and len(events) == 59
        and canonical[51:] == append_canonical
        and events[53:] == append_events,
        "UserIntegrationNotificationAppendIndexMismatch",
        "$.notification_variants.append_mapping",
        "final notification variant size/order changed",
    )

    plugin_source = _variant(
        arguments.plugins_header.read_text(encoding="utf-8"), "PluginSource"
    )
    expected_plugin_types = [
        "GitPluginSource",
        "LocalPluginSource",
        "NpmPluginSource",
        "RemotePluginSource",
        "UnknownPluginSource",
    ]
    _require(
        plugin_source == expected_plugin_types,
        "UserIntegrationPluginSourceOrderMismatch",
        "$.plugin_source",
        "PluginSource known order or raw-preserving unknown tail changed",
    )
    plugin_codec = arguments.plugin_codec.read_text(encoding="utf-8")
    _require(
        "preserveUnknownPluginSource" in plugin_codec
        and "preserveMalformedPluginSource" in plugin_codec
        and "unknownDiscriminatorDiagnostic" in plugin_codec
        and "malformedKnownDiagnostic" in plugin_codec,
        "UserIntegrationPluginSourceOrderMismatch",
        "$.plugin_source.decode_policy",
        "future-unknown and malformed-known decode paths are not distinct",
    )
    build_files = [
        path
        for path in repo_root.rglob("CMakeLists.txt")
        if ".git" not in path.parts
        and not any(part.startswith("build") for part in path.parts)
    ]
    dependency_leaks = [
        path.relative_to(repo_root).as_posix()
        for path in build_files
        if re.search(
            r"\b(find_package|FetchContent|add_subdirectory)\s*\([^\n)]*\bnpm\b",
            path.read_text(encoding="utf-8"),
            flags=re.IGNORECASE,
        )
    ]
    _require(
        not dependency_leaks,
        "UserIntegrationPluginSourceDependencyLeak",
        "$.plugin_source.npm_build_or_runtime_dependency",
        f"npm dependency declarations found: {dependency_leaks}",
    )

    descriptors = _descriptor_evidence(arguments, current)
    public_api = _public_api_evidence(arguments)
    fixtures = _fixture_evidence(arguments)
    package_boundary = _package_boundary(arguments)
    api_abi = _api_abi_evidence(arguments)

    root_cmake = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    soversion = re.search(
        r"set\(AISUITE_CODEX_SOVERSION\s+(\d+)\)", root_cmake
    )
    project_version = re.search(
        r"project\(\s*AISuite\s+VERSION\s+([0-9.]+)",
        root_cmake,
        flags=re.DOTALL,
    )
    _require(
        soversion is not None
        and int(soversion.group(1)) == audit.EXPECTED_SOVERSION,
        "UserIntegrationSOVERSIONDrift",
        "$.project.codex_soversion",
        "Codex SOVERSION changed before final A1 closure",
    )
    _require(
        project_version is not None,
        "UserIntegrationPredecessorEvidenceDrift",
        "$.project.version",
        "AISuite project version is not parseable",
    )

    backend_command = "src/ai/openai/codex/backend/BackendCommand.h"
    backend_state = "src/ai/openai/codex/backend/BackendState.h"
    frontend = "src/ai/openai/codex/frontend"
    for relative in (backend_command, backend_state):
        current_text = (repo_root / relative).read_text(
            encoding="utf-8"
        ).strip()
        base_text_boundary = _git_blob(
            repo_root, audit.EXPECTED_BASE_SHA, relative
        )
        _require(
            current_text == base_text_boundary,
            "UserIntegrationFalseComplete",
            f"$.architecture.{relative}",
            "backend command/state public surface expanded",
        )
    frontend_unchanged = (
        subprocess.run(
            (
                "git",
                "diff",
                "--quiet",
                audit.EXPECTED_BASE_SHA,
                "--",
                frontend,
            ),
            cwd=repo_root,
            check=False,
        ).returncode
        == 0
    )
    _require(
        frontend_unchanged,
        "UserIntegrationFalseComplete",
        "$.architecture.frontend",
        "frontend protocol implementation expanded",
    )

    closure_counts = plan["schema_closure"]["counts"]
    _require(
        closure_counts == audit.EXPECTED_CLOSURE,
        "UserIntegrationSchemaClosureMismatch",
        "$.counts.schema_closure",
        "frozen 52/118/411 closure changed",
    )
    result_contracts = Counter(
        contract[2] for contract in audit.REQUEST_CONTRACTS.values()
    )
    taxonomy = {
        "client_requests": len(audit.REQUEST_CONTRACTS),
        "server_notifications": len(audit.NOTIFICATIONS),
        "server_requests": 0,
        "tagged_union_alternatives": len(audit.PLUGIN_SOURCE_ORDER),
    }

    predecessor_reports: list[dict[str, Any]] = []
    for name in ("a1-1-closure-report.json", "a1-2-closure-report.json", "a1-3-closure-report.json"):
        path = arguments.evidence_root / name
        document = _load(path)
        predecessor_reports.append(
            {
                **_record(path, repo_root),
                "semantic_counts_sha256": _sha256_json(
                    {
                        "codex_version": document.get("codex_version"),
                        "counts": document.get("counts"),
                        "upstream_tag": document.get("upstream_tag"),
                    }
                ),
            }
        )

    report: dict[str, Any] = {
        "format_version": FORMAT_VERSION,
        "generated_notice": (
            "Generated by tools/codex/"
            "app_server_a1_4_user_integrations_closure.py; do not edit."
        ),
        "authority": {
            "base": {
                "sha": audit.EXPECTED_BASE_SHA,
                "tree": audit.EXPECTED_BASE_TREE,
            },
            "implementation_head": _history_evidence(repo_root),
            "codex_version": audit.CODEX_VERSION,
            "upstream_tag": audit.UPSTREAM_TAG,
            "upstream_source_commit": audit.UPSTREAM_SOURCE_COMMIT,
            "production_status_authority": (
                "ai::openai::codex::detail::ProtocolSurfaceRegistry"
            ),
            "report_role": "non-authoritative deterministic closure evidence",
        },
        "history_policy": _history_policy(repo_root),
        "counts": {
            "global_status": global_status,
            "native_a1_4_status": native_status,
            "taxonomy": taxonomy,
            "result_contracts": dict(sorted(result_contracts.items())),
            "schema_closure": closure_counts,
        },
        "exact_complete_identities": [
            _key_object(key) for key in sorted(expected)
        ],
        "residual_partial_identities": [
            _key_object(key) for key in partial
        ],
        "residual_not_implemented_identities": [
            _key_object(key) for key in not_implemented
        ],
        "staged_arithmetic": _staged_arithmetic(repo_root, base),
        "plugin_source": {
            "registry_order": list(audit.PLUGIN_SOURCE_ORDER),
            "public_variant_order": plugin_source,
            "unknown_alternative": "UnknownPluginSource",
            "future_unknown_raw_preserving": True,
            "malformed_known_distinct": True,
            "reaching_request_roots": [
                "plugin/installed",
                "plugin/list",
                "plugin/read",
                "plugin/share/list",
            ],
            "npm_build_or_runtime_dependency": False,
        },
        "notification_variants": {
            "predecessor_sizes": {
                "CanonicalServerNotification": 51,
                "Event": 53,
            },
            "final_sizes": {
                "CanonicalServerNotification": 57,
                "Event": 59,
            },
            "predecessor_mapping": {
                "CanonicalServerNotification": predecessor[
                    "CanonicalServerNotification"
                ],
                "Event": predecessor["Event"],
            },
            "append_mapping": [
                {
                    "type": type_name,
                    "canonical_index": canonical_index,
                    "event_index": event_index,
                    "method": method,
                }
                for type_name, canonical_index, event_index, method
                in audit.APPENDS
            ],
        },
        "descriptors": descriptors,
        "public_api": public_api,
        "fixtures": fixtures,
        "package_boundary": package_boundary,
        "api_abi": api_abi,
        "architecture": {
            "raw_protocol_instances": 1,
            "pending_operation_maps": 1,
            "notification_dispatchers": 1,
            "observer_mechanisms": 1,
            "typed_client_one_pointer_pimpl": public_api[
                "client_one_pointer_pimpl"
            ],
            "backend_command_expansion": False,
            "backend_state_expansion": False,
            "frontend_protocol_expansion": False,
        },
        "project": {
            "version": project_version.group(1),
            "codex_soversion": int(soversion.group(1)),
            "soversion_bump_deferred_to_final_a1_closure": True,
        },
        "predecessor_evidence": {
            "audit_start": _record(arguments.start_state, repo_root),
            "audit_batch_plan": _record(arguments.batch_plan, repo_root),
            "base_registry_sha256": _sha256_bytes(base_bytes),
            "changed_registry_identity_count": len(changed),
            "changed_registry_identities": [
                _key_object(key) for key in sorted(changed)
            ],
            "unchanged_non_pr_a_identity_count": len(current) - len(changed),
            "predecessor_closure_reports": predecessor_reports,
            "snodec_source_commit": audit.EXPECTED_SNODEC_SOURCE,
            "snodec_source_tree": audit.EXPECTED_SNODEC_TREE,
        },
    }

    report["deterministic_generation"] = _generation_report_section(
        arguments
    )
    return json.loads(json.dumps(report, ensure_ascii=False))


def report_diagnostics(
    actual: Mapping[str, Any], expected: Mapping[str, Any]
) -> list[Diagnostic]:
    """Return stable one-section/one-code diagnostics for planted mutations."""

    diagnostics: list[Diagnostic] = []

    def compare(key: str, code: str) -> None:
        if actual.get(key) != expected.get(key):
            diagnostics.append(
                Diagnostic(code, f"$.{key}", "closure evidence changed")
            )

    compare("authority", "UserIntegrationPredecessorEvidenceDrift")
    compare("history_policy", "UserIntegrationPromotionStageMismatch")

    actual_counts = actual.get("counts")
    expected_counts = expected.get("counts")
    if not isinstance(actual_counts, Mapping):
        actual_counts = {}
    if not isinstance(expected_counts, Mapping):
        expected_counts = {}
    for field, code in (
        ("global_status", "UserIntegrationFalseComplete"),
        ("native_a1_4_status", "UserIntegrationFalseComplete"),
        ("taxonomy", "UserIntegrationIdentitySetMismatch"),
        ("result_contracts", "UserIntegrationResultContractMismatch"),
        ("schema_closure", "UserIntegrationSchemaClosureMismatch"),
    ):
        if actual_counts.get(field) != expected_counts.get(field):
            diagnostics.append(
                Diagnostic(
                    code, f"$.counts.{field}", "closure count changed"
                )
            )

    compare(
        "exact_complete_identities",
        "UserIntegrationIdentitySetMismatch",
    )
    compare("residual_partial_identities", "UserIntegrationScopeLeak")
    compare(
        "residual_not_implemented_identities",
        "UserIntegrationScopeLeak",
    )
    compare(
        "staged_arithmetic",
        "UserIntegrationStageArithmeticMismatch",
    )
    compare("plugin_source", "UserIntegrationPluginSourceOrderMismatch")
    if actual.get("plugin_source") != expected.get("plugin_source"):
        actual_plugin = actual.get("plugin_source")
        expected_plugin = expected.get("plugin_source")
        if (
            isinstance(actual_plugin, Mapping)
            and isinstance(expected_plugin, Mapping)
            and actual_plugin.get("npm_build_or_runtime_dependency")
            != expected_plugin.get("npm_build_or_runtime_dependency")
        ):
            diagnostics = [
                row
                for row in diagnostics
                if row.location != "$.plugin_source"
            ]
            diagnostics.append(
                Diagnostic(
                    "UserIntegrationPluginSourceDependencyLeak",
                    "$.plugin_source.npm_build_or_runtime_dependency",
                    "npm dependency invariant changed",
                )
            )

    actual_variants = actual.get("notification_variants")
    expected_variants = expected.get("notification_variants")
    if not isinstance(actual_variants, Mapping):
        actual_variants = {}
    if not isinstance(expected_variants, Mapping):
        expected_variants = {}
    predecessor_changed = any(
        actual_variants.get(field) != expected_variants.get(field)
        for field in ("predecessor_sizes", "predecessor_mapping")
    )
    append_changed = any(
        actual_variants.get(field) != expected_variants.get(field)
        for field in ("final_sizes", "append_mapping")
    )
    if predecessor_changed:
        diagnostics.append(
            Diagnostic(
                "UserIntegrationNotificationBaseIndexMismatch",
                "$.notification_variants.predecessor_mapping",
                "predecessor notification mapping changed",
            )
        )
    if append_changed:
        diagnostics.append(
            Diagnostic(
                "UserIntegrationNotificationAppendIndexMismatch",
                "$.notification_variants.append_mapping",
                "appended notification mapping changed",
            )
        )

    compare("descriptors", "UserIntegrationDescriptorMismatch")
    compare("public_api", "UserIntegrationDescriptorMismatch")
    compare("fixtures", "UserIntegrationFixtureMismatch")
    actual_package = actual.get("package_boundary")
    expected_package = expected.get("package_boundary")
    if actual_package != expected_package:
        if (
            isinstance(actual_package, Mapping)
            and isinstance(expected_package, Mapping)
        ):
            changed_fields = {
                key
                for key in set(actual_package) | set(expected_package)
                if actual_package.get(key) != expected_package.get(key)
            }
        else:
            changed_fields = set()
        if changed_fields and changed_fields <= {
            "installed_consumer",
            "installed_consumer_guard",
        }:
            diagnostics.append(
                Diagnostic(
                    "UserIntegrationInstalledConsumerNotInstalled",
                    "$.package_boundary.installed_consumer",
                    "installed-consumer evidence changed",
                )
            )
        elif changed_fields == {"cross_repo_dependency"}:
            diagnostics.append(
                Diagnostic(
                    "UserIntegrationCrossRepoDependencyMismatch",
                    "$.package_boundary.cross_repo_dependency",
                    "cross-repository dependency evidence changed",
                )
            )
        else:
            diagnostics.append(
                Diagnostic(
                    "UserIntegrationPackageBoundaryMismatch",
                    "$.package_boundary",
                    "package-boundary evidence changed",
                )
            )
    compare("api_abi", "UserIntegrationPredecessorEvidenceDrift")
    compare("architecture", "UserIntegrationFalseComplete")
    compare("project", "UserIntegrationSOVERSIONDrift")
    compare(
        "predecessor_evidence",
        "UserIntegrationPredecessorEvidenceDrift",
    )
    compare(
        "deterministic_generation",
        "UserIntegrationSecondPassNondeterminism",
    )
    for field in ("format_version", "generated_notice"):
        if actual.get(field) != expected.get(field):
            diagnostics.append(
                Diagnostic(
                    "UserIntegrationPredecessorEvidenceDrift",
                    f"$.{field}",
                    "closure metadata changed",
                )
            )
    return sorted(set(diagnostics))


def validate_report(
    actual: Mapping[str, Any], expected: Mapping[str, Any]
) -> None:
    diagnostics = report_diagnostics(actual, expected)
    if diagnostics:
        raise ClosureError(diagnostics)


def write_or_check(
    path: Path, report: Mapping[str, Any], check: bool
) -> None:
    rendered = _render(report)
    if check:
        if not path.is_file() or path.read_text(encoding="utf-8") != rendered:
            _fail(
                "UserIntegrationPredecessorEvidenceDrift",
                str(path),
                "checked PR-A closure report is stale",
            )
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(rendered, encoding="utf-8")


def parser() -> argparse.ArgumentParser:
    repo = Path(__file__).resolve().parents[2]
    evidence = repo / "tools/codex/app-server-evidence/0.144.6"
    schema = repo / "tools/codex/app-server-schema/0.144.6"
    protocol_source = (
        repo / "tools/codex/app-server-protocol-source/0.144.6"
    )
    fixtures = repo / "tools/codex/app-server-fixtures/0.144.6"
    detail = repo / "src/ai/openai/codex/detail"
    typed = repo / "src/ai/openai/codex/typed"
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument(
        "command",
        choices=("generate", "check", "check-package", "regenerate"),
    )
    result.add_argument("--repo-root", type=Path, default=repo)
    result.add_argument("--evidence-root", type=Path, default=evidence)
    result.add_argument("--schema-root", type=Path, default=schema)
    result.add_argument(
        "--schema-provenance",
        type=Path,
        default=schema / "PROVENANCE.json",
    )
    result.add_argument(
        "--surface-manifest",
        type=Path,
        default=repo / "tools/codex/app-server-surface/0.144.6.json",
    )
    result.add_argument(
        "--protocol-source-root",
        type=Path,
        default=protocol_source,
    )
    result.add_argument(
        "--protocol-provenance",
        type=Path,
        default=protocol_source / "PROVENANCE.json",
    )
    result.add_argument("--fixture-root", type=Path, default=fixtures)
    result.add_argument(
        "--start-state",
        type=Path,
        default=evidence / "a1-4-user-integrations-start-state.json",
    )
    result.add_argument(
        "--batch-plan",
        type=Path,
        default=evidence / "a1-4-user-integrations-batch-plan.json",
    )
    result.add_argument(
        "--predecessor-plan",
        type=Path,
        default=evidence / "a1-4-implementation-plan.json",
    )
    result.add_argument(
        "--native-closure",
        type=Path,
        default=evidence / "a1-4-type-closure.json",
    )
    result.add_argument(
        "--extraction-manifest",
        type=Path,
        default=repo / "docs/extraction/source-manifest.json",
    )
    result.add_argument("--policy-baseline-ctest", type=Path)
    result.add_argument("--policy-final-ctest", type=Path)
    result.add_argument("--policy-snodec-root", type=Path)
    result.add_argument(
        "--policy-ownership-output",
        type=Path,
        default=repo / POLICY_OWNERSHIP_EVIDENCE_RELATIVE_PATHS[0],
    )
    result.add_argument(
        "--registry",
        type=Path,
        default=detail / "ProtocolSurfaceRegistryData.inc",
    )
    result.add_argument(
        "--operation-descriptors",
        type=Path,
        default=detail / "ClientOperationCodecDescriptors.inc",
    )
    result.add_argument(
        "--conversation-descriptors",
        type=Path,
        default=detail / "ConversationUnionCodecDescriptors.inc",
    )
    result.add_argument(
        "--accounts-models-configuration-descriptors",
        type=Path,
        default=(
            detail
            / "AccountsModelsConfigurationUnionCodecDescriptors.inc"
        ),
    )
    result.add_argument(
        "--commands-filesystem-reviews-approvals-descriptors",
        type=Path,
        default=(
            detail
            / "CommandsFilesystemReviewsApprovalsUnionCodecDescriptors.inc"
        ),
    )
    result.add_argument(
        "--notification-descriptors",
        type=Path,
        default=detail / "ServerNotificationCodecDescriptors.inc",
    )
    result.add_argument(
        "--union-descriptors",
        type=Path,
        default=detail / "IntegrationsAndLongTailUnionCodecDescriptors.inc",
    )
    result.add_argument(
        "--server-request-descriptors",
        type=Path,
        default=detail / "ServerRequestCodecDescriptors.inc",
    )
    result.add_argument(
        "--thread-item-descriptors",
        type=Path,
        default=detail / "ThreadItemCodecDescriptors.inc",
    )
    result.add_argument(
        "--response-item-descriptors",
        type=Path,
        default=detail / "ResponseItemCodecDescriptors.inc",
    )
    result.add_argument(
        "--coverage-document",
        type=Path,
        default=repo
        / "docs/ai/openai/codex/app-server-api-coverage.md",
    )
    result.add_argument(
        "--security-document",
        type=Path,
        default=repo
        / "docs/ai/openai/codex/app-server-security-decisions.md",
    )
    result.add_argument(
        "--events-header", type=Path, default=typed / "Events.h"
    )
    result.add_argument(
        "--plugins-header", type=Path, default=typed / "Plugins.h"
    )
    result.add_argument(
        "--plugin-codec", type=Path, default=detail / "PluginCodec.cpp"
    )
    result.add_argument(
        "--client-header", type=Path, default=typed / "Client.h"
    )
    result.add_argument(
        "--codex-cmake",
        type=Path,
        default=repo / "src/ai/openai/codex/CMakeLists.txt",
    )
    result.add_argument(
        "--fixture-index",
        type=Path,
        default=fixtures / "index.json",
    )
    result.add_argument(
        "--fixture-coverage",
        type=Path,
        default=evidence / "fixture-coverage.json",
    )
    result.add_argument(
        "--schema-completeness",
        type=Path,
        default=evidence / "schema-completeness-evidence.json",
    )
    result.add_argument(
        "--component-cmake",
        type=Path,
        default=repo / "tests/component/codex/CMakeLists.txt",
    )
    result.add_argument(
        "--component-test-root",
        type=Path,
        default=repo / "tests/component/codex",
    )
    result.add_argument(
        "--installed-consumer-source",
        type=Path,
        default=repo / "tests/installed/codex/CodexTypedConsumer.cpp",
    )
    result.add_argument(
        "--installed-consumer-test",
        type=Path,
        default=repo / "tests/AISuiteInstalledConsumerTest.cmake",
    )
    result.add_argument(
        "--installed-consumer-cmake",
        type=Path,
        default=repo / "tests/installed/codex/CMakeLists.txt",
    )
    result.add_argument(
        "--source-package-test",
        type=Path,
        default=repo / "tests/AISuiteSourcePackageTest.cmake",
    )
    result.add_argument(
        "--abi-tool",
        type=Path,
        default=repo
        / "tools/codex/app_server_a1_4_user_integrations_abi.py",
    )
    result.add_argument(
        "--abi-probe",
        type=Path,
        default=repo
        / "tests/installed/codex/"
        "CodexA14UserIntegrationsAbiLayoutProbe.cpp",
    )
    result.add_argument(
        "--abi-evidence",
        type=Path,
        default=evidence
        / "a1-4-user-integrations-api-abi-evidence.json",
    )
    result.add_argument(
        "--abi-symbols",
        type=Path,
        default=evidence / "a1-4-user-integrations-symbols.txt",
    )
    result.add_argument("--abi-library", type=Path)
    result.add_argument("--abi-compiler", default="g++")
    result.add_argument(
        "--generation-pre",
        type=Path,
        default=evidence / GENERATION_PROOF_FILENAMES[0],
    )
    result.add_argument(
        "--generation-pass-1",
        type=Path,
        default=evidence / GENERATION_PROOF_FILENAMES[1],
    )
    result.add_argument(
        "--generation-pass-2",
        type=Path,
        default=evidence / GENERATION_PROOF_FILENAMES[2],
    )
    result.add_argument(
        "--output",
        type=Path,
        default=evidence / "a1-4-user-integrations-closure-report.json",
    )
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    for name, value in vars(arguments).items():
        if isinstance(value, Path):
            setattr(arguments, name, value.resolve())
    if arguments.command == "regenerate":
        regenerate_all(arguments)
        return 0
    if arguments.command == "check-package":
        _validate_package_report(arguments)
        if not _has_mcp_reverse_successor_marker(
            arguments.repo_root.resolve()
        ):
            _validate_generation_proof(arguments, package_safe=True)
        return 0
    if (
        arguments.command == "check"
        and not _has_mcp_reverse_successor_marker(
            arguments.repo_root.resolve()
        )
    ):
        _validate_generation_proof(arguments)
    report = build_report(arguments)
    if arguments.command == "check":
        _require_published_generation_proof(report)
    write_or_check(
        arguments.output,
        report,
        check=arguments.command == "check",
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        ClosureError,
        audit.AuditError,
        surface.SurfaceError,
        OSError,
        subprocess.CalledProcessError,
    ) as error:
        codes = getattr(error, "codes", ())
        code = codes[0] if codes else getattr(
            error, "code", "UserIntegrationPredecessorEvidenceDrift"
        )
        print(
            f"app-server-a1-4-user-integrations-closure: "
            f"error [{code}]: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
