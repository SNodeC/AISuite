#!/usr/bin/env python3
"""Generate and verify concise Codex A1.4b closure evidence.

The production ProtocolSurfaceRegistry remains the status authority.  This
tool checks that authority, the frozen A1.4b audit, public variants and API,
dependency separation, and the bounded six-commit history.  Its JSON output is
only a deterministic review summary; it is not a second registry.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence

sys.dont_write_bytecode = True

import app_server_a1_4_mcp_reverse as audit
import app_server_surface as surface


FORMAT_VERSION = 1
BASE_SHA = "0c3a5838359eb283aca67840325ce6019345b462"
BASE_TREE = "f86196b41d695f7165dca6a80ec017a8b9166de1"
CLEAN_SNODEC_SHA = "77415c71a87fb7955e9a050bedaca02b65754324"
CLEAN_SNODEC_TREE = "2d39c334f12c308828936656c820447bfcc38d47"
PROVENANCE_SHA = "d18b231a1d2ec2235fd6f204786b0a761cc24ff5"
PROVENANCE_TREE = "88a63edc985a851b2b76b0c56df19fae74ea8069"

COMMIT_SUBJECTS = audit.SIX_COMMIT_SUBJECTS
FINAL_GLOBAL = {
    "Complete": 326,
    "Partial": 3,
    "NotImplemented": 10,
    "NotApplicable": 48,
    "Total": 387,
}
FINAL_NATIVE = {
    "Complete": 46,
    "Partial": 0,
    "NotImplemented": 10,
    "Total": 56,
}
FINAL_PARTIALS = ("error", "initialize", "initialized")
ELICITATION_TYPES = (
    "McpElicitationForm",
    "McpElicitationOpenAiForm",
    "McpElicitationUrl",
    "UnknownMcpElicitation",
)
CANONICAL_NOTIFICATION_TAIL = (
    "McpServerOauthLoginCompletedNotification",
    "McpServerStatusUpdatedNotification",
)
EVENT_TAIL = CANONICAL_NOTIFICATION_TAIL
REQUEST_ORDER = (
    "CommandApprovalRequest",
    "FileChangeApprovalRequest",
    "UserInputRequest",
    "AuthenticationRequest",
    "UnknownServerRequest",
    "ApplyPatchApprovalRequest",
    "ExecCommandApprovalRequest",
    "PermissionsApprovalRequest",
    "AttestationGenerateRequest",
    "DynamicToolCallRequest",
    "McpServerElicitationRequest",
)
PR_C_KEYS = frozenset(audit.PR_C_IDENTITIES)
INHERITED_KEYS = frozenset(audit.INHERITED_PARTIALS)


def key(
    category: str,
    domain: str,
    field: str,
    name: str,
) -> tuple[str, str, str, str]:
    return category, domain, field, name


SCOPE_KEYS = frozenset(
    [
        *(
            key("client_request", "ClientRequest", "method", name)
            for name, _params, _result in audit.CLIENT_REQUESTS
        ),
        *(
            key("server_notification", "ServerNotification", "method", name)
            for name, _payload in audit.SERVER_NOTIFICATIONS
        ),
        *(
            key("server_request", "ServerRequest", "method", name)
            for name, *_rest in audit.SERVER_REQUESTS
        ),
        *(
            key(
                "tagged_union_discriminator",
                "McpServerElicitationRequestParams",
                "mode",
                mode,
            )
            for mode in audit.ELICITATION_MODES
        ),
    ]
)
STAGE_KEYS = (
    frozenset(
        [
            *(
                key("client_request", "ClientRequest", "method", name)
                for name, _params, _result in audit.CLIENT_REQUESTS
            ),
            *(
                key(
                    "server_notification",
                    "ServerNotification",
                    "method",
                    name,
                )
                for name, _payload in audit.SERVER_NOTIFICATIONS
            ),
        ]
    ),
    frozenset(
        key("server_request", "ServerRequest", "method", name)
        for name, *_rest in audit.SERVER_REQUESTS[:2]
    ),
    frozenset(
        {
            key(
                "server_request",
                "ServerRequest",
                "method",
                "item/tool/requestUserInput",
            ),
            key(
                "server_request",
                "ServerRequest",
                "method",
                "mcpServer/elicitation/request",
            ),
            *(
                key(
                    "tagged_union_discriminator",
                    "McpServerElicitationRequestParams",
                    "mode",
                    mode,
                )
                for mode in audit.ELICITATION_MODES
            ),
        }
    ),
)
STAGE_GLOBAL = (
    {
        "Complete": 319,
        "Partial": 4,
        "NotImplemented": 16,
        "NotApplicable": 48,
        "Total": 387,
    },
    {
        "Complete": 321,
        "Partial": 4,
        "NotImplemented": 14,
        "NotApplicable": 48,
        "Total": 387,
    },
    FINAL_GLOBAL,
)
STAGE_NATIVE = (
    {
        "Complete": 39,
        "Partial": 1,
        "NotImplemented": 16,
        "Total": 56,
    },
    {
        "Complete": 41,
        "Partial": 1,
        "NotImplemented": 14,
        "Total": 56,
    },
    FINAL_NATIVE,
)

BACKEND_COMPILE_ONLY_ADAPTATIONS = (
    {
        "path": "src/ai/openai/codex/backend/BackendCore.cpp",
        "base_blob": "a69948cfcaa85f830a89a6513d69ab28a976df46",
        "implementation_blob": "3cc582b523bc181115d0cc6f50b43e87cca6046f",
        "added_lines": 9,
        "deleted_lines": 0,
        "effect": (
            "leave attestation, dynamic-tool, and MCP-elicitation requests "
            "owned by typed Requests"
        ),
    },
    {
        "path": "src/ai/openai/codex/backend/Reducer.cpp",
        "base_blob": "07a6fda3de03f8ed0f0ab9fd12fe91407e0ca7b0",
        "implementation_blob": "e4bd7a3bc9b6b5a79dc4261fe62abcc56f615cdf",
        "added_lines": 6,
        "deleted_lines": 0,
        "effect": "emit no BackendEvent for the two MCP notifications",
    },
    {
        "path": "src/ai/openai/codex/backend/Snapshot.cpp",
        "base_blob": "7680251915d5f37fcd86dae0a453bb82eece8dcb",
        "implementation_blob": "4057e222c2423bf165aad56f6f8ad5ecafc71dc9",
        "added_lines": 9,
        "deleted_lines": 0,
        "effect": "use the existing unknown snapshot fallback",
    },
)


@dataclass(frozen=True, order=True)
class Diagnostic:
    code: str
    location: str
    message: str


class ClosureError(RuntimeError):
    def __init__(self, diagnostics: Sequence[Diagnostic]) -> None:
        self.diagnostics = tuple(diagnostics)
        self.codes = tuple(sorted({row.code for row in diagnostics}))
        super().__init__(
            "; ".join(
                f"{row.code} at {row.location}: {row.message}"
                for row in diagnostics
            )
        )


def require(
    condition: bool,
    code: str,
    location: str,
    message: str,
) -> None:
    if not condition:
        raise ClosureError((Diagnostic(code, location, message),))


def run(repo_root: Path, *arguments: str) -> str:
    return subprocess.run(
        arguments,
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout.strip()


def load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object-valued JSON: {path}")
    return value


def render(value: Mapping[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def row_key(row: Mapping[str, Any]) -> tuple[str, str, str, str]:
    return (
        str(row["category"]),
        str(row["domain"]),
        str(row["discriminator_field"]),
        str(row["name"]),
    )


def rows_by_key(
    rows: Sequence[Mapping[str, Any]],
) -> dict[tuple[str, str, str, str], Mapping[str, Any]]:
    result = {row_key(row): row for row in rows}
    require(
        len(result) == len(rows),
        "McpReverseClosureRegistryMismatch",
        "$.registry",
        "duplicate production registry identity",
    )
    return result


def status_counts(
    rows: Sequence[Mapping[str, Any]],
) -> dict[str, int]:
    counts = Counter(str(row["typed_schema_status"]) for row in rows)
    return {
        "Complete": counts["Complete"],
        "Partial": counts["Partial"],
        "NotImplemented": counts["NotImplemented"],
        "NotApplicable": counts["NotApplicable"],
        "Total": len(rows),
    }


def native_counts(
    rows: Sequence[Mapping[str, Any]],
) -> dict[str, int]:
    native = [row for row in rows if row["a1_slice"] == "A1.4"]
    counts = Counter(str(row["typed_schema_status"]) for row in native)
    return {
        "Complete": counts["Complete"],
        "Partial": counts["Partial"],
        "NotImplemented": counts["NotImplemented"],
        "Total": len(native),
    }


def registry_at(repo_root: Path, revision: str) -> list[dict[str, Any]]:
    relative = "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc"
    return surface.parse_registry_data_text(
        run(repo_root, "git", "show", f"{revision}:{relative}"),
        f"{revision}:{relative}",
    )


def changed_registry_keys(
    before: Sequence[Mapping[str, Any]],
    after: Sequence[Mapping[str, Any]],
) -> frozenset[tuple[str, str, str, str]]:
    left = rows_by_key(before)
    right = rows_by_key(after)
    require(
        left.keys() == right.keys(),
        "McpReverseClosureRegistryMismatch",
        "$.registry",
        "registry identity set changed",
    )
    return frozenset(name for name in left if left[name] != right[name])


def parse_variant(source: str, alias: str) -> tuple[str, ...]:
    match = re.search(
        rf"using\s+{re.escape(alias)}\s*=\s*std::variant<(?P<body>.*?)>;",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        raise ValueError(f"unable to locate public variant {alias}")
    return tuple(
        re.sub(r"\s+", "", value)
        for value in match.group("body").split(",")
        if value.strip()
    )


def struct_body(source: str, name: str) -> str:
    match = re.search(
        rf"struct\s+{re.escape(name)}\s*\{{(?P<body>.*?)\n\s*\}};",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        raise ValueError(f"unable to locate public struct {name}")
    return match.group("body")


def normalized(source: str) -> str:
    return re.sub(r"\s+", " ", source)


def validate_history(repo_root: Path, *, require_final: bool) -> None:
    require(
        run(repo_root, "git", "rev-parse", BASE_SHA) == BASE_SHA
        and run(repo_root, "git", "show", "-s", "--format=%T", BASE_SHA)
        == BASE_TREE
        and run(repo_root, "git", "merge-base", BASE_SHA, "HEAD") == BASE_SHA,
        "McpReverseClosureBaseMismatch",
        "$.authority.base",
        "AISuite base SHA or tree changed",
    )
    rows = run(
        repo_root,
        "git",
        "log",
        "--reverse",
        "--format=%H%x09%P%x09%s",
        f"{BASE_SHA}..HEAD",
    ).splitlines()
    parsed = [tuple(row.split("\t", 2)) for row in rows if row.count("\t") == 2]
    subjects = tuple(row[2] for row in parsed)
    actual_count = len(parsed)
    count_is_valid = (
        actual_count == 6 if require_final else actual_count in {5, 6}
    )
    expected_parent = BASE_SHA
    linear = True
    for sha, parents, _subject in parsed:
        if parents != expected_parent:
            linear = False
            break
        expected_parent = sha
    require(
        count_is_valid
        and int(
            run(
                repo_root,
                "git",
                "rev-list",
                "--count",
                f"{BASE_SHA}..HEAD",
            )
        )
        == actual_count
        and linear
        and subjects == COMMIT_SUBJECTS[:actual_count],
        "McpReverseClosureHistoryMismatch",
        "$.history",
        (
            "final history must contain the exact six subjects"
            if require_final
            else (
                "construction history must contain the exact five-subject "
                "prefix or completed six-subject history"
            )
        ),
    )

    revisions = [BASE_SHA, *(row[0] for row in parsed)]
    registry_rows = [registry_at(repo_root, revision) for revision in revisions]
    require(
        not changed_registry_keys(registry_rows[0], registry_rows[1])
        and not changed_registry_keys(registry_rows[1], registry_rows[2]),
        "McpReverseClosurePromotionStageMismatch",
        "$.history.commits[0:2]",
        "Commit 1 or Commit 2 changed the registry",
    )
    for offset, expected_keys in enumerate(STAGE_KEYS, start=3):
        actual = changed_registry_keys(
            registry_rows[offset - 1],
            registry_rows[offset],
        )
        require(
            actual == expected_keys
            and status_counts(registry_rows[offset]) == STAGE_GLOBAL[offset - 3]
            and native_counts(registry_rows[offset]) == STAGE_NATIVE[offset - 3],
            "McpReverseClosurePromotionStageMismatch",
            f"$.history.commits[{offset - 1}]",
            f"Commit {offset} identity ownership or arithmetic changed",
        )

    if actual_count < 6:
        return
    require(
        not changed_registry_keys(registry_rows[5], registry_rows[6]),
        "McpReverseClosureCommit6Correction",
        "$.history.commits[5]",
        "Commit 6 changed the production registry",
    )
    commit_5, commit_6 = parsed[4][0], parsed[5][0]
    changed_paths = run(
        repo_root,
        "git",
        "diff",
        "--name-only",
        f"{commit_5}..{commit_6}",
    ).splitlines()
    forbidden_prefixes = (
        "src/",
        ".github/workflows/",
        "cmake/",
        "docs/extraction/",
        "tools/extraction/",
    )
    forbidden_files = {
        "tests/AISuiteInstalledConsumerTest.cmake",
        "CMakeLists.txt",
        "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc",
    }
    generated_manifest = "docs/extraction/source-manifest.json"
    require(
        not any(
            path != generated_manifest
            and (
                path in forbidden_files
                or any(path.startswith(prefix) for prefix in forbidden_prefixes)
            )
            for path in changed_paths
        ),
        "McpReverseClosureCommit6Correction",
        "$.history.commits[5]",
        "Commit 6 contains a production, dependency, or extraction correction",
    )


def validate_audit(arguments: argparse.Namespace) -> dict[str, Any]:
    audit_arguments = audit.parser().parse_args(
        [
            "generate",
            "--repo-root",
            str(arguments.repo_root),
            "--schema-root",
            str(arguments.schema_root),
            "--draft07-validator",
            str(arguments.draft07_validator),
            "--schema-provenance",
            str(arguments.schema_provenance),
            "--predecessor-plan",
            str(arguments.predecessor_plan),
            "--ownership-evidence",
            str(arguments.ownership),
            "--start-state",
            str(arguments.start_state),
            "--batch-plan",
            str(arguments.batch_plan),
        ]
    )
    rebuilt_start, rebuilt_plan = audit.build_reports(audit_arguments)
    audit.validate_reports(rebuilt_start, rebuilt_plan)
    require(
        rebuilt_start == load(arguments.start_state)
        and rebuilt_plan == load(arguments.batch_plan),
        "McpReverseClosureAuditDrift",
        "$.schema_closure",
        "the pinned A1.4b audit no longer regenerates checked evidence",
    )
    counts = rebuilt_plan["schema_closure"]["counts"]
    require(
        counts == audit.EXPECTED_CLOSURE,
        "McpReverseClosureSchemaMismatch",
        "$.schema_closure",
        "the exact 18/55/204 closure or its taxonomy changed",
    )
    return counts


def validate_registry(
    repo_root: Path,
    registry_path: Path,
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    current = surface.parse_registry_data(registry_path)
    base = registry_at(repo_root, BASE_SHA)
    current_by_key = rows_by_key(current)
    base_by_key = rows_by_key(base)
    require(
        status_counts(current) == FINAL_GLOBAL
        and native_counts(current) == FINAL_NATIVE,
        "McpReverseClosureStatusMismatch",
        "$.registry.final",
        "final global or native A1.4 status arithmetic changed",
    )
    partials = tuple(
        sorted(
            str(row["name"])
            for row in current
            if row["typed_schema_status"] == "Partial"
        )
    )
    require(
        partials == FINAL_PARTIALS
        and all(current_by_key[name] == base_by_key[name] for name in INHERITED_KEYS),
        "McpReverseClosurePartialMismatch",
        "$.registry.remaining_partials",
        "the three inherited A1.0 partial identities changed",
    )
    require(
        changed_registry_keys(base, current) == SCOPE_KEYS,
        "McpReverseClosureScopeMismatch",
        "$.scope.identities",
        "registry changes differ from the exact 13-identity A1.4b scope",
    )
    require(
        all(
            current_by_key[name]["typed_schema_status"] == "Complete"
            and current_by_key[name]["runtime_disposition"] == "Typed"
            and current_by_key[name]["typed_status"] == "Implemented"
            and all(current_by_key[name]["schema_completeness"].values())
            for name in SCOPE_KEYS
        ),
        "McpReverseClosureScopeMismatch",
        "$.scope.identities",
        "an A1.4b identity lacks complete typed runtime/schema evidence",
    )
    inventory = [
        row for row in current if row["a1_slice"] == "InventoryOnly"
    ]
    require(
        len(inventory) == 48
        and all(row["typed_schema_status"] == "NotApplicable" for row in inventory)
        and all(current_by_key[row_key(row)] == base_by_key[row_key(row)] for row in inventory),
        "McpReverseClosureInventoryMismatch",
        "$.boundaries.inventory_only",
        "InventoryOnly identity status or content changed",
    )
    require(
        all(current_by_key[name] == base_by_key[name] for name in PR_C_KEYS),
        "McpReverseClosurePrCMismatch",
        "$.boundaries.pr_c",
        "a PR-C registry identity changed",
    )
    identities = [
        {
            "category": name[0],
            "domain": name[1],
            "discriminator_field": name[2],
            "name": name[3],
        }
        for name in sorted(SCOPE_KEYS)
    ]
    return current, identities


def validate_variants(arguments: argparse.Namespace) -> None:
    events = arguments.events.read_text(encoding="utf-8")
    requests = arguments.requests.read_text(encoding="utf-8")
    canonical = parse_variant(events, "CanonicalServerNotification")
    event = parse_variant(events, "Event")
    typed_requests = parse_variant(requests, "TypedServerRequest")
    elicitation = parse_variant(requests, "McpElicitation")
    require(
        len(canonical) == 59
        and canonical[-2:] == CANONICAL_NOTIFICATION_TAIL
        and len(event) == 61
        and event[-2:] == EVENT_TAIL,
        "McpReverseClosureNotificationIndexMismatch",
        "$.variants.notifications",
        "notification alternatives are not appended at 57-58 and 59-60",
    )
    require(
        typed_requests == REQUEST_ORDER,
        "McpReverseClosureRequestIndexMismatch",
        "$.variants.server_requests",
        "TypedServerRequest order, preserved indices, or final size changed",
    )
    require(
        elicitation == ELICITATION_TYPES,
        "McpReverseClosureElicitationMismatch",
        "$.elicitation_union",
        "elicitation known/future alternative order changed",
    )
    params = struct_body(requests, "McpServerElicitationRequestParams")
    user_input = struct_body(requests, "ToolRequestUserInputParams")
    require(
        "McpElicitation elicitation;" in params
        and "std::string serverName;" in params
        and "ThreadId threadId;" in params
        and "OptionalNullable<TurnId> turnId;" in params
        and not re.search(r"\bmode\b", user_input),
        "McpReverseClosureElicitationMismatch",
        "$.elicitation_union.owner",
        "elicitation alternatives moved into tool user input",
    )


def require_tokens(path: Path, tokens: Sequence[str], code: str) -> str:
    source = path.read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in source]
    require(
        not missing,
        code,
        str(path),
        f"required contract token is missing: {missing[0] if missing else ''}",
    )
    return source


def validate_api(arguments: argparse.Namespace) -> None:
    mcp = normalized(
        require_tokens(
            arguments.mcp,
            (
                "class Mcp",
                "Submission startOauthLogin(",
                "Submission readResource(",
                "Submission callTool(",
                "Submission listServers(",
            ),
            "McpReverseClosureMcpApiMismatch",
        )
    )
    for signature in (
        "Submission startOauthLogin(McpServerOauthLoginParams params, StartOauthLoginResultHandler handler);",
        "Submission readResource(McpResourceReadParams params, ReadResourceResultHandler handler);",
        "Submission callTool(McpServerToolCallParams params, CallToolResultHandler handler);",
        "Submission listServers(ListMcpServerStatusParams params, ListServersResultHandler handler);",
    ):
        require(
            signature in mcp,
            "McpReverseClosureMcpApiMismatch",
            "$.public_api.mcp",
            f"canonical typed method changed: {signature}",
        )
    client = require_tokens(
        arguments.client,
        (
            "Mcp& mcp() noexcept;",
            "const Mcp& mcp() const noexcept;",
            "std::unique_ptr<Impl> impl;",
        ),
        "McpReverseClosureMcpApiMismatch",
    )
    require(
        client.count("std::unique_ptr<Impl> impl;") == 1,
        "McpReverseClosureArchitectureMismatch",
        "$.architecture.typed_client_pimpl",
        "typed::Client no longer has exactly one PIMPL pointer",
    )
    request_source = normalized(arguments.requests.read_text(encoding="utf-8"))
    for signature in (
        "SendResult respond(const AttestationGenerateRequest& request, AttestationGenerateResponse response);",
        "SendResult respond(const DynamicToolCallRequest& request, DynamicToolCallResponse response);",
        "SendResult respond(const UserInputRequest& request, ToolRequestUserInputResponse response);",
        "SendResult respond(const UserInputRequest& request, std::vector<UserInputAnswer> answers);",
        "SendResult respond(const McpServerElicitationRequest& request, McpServerElicitationRequestResponse response);",
        "SendResult reject(const AttestationGenerateRequest& request, ProtocolError error);",
        "SendResult reject(const DynamicToolCallRequest& request, ProtocolError error);",
        "SendResult reject(const UserInputRequest& request, ProtocolError error);",
        "SendResult reject(const McpServerElicitationRequest& request, ProtocolError error);",
    ):
        require(
            signature in request_source,
            "McpReverseClosureRequestApiMismatch",
            "$.public_api.reverse_requests",
            f"canonical reverse-request method changed: {signature}",
        )
    require_tokens(
        arguments.requests,
        (
            'return {"accept"};',
            'return {"decline"};',
            'return {"cancel"};',
            "OptionalNullable<Json> content;",
            "OptionalNullable<Json> meta;",
            "ToolRequestUserInputParams canonicalParams;",
        ),
        "McpReverseClosureRequestApiMismatch",
    )


def validate_descriptors(arguments: argparse.Namespace) -> None:
    client = arguments.client_descriptors.read_text(encoding="utf-8")
    notifications = arguments.notification_descriptors.read_text(
        encoding="utf-8"
    )
    requests = arguments.request_descriptors.read_text(encoding="utf-8")
    for name, _params, _result in audit.CLIENT_REQUESTS:
        require(
            client.count(f'"{name}"') == 1,
            "McpReverseClosureDescriptorMismatch",
            "$.descriptors.client_operations",
            f"missing client descriptor for {name}",
        )
    for name, _payload in audit.SERVER_NOTIFICATIONS:
        require(
            notifications.count(f'"{name}"') == 1,
            "McpReverseClosureDescriptorMismatch",
            "$.descriptors.notifications",
            f"missing notification descriptor for {name}",
        )
    for name, *_rest in audit.SERVER_REQUESTS:
        require(
            requests.count(f'"{name}"') == 1,
            "McpReverseClosureDescriptorMismatch",
            "$.descriptors.server_requests",
            f"missing server-request descriptor for {name}",
        )
    require(
        requests.count("CODEX_SERVER_REQUEST_CODEC_DESCRIPTOR(") == 10,
        "McpReverseClosureDescriptorMismatch",
        "$.descriptors.server_requests",
        "final server-request descriptor denominator is not ten",
    )


def validate_dependency(arguments: argparse.Namespace) -> None:
    ownership = load(arguments.ownership)
    readiness = ownership.get("cutover_readiness", {})
    normal = ownership.get("snodec_normal_dependency", {})
    provenance = ownership.get("snodec_source_authority", {})
    require(
        readiness.get("ready") is True
        and readiness.get("snodec_cutover_performed") is True
        and readiness.get("normal_dependency_commit") == CLEAN_SNODEC_SHA
        and readiness.get("normal_dependency_tree") == CLEAN_SNODEC_TREE
        and readiness.get("extraction_provenance_commit") == PROVENANCE_SHA
        and readiness.get("extraction_provenance_tree") == PROVENANCE_TREE
        and normal.get("commit") == CLEAN_SNODEC_SHA
        and normal.get("tree") == CLEAN_SNODEC_TREE
        and normal.get("repository") == "https://github.com/SNodeC/snode.c"
        and provenance.get("commit") == PROVENANCE_SHA
        and provenance.get("tree") == PROVENANCE_TREE
        and provenance.get("repository")
        == "https://github.com/SNodeC/snode.c",
        "McpReverseClosureDependencyMismatch",
        "$.dependency",
        "cleaned dependency and immutable provenance roles changed",
    )
    workflow = require_tokens(
        arguments.workflow,
        (
            f"git -C ../snodec checkout {CLEAN_SNODEC_SHA}",
            "git -C ../snodec worktree add --detach ../snodec-provenance",
            PROVENANCE_SHA,
            "test ! -d ../snodec/src/ai",
            'AISUITE_TEST_SNODEC_SOURCE_REPOSITORY="$PWD/../snodec-provenance"',
            'CMAKE_PREFIX_PATH="$PWD/../snodec-stage"',
        ),
        "McpReverseClosureDependencyMismatch",
    )
    require(
        workflow.count(f"git -C ../snodec checkout {CLEAN_SNODEC_SHA}") == 2
        and workflow.count(
            "git -C ../snodec worktree add --detach ../snodec-provenance"
        )
        == 2
        and workflow.count(
            'CMAKE_PREFIX_PATH="$PWD/../snodec-stage"'
        )
        == 2
        and workflow.count(
            'AISUITE_TEST_SNODEC_SOURCE_REPOSITORY="$PWD/../snodec-provenance"'
        )
        == 2
        and workflow.count("test ! -d ../snodec/src/ai") == 2
        and "cmake -S ../snodec-provenance" not in workflow
        and "cmake --build ../snodec-provenance" not in workflow,
        "McpReverseClosureDependencyMismatch",
        "$.dependency.ci",
        "both CI jobs must build the cleaned checkout and never build provenance",
    )


def validate_architecture(arguments: argparse.Namespace) -> None:
    protocol = arguments.protocol.read_text(encoding="utf-8")
    counts = {
        "transport": protocol.count(
            "std::unique_ptr<detail::Transport> transport;"
        ),
        "raw_protocol": protocol.count("RawProtocol rawProtocol;"),
        "json_rpc_request_id_allocator": protocol.count(
            "std::int64_t nextRequestId = 0;"
        ),
        "client_pending_map": protocol.count(
            "std::map<std::int64_t, PendingRequest> pendingRequests;"
        ),
        "occurrence_registry": protocol.count(
            "std::map<ServerRequestId, ServerRequest> pendingServerRequests;"
        ),
        "generation": protocol.count(
            "\n        std::uint64_t connectionGeneration = 0;"
        ),
        "occurrence_token_allocator": protocol.count(
            "std::uint64_t nextServerRequestToken = 1;"
        ),
        "notification_dispatcher": protocol.count(
            "RawProtocol::NotificationHandler typedNotificationDispatcher;"
        ),
        "request_dispatcher": protocol.count(
            "RawProtocol::ServerRequestHandler typedServerRequestDispatcher;"
        ),
    }
    require(
        all(value == 1 for value in counts.values())
        and protocol.count("core::EventReceiver::atNextTick(") == 4,
        "McpReverseClosureArchitectureMismatch",
        "$.architecture",
        "the single RawProtocol/transport/occurrence lifecycle changed",
    )
    implementation_matches = [
        row.split("\t", 1)[0]
        for row in run(
            arguments.repo_root,
            "git",
            "log",
            "--format=%H%x09%s",
            f"{BASE_SHA}..HEAD",
        ).splitlines()
        if "\t" in row and row.split("\t", 1)[1] == COMMIT_SUBJECTS[4]
    ]
    require(
        len(implementation_matches) == 1,
        "McpReverseClosureHistoryMismatch",
        "$.history.commit_5",
        "unable to locate Commit 5 implementation boundary",
    )
    implementation_commit = implementation_matches[0]
    source_diff = run(
        arguments.repo_root,
        "git",
        "diff",
        "--unified=0",
        f"{BASE_SHA}..{implementation_commit}",
        "--",
        "src",
    )
    additions = "\n".join(
        line[1:]
        for line in source_diff.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )
    banned = (
        "sleep_for",
        "sleep_until",
        "std::future",
        "std::async",
        "std::thread",
        "condition_variable",
        "co_await",
    )
    require(
        not any(token in additions for token in banned),
        "McpReverseClosureArchitectureMismatch",
        "$.architecture.non_blocking",
        "A1.4b introduced a blocking wait, worker, future, or coroutine lifecycle",
    )
    changed_sources = run(
        arguments.repo_root,
        "git",
        "diff",
        "--name-only",
        f"{BASE_SHA}..{implementation_commit}",
        "--",
        "src",
    ).splitlines()
    expected_backend_paths = {
        str(row["path"]) for row in BACKEND_COMPILE_ONLY_ADAPTATIONS
    }
    backend_sources = {
        path
        for path in changed_sources
        if path.startswith("src/ai/openai/codex/backend/")
    }
    frontend_sources = {
        path
        for path in changed_sources
        if path.startswith("src/ai/openai/codex/frontend/")
    }
    require(
        all(path.startswith("src/ai/openai/codex/") for path in changed_sources)
        and not frontend_sources
        and backend_sources == expected_backend_paths,
        "McpReverseClosureBoundaryMismatch",
        "$.boundaries.product",
        (
            "A1.4b changed a non-Codex source, a frontend source, or a backend "
            "source outside the exact compile-only compatibility allowlist"
        ),
    )
    for row in BACKEND_COMPILE_ONLY_ADAPTATIONS:
        path = str(row["path"])
        base_blob = run(
            arguments.repo_root,
            "git",
            "rev-parse",
            f"{BASE_SHA}:{path}",
        )
        implementation_blob = run(
            arguments.repo_root,
            "git",
            "rev-parse",
            f"{implementation_commit}:{path}",
        )
        numstat = run(
            arguments.repo_root,
            "git",
            "diff",
            "--numstat",
            f"{BASE_SHA}..{implementation_commit}",
            "--",
            path,
        ).split("\t")
        require(
            base_blob == row["base_blob"]
            and implementation_blob == row["implementation_blob"]
            and len(numstat) == 3
            and numstat[0] == str(row["added_lines"])
            and numstat[1] == str(row["deleted_lines"])
            and numstat[2] == path,
            "McpReverseClosureBoundaryMismatch",
            f"$.boundaries.backend_compile_only_adaptations.{path}",
            "bounded backend compatibility blob or numstat changed",
        )


def validate_runtime_contract(arguments: argparse.Namespace) -> None:
    source = require_tokens(
        arguments.nine_request_test,
        (
            "codex::stdio::Client client;",
            "constexpr std::size_t RequestCount = 9;",
            "tokens.size() == RequestCount",
            "all nine exact successful responses enqueue out of order",
            "every duplicate response is rejected",
            "stale-generation response attempts are rejected with ESTALE",
            "a typed MCP client request submitted by a reverse-request callback completes asynchronously",
            "disconnect cleanup",
            "explicit shutdown retires all nine pending occurrences exactly once",
        ),
        "McpReverseClosureConcurrencyContractMismatch",
    )
    require(
        "sleep_for" not in source and "sleep(" not in source,
        "McpReverseClosureConcurrencyContractMismatch",
        "$.concurrency",
        "nine-request transport test introduced polling or sleep",
    )
    require_tokens(
        arguments.component_cmake,
        ("CodexA14NineRequestStdioTest",),
        "McpReverseClosureConcurrencyContractMismatch",
    )
    require_tokens(
        arguments.tests_cmake,
        ("CodexA14McpReverseClosureTest",),
        "McpReverseClosureConcurrencyContractMismatch",
    )


def validate_installed_consumer(arguments: argparse.Namespace) -> None:
    require_tokens(
        arguments.installed_consumer,
        (
            "#include <ai/openai/codex/typed/Mcp.h>",
            "#include <ai/openai/codex/typed/ServerRequests.h>",
            "std::variant_size_v<typed::CanonicalServerNotification> == 59",
            "std::variant_size_v<typed::Event> == 61",
            "std::variant_size_v<typed::TypedServerRequest> == 11",
            "client.typed().mcp().listServers(",
        ),
        "McpReverseClosureInstalledConsumerMismatch",
    )
    require_tokens(
        arguments.installed_consumer_cmake,
        (
            "find_package(AISuite CONFIG REQUIRED)",
            "AISuiteInstalledCodexTypedConsumer PRIVATE AISuite::OpenAICodex",
        ),
        "McpReverseClosureInstalledConsumerMismatch",
    )


def validate_documentation(arguments: argparse.Namespace) -> None:
    require_tokens(
        arguments.documentation,
        (
            "A14-McpReverse complete.",
            "Native A1.4 has ten runtime/platform identities",
            CLEAN_SNODEC_SHA,
            PROVENANCE_SHA,
            "`mcp().startOauthLogin`",
            "`mcp().readResource`",
            "`mcp().callTool`",
            "`mcp().listServers`",
            "They do not belong",
            "to tool user input.",
            "18 seed definitions, 55 reachable",
            "| Global | 326 | 3 | 10 | 48 |",
            "`serverRequest/resolved`",
            "Codex SOVERSION remains 1",
        ),
        "McpReverseClosureDocumentationMismatch",
    )
    require_tokens(
        arguments.typed_documentation,
        (
            "accessors return 19 objects",
            "client.typed().mcp().listServers",
            "A14-McpReverse is complete",
            "326 Complete / 3 Partial / 10 NotImplemented / 48 NotApplicable",
        ),
        "McpReverseClosureDocumentationMismatch",
    )


def validate_soversion(arguments: argparse.Namespace) -> None:
    cmake = arguments.root_cmake.read_text(encoding="utf-8")
    match = re.search(r"set\(AISUITE_CODEX_SOVERSION\s+(\d+)\)", cmake)
    require(
        match is not None and int(match.group(1)) == 1,
        "McpReverseClosureSOVERSIONMismatch",
        "$.project.soversion",
        "Codex SOVERSION changed before final-A1 closure",
    )


def expected_report(identities: Sequence[Mapping[str, str]]) -> dict[str, Any]:
    return {
        "format_version": FORMAT_VERSION,
        "generated_notice": (
            "Generated concise closure evidence for Codex A1.4 MCP and "
            "reverse requests; ProtocolSurfaceRegistry remains authoritative."
        ),
        "authority": {
            "aisuite_base": {"sha": BASE_SHA, "tree": BASE_TREE},
            "codex_version": audit.CODEX_VERSION,
            "upstream_tag": audit.UPSTREAM_TAG,
            "upstream_source_commit": audit.UPSTREAM_SOURCE_COMMIT,
        },
        "dependency": {
            "normal": {
                "sha": CLEAN_SNODEC_SHA,
                "tree": CLEAN_SNODEC_TREE,
                "role": "one cleaned installed production dependency",
            },
            "extraction_provenance": {
                "sha": PROVENANCE_SHA,
                "tree": PROVENANCE_TREE,
                "role": "read-only provenance; never built or linked",
            },
            "cutover_readiness": True,
            "cutover_performed": True,
        },
        "scope": {
            "identity_count": 13,
            "taxonomy": {
                "client_requests": 4,
                "server_notifications": 2,
                "server_requests": 4,
                "tagged_union_alternatives": 3,
            },
            "complete_identities": list(identities),
        },
        "schema_closure": {
            "seed_definitions": 18,
            "reachable_named_definitions": 55,
            "definition_namespaces": {"legacy": 34, "v2": 21},
            "schema_paths": 204,
            "full_taxonomy": audit.EXPECTED_CLOSURE,
        },
        "registry": {
            "stages": [
                {
                    "commit": 3,
                    "global": STAGE_GLOBAL[0],
                    "native_a1_4": STAGE_NATIVE[0],
                    "promotion_count": 6,
                },
                {
                    "commit": 4,
                    "global": STAGE_GLOBAL[1],
                    "native_a1_4": STAGE_NATIVE[1],
                    "promotion_count": 2,
                },
                {
                    "commit": 5,
                    "global": STAGE_GLOBAL[2],
                    "native_a1_4": STAGE_NATIVE[2],
                    "promotion_count": 5,
                },
            ],
            "final_global": FINAL_GLOBAL,
            "final_native_a1_4": FINAL_NATIVE,
            "remaining_partials": list(FINAL_PARTIALS),
        },
        "public_api": {
            "mcp_facade": {
                "accessor": "client.typed().mcp()",
                "methods": [
                    "startOauthLogin",
                    "readResource",
                    "callTool",
                    "listServers",
                ],
                "completion": "OperationResult<T> asynchronous callback",
                "submission": "RawProtocol::Submission",
            },
            "reverse_requests": {
                "types": [
                    "AttestationGenerateRequest",
                    "DynamicToolCallRequest",
                    "UserInputRequest",
                    "McpServerElicitationRequest",
                ],
                "response_methods": ["respond", "reject"],
                "elicitation_actions": ["accept", "decline", "cancel"],
            },
            "installed_consumer": {
                "find_package": "AISuite CONFIG REQUIRED",
                "target": "AISuite::OpenAICodex",
                "headers": [
                    "ai/openai/codex/typed/Mcp.h",
                    "ai/openai/codex/typed/ServerRequests.h",
                ],
            },
        },
        "variants": {
            "CanonicalServerNotification": {
                "size": 59,
                "indices": {
                    "57": CANONICAL_NOTIFICATION_TAIL[0],
                    "58": CANONICAL_NOTIFICATION_TAIL[1],
                },
            },
            "Event": {
                "size": 61,
                "indices": {
                    "59": EVENT_TAIL[0],
                    "60": EVENT_TAIL[1],
                },
            },
            "TypedServerRequest": {
                "size": 11,
                "preserved": {
                    "UserInputRequest": 2,
                    "UnknownServerRequest": 4,
                },
                "appended": {
                    "AttestationGenerateRequest": 8,
                    "DynamicToolCallRequest": 9,
                    "McpServerElicitationRequest": 10,
                },
            },
        },
        "elicitation_union": {
            "owner": "McpServerElicitationRequestParams",
            "discriminator": "mode",
            "order": list(ELICITATION_TYPES),
            "tool_user_input_has_mode": False,
        },
        "architecture": {
            "event_loop_native": True,
            "non_blocking": True,
            "transport_instances": 1,
            "raw_protocol_instances": 1,
            "json_rpc_request_id_allocators": 1,
            "client_pending_maps": 1,
            "server_request_occurrence_registries": 1,
            "occurrence_token_allocators": 1,
            "transport_generations": 1,
            "notification_dispatchers": 1,
            "server_request_dispatchers": 1,
            "observer_mechanisms": 1,
            "callback_schedulers": 1,
            "second_lifecycle_engines": 0,
        },
        "concurrency": {
            "transport": "real SNode.C-managed stdio descriptors",
            "simultaneous_reverse_requests": 9,
            "registered_test": "CodexA14NineRequestStdioTest",
            "source_contract": "mechanically verified",
            "execution_status": (
                "not claimed by source-only closure; required from final-head "
                "CTest and exact-head CI"
            ),
        },
        "boundaries": {
            "pr_c_unchanged": True,
            "final_a1_partials_unchanged": True,
            "inventory_only_unchanged": 48,
            "backend_product_expansion": False,
            "backend_compile_only_adaptations": [
                dict(row) for row in BACKEND_COMPILE_ONLY_ADAPTATIONS
            ],
            "frontend_product_expansion": False,
        },
        "history": {
            "base": BASE_SHA,
            "commit_count": 6,
            "subjects": list(COMMIT_SUBJECTS),
            "registry_promotion_commits": [3, 4, 5],
            "commit_6_production_correction": False,
        },
        "project": {
            "codex_soversion": 1,
            "soversion_change_deferred_to_final_a1_closure": True,
        },
    }


def build_report(
    arguments: argparse.Namespace,
    *,
    require_final_history: bool,
) -> dict[str, Any]:
    validate_history(
        arguments.repo_root,
        require_final=require_final_history,
    )
    validate_audit(arguments)
    _rows, identities = validate_registry(
        arguments.repo_root,
        arguments.registry,
    )
    validate_dependency(arguments)
    validate_variants(arguments)
    validate_api(arguments)
    validate_descriptors(arguments)
    validate_architecture(arguments)
    validate_runtime_contract(arguments)
    validate_installed_consumer(arguments)
    validate_documentation(arguments)
    validate_soversion(arguments)
    return expected_report(identities)


SECTION_CODES = {
    "authority": "McpReverseClosureAuthorityMismatch",
    "dependency": "McpReverseClosureDependencyMismatch",
    "scope": "McpReverseClosureScopeMismatch",
    "schema_closure": "McpReverseClosureSchemaMismatch",
    "registry": "McpReverseClosureStatusMismatch",
    "public_api": "McpReverseClosureApiMismatch",
    "variants": "McpReverseClosureVariantMismatch",
    "elicitation_union": "McpReverseClosureElicitationMismatch",
    "architecture": "McpReverseClosureArchitectureMismatch",
    "concurrency": "McpReverseClosureConcurrencyContractMismatch",
    "boundaries": "McpReverseClosureBoundaryMismatch",
    "history": "McpReverseClosureHistoryMismatch",
    "project": "McpReverseClosureSOVERSIONMismatch",
}


def report_diagnostics(
    actual: Mapping[str, Any],
    expected: Mapping[str, Any],
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    if actual.get("format_version") != expected.get("format_version"):
        diagnostics.append(
            Diagnostic(
                "McpReverseClosureFormatMismatch",
                "$.format_version",
                "closure format version changed",
            )
        )
    if actual.get("generated_notice") != expected.get("generated_notice"):
        diagnostics.append(
            Diagnostic(
                "McpReverseClosureFormatMismatch",
                "$.generated_notice",
                "generated notice changed",
            )
        )
    for section, code in SECTION_CODES.items():
        if actual.get(section) != expected.get(section):
            diagnostics.append(
                Diagnostic(
                    code,
                    f"$.{section}",
                    f"{section} closure evidence changed",
                )
            )
    extras = set(actual) - {
        "format_version",
        "generated_notice",
        *SECTION_CODES,
    }
    if extras:
        diagnostics.append(
            Diagnostic(
                "McpReverseClosureFormatMismatch",
                "$",
                "unexpected top-level closure evidence section",
            )
        )
    return sorted(diagnostics)


def validate_report(
    actual: Mapping[str, Any],
    expected: Mapping[str, Any],
) -> None:
    diagnostics = report_diagnostics(actual, expected)
    if diagnostics:
        raise ClosureError(diagnostics)


def parser() -> argparse.ArgumentParser:
    repo = Path(__file__).resolve().parents[2]
    evidence = repo / "tools/codex/app-server-evidence/0.144.6"
    schema = repo / "tools/codex/app-server-schema/0.144.6"
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("command", choices=("generate", "check"))
    result.add_argument("--repo-root", type=Path, default=repo)
    result.add_argument("--schema-root", type=Path, default=schema)
    result.add_argument(
        "--draft07-validator",
        type=Path,
        default=repo / "tools/codex/draft07.py",
    )
    result.add_argument(
        "--schema-provenance",
        type=Path,
        default=schema / "PROVENANCE.json",
    )
    result.add_argument(
        "--predecessor-plan",
        type=Path,
        default=evidence / "a1-4-user-integrations-batch-plan.json",
    )
    result.add_argument(
        "--ownership",
        type=Path,
        default=repo / "docs/extraction/codex-policy-ownership.json",
    )
    result.add_argument(
        "--start-state",
        type=Path,
        default=evidence / "a1-4-mcp-reverse-start-state.json",
    )
    result.add_argument(
        "--batch-plan",
        type=Path,
        default=evidence / "a1-4-mcp-reverse-plan.json",
    )
    result.add_argument(
        "--registry",
        type=Path,
        default=repo
        / "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc",
    )
    result.add_argument(
        "--events",
        type=Path,
        default=repo / "src/ai/openai/codex/typed/Events.h",
    )
    result.add_argument(
        "--requests",
        type=Path,
        default=repo / "src/ai/openai/codex/typed/ServerRequests.h",
    )
    result.add_argument(
        "--mcp",
        type=Path,
        default=repo / "src/ai/openai/codex/typed/Mcp.h",
    )
    result.add_argument(
        "--client",
        type=Path,
        default=repo / "src/ai/openai/codex/typed/Client.h",
    )
    result.add_argument(
        "--protocol",
        type=Path,
        default=repo / "src/ai/openai/codex/AppServerClient.cpp",
    )
    result.add_argument(
        "--client-descriptors",
        type=Path,
        default=repo
        / "src/ai/openai/codex/detail/ClientOperationCodecDescriptors.inc",
    )
    result.add_argument(
        "--notification-descriptors",
        type=Path,
        default=repo
        / "src/ai/openai/codex/detail/ServerNotificationCodecDescriptors.inc",
    )
    result.add_argument(
        "--request-descriptors",
        type=Path,
        default=repo
        / "src/ai/openai/codex/detail/ServerRequestCodecDescriptors.inc",
    )
    result.add_argument(
        "--workflow",
        type=Path,
        default=repo / ".github/workflows/ci.yml",
    )
    result.add_argument(
        "--root-cmake",
        type=Path,
        default=repo / "CMakeLists.txt",
    )
    result.add_argument(
        "--component-cmake",
        type=Path,
        default=repo / "tests/component/codex/CMakeLists.txt",
    )
    result.add_argument(
        "--tests-cmake",
        type=Path,
        default=repo / "tests/CMakeLists.txt",
    )
    result.add_argument(
        "--nine-request-test",
        type=Path,
        default=repo
        / "tests/component/codex/CodexA14NineRequestStdioTest.cpp",
    )
    result.add_argument(
        "--installed-consumer",
        type=Path,
        default=repo / "tests/installed/codex/CodexTypedConsumer.cpp",
    )
    result.add_argument(
        "--installed-consumer-cmake",
        type=Path,
        default=repo / "tests/installed/codex/CMakeLists.txt",
    )
    result.add_argument(
        "--documentation",
        type=Path,
        default=repo
        / "docs/ai/openai/codex/a1-4-mcp-and-reverse-requests.md",
    )
    result.add_argument(
        "--typed-documentation",
        type=Path,
        default=repo / "docs/ai/openai/codex/typed-api.md",
    )
    result.add_argument(
        "--output",
        type=Path,
        default=evidence / "a1-4-mcp-reverse-closure-report.json",
    )
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    for name, value in vars(arguments).items():
        if isinstance(value, Path):
            setattr(arguments, name, value.resolve())
    try:
        expected = build_report(
            arguments,
            require_final_history=arguments.command == "check",
        )
        if arguments.command == "generate":
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text(render(expected), encoding="utf-8")
        else:
            actual = load(arguments.output)
            validate_report(actual, expected)
            require(
                arguments.output.read_text(encoding="utf-8") == render(expected),
                "McpReverseClosureFormatMismatch",
                str(arguments.output),
                "checked closure evidence is not canonical JSON",
            )
    except (
        ClosureError,
        OSError,
        ValueError,
        KeyError,
        subprocess.CalledProcessError,
        surface.SurfaceError,
    ) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
