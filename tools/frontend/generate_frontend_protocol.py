#!/usr/bin/env python3

"""Generate the additive Codex Frontend Protocol v1 contract artifacts.

The sole provider-inventory input is the committed registry export produced by
``tools/codex/app_server_surface.py frontend-registry``.  This generator never
parses vendored schema, Rust, TypeScript, or an installed Codex binary.
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


class GenerationError(RuntimeError):
    pass


RUNTIME_ASSERTION_KEYWORDS = frozenset(
    {
        "$ref",
        "allOf",
        "anyOf",
        "oneOf",
        "not",
        "if",
        "then",
        "else",
        "type",
        "const",
        "enum",
        "properties",
        "propertyNames",
        "additionalProperties",
        "required",
        "minProperties",
        "maxProperties",
        "items",
        "minItems",
        "maxItems",
        "uniqueItems",
        "minLength",
        "maxLength",
        "pattern",
        "minimum",
        "maximum",
        "format",
        "x-aisuite-sensitiveFieldNamesForbidden",
        "x-aisuite-forbiddenNormalizedPropertyNames",
    }
)

STRUCTURAL_SCHEMA_KEYWORDS = frozenset({"$defs"})

# The audit reports only the members of this reviewed annotation vocabulary
# that are actually reachable in the generated runtime profile.
STANDARD_ANNOTATION_KEYWORDS = frozenset(
    {
        "$schema",
        "$id",
        "title",
        "description",
        "default",
        "examples",
        "deprecated",
        "readOnly",
        "writeOnly",
        "$comment",
    }
)
CUSTOM_ANNOTATION_KEYWORDS = frozenset(
    {
        "x-aisuite-frontend-contract",
        "x-aisuite-redactionClass",
    }
)
SUPPORTED_NUMERIC_FORMATS = frozenset(
    {"int32", "int64", "uint16", "uint32", "uint", "uint64"}
)
SCHEMA_TYPES = frozenset(
    {"null", "boolean", "object", "array", "number", "integer", "string"}
)


@dataclass(frozen=True)
class RuntimeSchemaAudit:
    assertion_keywords: tuple[str, ...]
    structural_keywords: tuple[str, ...]
    annotation_keywords: tuple[str, ...]
    numeric_formats: tuple[str, ...]
    patterns: tuple[str, ...]
    unique_item_schema_count: int
    maximum_unique_item_cardinality: int
    maximum_unique_item_comparisons: int


CAPABILITIES = (
    "method_discovery",
    "security_scopes",
    "complete_provider_operations",
    "complete_reverse_requests",
    "complete_backend_domains",
    "conditional_filesystem",
    "conditional_command_execution",
    "dedicated_pending_requests",
    "dedicated_notification_events",
    "complete_thread_items",
    "authenticated_frontend",
    "scope_projected_state",
    "provider_lifecycle",
    "multi_transport",
    "cpp_client_sdk",
    "typescript_client_sdk",
    "browser_ui",
    "qt_ui",
)

MECHANISM_CAPABILITIES = frozenset(
    {
        "method_discovery",
        "security_scopes",
        "complete_provider_operations",
        "complete_reverse_requests",
        "complete_backend_domains",
        "conditional_filesystem",
        "conditional_command_execution",
        "dedicated_pending_requests",
        "dedicated_notification_events",
        "complete_thread_items",
        "authenticated_frontend",
        "scope_projected_state",
        "provider_lifecycle",
    }
)

# A1.7b implements every mechanism/build capability.  Runtime listener topology
# remains separate: ``multi_transport`` is advertised only when more than one
# distinct successfully bound transport family has been declared to the one
# FrontendService.
IMPLEMENTED_MECHANISM_CAPABILITIES = MECHANISM_CAPABILITIES

FUTURE_CAPABILITIES = frozenset(
    {
        "cpp_client_sdk",
        "typescript_client_sdk",
        "browser_ui",
        "qt_ui",
    }
)

# The provider registry description already states that command/exec rejects an
# empty argv vector, but the pinned upstream JSON Schema omitted the matching
# assertion.  Frontend Protocol validates that stable application invariant at
# its own boundary instead of admitting a command the typed provider codec must
# reject later.
PROVIDER_PARAMETER_MIN_ITEMS = {
    "command.exec": {"command": 1},
}

# The provider registry description already states that command/exec rejects an
# empty argv vector, but the pinned upstream JSON Schema omitted the matching
# assertion.  Frontend Protocol validates that stable application invariant at
# its own boundary instead of admitting a command the typed provider codec must
# reject later.
PROVIDER_PARAMETER_MIN_ITEMS = {
    "command.exec": {"command": 1},
}

EXISTING_METHODS = (
    "controller.acquire",
    "controller.release",
    "snapshot.get",
    "events.replay",
    "thread.start",
    "thread.resume",
    "thread.list",
    "thread.read",
    "turn.start",
    "turn.interrupt",
    "request.approval.respond",
    "request.userInput.respond",
    "request.authentication.respond",
    "request.unknown.respond",
    "request.unknown.reject",
)

NATIVE_METHODS = (
    {
        "id": "ControllerAcquire",
        "method": "controller.acquire",
        "category": "backend_control",
        "serviceAction": "ControllerAcquire",
        "scopes": ["control"],
        "controllerRequired": False,
        "security": "ControllerRequiredApproved",
        "capability": "method_discovery",
        "resultType": "ControllerResult",
    },
    {
        "id": "ControllerRelease",
        "method": "controller.release",
        "category": "backend_control",
        "serviceAction": "ControllerRelease",
        "scopes": ["control"],
        "controllerRequired": True,
        "security": "ControllerRequiredApproved",
        "capability": "method_discovery",
        "resultType": "ControllerResult",
    },
    {
        "id": "SnapshotGet",
        "method": "snapshot.get",
        "category": "backend_control",
        "serviceAction": "SnapshotGet",
        "scopes": ["observe"],
        "controllerRequired": False,
        "security": "PublicSynchronizationApproved",
        "capability": "method_discovery",
        "resultType": "SnapshotSyncResult",
    },
    {
        "id": "EventsReplay",
        "method": "events.replay",
        "category": "frontend_replay",
        "serviceAction": "ReplayAfter",
        "scopes": ["observe"],
        "controllerRequired": False,
        "security": "PublicSynchronizationApproved",
        "capability": "method_discovery",
        "resultType": "ReplayResult",
    },
    {
        "id": "ProviderStart",
        "method": "provider.start",
        "category": "provider_lifecycle",
        "serviceAction": "ProviderStart",
        "scopes": ["control", "provider_lifecycle"],
        "controllerRequired": True,
        "security": "PrivilegedScopedApproved",
        "capability": "provider_lifecycle",
        "resultType": "Unit",
    },
    {
        "id": "ProviderStop",
        "method": "provider.stop",
        "category": "provider_lifecycle",
        "serviceAction": "ProviderStop",
        "scopes": ["control", "provider_lifecycle"],
        "controllerRequired": True,
        "security": "PrivilegedScopedApproved",
        "capability": "provider_lifecycle",
        "resultType": "Unit",
    },
    {
        "id": "ProviderRestart",
        "method": "provider.restart",
        "category": "provider_lifecycle",
        "serviceAction": "ProviderRestart",
        "scopes": ["control", "provider_lifecycle"],
        "controllerRequired": True,
        "security": "PrivilegedScopedApproved",
        "capability": "provider_lifecycle",
        "resultType": "Unit",
    },
)

REVERSE_METHODS = (
    ("ApprovalRespond", "request.approval.respond", ("item/commandExecution/requestApproval", "item/fileChange/requestApproval"), "ApprovalRespond"),
    ("UserInputRespond", "request.userInput.respond", ("item/tool/requestUserInput",), "UserInputRespond"),
    ("AuthenticationRespond", "request.authentication.respond", ("account/chatgptAuthTokens/refresh",), "AuthenticationRespond"),
    ("UnknownRequestRespond", "request.unknown.respond", (), "UnknownRequestRespondRaw"),
    ("UnknownRequestReject", "request.unknown.reject", (), "UnknownRequestReject"),
    ("ApplyPatchApprovalRespond", "request.applyPatchApproval.respond", ("applyPatchApproval",), "ApplyPatchApprovalRespond"),
    ("ExecCommandApprovalRespond", "request.execCommandApproval.respond", ("execCommandApproval",), "ExecCommandApprovalRespond"),
    ("PermissionsApprovalRespond", "request.permissionsApproval.respond", ("item/permissions/requestApproval",), "PermissionsApprovalRespond"),
    ("AttestationRespond", "request.attestation.respond", ("attestation/generate",), "AttestationGenerateRespond"),
    ("DynamicToolRespond", "request.dynamicTool.respond", ("item/tool/call",), "DynamicToolCallRespond"),
    ("McpElicitationRespond", "request.mcpElicitation.respond", ("mcpServer/elicitation/request",), "McpServerElicitationRespond"),
    ("KnownRequestReject", "request.known.reject", ("item/tool/requestUserInput", "attestation/generate", "item/tool/call", "mcpServer/elicitation/request"), "KnownRequestReject"),
)

REVERSE_PARAMETER_SHAPES = {
    "request.approval.respond": (["pendingRequestId", "decision", "response"], ["pendingRequestId"]),
    "request.userInput.respond": (["pendingRequestId", "answers", "response"], ["pendingRequestId"]),
    "request.authentication.respond": (["pendingRequestId", "accessToken", "chatgptAccountId", "chatgptPlanType", "response"], ["pendingRequestId"]),
    "request.unknown.respond": (["pendingRequestId", "result"], ["pendingRequestId", "result"]),
    "request.unknown.reject": (["pendingRequestId", "code", "message", "data"], ["pendingRequestId", "code", "message"]),
    "request.applyPatchApproval.respond": (["pendingRequestId", "response"], ["pendingRequestId", "response"]),
    "request.execCommandApproval.respond": (["pendingRequestId", "response"], ["pendingRequestId", "response"]),
    "request.permissionsApproval.respond": (["pendingRequestId", "response"], ["pendingRequestId", "response"]),
    "request.attestation.respond": (["pendingRequestId", "response"], ["pendingRequestId", "response"]),
    "request.dynamicTool.respond": (["pendingRequestId", "response"], ["pendingRequestId", "response"]),
    "request.mcpElicitation.respond": (["pendingRequestId", "response"], ["pendingRequestId", "response"]),
    "request.known.reject": (["pendingRequestId", "error"], ["pendingRequestId", "error"]),
}

# The stable server-request inventory remains owned by the exported production
# registry.  This table supplies only the product discriminator chosen by the
# reviewed frontend contract; response method cardinality, scopes, redaction,
# and compatibility policy are derived and cross-checked from that registry.
PENDING_REQUEST_KINDS = (
    ("item/commandExecution/requestApproval", "command_execution_approval"),
    ("item/fileChange/requestApproval", "file_change_approval"),
    ("item/tool/requestUserInput", "user_input"),
    ("account/chatgptAuthTokens/refresh", "authentication"),
    ("applyPatchApproval", "apply_patch_approval"),
    ("execCommandApproval", "exec_command_approval"),
    ("item/permissions/requestApproval", "permissions_approval"),
    ("attestation/generate", "attestation"),
    ("item/tool/call", "dynamic_tool_call"),
    ("mcpServer/elicitation/request", "mcp_elicitation"),
)

SENSITIVE_PROVIDER_METHODS = frozenset(
    {
        "account.login.start",
        "account.read",
        "config.read",
        "mcpServer.oauth.login",
        "mcpServer.resource.read",
        "mcpServer.tool.call",
    }
)
LARGE_PROVIDER_METHODS = frozenset(
    {
        "app.list",
        "config.read",
        "fs.readDirectory",
        "fs.readFile",
        "fuzzyFileSearch",
        "model.list",
        "plugin.installed",
        "plugin.list",
        "skills.list",
        "thread.list",
        "thread.loaded.list",
        "thread.read",
    }
)

SCOPE_ENUM = {
    "observe": "Observe",
    "control": "Control",
    "provider_lifecycle": "ProviderLifecycle",
    "account_management": "AccountManagement",
    "configuration_write": "ConfigurationWrite",
    "command_execution": "CommandExecution",
    "filesystem_read": "FilesystemRead",
    "filesystem_write": "FilesystemWrite",
    "extension_management": "ExtensionManagement",
    "mcp_invoke": "McpInvoke",
    "sensitive_response": "SensitiveResponse",
    "unknown_request_response": "UnknownRequestResponse",
}


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GenerationError(f"unable to read {path}: {error}") from error


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def registry_key(row: dict[str, Any]) -> str:
    key = row["registryKey"]
    return f"{key['category']}:{key['domain']}:{key['field']}:{key['name']}"


def cpp_id(value: str) -> str:
    words = re.findall(r"[A-Za-z0-9]+", value)
    result = "".join(word[:1].upper() + word[1:] for word in words)
    if not result or result[0].isdigit():
        result = "Method" + result
    return result


def schema_name(method_id: str, suffix: str) -> str:
    return f"#/$defs/{method_id}{suffix}"


def validate_source(source: dict[str, Any]) -> list[dict[str, Any]]:
    if source.get("protocolIdentity") != "snodec.codex-frontend" or source.get("protocolVersion") != 1:
        raise GenerationError("registry source changed the frozen protocol identity/version")
    review = source.get("review")
    expected_review = (148, 86, 234, 0)
    actual_review = (
        review.get("unresolvedBaseline"),
        review.get("compatibilityContracts"),
        review.get("total"),
        review.get("finalUnresolved"),
    ) if isinstance(review, dict) else ()
    if actual_review != expected_review:
        raise GenerationError(f"review denominator must be 148 + 86 = 234 with zero unresolved, got {actual_review}")
    if tuple(source.get("scopeStrings", ())) != tuple(SCOPE_ENUM):
        raise GenerationError("registry source scope strings changed")
    if source.get("defaultRemoteScopes") != ["observe", "control"]:
        raise GenerationError("default remote scopes must be exactly observe + control")
    event_families = source.get("eventFamilies")
    if (
        not isinstance(event_families, list)
        or len(event_families) != 26
        or len(set(event_families)) != 26
        or any(not isinstance(value, str) or not value for value in event_families)
        or "threadList.updated" not in event_families
    ):
        raise GenerationError("registry source must contain exactly 26 unique expanded event families")
    rows = source.get("entries")
    if not isinstance(rows, list) or len(rows) != 234:
        raise GenerationError("registry source must contain exactly 234 reviewed entries")
    keys = [registry_key(row) for row in rows]
    if len(set(keys)) != 234:
        raise GenerationError("registry source contains duplicate reviewed identities")
    unresolved = {
        "Unresolved",
        "ExistingOperationSubsetExpansionUnresolved",
        "ExistingGenericContractDedicatedUnresolved",
    }
    if any(row.get("securityDecision") in unresolved for row in rows):
        raise GenerationError("registry source retains an unresolved security decision")
    if sum(row.get("exposure") == "NotApplicable" for row in rows) != 16:
        raise GenerationError("exactly 16 reviewed rows must be NotApplicable")
    if any(
        row.get("exposure") == "NotApplicable"
        and not (
            row["registryKey"]["category"] == "item_discriminator"
            and row["registryKey"]["domain"] == "ResponseItem"
        )
        for row in rows
    ):
        raise GenerationError("only ResponseItem rows may be NotApplicable")
    if sum(row.get("exposure") == "NotExposedBySecurityPolicy" for row in rows) != 36:
        raise GenerationError("exactly 36 experimental requests must be policy-excluded")
    if sum(row.get("exposure") == "ConditionallyExposedFrontendMethod" for row in rows) != 15:
        raise GenerationError("exactly 15 methods must be conditional")
    if any(
        row.get("exposure") == "ConditionallyExposedFrontendMethod" and row.get("defaultEnabled") is not False
        for row in rows
    ):
        raise GenerationError("conditional methods must be default-disabled")
    return rows


def provider_methods(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for row in rows:
        key = row["registryKey"]
        if key["category"] != "client_request" or row["stability"] != "stable":
            continue
        mappings = row["mappings"]
        if len(mappings) != 1:
            raise GenerationError(f"stable operation lacks one frontend method: {registry_key(row)}")
        target = row["runtimeTarget"]
        if not target.startswith("ClientRequestTarget::"):
            raise GenerationError(f"stable operation lacks a client target: {registry_key(row)}")
        method_id = target.removeprefix("ClientRequestTarget::")
        scopes = row["requiredScopes"]
        capability = "complete_provider_operations"
        if "filesystem_read" in scopes or "filesystem_write" in scopes:
            capability = "conditional_filesystem"
        elif "command_execution" in scopes:
            capability = "conditional_command_execution"
        result.append(
            {
                "id": method_id,
                "method": mappings[0],
                "frontendNative": False,
                "registryKeys": [registry_key(row)],
                "genericContractKey": "",
                "category": "provider_operation",
                "providerMethod": key["name"],
                "backendCommand": method_id,
                "serviceAction": "",
                "parameterSchema": schema_name(method_id, "Params"),
                "resultSchema": schema_name(method_id, "Result"),
                "resultType": row["operationContract"]["resultType"],
                "parameterFields": row["parameterShape"]["fields"],
                "requiredParameterFields": row["parameterShape"]["requiredFields"],
                "exposure": row["exposure"],
                "securityDecision": row["securityDecision"],
                "requiredScopes": scopes,
                "controllerRequired": row["controllerRequired"],
                "providerReadyRequired": True,
                "defaultEnabled": row["defaultEnabled"],
                "currentlyImplemented": True,
                "legacyCompatibilityMethod": mappings[0] in EXISTING_METHODS,
                "observerAvailability": "observe" in scopes and not row["controllerRequired"],
                "sensitiveResult": mappings[0] in SENSITIVE_PROVIDER_METHODS,
                "largeResult": mappings[0] in LARGE_PROVIDER_METHODS,
                "compatibilityStatus": "existing_v1" if mappings[0] in EXISTING_METHODS else "additive_v1",
                "capability": capability,
                "implementationPhase": "existing" if mappings[0] in EXISTING_METHODS else "A1.7b",
                "parameterPolicy": (
                    "refreshToken absent/false: observe; refreshToken true: control + account_management + current controller"
                    if mappings[0] == "account.read"
                    else "static"
                ),
            }
        )
    if len(result) != 86 or len({row["method"] for row in result}) != 86:
        raise GenerationError("provider method mapping must be a collision-free 86-row bijection")
    return result


def validate_runtime_authorization(
    rows: list[dict[str, Any]],
    methods: list[dict[str, Any]],
    default_remote_scopes: Sequence[str],
) -> dict[str, int]:
    """Freeze A1.7b method availability through two independent authorities."""

    if len(methods) != 105:
        raise GenerationError("A1.7b authorization inventory must contain 105 defined methods")
    if sum(bool(row.get("currentlyImplemented")) for row in methods) != 105:
        raise GenerationError("A1.7b runtime must implement all 105 defined methods")
    legacy = tuple(row["method"] for row in methods if row.get("legacyCompatibilityMethod"))
    if legacy != EXISTING_METHODS:
        raise GenerationError("the original 15-method legacy compatibility set changed")

    available = [row for row in methods if row["currentlyImplemented"] and row["defaultEnabled"]]
    if len(available) != 90:
        raise GenerationError("A1.7b default available-method denominator must remain 90")

    default_scope_set = set(default_remote_scopes)
    if default_scope_set != {"observe", "control"}:
        raise GenerationError("A1.7b default remote scopes must remain exactly observe and control")
    local_scope_set = set(SCOPE_ENUM)
    default_permitted = [row for row in available if set(row["requiredScopes"]) <= default_scope_set]
    local_permitted = [row for row in available if set(row["requiredScopes"]) <= local_scope_set]
    if len(default_permitted) != 53:
        raise GenerationError("A1.7b default_remote permitted numerator must remain 53 of 90")
    if len(local_permitted) != 90:
        raise GenerationError("A1.7b local_trusted permitted numerator must remain 90 of 90")

    privileged_provider = [
        row
        for row in available
        if row["category"] == "provider_operation" and row["securityDecision"] == "PrivilegedScopedApproved"
    ]
    reverse = [row for row in available if row["category"] == "reverse_response"]
    lifecycle = [row for row in available if row["category"] == "provider_lifecycle"]
    if (len(privileged_provider), len(reverse), len(lifecycle)) != (22, 12, 3):
        raise GenerationError("A1.7b default_remote exclusion decomposition must remain 22 privileged + 12 reverse + 3 lifecycle")
    if len(default_permitted) + len(privileged_provider) + len(reverse) + len(lifecycle) != len(available):
        raise GenerationError("A1.7b default_remote permission arithmetic must remain 53 + 37 = 90")
    provider_ready = [row for row in methods if row["providerReadyRequired"]]
    if len(provider_ready) != 98 or any(
        row["category"] not in {"provider_operation", "reverse_response"} for row in provider_ready
    ) or any(
        not row["providerReadyRequired"]
        for row in methods
        if row["category"] in {"provider_operation", "reverse_response"}
    ):
        raise GenerationError("provider readiness must apply to exactly 86 provider operations and 12 reverse methods")

    # Independent derivation B starts from the owner-reviewed registry rows,
    # rather than consuming the generated final permitted method set.
    stable_operations = [
        row
        for row in rows
        if row["registryKey"]["category"] == "client_request"
        and row["stability"] == "stable"
        and row["registryKey"]["name"] != "initialize"
    ]
    security_counts = collections.Counter(row["securityDecision"] for row in stable_operations)
    expected_security_counts = {
        "ObserverReadApproved": 26,
        "ControllerRequiredApproved": 22,
        "PrivilegedScopedApproved": 22,
        "ConditionalExplicitEnablementApproved": 15,
        "ParameterSensitiveApproved": 1,
    }
    if len(stable_operations) != 86 or dict(security_counts) != expected_security_counts:
        raise GenerationError("owner-reviewed operation categories must remain 26/22/22/15/1 across 86 operations")
    registry_reachable_operations = (
        security_counts["ObserverReadApproved"]
        + security_counts["ControllerRequiredApproved"]
        + security_counts["ParameterSensitiveApproved"]
    )
    reachable_non_lifecycle_native = sum(
        row["frontendNative"]
        and row["category"] != "provider_lifecycle"
        and set(row["requiredScopes"]) <= default_scope_set
        for row in methods
    )
    if registry_reachable_operations != 49 or reachable_non_lifecycle_native != 4:
        raise GenerationError("independent default_remote derivation must remain 49 operations + 4 native methods")
    if registry_reachable_operations + reachable_non_lifecycle_native != len(default_permitted):
        raise GenerationError("independent default_remote derivations disagree")

    for row in methods:
        if row["category"] == "reverse_response" and "sensitive_response" not in row["requiredScopes"] and \
                "unknown_request_response" not in row["requiredScopes"]:
            raise GenerationError("every reverse method must retain its sensitive-response scope")
        if row["category"] == "provider_lifecycle" and set(row["requiredScopes"]) <= default_scope_set:
            raise GenerationError("provider lifecycle methods cannot enter default_remote")

    return {
        "defined": 105,
        "implemented": 105,
        "legacyCompatibility": 15,
        "available": 90,
        "defaultRemotePermitted": 53,
        "localTrustedPermitted": 90,
        "privilegedProviderExcluded": 22,
        "reverseExcluded": 12,
        "lifecycleExcluded": 3,
        "registryReachableOperations": 49,
        "reachableFrontendNative": 4,
        "providerReadyRequired": 98,
    }


def method_manifest(source: dict[str, Any]) -> list[dict[str, Any]]:
    rows = validate_source(source)
    by_server_name = {
        row["registryKey"]["name"]: row
        for row in rows
        if row["registryKey"]["category"] == "server_request" and row["stability"] == "stable"
    }
    methods: list[dict[str, Any]] = []
    for native in NATIVE_METHODS:
        existing = native["method"] in EXISTING_METHODS
        methods.append(
            {
                "id": native["id"],
                "method": native["method"],
                "frontendNative": True,
                "registryKeys": [],
                "genericContractKey": "",
                "category": native["category"],
                "providerMethod": "",
                "backendCommand": "" if native["category"] == "provider_lifecycle" else native["serviceAction"],
                "serviceAction": native["serviceAction"],
                "parameterSchema": schema_name(native["id"], "Params"),
                "resultSchema": schema_name(native["id"], "Result"),
                "resultType": native["resultType"],
                "parameterFields": ["after"] if native["method"] == "events.replay" else [],
                "requiredParameterFields": ["after"] if native["method"] == "events.replay" else [],
                "exposure": "DedicatedFrontendMethod",
                "securityDecision": native["security"],
                "requiredScopes": native["scopes"],
                "controllerRequired": native["controllerRequired"],
                "providerReadyRequired": False,
                "defaultEnabled": True,
                "currentlyImplemented": True,
                "legacyCompatibilityMethod": existing,
                "observerAvailability": native["scopes"] == ["observe"],
                "sensitiveResult": False,
                "largeResult": native["method"] == "snapshot.get",
                "compatibilityStatus": "existing_v1" if existing else "additive_v1",
                "capability": native["capability"],
                "implementationPhase": "existing" if existing else "A1.7b",
                "parameterPolicy": "static",
            }
        )

    providers = provider_methods(rows)
    existing_provider_order = [name for name in EXISTING_METHODS if name in {row["method"] for row in providers}]
    providers.sort(key=lambda row: (row["method"] not in existing_provider_order, existing_provider_order.index(row["method"]) if row["method"] in existing_provider_order else row["method"]))
    methods.extend(providers)

    for method_id, method, request_names, backend_command in REVERSE_METHODS:
        keys = [registry_key(by_server_name[name]) for name in request_names]
        unknown = method in {"request.unknown.respond", "request.unknown.reject"}
        existing = method in EXISTING_METHODS
        methods.append(
            {
                "id": method_id,
                "method": method,
                "frontendNative": False,
                "registryKeys": keys,
                "genericContractKey": "unknown_server_request" if unknown else "",
                "category": "reverse_response",
                "providerMethod": "",
                "backendCommand": backend_command,
                "serviceAction": "",
                "parameterSchema": schema_name(method_id, "Params"),
                "resultSchema": schema_name(method_id, "Result"),
                "resultType": "Unit",
                "parameterFields": REVERSE_PARAMETER_SHAPES[method][0],
                "requiredParameterFields": REVERSE_PARAMETER_SHAPES[method][1],
                "exposure": "DedicatedFrontendMethod",
                "securityDecision": "PrivilegedScopedApproved",
                "requiredScopes": ["control", "unknown_request_response" if unknown else "sensitive_response"],
                "controllerRequired": True,
                "providerReadyRequired": True,
                "defaultEnabled": True,
                "currentlyImplemented": True,
                "legacyCompatibilityMethod": existing,
                "observerAvailability": False,
                "sensitiveResult": True,
                "largeResult": False,
                "compatibilityStatus": "existing_v1" if existing else "additive_v1",
                "capability": "complete_reverse_requests",
                "implementationPhase": "existing" if existing else "A1.7b",
                "parameterPolicy": "exact pending-request type",
            }
        )

    methods.sort(key=lambda row: (row["method"] not in EXISTING_METHODS, EXISTING_METHODS.index(row["method"]) if row["method"] in EXISTING_METHODS else row["method"]))
    spellings = [row["method"] for row in methods]
    if len(methods) != 105 or len(set(spellings)) != 105:
        raise GenerationError("method catalog must contain exactly 105 unique methods")
    if [row["method"] for row in methods[:15]] != list(EXISTING_METHODS):
        raise GenerationError("the original 15 method order/spellings changed")
    if sum(row["frontendNative"] for row in methods) != 7:
        raise GenerationError("exactly seven methods must be frontend-native")
    if sum(not row["frontendNative"] for row in methods) != 98:
        raise GenerationError("exactly 98 methods must be non-native")
    if sum(row["category"] == "provider_operation" for row in methods) != 86:
        raise GenerationError("exactly 86 provider-operation methods are required")
    if sum(row["category"] == "reverse_response" for row in methods) != 12:
        raise GenerationError("exactly 12 reverse methods are required")
    if sum(row["category"] == "provider_lifecycle" for row in methods) != 3:
        raise GenerationError("exactly three lifecycle methods are required")
    validate_runtime_authorization(rows, methods, source["defaultRemoteScopes"])
    return methods


def generate_manifest(source: dict[str, Any]) -> dict[str, Any]:
    rows = validate_source(source)
    methods = method_manifest(source)
    authorization = validate_runtime_authorization(rows, methods, source["defaultRemoteScopes"])
    capabilities = [
        {
            "key": key,
            "defined": True,
            "implementedByCurrentRuntime": key in IMPLEMENTED_MECHANISM_CAPABILITIES,
            "category": (
                "static_mechanism"
                if key in MECHANISM_CAPABILITIES
                else "conditional_topology"
                if key == "multi_transport"
                else "product"
            ),
            "futurePhase": (
                "A1.7b" if key in {"authenticated_frontend", "scope_projected_state", "provider_lifecycle", "multi_transport"}
                else "A1.7c" if key in {"cpp_client_sdk", "qt_ui"}
                else "A1.7d" if key in {"typescript_client_sdk", "browser_ui"}
                else "A1.7a-contract"
            ),
        }
        for key in CAPABILITIES
    ]
    implemented_capabilities = {item["key"] for item in capabilities if item["implementedByCurrentRuntime"]}
    if implemented_capabilities != IMPLEMENTED_MECHANISM_CAPABILITIES:
        raise GenerationError("A1.7b must implement exactly thirteen mechanism/build capabilities")
    if any(item["implementedByCurrentRuntime"] for item in capabilities if item["key"] in FUTURE_CAPABILITIES):
        raise GenerationError("A1.7b claims a future product capability as implemented")
    notifications = [
        {
            "registryKey": registry_key(row),
            "finalExposure": row["exposure"],
            "securityDecision": row["securityDecision"],
            "legacyContract": row["compatibilityBehavior"],
            "expandedMappings": row["mappings"],
            "requiredScopes": row["requiredScopes"],
            "redactionClass": row["redactionClass"],
            "capability": "dedicated_notification_events",
            "legacyCapabilityBehavior": "use legacy contract when capability is absent",
            "expandedCapabilityBehavior": "use dedicated domain projection when capability is present",
            "duplicateSuppression": "exactly_one_compatibility_representation_per_connection",
        }
        for row in rows
        if row["registryKey"]["category"] == "server_notification"
    ]
    items = [
        {
            "registryKey": registry_key(row),
            "finalExposure": row["exposure"],
            "securityDecision": row["securityDecision"],
            "legacyContract": row["compatibilityBehavior"],
            "expandedMappings": row["mappings"],
            "requiredScopes": row["requiredScopes"],
            "redactionClass": row["redactionClass"],
            "capability": "complete_thread_items",
            "legacyCapabilityBehavior": "use legacy contract when capability is absent",
            "expandedCapabilityBehavior": "use bounded safe item projection when capability is present",
            "duplicateSuppression": "exactly_one_compatibility_representation_per_connection",
        }
        for row in rows
        if row["registryKey"]["category"] == "item_discriminator" and row["registryKey"]["domain"] == "ThreadItem"
    ]
    pending_kind_by_method = dict(PENDING_REQUEST_KINDS)
    stable_server_requests = [
        row
        for row in rows
        if row["registryKey"]["category"] == "server_request"
        and row["stability"] == "stable"
    ]
    pending_kind_order = {
        method: index for index, (method, _) in enumerate(PENDING_REQUEST_KINDS)
    }
    stable_server_requests.sort(
        key=lambda row: pending_kind_order.get(row["registryKey"]["name"], len(pending_kind_order))
    )
    stable_server_request_methods = {
        row["registryKey"]["name"] for row in stable_server_requests
    }
    if (
        len(pending_kind_by_method) != 10
        or set(pending_kind_by_method) != stable_server_request_methods
        or len(set(pending_kind_by_method.values())) != 10
    ):
        raise GenerationError(
            "pending-request discriminator table must bijectively cover the ten stable server requests"
        )
    reverse_methods = {
        row["method"]: row
        for row in methods
        if row["category"] == "reverse_response"
    }
    pending_requests = []
    for row in stable_server_requests:
        provider_method = row["registryKey"]["name"]
        response_methods = row["mappings"]
        if (
            row["exposure"] != "DedicatedPendingRequestContract"
            or row["securityDecision"] != "ScopeProjectedStateEventApproved"
            or row["requiredScopes"] != ["observe"]
            or row["redactionClass"] != "safe_pending_request"
            or not response_methods
            or any(method not in reverse_methods for method in response_methods)
            or any(
                registry_key(row) not in reverse_methods[method]["registryKeys"]
                for method in response_methods
            )
        ):
            raise GenerationError(
                f"stable server request {provider_method!r} lacks its complete reviewed pending-request projection"
            )
        response_scope_sets = {
            tuple(reverse_methods[method]["requiredScopes"])
            for method in response_methods
        }
        controller_requirements = {
            reverse_methods[method]["controllerRequired"]
            for method in response_methods
        }
        if response_scope_sets != {("control", "sensitive_response")} or controller_requirements != {True}:
            raise GenerationError(
                f"stable server request {provider_method!r} response policy differs from the reviewed typed-request policy"
            )
        pending_requests.append(
            {
                "registryKey": registry_key(row),
                "providerMethod": provider_method,
                "kind": pending_kind_by_method[provider_method],
                "finalExposure": row["exposure"],
                "securityDecision": row["securityDecision"],
                "legacyContract": row["compatibilityBehavior"],
                "expandedEvent": "pendingRequests.updated",
                "responseMethods": response_methods,
                "presentationRequiredScopes": row["requiredScopes"],
                "controllerRequiredForPresentation": row["controllerRequired"],
                "responseRequiredScopes": ["control", "sensitive_response"],
                "controllerRequiredForResponse": True,
                "redactionClass": row["redactionClass"],
                "capability": "dedicated_pending_requests",
                "duplicateSuppression": "exactly_one_compatibility_representation_per_connection",
            }
        )
    if len(notifications) != 68 or len(items) != 18 or len(pending_requests) != 10:
        raise GenerationError("notification/item/pending-request mappings must be 68/18/10")
    excluded = [
        {
            "registryKey": registry_key(row),
            "exposure": row["exposure"],
            "securityDecision": row["securityDecision"],
            "rationale": row["rationale"],
            "generatedPath": False,
        }
        for row in rows
        if row["exposure"] in {"NotExposedBySecurityPolicy", "NotApplicable"}
    ]
    reviewed_contracts = [
        {
            "registryKey": row["registryKey"],
            "stability": row["stability"],
            "priorCompatibilityExposure": row["priorCompatibilityExposure"],
            "priorCompatibilitySecurity": row["priorCompatibilitySecurity"],
            "exposure": row["exposure"],
            "securityDecision": row["securityDecision"],
            "mappings": row["mappings"],
            "requiredScopes": row["requiredScopes"],
            "controllerRequired": row["controllerRequired"],
            "defaultEnabled": row["defaultEnabled"],
            "redactionClass": row["redactionClass"],
            "compatibilityBehavior": row["compatibilityBehavior"],
            "rationale": row["rationale"],
        }
        for row in rows
    ]
    return {
        "generatedBy": "tools/frontend/generate_frontend_protocol.py",
        "protocolIdentity": "snodec.codex-frontend",
        "protocolVersion": 1,
        "supportedVersions": [1],
        "compatibilityFamily": "additive-v1",
        "messageKinds": ["hello", "welcome", "sync.complete", "command", "response", "snapshot", "events", "protocol.error"],
        "counts": {
            "messageKinds": 8,
            "methods": 105,
            "existingMethods": 15,
            "additiveMethods": 90,
            "frontendNativeMethods": 7,
            "nonNativeMethods": 98,
            "providerLifecycleMethods": 3,
            "providerOperationMethods": 86,
            "reverseMethods": 12,
            "currentRuntimeMethods": 105,
            "implementedMethods": 105,
            "defaultAvailableMethods": 90,
            "defaultRemotePermittedMethods": 53,
            "localTrustedPermittedMethods": 90,
            "implementedMechanismCapabilities": 13,
            "runtimeTopologyCapabilities": 1,
            "futureProductCapabilities": 4,
            "reviewedIdentities": 234,
            "notifications": 68,
            "threadItems": 18,
            "pendingRequests": 10,
            "expandedEventFamilies": len(source["eventFamilies"]),
        },
        "scopeProfiles": {
            "default_remote": ["observe", "control"],
            "local_trusted": list(SCOPE_ENUM),
        },
        "helloAuthentication": {
            "optional": True,
            "credentialLocation": "hello.authentication",
            "schemes": ["bearer"],
            "secretFields": ["token"],
            "legacyHelloWithoutCredentialRemainsValid": True,
        },
        "authorization": authorization,
        "capabilities": capabilities,
        "eventFamilies": list(source["eventFamilies"]),
        "methods": methods,
        "notificationMappings": notifications,
        "threadItemMappings": items,
        "pendingRequestMappings": pending_requests,
        "nonExposedOrNotApplicable": excluded,
        "reviewedContracts": reviewed_contracts,
    }


def q(value: str) -> str:
    return json.dumps(value)


def category_cpp(value: str) -> str:
    return {
        "backend_control": "BackendControl",
        "frontend_replay": "FrontendReplay",
        "provider_lifecycle": "ProviderLifecycle",
        "provider_operation": "ProviderOperation",
        "reverse_response": "ReverseResponse",
    }[value]


def normalize_embedded_schema(schema: Any, definition_name: str) -> Any:
    """Translate a self-contained draft-07 operation schema into a nested 2020-12 definition."""

    if schema is None:
        return {"type": "object", "additionalProperties": True}

    def visit(value: Any) -> Any:
        if isinstance(value, list):
            return [visit(element) for element in value]
        if not isinstance(value, dict):
            return value
        result: dict[str, Any] = {}
        for key, child in value.items():
            if key == "$schema":
                continue
            output_key = "$defs" if key == "definitions" else key
            if key == "$ref" and isinstance(child, str) and child.startswith("#/definitions/"):
                child = f"#/$defs/{definition_name}/$defs/{child.removeprefix('#/definitions/')}"
            result[output_key] = visit(child)
        return result

    normalized = visit(schema)
    if not isinstance(normalized, dict):
        raise GenerationError(f"operation schema for {definition_name} is not an object")
    return normalized


SENSITIVE_RESULT_FIELD_NAMES = frozenset(
    {
        "access_token",
        "accesstoken",
        "api_key",
        "apikey",
        "auth_token",
        "authtoken",
        "authorization",
        "cookie",
        "id_token",
        "idtoken",
        "password",
        "refresh_token",
        "refreshtoken",
        "secret",
        "session_token",
        "sessiontoken",
    }
)


def sensitive_property_name_pattern() -> str:
    alternatives = []
    for normalized in sorted({name.replace("_", "") for name in SENSITIVE_RESULT_FIELD_NAMES}):
        letters = []
        for character in normalized:
            if character.isalpha():
                letters.append(f"[{character.lower()}{character.upper()}]")
            else:
                letters.append(re.escape(character))
        alternatives.append("[^A-Za-z0-9]*".join(letters))
    return "^(?:" + "|".join(alternatives) + ")$"


SAFE_PROPERTY_NAMES = {"$ref": "#/$defs/SafePropertyName"}


def secure_safe_object_extensions(schema: Any) -> Any:
    """Bound and credential-filter unknown fields in safe frontend projections."""

    if isinstance(schema, list):
        return [secure_safe_object_extensions(value) for value in schema]
    if not isinstance(schema, dict):
        return schema
    secured = {
        key: secure_safe_object_extensions(value) for key, value in schema.items()
    }
    schema_type = secured.get("type")
    schema_types = (
        {schema_type}
        if isinstance(schema_type, str)
        else set(schema_type or ())
    )
    if "object" in schema_types or "properties" in secured or "required" in secured:
        properties = secured.get("properties", {})
        if isinstance(properties, dict):
            forbidden = {
                re.sub(r"[^a-z0-9]", "", name.lower())
                for name in SENSITIVE_RESULT_FIELD_NAMES
            }
            unsafe = sorted(
                name
                for name in properties
                if re.sub(r"[^a-z0-9]", "", name.lower()) in forbidden
            )
            if unsafe:
                raise GenerationError(
                    "safe frontend projection declares credential-shaped known "
                    f"properties: {', '.join(unsafe)}"
                )
        secured.setdefault("maxProperties", 512)
        secured["propertyNames"] = copy_json(SAFE_PROPERTY_NAMES)
        if secured.get("additionalProperties") is True:
            secured["additionalProperties"] = {
                "$ref": "#/$defs/SafeDetailValue"
            }
    return secured


def bound_and_redact_result_schema(schema: Any, definition_name: str) -> dict[str, Any]:
    """Create the exact bounded frontend view of an operation result schema.

    The provider schema remains the field/type authority, but a remote
    frontend result is never permitted to contain a known credential field or
    an unbounded string/collection.  Runtime scope-aware projection is A1.7b;
    A1.7a freezes the safe result contract it must implement.
    """

    normalized = normalize_embedded_schema(schema, definition_name)

    def visit(value: Any) -> Any:
        if isinstance(value, list):
            return [visit(element) for element in value]
        if not isinstance(value, dict):
            return value
        result = {key: visit(child) for key, child in value.items()}
        properties = result.get("properties")
        if isinstance(properties, dict):
            removed = {
                name
                for name in properties
                if re.sub(r"[^a-z0-9_]", "", name.lower())
                in SENSITIVE_RESULT_FIELD_NAMES
            }
            for name in removed:
                properties.pop(name, None)
            required = result.get("required")
            if isinstance(required, list):
                result["required"] = [name for name in required if name not in removed]
            result.setdefault("maxProperties", 512)
            result["propertyNames"] = copy_json(SAFE_PROPERTY_NAMES)
        schema_type = result.get("type")
        schema_types = {schema_type} if isinstance(schema_type, str) else set(schema_type or ())
        if "string" in schema_types:
            result.setdefault("maxLength", 1 << 20)
        if "array" in schema_types:
            result.setdefault("maxItems", 4096)
        if "object" in schema_types:
            result.setdefault("maxProperties", 512)
            result["propertyNames"] = copy_json(SAFE_PROPERTY_NAMES)
            if result.get("additionalProperties") is True:
                result["additionalProperties"] = {
                    "$ref": "#/$defs/SafeDetailValue"
                }
        return result

    bounded = visit(normalized)
    bounded["x-aisuite-redactionClass"] = "bounded_safe_operation_result"
    bounded["x-aisuite-sensitiveFieldNamesForbidden"] = sorted(
        SENSITIVE_RESULT_FIELD_NAMES
    )
    return bounded


def _schema_pointer_token(token: str, reference: str) -> str:
    decoded: list[str] = []
    index = 0
    while index < len(token):
        character = token[index]
        if character != "~":
            decoded.append(character)
            index += 1
            continue
        if index + 1 >= len(token) or token[index + 1] not in {"0", "1"}:
            raise GenerationError(
                f"runtime schema contains a malformed local reference {reference!r}"
            )
        decoded.append("~" if token[index + 1] == "0" else "/")
        index += 2
    return "".join(decoded)


def _resolve_runtime_schema_reference(root: dict[str, Any], reference: str) -> Any:
    if reference == "#":
        return root
    if not reference.startswith("#/"):
        raise GenerationError(
            f"runtime schema contains a non-local reference {reference!r}"
        )
    value: Any = root
    for encoded in reference[2:].split("/"):
        token = _schema_pointer_token(encoded, reference)
        if isinstance(value, dict) and token in value:
            value = value[token]
        elif (
            isinstance(value, list)
            and token.isdigit()
            and (token == "0" or not token.startswith("0"))
            and int(token) < len(value)
        ):
            value = value[int(token)]
        elif isinstance(value, list) and token.isdigit() and token.startswith("0"):
            raise GenerationError(
                f"runtime schema contains a malformed local reference {reference!r}"
            )
        else:
            raise GenerationError(
                f"runtime schema contains an unresolved local reference {reference!r}"
            )
    return value


def _is_schema(value: Any) -> bool:
    return isinstance(value, (bool, dict))


def _non_negative_schema_size(value: Any, keyword: str, path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise GenerationError(
            f"runtime schema {path}/{keyword} must be a non-negative integer"
        )
    return value


def _finite_enum_cardinality(
    root: dict[str, Any], schema: Any, active_references: frozenset[str] = frozenset()
) -> int | None:
    if not isinstance(schema, dict):
        return None
    enumeration = schema.get("enum")
    if isinstance(enumeration, list):
        return len(enumeration)
    reference = schema.get("$ref")
    if not isinstance(reference, str) or reference in active_references:
        return None
    return _finite_enum_cardinality(
        root,
        _resolve_runtime_schema_reference(root, reference),
        active_references | {reference},
    )


def audit_runtime_schema_profile(
    schema: dict[str, Any], manifest: dict[str, Any]
) -> RuntimeSchemaAudit:
    """Audit the exact schema profile reachable by the production codec.

    The published document root covers all message envelopes.  Method results
    are also direct runtime validation roots even though response correlation
    keeps their individual definitions out of the document-level ``oneOf``.
    """

    if not isinstance(schema, dict):
        raise GenerationError("generated runtime schema root is not an object")
    methods = manifest.get("methods")
    if not isinstance(methods, list):
        raise GenerationError("runtime schema audit requires the method manifest")

    assertion_keywords: set[str] = set()
    structural_keywords: set[str] = set()
    annotation_keywords: set[str] = set()
    numeric_formats: set[str] = set()
    patterns: set[str] = set()
    unique_item_bounds: list[int] = []
    visited_nodes: set[int] = set()
    known_keywords = (
        RUNTIME_ASSERTION_KEYWORDS
        | STRUCTURAL_SCHEMA_KEYWORDS
        | STANDARD_ANNOTATION_KEYWORDS
        | CUSTOM_ANNOTATION_KEYWORDS
    )

    def require_schema(value: Any, path: str) -> None:
        if not _is_schema(value):
            raise GenerationError(
                f"runtime schema {path} must be an object or boolean schema"
            )

    def validate_string_array(value: Any, keyword: str, path: str) -> None:
        if (
            not isinstance(value, list)
            or any(not isinstance(element, str) for element in value)
            or len(set(value)) != len(value)
        ):
            raise GenerationError(
                f"runtime schema {path}/{keyword} must be an array of unique strings"
            )

    def visit(node: Any, path: str) -> None:
        require_schema(node, path)
        if isinstance(node, bool) or id(node) in visited_nodes:
            return
        visited_nodes.add(id(node))

        for keyword in node:
            if keyword not in known_keywords:
                if keyword.startswith("x-aisuite-"):
                    raise GenerationError(
                        f"runtime schema {path} contains an unreviewed custom AISuite keyword {keyword!r}"
                    )
                raise GenerationError(
                    f"runtime schema {path} contains unsupported assertion keyword {keyword!r}"
                )
            if keyword in RUNTIME_ASSERTION_KEYWORDS:
                assertion_keywords.add(keyword)
            elif keyword in STRUCTURAL_SCHEMA_KEYWORDS:
                structural_keywords.add(keyword)
            else:
                annotation_keywords.add(keyword)

        definitions = node.get("$defs")
        if "$defs" in node:
            if not isinstance(definitions, dict):
                raise GenerationError(f"runtime schema {path}/$defs must be an object")
            for name, definition in definitions.items():
                require_schema(definition, f"{path}/$defs/{name}")

        reference = node.get("$ref")
        if "$ref" in node:
            if not isinstance(reference, str):
                raise GenerationError(f"runtime schema {path}/$ref must be a string")
            visit(_resolve_runtime_schema_reference(schema, reference), reference)

        for keyword in ("allOf", "anyOf", "oneOf"):
            alternatives = node.get(keyword)
            if keyword not in node:
                continue
            if not isinstance(alternatives, list) or not alternatives:
                raise GenerationError(
                    f"runtime schema {path}/{keyword} must be a non-empty array of schemas"
                )
            for index, alternative in enumerate(alternatives):
                visit(alternative, f"{path}/{keyword}/{index}")

        for keyword in ("not", "if", "then", "else", "propertyNames", "items"):
            child = node.get(keyword)
            if keyword in node:
                visit(child, f"{path}/{keyword}")
        if ("then" in node or "else" in node) and "if" not in node:
            raise GenerationError(
                f"runtime schema {path} contains then/else without if"
            )

        properties = node.get("properties")
        if "properties" in node:
            if not isinstance(properties, dict):
                raise GenerationError(
                    f"runtime schema {path}/properties must be an object"
                )
            for name, child in properties.items():
                visit(child, f"{path}/properties/{name}")

        additional = node.get("additionalProperties")
        if "additionalProperties" in node:
            require_schema(additional, f"{path}/additionalProperties")
            if isinstance(additional, dict):
                visit(additional, f"{path}/additionalProperties")

        schema_type = node.get("type")
        if "type" in node:
            if isinstance(schema_type, str):
                valid_type = schema_type in SCHEMA_TYPES
            else:
                valid_type = (
                    isinstance(schema_type, list)
                    and bool(schema_type)
                    and all(
                        isinstance(value, str) and value in SCHEMA_TYPES
                        for value in schema_type
                    )
                    and len(set(schema_type)) == len(schema_type)
                )
            if not valid_type:
                raise GenerationError(
                    f"runtime schema {path}/type has an invalid schema type"
                )

        enumeration = node.get("enum")
        if "enum" in node:
            if not isinstance(enumeration, list) or not enumeration:
                raise GenerationError(
                    f"runtime schema {path}/enum must be a non-empty array"
                )
            encoded_values = [
                json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
                for value in enumeration
            ]
            if len(set(encoded_values)) != len(encoded_values):
                raise GenerationError(
                    f"runtime schema {path}/enum contains duplicate values"
                )

        required = node.get("required")
        if "required" in node:
            validate_string_array(required, "required", path)

        sizes: dict[str, int] = {}
        for keyword in (
            "minProperties",
            "maxProperties",
            "minItems",
            "maxItems",
            "minLength",
            "maxLength",
        ):
            if keyword in node:
                sizes[keyword] = _non_negative_schema_size(
                    node[keyword], keyword, path
                )
        for minimum, maximum in (
            ("minProperties", "maxProperties"),
            ("minItems", "maxItems"),
            ("minLength", "maxLength"),
        ):
            if minimum in sizes and maximum in sizes and sizes[minimum] > sizes[maximum]:
                raise GenerationError(
                    f"runtime schema {path} has {minimum} greater than {maximum}"
                )

        unique_items = node.get("uniqueItems")
        if "uniqueItems" in node and not isinstance(unique_items, bool):
            raise GenerationError(
                f"runtime schema {path}/uniqueItems must be a boolean"
            )
        if unique_items is True:
            possible_bounds = []
            if "maxItems" in sizes:
                possible_bounds.append(sizes["maxItems"])
            item_cardinality = _finite_enum_cardinality(schema, node.get("items"))
            if item_cardinality is not None:
                possible_bounds.append(item_cardinality)
            if not possible_bounds:
                raise GenerationError(
                    f"runtime schema {path} has unbounded uniqueItems cardinality"
                )
            unique_item_bounds.append(min(possible_bounds))

        pattern = node.get("pattern")
        if "pattern" in node:
            if not isinstance(pattern, str):
                raise GenerationError(f"runtime schema {path}/pattern must be a string")
            try:
                re.compile(pattern)
            except re.error as error:
                raise GenerationError(
                    f"runtime schema {path}/pattern is malformed: {error}"
                ) from error
            patterns.add(pattern)

        for keyword in ("minimum", "maximum"):
            numeric_bound = node.get(keyword)
            if keyword in node and (
                isinstance(numeric_bound, bool)
                or not isinstance(numeric_bound, (int, float))
                or (isinstance(numeric_bound, float) and not math.isfinite(numeric_bound))
            ):
                raise GenerationError(
                    f"runtime schema {path}/{keyword} must be a finite number"
                )
        if (
            "minimum" in node
            and "maximum" in node
            and node["minimum"] > node["maximum"]
        ):
            raise GenerationError(
                f"runtime schema {path} has minimum greater than maximum"
            )

        numeric_format = node.get("format")
        if "format" in node:
            if not isinstance(numeric_format, str):
                raise GenerationError(f"runtime schema {path}/format must be a string")
            if numeric_format not in SUPPORTED_NUMERIC_FORMATS:
                raise GenerationError(
                    f"runtime schema {path} uses unsupported numeric format {numeric_format!r}"
                )
            numeric_formats.add(numeric_format)

        for keyword in (
            "x-aisuite-sensitiveFieldNamesForbidden",
            "x-aisuite-forbiddenNormalizedPropertyNames",
        ):
            if keyword in node:
                validate_string_array(node[keyword], keyword, path)

        for keyword in ("$schema", "$id", "title", "description", "$comment"):
            if keyword in node and not isinstance(node[keyword], str):
                raise GenerationError(
                    f"runtime schema annotation {path}/{keyword} must be a string"
                )
        if "examples" in node and not isinstance(node["examples"], list):
            raise GenerationError(
                f"runtime schema annotation {path}/examples must be an array"
            )
        for keyword in ("deprecated", "readOnly", "writeOnly"):
            if keyword in node and not isinstance(node[keyword], bool):
                raise GenerationError(
                    f"runtime schema annotation {path}/{keyword} must be a boolean"
                )
        if "x-aisuite-redactionClass" in node and not isinstance(
            node["x-aisuite-redactionClass"], str
        ):
            raise GenerationError(
                f"runtime schema annotation {path}/x-aisuite-redactionClass must be a string"
            )
        if "x-aisuite-frontend-contract" in node and not isinstance(
            node["x-aisuite-frontend-contract"], dict
        ):
            raise GenerationError(
                f"runtime schema annotation {path}/x-aisuite-frontend-contract must be an object"
            )

    visit(schema, "#")
    for method in methods:
        for field in ("parameterSchema", "resultSchema"):
            reference = method.get(field)
            if not isinstance(reference, str):
                raise GenerationError(
                    f"runtime schema audit found an invalid method {field} reference"
                )
            visit(_resolve_runtime_schema_reference(schema, reference), reference)

    maximum_cardinality = max(unique_item_bounds, default=0)
    return RuntimeSchemaAudit(
        assertion_keywords=tuple(sorted(assertion_keywords)),
        structural_keywords=tuple(sorted(structural_keywords)),
        annotation_keywords=tuple(sorted(annotation_keywords)),
        numeric_formats=tuple(sorted(numeric_formats)),
        patterns=tuple(sorted(patterns)),
        unique_item_schema_count=len(unique_item_bounds),
        maximum_unique_item_cardinality=maximum_cardinality,
        maximum_unique_item_comparisons=(
            maximum_cardinality * (maximum_cardinality - 1) // 2
        ),
    )


def legacy_result_schema(method: str) -> dict[str, Any]:
    unit = {"type": "object", "maxProperties": 0}
    if method in {"controller.acquire", "controller.release"}:
        return {
            "type": "object",
            "required": ["role"],
            "properties": {
                "role": {"enum": ["observer", "controller"]},
                "controllerSessionId": {"$ref": "#/$defs/DecimalId"},
            },
            "additionalProperties": True,
        }
    if method == "snapshot.get":
        return {
            "type": "object",
            "required": ["sequence"],
            "properties": {"sequence": {"$ref": "#/$defs/UInt64"}},
            "additionalProperties": True,
        }
    if method == "events.replay":
        return {
            "type": "object",
            "required": ["syncMode", "sequence"],
            "properties": {
                "syncMode": {"enum": ["replay", "snapshot"]},
                "sequence": {"$ref": "#/$defs/UInt64"},
            },
            "additionalProperties": True,
        }
    if method in {"thread.start", "thread.resume", "thread.read"}:
        return {
            "oneOf": [
                {
                    "type": "object",
                    "required": ["thread"],
                    "properties": {"thread": {"$ref": "#/$defs/ThreadState"}},
                    "additionalProperties": True,
                },
                {
                    "type": "object",
                    "required": ["threadId"],
                    "properties": {"threadId": {"type": "string", "minLength": 1}},
                    "additionalProperties": True,
                },
            ]
        }
    if method == "thread.list":
        return {
            "type": "object",
            "required": ["threads"],
            "properties": {
                "threads": {"type": "array", "items": {"$ref": "#/$defs/ThreadState"}},
                "nextCursor": {"type": "string"},
                "backwardsCursor": {"type": "string"},
            },
            "additionalProperties": True,
        }
    if method == "turn.start":
        return {
            "oneOf": [
                {
                    "type": "object",
                    "required": ["turn"],
                    "properties": {"turn": {"$ref": "#/$defs/TurnState"}},
                    "additionalProperties": True,
                },
                {
                    "type": "object",
                    "required": ["turnId"],
                    "properties": {"turnId": {"type": "string", "minLength": 1}},
                    "additionalProperties": True,
                },
            ]
        }
    return unit


def apply_provider_parameter_constraints(
    method: str, schema: dict[str, Any]
) -> dict[str, Any]:
    constrained = copy_json(schema)
    for field, minimum in PROVIDER_PARAMETER_MIN_ITEMS.get(method, {}).items():
        properties = constrained.get("properties")
        field_schema = properties.get(field) if isinstance(properties, dict) else None
        if not isinstance(field_schema, dict) or field_schema.get("type") != "array":
            raise GenerationError(
                f"frontend provider parameter constraint {method}.{field} "
                "does not target an array property"
            )
        field_schema["minItems"] = minimum
    return constrained


def generate_schema(
    template: dict[str, Any], manifest: dict[str, Any], source: dict[str, Any]
) -> dict[str, Any]:
    """Add the complete capability-gated v1 contract to the legacy schema."""

    schema = copy_json(template)
    if schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
        raise GenerationError("frontend schema template is not Draft 2020-12")
    definitions = schema.get("$defs")
    if not isinstance(definitions, dict):
        raise GenerationError("frontend schema template has no $defs")
    event_families = tuple(manifest["eventFamilies"])

    command = definitions.get("Command")
    try:
        branches = command["allOf"][1]["oneOf"]
    except (KeyError, IndexError, TypeError) as error:
        raise GenerationError("frontend schema template Command shape changed") from error
    existing_branches = {
        branch["properties"]["method"]["const"]: branch for branch in branches
    }
    if tuple(existing_branches) != EXISTING_METHODS:
        raise GenerationError("legacy command branches changed before additive generation")

    source_by_key = {registry_key(row): row for row in source["entries"]}
    generated_branches: list[dict[str, Any]] = []
    for method in manifest["methods"]:
        name = method["method"]
        method_id = method["id"]
        if name in existing_branches:
            parameter_definition = f"{method_id}Params"
            existing_parameter_ref = existing_branches[name]["properties"]["params"]["$ref"]
            if existing_parameter_ref != f"#/$defs/{parameter_definition}":
                definitions[parameter_definition] = {"$ref": existing_parameter_ref}
            definitions[f"{method_id}Result"] = legacy_result_schema(name)
            generated_branches.append(existing_branches[name])
            continue
        parameter_definition = f"{method_id}Params"
        result_definition = f"{method_id}Result"
        operation_row = (
            source_by_key.get(method["registryKeys"][0])
            if method["category"] == "provider_operation" and method["registryKeys"]
            else None
        )
        if operation_row is not None:
            operation = operation_row["operationContract"]
            definitions[parameter_definition] = apply_provider_parameter_constraints(
                name,
                normalize_embedded_schema(
                    operation.get("parameterJsonSchema"), parameter_definition
                ),
            )
            definitions[result_definition] = bound_and_redact_result_schema(
                operation.get("resultJsonSchema"), result_definition
            )
        else:
            properties: dict[str, Any] = {
                field: {} for field in method["parameterFields"]
            }
            nested_definitions: dict[str, Any] = {}
            if method["category"] == "reverse_response":
                properties["pendingRequestId"] = {"$ref": "#/$defs/DecimalId"}
                if "response" in properties and method["registryKeys"]:
                    response_row = source_by_key[method["registryKeys"][0]]
                    response_schema = bound_and_redact_result_schema(
                        response_row["operationContract"].get("resultJsonSchema"),
                        parameter_definition,
                    )
                    nested_definitions = response_schema.pop("$defs", {})
                    properties["response"] = response_schema
                if "error" in properties:
                    properties["error"] = {
                        "type": "object",
                        "required": ["code", "message"],
                        "properties": {
                            "code": {"type": "integer"},
                            "message": {"type": "string", "maxLength": 16384},
                            "data": {},
                        },
                        "additionalProperties": True,
                    }
            definitions[parameter_definition] = {
                "type": "object",
                "required": method["requiredParameterFields"],
                "properties": properties,
                "additionalProperties": True,
            }
            if nested_definitions:
                definitions[parameter_definition]["$defs"] = nested_definitions
            definitions[result_definition] = {
                "type": "object",
                "maxProperties": 0,
                "additionalProperties": True,
            }
        generated_branches.append(
            {
                "properties": {
                    "method": {"const": name},
                    "params": {"$ref": f"#/$defs/{method_id}Params"},
                }
            }
        )
    command["allOf"][1]["oneOf"] = generated_branches

    error_codes = definitions.get("ErrorCode", {}).get("enum")
    if not isinstance(error_codes, list):
        raise GenerationError("frontend schema template ErrorCode shape changed")
    for code in (
        "authentication_required",
        "authentication_failed",
        "origin_rejected",
        "transport_security_required",
        "rate_limited",
    ):
        if code not in error_codes:
            error_codes.append(code)

    definitions["FrontendScope"] = {"type": "string", "enum": list(SCOPE_ENUM)}
    definitions["FrontendCapability"] = {"type": "string", "enum": list(CAPABILITIES)}
    definitions["FrontendMethod"] = {"type": "string", "enum": [method["method"] for method in manifest["methods"]]}
    definitions["CapabilityAdvertisement"] = {
        "type": "object",
        "required": ["defined", "implemented", "permitted"],
        "properties": {
            "defined": {"type": "array", "items": {"$ref": "#/$defs/FrontendCapability"}, "uniqueItems": True},
            "implemented": {"type": "array", "items": {"$ref": "#/$defs/FrontendCapability"}, "uniqueItems": True},
            "permitted": {"type": "array", "items": {"$ref": "#/$defs/FrontendCapability"}, "uniqueItems": True},
        },
        "additionalProperties": True,
    }

    definitions["HelloAuthentication"] = {
        "type": "object",
        "required": ["scheme", "token"],
        "properties": {
            "scheme": {"const": "bearer"},
            "token": {"type": "string", "minLength": 1, "maxLength": 65536},
        },
        "additionalProperties": False,
    }
    hello_properties = definitions["Hello"]["allOf"][1]["properties"]
    hello_properties["capabilities"] = {
        "type": "array",
        "items": {"$ref": "#/$defs/FrontendCapability"},
        "uniqueItems": True,
    }
    hello_properties["authentication"] = {"$ref": "#/$defs/HelloAuthentication"}
    welcome_properties = definitions["Welcome"]["allOf"][1]["properties"]
    welcome_properties.update(
        {
            "capabilities": {"$ref": "#/$defs/CapabilityAdvertisement"},
            "availableMethods": {"type": "array", "items": {"$ref": "#/$defs/FrontendMethod"}, "uniqueItems": True},
            "permittedMethods": {"type": "array", "items": {"$ref": "#/$defs/FrontendMethod"}, "uniqueItems": True},
            "serverVersion": {"type": "string", "minLength": 1},
        }
    )

    pre_expanded_definition_names = set(definitions)
    bounded_string = {"type": "string", "maxLength": 16384}
    bounded_identifier = {"type": "string", "minLength": 1, "maxLength": 1024}
    bounded_count = {"type": "integer", "minimum": 0}
    definitions["SafePropertyName"] = {
        "not": {"pattern": sensitive_property_name_pattern()},
        "x-aisuite-forbiddenNormalizedPropertyNames": sorted(
            SENSITIVE_RESULT_FIELD_NAMES
        ),
    }
    definitions["StateFreshness"] = {
        "type": "string",
        "enum": ["unknown", "current", "stale"],
    }
    definitions["SourceStamp"] = {
        "type": "object",
        "required": ["generation", "freshness"],
        "properties": {
            "generation": {"$ref": "#/$defs/UInt64"},
            "freshness": {"$ref": "#/$defs/StateFreshness"},
        },
        "additionalProperties": True,
    }
    definitions["ExpandedThreadListState"] = {
        "type": "object",
        "required": ["hasLoadedPage", "complete", "pagesLoaded"],
        "properties": {
            "hasLoadedPage": {"type": "boolean"},
            "complete": {"type": "boolean"},
            "pagesLoaded": bounded_count,
            "nextCursor": bounded_string,
            "backwardsCursor": bounded_string,
            "stamp": {"$ref": "#/$defs/SourceStamp"},
        },
        "additionalProperties": True,
    }
    definitions["SafeDetailScalar"] = {
        "anyOf": [
            bounded_string,
            {"type": "boolean"},
            {"type": "integer"},
            {"type": "number"},
            {"type": "null"},
        ]
    }
    definitions["SafeDetailLeafValue"] = {
        "oneOf": [
            {"$ref": "#/$defs/SafeDetailScalar"},
            {
                "type": "array",
                "maxItems": 64,
                "items": {"$ref": "#/$defs/SafeDetailScalar"},
            },
        ]
    }
    definitions["SafeDetailObject"] = {
        "type": "object",
        "maxProperties": 64,
        "propertyNames": copy_json(SAFE_PROPERTY_NAMES),
        "additionalProperties": {"$ref": "#/$defs/SafeDetailLeafValue"},
    }
    definitions["SafeDetailValue"] = {
        "oneOf": [
            {"$ref": "#/$defs/SafeDetailScalar"},
            {
                "type": "array",
                "maxItems": 64,
                "items": {"$ref": "#/$defs/SafeDetailScalar"},
            },
            {"$ref": "#/$defs/SafeDetailObject"},
        ]
    }
    definitions["TruncationMetadata"] = {
        "type": "object",
        "required": ["truncated"],
        "properties": {
            "truncated": {"type": "boolean"},
            "omittedFields": {
                "type": "array",
                "maxItems": 64,
                "items": {"type": "string", "maxLength": 256},
                "uniqueItems": True,
            },
            "omittedEntries": bounded_count,
            "droppedBytes": {"$ref": "#/$defs/UInt64"},
        },
        "additionalProperties": True,
    }

    thread_item_names = [
        mapping["registryKey"].rsplit(":", 1)[1]
        for mapping in manifest["threadItemMappings"]
    ]
    definitions["ExpandedThreadItemBase"] = {
        "type": "object",
        "required": ["id", "type"],
        "properties": {
            "id": bounded_identifier,
            "type": {"type": "string"},
            "threadId": bounded_identifier,
            "turnId": bounded_identifier,
            "status": {"type": "string", "maxLength": 256},
            "summary": bounded_string,
            "location": {"$ref": "#/$defs/SafeDetailObject"},
            "agentText": bounded_string,
            "reasoningText": bounded_string,
            "reasoningSummary": bounded_string,
            "commandOutput": bounded_string,
            "droppedContentBytes": {"$ref": "#/$defs/UInt64"},
            "contentTruncated": {"type": "boolean"},
            "startedAtMs": {"type": "integer"},
            "completedAtMs": {"type": "integer"},
            "data": {"$ref": "#/$defs/SafeDetailObject"},
            "truncated": {"type": "boolean"},
            "omittedFields": {
                "type": "array",
                "maxItems": 64,
                "items": {"type": "string", "maxLength": 256},
                "uniqueItems": True,
            },
            "connectionInvalidated": {"type": "boolean"},
            "generation": {"$ref": "#/$defs/UInt64"},
            "freshness": {"$ref": "#/$defs/StateFreshness"},
        },
        "additionalProperties": True,
    }
    definitions["ExpandedThreadItem"] = {
        "oneOf": [
            {
                "allOf": [
                    {"$ref": "#/$defs/ExpandedThreadItemBase"},
                    {
                        "type": "object",
                        "properties": {"type": {"const": item_name}},
                    },
                ]
            }
            for item_name in thread_item_names
        ]
    }
    definitions["PendingRequestKind"] = {
        "type": "string",
        "enum": [mapping["kind"] for mapping in manifest["pendingRequestMappings"]],
    }
    definitions["ExpandedPendingRequestOption"] = {
        "type": "object",
        "required": ["label", "description"],
        "properties": {
            "label": bounded_string,
            "description": bounded_string,
        },
        "additionalProperties": True,
    }
    definitions["ExpandedPendingRequestQuestion"] = {
        "type": "object",
        "required": ["id", "header", "prompt", "allowsFreeText", "isSecret", "options"],
        "properties": {
            "id": bounded_string,
            "header": bounded_string,
            "prompt": bounded_string,
            "allowsFreeText": {"type": "boolean"},
            # This flag describes the input control. Secret answers are never
            # retained in the pending-request state or frontend journal.
            "isSecret": {"type": "boolean"},
            "options": {
                "type": "array",
                "maxItems": 64,
                "items": {"$ref": "#/$defs/ExpandedPendingRequestOption"},
            },
        },
        "additionalProperties": True,
    }
    definitions["ExpandedPendingRequestBase"] = {
        "type": "object",
        "required": ["pendingRequestId", "kind"],
        "properties": {
            "pendingRequestId": {"$ref": "#/$defs/DecimalId"},
            "kind": {"$ref": "#/$defs/PendingRequestKind"},
            "threadId": bounded_identifier,
            "turnId": bounded_identifier,
            "itemId": bounded_identifier,
            "summary": bounded_string,
            "details": {"$ref": "#/$defs/SafeDetailObject"},
            "questions": {
                "type": "array",
                "maxItems": 64,
                "items": {"$ref": "#/$defs/ExpandedPendingRequestQuestion"},
            },
            "autoResolutionMs": {"$ref": "#/$defs/UInt64"},
            "truncated": {"type": "boolean"},
            "omittedFields": {
                "type": "array",
                "maxItems": 64,
                "items": {"type": "string", "maxLength": 256},
                "uniqueItems": True,
            },
        },
        "additionalProperties": True,
    }
    definitions["ExpandedPendingRequest"] = {
        "oneOf": [
            {
                "allOf": [
                    {"$ref": "#/$defs/ExpandedPendingRequestBase"},
                    {
                        "type": "object",
                        **(
                            {"required": ["questions"]}
                            if kind == "user_input"
                            else {
                                "not": {
                                    "anyOf": [
                                        {"required": ["questions"]},
                                        {"required": ["autoResolutionMs"]},
                                    ]
                                }
                            }
                        ),
                        "properties": {"kind": {"const": kind}},
                    },
                ]
            }
            for kind in definitions["PendingRequestKind"]["enum"]
        ]
    }

    definitions["ProviderSnapshotState"] = {
        "type": "object",
        "required": ["lifecycle", "generation", "desiredRunning", "recovery"],
        "properties": {
            "lifecycle": {
                "enum": [
                    "stopped",
                    "starting",
                    "initializing",
                    "ready",
                    "stopping",
                    "failed",
                    "recovering",
                ]
            },
            "generation": {"$ref": "#/$defs/UInt64"},
            "desiredRunning": {"type": "boolean"},
            "recovery": {
                "type": "object",
                "required": ["status", "attempts"],
                "properties": {
                    "status": {"enum": ["idle", "waiting", "exhausted"]},
                    "attempts": bounded_count,
                    "delayMs": {"$ref": "#/$defs/UInt64"},
                },
                "additionalProperties": True,
            },
            "lastError": {"$ref": "#/$defs/SafeDetailObject"},
            "initialization": {"$ref": "#/$defs/SafeDetailObject"},
        },
        "additionalProperties": True,
    }
    definitions["ControllerSnapshotState"] = {
        "type": "object",
        "properties": {
            "controllerSessionId": {"$ref": "#/$defs/DecimalId"},
            "present": {"type": "boolean"},
        },
        "additionalProperties": True,
    }
    definitions["SessionSnapshotState"] = {
        "type": "object",
        "required": ["sessionId", "role"],
        "properties": {
            "sessionId": {"$ref": "#/$defs/DecimalId"},
            "role": {"enum": ["observer", "controller"]},
        },
        "additionalProperties": True,
    }
    definitions["ExpandedTurnState"] = {
        "type": "object",
        "required": ["id", "threadId", "status", "active", "terminal"],
        "properties": {
            "id": bounded_identifier,
            "threadId": bounded_identifier,
            "status": {"type": "string", "maxLength": 256},
            "active": {"type": "boolean"},
            "terminal": {"type": "boolean"},
            "items": {
                "type": "array",
                "maxItems": 65536,
                "items": {"$ref": "#/$defs/ExpandedThreadItem"},
            },
            "stamp": {"$ref": "#/$defs/SourceStamp"},
            "connectionInvalidated": {"type": "boolean"},
            "failure": {"$ref": "#/$defs/SafeDetailObject"},
            "tokenUsage": {"$ref": "#/$defs/SafeDetailObject"},
        },
        "additionalProperties": True,
    }
    definitions["ExpandedThreadState"] = {
        "type": "object",
        "required": ["id"],
        "properties": {
            "id": bounded_identifier,
            "title": bounded_string,
            "cwd": bounded_string,
            "model": {"type": "string", "maxLength": 1024},
            "modelProvider": {"type": "string", "maxLength": 1024},
            "preview": bounded_string,
            "status": {"type": "string", "maxLength": 256},
            "fullyLoaded": {"type": "boolean"},
            "turns": {
                "type": "array",
                "maxItems": 16384,
                "items": {"$ref": "#/$defs/ExpandedTurnState"},
            },
            "realtime": {"$ref": "#/$defs/SafeDetailObject"},
            "stamp": {"$ref": "#/$defs/SourceStamp"},
        },
        "additionalProperties": True,
    }
    definitions["DomainResultSummary"] = {
        "type": "object",
        "required": ["method", "status", "stamp"],
        "properties": {
            "method": {"type": "string", "minLength": 1, "maxLength": 1024},
            "status": {"type": "string", "maxLength": 256},
            "subjectId": bounded_identifier,
            "nextCursor": bounded_string,
            "itemCount": bounded_count,
            "complete": {"type": "boolean"},
            "stamp": {"$ref": "#/$defs/SourceStamp"},
        },
        "additionalProperties": True,
    }
    definitions["DomainProjection"] = {
        "type": "object",
        "properties": {
            "stamp": {"$ref": "#/$defs/SourceStamp"},
            "status": {"type": "string", "maxLength": 256},
            "summary": bounded_string,
            "nextCursor": bounded_string,
            "complete": {"type": "boolean"},
            "itemCount": bounded_count,
            "latestResults": {
                "type": "array",
                "maxItems": 128,
                "items": {"$ref": "#/$defs/DomainResultSummary"},
            },
            "details": {"$ref": "#/$defs/SafeDetailObject"},
            "truncation": {"$ref": "#/$defs/TruncationMetadata"},
        },
        "additionalProperties": True,
    }
    definitions["ProcessState"] = {
        "type": "object",
        "required": ["processHandle", "lifecycle", "stamp"],
        "properties": {
            "processHandle": bounded_identifier,
            "lifecycle": {"type": "string", "maxLength": 256},
            "stdout": bounded_string,
            "stderr": bounded_string,
            "stdoutBytes": bounded_count,
            "stderrBytes": bounded_count,
            "stdoutTruncated": {"type": "boolean"},
            "stderrTruncated": {"type": "boolean"},
            "droppedOutputBytes": {"$ref": "#/$defs/UInt64"},
            "exitCode": {"type": "integer"},
            "stamp": {"$ref": "#/$defs/SourceStamp"},
            "connectionInvalidated": {"type": "boolean"},
        },
        "additionalProperties": True,
    }
    definitions["FilesystemWatchState"] = {
        "type": "object",
        "required": ["watchId", "stamp"],
        "properties": {
            "watchId": bounded_identifier,
            "root": bounded_string,
            "changedPathCount": bounded_count,
            "stamp": {"$ref": "#/$defs/SourceStamp"},
            "connectionInvalidated": {"type": "boolean"},
        },
        "additionalProperties": True,
    }
    definitions["FuzzySearchState"] = {
        "type": "object",
        "required": ["sessionId", "complete", "stamp"],
        "properties": {
            "sessionId": bounded_identifier,
            "resultCount": bounded_count,
            "complete": {"type": "boolean"},
            "stamp": {"$ref": "#/$defs/SourceStamp"},
            "connectionInvalidated": {"type": "boolean"},
        },
        "additionalProperties": True,
    }
    definitions["NoticeState"] = {
        "type": "object",
        "required": ["category", "summary", "stamp"],
        "properties": {
            "occurrence": {"$ref": "#/$defs/UInt64"},
            "category": {"type": "string", "maxLength": 256},
            "summary": bounded_string,
            "details": bounded_string,
            "threadId": bounded_identifier,
            "stamp": {"$ref": "#/$defs/SourceStamp"},
        },
        "additionalProperties": True,
    }
    definitions["ActivityState"] = {
        "type": "object",
        "required": ["kind", "lifecycle", "active", "stamp"],
        "properties": {
            "key": bounded_identifier,
            "subjectId": bounded_identifier,
            "kind": {"type": "string", "maxLength": 256},
            "lifecycle": {"type": "string", "maxLength": 256},
            "summary": bounded_string,
            "details": bounded_string,
            "threadId": bounded_identifier,
            "turnId": bounded_identifier,
            "active": {"type": "boolean"},
            "stamp": {"$ref": "#/$defs/SourceStamp"},
        },
        "additionalProperties": True,
    }
    definitions["CapacityProjection"] = {
        "type": "object",
        "properties": {
            name: bounded_count
            for name in (
                "sessions",
                "observers",
                "activeOperations",
                "pendingRequests",
                "retainedThreads",
                "retainedTurns",
                "retainedItems",
                "accumulatedContentBytes",
                "retainedNotices",
                "retainedProcesses",
                "accumulatedProcessOutputBytes",
                "retainedFilesystemWatches",
                "retainedFuzzySearchSessions",
                "retainedActivityRecords",
                "evictedNotices",
                "evictedProcesses",
                "droppedProcessOutputBytes",
                "evictedFilesystemWatches",
                "evictedFuzzySearchSessions",
                "evictedActivityRecords",
            )
        },
        "additionalProperties": True,
    }

    definitions["ExpandedEventType"] = {"type": "string", "enum": list(event_families)}
    event_required_fields = {
        "provider.updated": ("provider",),
        "controller.updated": ("controller",),
        "sessions.updated": ("sessions",),
        "threadList.updated": ("threadList",),
        "thread.upserted": ("thread",),
        "thread.removed": ("threadId",),
        "turn.upserted": ("turn",),
        "item.upserted": ("item",),
        "item.content.updated": ("threadId", "turnId", "itemId", "content"),
        "pendingRequests.updated": ("pendingRequests",),
        "account.updated": ("domain",),
        "models.updated": ("domain",),
        "configuration.updated": ("domain",),
        "process.updated": ("process",),
        "filesystemWatch.updated": ("filesystemWatch",),
        "fuzzySearch.updated": ("fuzzySearch",),
        "reviews.updated": ("domain",),
        "integrations.updated": ("domain",),
        "plugins.updated": ("domain",),
        "skills.updated": ("domain",),
        "mcp.updated": ("domain",),
        "platform.updated": ("domain",),
        "notice.added": ("notice",),
        "activity.updated": ("activity",),
        "capacity.updated": ("capacity",),
        "diagnostics.updated": ("diagnostic",),
    }
    if set(event_required_fields) != set(event_families):
        raise GenerationError("expanded event schema alternatives drifted from the owner-reviewed event-family authority")
    event_data_properties = {
        "provider": {"$ref": "#/$defs/ProviderSnapshotState"},
        "controller": {"$ref": "#/$defs/ControllerSnapshotState"},
        "sessions": {
            "type": "array",
            "maxItems": 128,
            "items": {"$ref": "#/$defs/SessionSnapshotState"},
        },
        "threadList": {"$ref": "#/$defs/ExpandedThreadListState"},
        "thread": {"$ref": "#/$defs/ExpandedThreadState"},
        "threadId": bounded_identifier,
        "turn": {"$ref": "#/$defs/ExpandedTurnState"},
        "turnId": bounded_identifier,
        "item": {"$ref": "#/$defs/ExpandedThreadItem"},
        "itemId": bounded_identifier,
        "channel": {"type": "string", "maxLength": 256},
        "content": bounded_string,
        "pendingRequests": {
            "type": "array",
            "maxItems": 1024,
            "items": {"$ref": "#/$defs/ExpandedPendingRequest"},
        },
        "domain": {"$ref": "#/$defs/DomainProjection"},
        "process": {"$ref": "#/$defs/ProcessState"},
        "filesystemWatch": {"$ref": "#/$defs/FilesystemWatchState"},
        "fuzzySearch": {"$ref": "#/$defs/FuzzySearchState"},
        "notice": {"$ref": "#/$defs/NoticeState"},
        "activity": {"$ref": "#/$defs/ActivityState"},
        "capacity": {"$ref": "#/$defs/CapacityProjection"},
        "diagnostic": {"$ref": "#/$defs/SafeDetailObject"},
    }
    definitions["ExpandedFrontendEvent"] = {
        "oneOf": [
            {
                "type": "object",
                "required": ["sequence", "type", "data"],
                "properties": {
                    "sequence": {"$ref": "#/$defs/PositiveUInt64"},
                    "type": {"const": event_type},
                    "data": {
                        "type": "object",
                        "required": list(event_required_fields[event_type]),
                        "properties": event_data_properties,
                        "maxProperties": 32,
                        "additionalProperties": True,
                    },
                },
                "additionalProperties": True,
            }
            for event_type in event_families
        ]
    }
    domain_properties: dict[str, Any] = {
        "provider": {"$ref": "#/$defs/ProviderSnapshotState"},
        "controller": {"$ref": "#/$defs/ControllerSnapshotState"},
        "sessions": {
            "type": "array",
            "maxItems": 128,
            "items": {"$ref": "#/$defs/SessionSnapshotState"},
        },
        "threadList": {"$ref": "#/$defs/ExpandedThreadListState"},
        "threads": {
            "type": "array",
            "maxItems": 2048,
            "items": {"$ref": "#/$defs/ExpandedThreadState"},
        },
        "turns": {
            "type": "array",
            "maxItems": 16384,
            "items": {"$ref": "#/$defs/ExpandedTurnState"},
        },
        "items": {
            "type": "array",
            "maxItems": 65536,
            "items": {"$ref": "#/$defs/ExpandedThreadItem"},
        },
        "pendingRequests": {
            "type": "array",
            "maxItems": 1024,
            "items": {"$ref": "#/$defs/ExpandedPendingRequest"},
        },
    }
    for domain in (
        "accounts",
        "models",
        "configuration",
        "permissionProfiles",
        "reviews",
        "apps",
        "externalAgents",
        "hooks",
        "marketplace",
        "plugins",
        "skills",
        "mcp",
        "windowsSandbox",
        "remoteControl",
    ):
        domain_properties[domain] = {"$ref": "#/$defs/DomainProjection"}
    domain_properties.update(
        {
            "processes": {
                "type": "object",
                "properties": {
                    "entries": {
                        "type": "array",
                        "maxItems": 256,
                        "items": {"$ref": "#/$defs/ProcessState"},
                    },
                    "truncation": {"$ref": "#/$defs/TruncationMetadata"},
                },
                "additionalProperties": True,
            },
            "filesystemWatches": {
                "type": "object",
                "properties": {
                    "entries": {
                        "type": "array",
                        "maxItems": 1024,
                        "items": {"$ref": "#/$defs/FilesystemWatchState"},
                    },
                    "truncation": {"$ref": "#/$defs/TruncationMetadata"},
                },
                "additionalProperties": True,
            },
            "fuzzySearches": {
                "type": "object",
                "properties": {
                    "entries": {
                        "type": "array",
                        "maxItems": 256,
                        "items": {"$ref": "#/$defs/FuzzySearchState"},
                    },
                    "truncation": {"$ref": "#/$defs/TruncationMetadata"},
                },
                "additionalProperties": True,
            },
            "notices": {
                "type": "object",
                "properties": {
                    "entries": {
                        "type": "array",
                        "maxItems": 256,
                        "items": {"$ref": "#/$defs/NoticeState"},
                    },
                    "truncation": {"$ref": "#/$defs/TruncationMetadata"},
                },
                "additionalProperties": True,
            },
            "activities": {
                "type": "object",
                "properties": {
                    "entries": {
                        "type": "array",
                        "maxItems": 512,
                        "items": {"$ref": "#/$defs/ActivityState"},
                    },
                    "truncation": {"$ref": "#/$defs/TruncationMetadata"},
                },
                "additionalProperties": True,
            },
            "capacity": {"$ref": "#/$defs/CapacityProjection"},
            "truncation": {"$ref": "#/$defs/TruncationMetadata"},
        }
    )
    definitions["ExpandedBackendSnapshotState"] = {
        "type": "object",
        "required": ["provider", "controller", "sessions", "threadList", "capacity", "truncation"],
        "properties": domain_properties,
        "additionalProperties": True,
    }
    definitions["ExpandedSnapshot"] = {
        "allOf": [
            {"$ref": "#/$defs/Envelope"},
            {
                "type": "object",
                "required": ["kind", "sequence", "state"],
                "properties": {
                    "kind": {"const": "snapshot"},
                    "sequence": {"$ref": "#/$defs/UInt64"},
                    "state": {"$ref": "#/$defs/ExpandedBackendSnapshotState"},
                },
                "additionalProperties": True,
            },
        ]
    }
    definitions["ExpandedEventBatch"] = {
        "allOf": [
            {"$ref": "#/$defs/Envelope"},
            {
                "type": "object",
                "required": ["kind", "fromSequence", "toSequence", "events"],
                "properties": {
                    "kind": {"const": "events"},
                    "fromSequence": {"$ref": "#/$defs/PositiveUInt64"},
                    "toSequence": {"$ref": "#/$defs/PositiveUInt64"},
                    "events": {
                        "type": "array",
                        "maxItems": 4096,
                        "items": {"$ref": "#/$defs/ExpandedFrontendEvent"},
                    },
                },
                "additionalProperties": True,
            },
        ],
        "description": (
            "Expanded event sequences increase strictly between canonical occurrences. "
            "All recognized expanded families projected from one occurrence may repeat that "
            "occurrence sequence as one atomic group. The outer range agrees exactly with the "
            "first and last event."
        ),
    }
    expanded_definition_names = set(definitions) - pre_expanded_definition_names
    for definition_name in expanded_definition_names:
        definitions[definition_name] = secure_safe_object_extensions(
            definitions[definition_name]
        )
    for method in manifest["methods"]:
        result_definition = f"{method['id']}Result"
        definitions[result_definition] = secure_safe_object_extensions(
            definitions[result_definition]
        )
    schema["oneOf"].extend(
        [
            {"$ref": "#/$defs/ExpandedSnapshot"},
            {"$ref": "#/$defs/ExpandedEventBatch"},
        ]
    )
    schema["x-aisuite-frontend-contract"] = {
        "methods": 105,
        "existingMethods": 15,
        "additiveMethods": 90,
        "runtimeAvailableMethods": 90,
        "reviewedIdentities": 234,
        "notificationMappings": 68,
        "threadItemMappings": 18,
        "pendingRequestMappings": 10,
    }
    audit_runtime_schema_profile(schema, manifest)
    return schema


def copy_json(value: Any) -> Any:
    return json.loads(json.dumps(value))


def generate_header(manifest: dict[str, Any]) -> str:
    methods = manifest["methods"]
    lines = [
        "/* Generated by tools/frontend/generate_frontend_protocol.py. Do not edit. */",
        "// clang-format off",
        "#ifndef AI_OPENAI_CODEX_FRONTEND_GENERATEDPROTOCOL_H",
        "#define AI_OPENAI_CODEX_FRONTEND_GENERATEDPROTOCOL_H",
        "",
        '#include "Security.h"',
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <optional>",
        "#include <span>",
        "#include <string>",
        "#include <string_view>",
        "#include <utility>",
        "#include <variant>",
        "#include <nlohmann/json.hpp>",
        "",
        "namespace ai::openai::codex::frontend::generated {",
        "",
        "    enum class MethodCategory { BackendControl, FrontendReplay, ProviderLifecycle, ProviderOperation, ReverseResponse };",
        "    enum class MethodId {",
    ]
    lines.extend(f"        {row['id']}," for row in methods)
    lines.extend(["        Count", "    };", ""])
    lines.append("    enum class Capability {")
    lines.extend(f"        {cpp_id(capability)}," for capability in CAPABILITIES)
    lines.extend(["        Count", "    };", ""])
    lines.extend(
        [
            "    enum class ProjectionFamily { ServerNotification, ThreadItem, PendingRequest };",
            "    enum class CompatibilityRepresentation { Legacy, Expanded };",
            "",
            "    struct MethodMetadata {",
            "        MethodId id;",
            "        std::string_view method;",
            "        MethodCategory category;",
            "        bool frontendNative;",
            "        std::span<const std::string_view> registryKeys;",
            "        std::string_view genericContractKey;",
            "        std::string_view backendCommand;",
            "        std::string_view serviceAction;",
            "        std::string_view parameterSchema;",
            "        std::string_view resultSchema;",
            "        std::string_view resultType;",
            "        std::span<const std::string_view> parameterFields;",
            "        std::span<const std::string_view> requiredParameterFields;",
            "        std::string_view exposure;",
            "        std::string_view securityDecision;",
            "        std::span<const FrontendScope> requiredScopes;",
            "        bool controllerRequired;",
            "        bool providerReadyRequired;",
            "        bool defaultEnabled;",
            "        bool currentlyImplemented;",
            "        bool legacyCompatibilityMethod;",
            "        bool observerAvailability;",
            "        bool sensitiveResult;",
            "        bool largeResult;",
            "        std::string_view compatibilityStatus;",
            "        std::string_view capability;",
            "        std::string_view implementationPhase;",
            "        std::string_view parameterPolicy;",
            "    };",
            "",
            "    enum class CapabilityCategory { StaticMechanism, ConditionalTopology, Product };",
            "    struct CapabilityMetadata { Capability id; std::string_view key; CapabilityCategory category; bool defined; bool implementedByCurrentRuntime; };",
            "    struct AuthenticationMetadata { std::string_view helloField; std::string_view bearerScheme; std::size_t maximumBearerTokenBytes; };",
            "    struct ContractMetadata { std::string_view registryKey; std::string_view exposure; std::string_view securityDecision; std::string_view mappings; std::string_view redactionClass; std::string_view compatibilityBehavior; bool controllerRequired; bool defaultEnabled; };",
            "    struct ProjectionMetadata {",
            "        std::string_view registryKey;",
            "        ProjectionFamily family;",
            "        std::string_view exposure;",
            "        std::string_view securityDecision;",
            "        std::string_view legacyContract;",
            "        std::span<const std::string_view> expandedMappings;",
            "        std::span<const FrontendScope> requiredScopes;",
            "        std::string_view redactionClass;",
            "        Capability expansionCapability;",
            "    };",
            "    struct PendingRequestProjectionMetadata {",
            "        std::string_view registryKey;",
            "        std::string_view providerMethod;",
            "        std::string_view kind;",
            "        std::string_view exposure;",
            "        std::string_view securityDecision;",
            "        std::string_view legacyContract;",
            "        std::string_view expandedEvent;",
            "        std::span<const std::string_view> responseMethods;",
            "        std::span<const FrontendScope> presentationRequiredScopes;",
            "        bool controllerRequiredForPresentation;",
            "        std::span<const FrontendScope> responseRequiredScopes;",
            "        bool controllerRequiredForResponse;",
            "        std::string_view redactionClass;",
            "        std::string_view duplicateSuppression;",
            "        Capability expansionCapability;",
            "    };",
            "",
            "    inline constexpr AuthenticationMetadata HelloAuthentication{\"authentication\", \"bearer\", 65536};",
            "",
        ]
    )
    for row in methods:
        name = row["id"]
        keys = ", ".join(q(key) for key in row["registryKeys"])
        scopes = ", ".join(f"FrontendScope::{SCOPE_ENUM[scope]}" for scope in row["requiredScopes"])
        parameter_fields = ", ".join(q(field) for field in row["parameterFields"])
        required_fields = ", ".join(q(field) for field in row["requiredParameterFields"])
        lines.append(f"    inline constexpr std::array<std::string_view, {len(row['registryKeys'])}> {name}RegistryKeys{{{keys}}};")
        lines.append(f"    inline constexpr std::array<FrontendScope, {len(row['requiredScopes'])}> {name}Scopes{{{scopes}}};")
        lines.append(f"    inline constexpr std::array<std::string_view, {len(row['parameterFields'])}> {name}ParameterFields{{{parameter_fields}}};")
        lines.append(f"    inline constexpr std::array<std::string_view, {len(row['requiredParameterFields'])}> {name}RequiredParameterFields{{{required_fields}}};")
    for family, mappings in (
        ("Notification", manifest["notificationMappings"]),
        ("ThreadItem", manifest["threadItemMappings"]),
    ):
        for index, mapping in enumerate(mappings):
            expanded = ", ".join(q(value) for value in mapping["expandedMappings"])
            scopes = ", ".join(
                f"FrontendScope::{SCOPE_ENUM[scope]}"
                for scope in mapping["requiredScopes"]
            )
            lines.append(
                f"    inline constexpr std::array<std::string_view, {len(mapping['expandedMappings'])}> "
                f"{family}Projection{index}Mappings{{{expanded}}};"
            )
            lines.append(
                f"    inline constexpr std::array<FrontendScope, {len(mapping['requiredScopes'])}> "
                f"{family}Projection{index}Scopes{{{scopes}}};"
            )
    for index, mapping in enumerate(manifest["pendingRequestMappings"]):
        responses = ", ".join(q(value) for value in mapping["responseMethods"])
        presentation_scopes = ", ".join(
            f"FrontendScope::{SCOPE_ENUM[scope]}"
            for scope in mapping["presentationRequiredScopes"]
        )
        response_scopes = ", ".join(
            f"FrontendScope::{SCOPE_ENUM[scope]}"
            for scope in mapping["responseRequiredScopes"]
        )
        lines.append(
            f"    inline constexpr std::array<std::string_view, {len(mapping['responseMethods'])}> "
            f"PendingRequestProjection{index}ResponseMethods{{{responses}}};"
        )
        lines.append(
            f"    inline constexpr std::array<FrontendScope, {len(mapping['presentationRequiredScopes'])}> "
            f"PendingRequestProjection{index}PresentationScopes{{{presentation_scopes}}};"
        )
        lines.append(
            f"    inline constexpr std::array<FrontendScope, {len(mapping['responseRequiredScopes'])}> "
            f"PendingRequestProjection{index}ResponseScopes{{{response_scopes}}};"
        )
    lines.extend(["", f"    inline constexpr std::array<MethodMetadata, {len(methods)}> AllMethods{{{{"])
    for row in methods:
        lines.append(
            "        {MethodId::%s, %s, MethodCategory::%s, %s, %sRegistryKeys, %s, %s, %s, %s, %s, %s, %sParameterFields, %sRequiredParameterFields, %s, %s, %sScopes, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s},"
            % (
                row["id"], q(row["method"]), category_cpp(row["category"]), str(row["frontendNative"]).lower(), row["id"],
                q(row["genericContractKey"]), q(row["backendCommand"]), q(row["serviceAction"]), q(row["parameterSchema"]), q(row["resultSchema"]), q(row["resultType"]),
                row["id"], row["id"], q(row["exposure"]), q(row["securityDecision"]), row["id"], str(row["controllerRequired"]).lower(), str(row["providerReadyRequired"]).lower(), str(row["defaultEnabled"]).lower(),
                str(row["currentlyImplemented"]).lower(), str(row["legacyCompatibilityMethod"]).lower(), str(row["observerAvailability"]).lower(), str(row["sensitiveResult"]).lower(), str(row["largeResult"]).lower(),
                q(row["compatibilityStatus"]), q(row["capability"]), q(row["implementationPhase"]), q(row["parameterPolicy"]),
            )
        )
    lines.extend(["    }};", ""])
    capabilities = manifest["capabilities"]
    lines.append(f"    inline constexpr std::array<CapabilityMetadata, {len(capabilities)}> AllCapabilities{{{{")
    for capability in capabilities:
        category = {
            "static_mechanism": "StaticMechanism",
            "conditional_topology": "ConditionalTopology",
            "product": "Product",
        }[capability["category"]]
        lines.append(
            f"        {{Capability::{cpp_id(capability['key'])}, {q(capability['key'])}, CapabilityCategory::{category}, true, {str(capability['implementedByCurrentRuntime']).lower()}}},"
        )
    lines.extend(["    }};", ""])
    for family, family_cpp, mappings in (
        ("Notification", "ServerNotification", manifest["notificationMappings"]),
        ("ThreadItem", "ThreadItem", manifest["threadItemMappings"]),
    ):
        lines.append(
            f"    inline constexpr std::array<ProjectionMetadata, {len(mappings)}> All{family}Projections{{{{"
        )
        for index, mapping in enumerate(mappings):
            lines.append(
                "        {%s, ProjectionFamily::%s, %s, %s, %s, %sProjection%dMappings, %sProjection%dScopes, %s, Capability::%s},"
                % (
                    q(mapping["registryKey"]),
                    family_cpp,
                    q(mapping["finalExposure"]),
                    q(mapping["securityDecision"]),
                    q(mapping["legacyContract"]),
                    family,
                    index,
                    family,
                    index,
                    q(mapping["redactionClass"]),
                    cpp_id(mapping["capability"]),
                )
            )
        lines.extend(["    }};", ""])
    pending_requests = manifest["pendingRequestMappings"]
    lines.append(
        f"    inline constexpr std::array<PendingRequestProjectionMetadata, {len(pending_requests)}> AllPendingRequestProjections{{{{"
    )
    for index, mapping in enumerate(pending_requests):
        lines.append(
            "        {%s, %s, %s, %s, %s, %s, %s, PendingRequestProjection%dResponseMethods, "
            "PendingRequestProjection%dPresentationScopes, %s, PendingRequestProjection%dResponseScopes, %s, %s, %s, Capability::%s},"
            % (
                q(mapping["registryKey"]),
                q(mapping["providerMethod"]),
                q(mapping["kind"]),
                q(mapping["finalExposure"]),
                q(mapping["securityDecision"]),
                q(mapping["legacyContract"]),
                q(mapping["expandedEvent"]),
                index,
                index,
                str(mapping["controllerRequiredForPresentation"]).lower(),
                index,
                str(mapping["controllerRequiredForResponse"]).lower(),
                q(mapping["redactionClass"]),
                q(mapping["duplicateSuppression"]),
                cpp_id(mapping["capability"]),
            )
        )
    lines.extend(["    }};", ""])
    contracts = manifest["reviewedContracts"]
    lines.append(f"    inline constexpr std::array<ContractMetadata, {len(contracts)}> AllReviewedContracts{{{{")
    for row in contracts:
        lines.append(
            "        {%s, %s, %s, %s, %s, %s, %s, %s},"
            % (
                q(registry_key(row)), q(row["exposure"]), q(row["securityDecision"]), q(",".join(row["mappings"])),
                q(row["redactionClass"]), q(row["compatibilityBehavior"]), str(row["controllerRequired"]).lower(), str(row["defaultEnabled"]).lower(),
            )
        )
    lines.extend(
        [
            "    }};",
            "",
            "    [[nodiscard]] constexpr std::string_view methodString(MethodId id) noexcept {",
            "        const auto index = static_cast<std::size_t>(id);",
            "        return index < AllMethods.size() ? AllMethods[index].method : std::string_view{};",
            "    }",
            "",
            "    [[nodiscard]] constexpr std::optional<MethodId> definedMethodFromString(std::string_view value) noexcept {",
            "        for (const auto& method : AllMethods) if (method.method == value) return method.id;",
            "        return std::nullopt;",
            "    }",
            "",
            "    [[nodiscard]] constexpr std::optional<MethodId> runtimeMethodFromString(std::string_view value) noexcept {",
            "        for (const auto& method : AllMethods) if (method.currentlyImplemented && method.method == value) return method.id;",
            "        return std::nullopt;",
            "    }",
            "",
            "    [[nodiscard]] constexpr std::optional<MethodId> legacyMethodFromString(std::string_view value) noexcept {",
            "        for (const auto& method : AllMethods) if (method.legacyCompatibilityMethod && method.method == value) return method.id;",
            "        return std::nullopt;",
            "    }",
            "",
            "    [[nodiscard]] constexpr CompatibilityRepresentation selectCompatibilityRepresentation(",
            "        const ProjectionMetadata& metadata,",
            "        std::span<const Capability> negotiatedCapabilities) noexcept {",
            "        // This selects one compatibility representation; it does not authorize or activate delivery.",
            "        for (const Capability capability : negotiatedCapabilities)",
            "            if (capability == metadata.expansionCapability) return CompatibilityRepresentation::Expanded;",
            "        return CompatibilityRepresentation::Legacy;",
            "    }",
            "",
            "    [[nodiscard]] constexpr bool emitsLegacy(CompatibilityRepresentation representation) noexcept {",
            "        return representation == CompatibilityRepresentation::Legacy;",
            "    }",
            "",
            "    [[nodiscard]] constexpr bool emitsExpanded(CompatibilityRepresentation representation) noexcept {",
            "        return representation == CompatibilityRepresentation::Expanded;",
            "    }",
            "",
            "    [[nodiscard]] constexpr const PendingRequestProjectionMetadata* pendingRequestProjectionFromKind(std::string_view kind) noexcept {",
            "        for (const auto& metadata : AllPendingRequestProjections) if (metadata.kind == kind) return &metadata;",
            "        return nullptr;",
            "    }",
            "",
            "    [[nodiscard]] constexpr const PendingRequestProjectionMetadata* pendingRequestProjectionFromProviderMethod(std::string_view method) noexcept {",
            "        for (const auto& metadata : AllPendingRequestProjections) if (metadata.providerMethod == method) return &metadata;",
            "        return nullptr;",
            "    }",
            "",
            "    consteval std::size_t countCategory(MethodCategory category) { std::size_t count = 0; for (const auto& method : AllMethods) count += method.category == category; return count; }",
            "    consteval std::size_t countNative() { std::size_t count = 0; for (const auto& method : AllMethods) count += method.frontendNative; return count; }",
            "    consteval std::size_t countImplemented() { std::size_t count = 0; for (const auto& method : AllMethods) count += method.currentlyImplemented; return count; }",
            "    consteval std::size_t countLegacy() { std::size_t count = 0; for (const auto& method : AllMethods) count += method.legacyCompatibilityMethod; return count; }",
            "    consteval bool profileContains(std::span<const FrontendScope> profile, FrontendScope required) { for (FrontendScope scope : profile) if (scope == required) return true; return false; }",
            "    consteval bool staticallyPermitted(const MethodMetadata& method, std::span<const FrontendScope> profile) { for (FrontendScope required : method.requiredScopes) if (!profileContains(profile, required)) return false; return true; }",
            "    consteval std::size_t countAvailable() { std::size_t count = 0; for (const auto& method : AllMethods) count += method.currentlyImplemented && method.defaultEnabled; return count; }",
            "    consteval std::size_t countPermitted(std::span<const FrontendScope> profile) { std::size_t count = 0; for (const auto& method : AllMethods) count += method.currentlyImplemented && method.defaultEnabled && staticallyPermitted(method, profile); return count; }",
            "    consteval std::size_t countProviderSecurity(std::string_view decision) { std::size_t count = 0; for (const auto& method : AllMethods) count += method.category == MethodCategory::ProviderOperation && method.securityDecision == decision; return count; }",
            "    consteval std::size_t countProviderReady() { std::size_t count = 0; for (const auto& method : AllMethods) count += method.providerReadyRequired; return count; }",
            "    consteval std::size_t countImplementedMechanismCapabilities() { std::size_t count = 0; for (const auto& capability : AllCapabilities) count += capability.category == CapabilityCategory::StaticMechanism && capability.implementedByCurrentRuntime; return count; }",
            "    consteval bool uniqueMethods() { for (std::size_t i = 0; i < AllMethods.size(); ++i) for (std::size_t j = i + 1; j < AllMethods.size(); ++j) if (AllMethods[i].method == AllMethods[j].method) return false; return true; }",
            "    consteval bool uniquePendingRequestProjections() { for (std::size_t i = 0; i < AllPendingRequestProjections.size(); ++i) for (std::size_t j = i + 1; j < AllPendingRequestProjections.size(); ++j) if (AllPendingRequestProjections[i].registryKey == AllPendingRequestProjections[j].registryKey || AllPendingRequestProjections[i].providerMethod == AllPendingRequestProjections[j].providerMethod || AllPendingRequestProjections[i].kind == AllPendingRequestProjections[j].kind) return false; return true; }",
            "",
            "    inline constexpr std::size_t MethodCount = AllMethods.size();",
            "    inline constexpr std::size_t FrontendNativeMethodCount = countNative();",
            "    inline constexpr std::size_t NonNativeMethodCount = MethodCount - FrontendNativeMethodCount;",
            "    inline constexpr std::size_t ImplementedMethodCount = countImplemented();",
            "    inline constexpr std::size_t ExistingMethodCount = countLegacy();",
            "    inline constexpr std::size_t AdditiveMethodCount = MethodCount - ExistingMethodCount;",
            "    inline constexpr std::size_t DefaultAvailableMethodCount = countAvailable();",
            "    inline constexpr std::size_t DefaultRemotePermittedMethodCount = countPermitted(DefaultRemoteScopes);",
            "    inline constexpr std::size_t LocalTrustedPermittedMethodCount = countPermitted(LocalTrustedScopes);",
            "    inline constexpr std::size_t ProviderOperationMethodCount = countCategory(MethodCategory::ProviderOperation);",
            "    inline constexpr std::size_t ReverseMethodCount = countCategory(MethodCategory::ReverseResponse);",
            "    inline constexpr std::size_t ProviderLifecycleMethodCount = countCategory(MethodCategory::ProviderLifecycle);",
            "    inline constexpr std::size_t ProviderReadyRequiredMethodCount = countProviderReady();",
            "    inline constexpr std::size_t ObserverReadProviderMethodCount = countProviderSecurity(\"ObserverReadApproved\");",
            "    inline constexpr std::size_t ControllerRequiredProviderMethodCount = countProviderSecurity(\"ControllerRequiredApproved\");",
            "    inline constexpr std::size_t PrivilegedProviderMethodCount = countProviderSecurity(\"PrivilegedScopedApproved\");",
            "    inline constexpr std::size_t ConditionalProviderMethodCount = countProviderSecurity(\"ConditionalExplicitEnablementApproved\");",
            "    inline constexpr std::size_t ParameterSensitiveProviderMethodCount = countProviderSecurity(\"ParameterSensitiveApproved\");",
            "    inline constexpr std::size_t ImplementedMechanismCapabilityCount = countImplementedMechanismCapabilities();",
            "    inline constexpr std::size_t ReviewedIdentityCount = AllReviewedContracts.size();",
            "",
            "    static_assert(MethodCount == 105);",
            "    static_assert(FrontendNativeMethodCount == 7);",
            "    static_assert(NonNativeMethodCount == 98);",
            "    static_assert(ImplementedMethodCount == 105);",
            "    static_assert(ExistingMethodCount == 15);",
            "    static_assert(AdditiveMethodCount == 90);",
            "    static_assert(DefaultAvailableMethodCount == 90);",
            "    static_assert(DefaultRemotePermittedMethodCount == 53);",
            "    static_assert(LocalTrustedPermittedMethodCount == 90);",
            "    static_assert(ProviderOperationMethodCount == 86);",
            "    static_assert(ReverseMethodCount == 12);",
            "    static_assert(ProviderLifecycleMethodCount == 3);",
            "    static_assert(ProviderReadyRequiredMethodCount == 98);",
            "    static_assert(ObserverReadProviderMethodCount == 26);",
            "    static_assert(ControllerRequiredProviderMethodCount == 22);",
            "    static_assert(PrivilegedProviderMethodCount == 22);",
            "    static_assert(ConditionalProviderMethodCount == 15);",
            "    static_assert(ParameterSensitiveProviderMethodCount == 1);",
            "    static_assert(ImplementedMechanismCapabilityCount == 13);",
            "    static_assert(ReviewedIdentityCount == 234);",
            "    static_assert(AllNotificationProjections.size() == 68);",
            "    static_assert(AllThreadItemProjections.size() == 18);",
            "    static_assert(AllPendingRequestProjections.size() == 10);",
            "    static_assert(uniquePendingRequestProjections());",
            "    static_assert(uniqueMethods());",
            "",
        ]
    )
    lines.extend(
        [
            "    template <MethodId Id>",
            "    struct MethodParameters {",
            "        static constexpr MethodId Method = Id;",
            "        nlohmann::json value = nlohmann::json::object();",
            "        bool operator==(const MethodParameters&) const = default;",
            "    };",
            "",
            "    using CompleteCommandParameters = std::variant<",
        ]
    )
    for index, row in enumerate(methods):
        suffix = "," if index + 1 < len(methods) else ">;"
        lines.append(f"        MethodParameters<MethodId::{row['id']}>{suffix}")
    lines.extend(
        [
            "",
            "    struct DefinedCommand {",
            "        std::string requestId;",
            "        CompleteCommandParameters parameters;",
            "        nlohmann::json extensions = nlohmann::json::object();",
            "        nlohmann::json parameterExtensions = nlohmann::json::object();",
            "        bool operator==(const DefinedCommand&) const = default;",
            "    };",
            "",
            "    [[nodiscard]] constexpr MethodId commandMethod(const CompleteCommandParameters& parameters) noexcept {",
            "        return std::visit([]<typename T>(const T&) constexpr noexcept { return T::Method; }, parameters);",
            "    }",
            "",
            "    [[nodiscard]] inline CompleteCommandParameters makeParameters(MethodId id, nlohmann::json value) {",
            "        switch (id) {",
        ]
    )
    for row in methods:
        lines.append(
            f"            case MethodId::{row['id']}: return MethodParameters<MethodId::{row['id']}>{{std::move(value)}};"
        )
    lines.extend(
        [
            "            case MethodId::Count: break;",
            "        }",
            "        return MethodParameters<MethodId::ControllerAcquire>{std::move(value)};",
            "    }",
            "",
            "    template <MethodId Id>",
            "    struct MethodResult {",
            "        static constexpr MethodId Method = Id;",
            "        nlohmann::json value = nlohmann::json::object();",
            "        bool operator==(const MethodResult&) const = default;",
            "    };",
            "",
            "    using CompleteCommandResult = std::variant<",
        ]
    )
    for index, row in enumerate(methods):
        suffix = "," if index + 1 < len(methods) else ">;"
        lines.append(f"        MethodResult<MethodId::{row['id']}>{suffix}")
    lines.extend(
        [
            "",
            "    static_assert(std::variant_size_v<CompleteCommandParameters> == 105);",
            "    static_assert(std::variant_size_v<CompleteCommandResult> == 105);",
            "",
            "    [[nodiscard]] constexpr MethodId commandMethod(const CompleteCommandResult& result) noexcept {",
            "        return std::visit([]<typename T>(const T&) constexpr noexcept { return T::Method; }, result);",
            "    }",
            "",
            "    [[nodiscard]] inline CompleteCommandResult makeResult(MethodId id, nlohmann::json value) {",
            "        switch (id) {",
        ]
    )
    for row in methods:
        lines.append(
            f"            case MethodId::{row['id']}: return MethodResult<MethodId::{row['id']}>{{std::move(value)}};"
        )
    lines.extend(
        [
            "            case MethodId::Count: break;",
            "        }",
            "        return MethodResult<MethodId::ControllerAcquire>{std::move(value)};",
            "    }",
            "",
            "} // namespace ai::openai::codex::frontend::generated",
            "",
        ]
    )
    lines.append("namespace ai::openai::codex::frontend::method {")
    for row in methods:
        lines.append(
            f"    inline constexpr std::string_view {row['id']} = generated::methodString(generated::MethodId::{row['id']});"
        )
    lines.extend(
        [
            "} // namespace ai::openai::codex::frontend::method",
            "",
            "#endif // AI_OPENAI_CODEX_FRONTEND_GENERATEDPROTOCOL_H",
            "// clang-format on",
            "",
        ]
    )
    return "\n".join(lines)


def generate_schema_data(schema: dict[str, Any]) -> str:
    """Embed the generated schema once for the production exact-shape codec.

    The installed metadata header intentionally contains only stable protocol
    metadata.  The relatively large validation document stays private to the
    frontend library while remaining generated from the same committed
    authority and checked by the same currentness test.
    """

    delimiter = "A17Schema"
    encoded = json.dumps(schema, separators=(",", ":"), ensure_ascii=False)
    if f"){delimiter}\"" in encoded:
        raise GenerationError("generated schema conflicts with its C++ raw-string delimiter")
    return (
        "/* Generated by tools/frontend/generate_frontend_protocol.py. Do not edit. */\n"
        f'R"{delimiter}({encoded}){delimiter}"\n'
    )


def resolve_schema_reference(root: dict[str, Any], reference: str) -> Any:
    if not reference.startswith("#/"):
        raise GenerationError(f"frontend fixture synthesis found non-local reference {reference!r}")
    value: Any = root
    for encoded in reference[2:].split("/"):
        token = encoded.replace("~1", "/").replace("~0", "~")
        if not isinstance(value, dict) or token not in value:
            raise GenerationError(f"frontend fixture synthesis cannot resolve {reference!r}")
        value = value[token]
    return value


def schema_example(
    schema: Any,
    root: dict[str, Any],
    *,
    complete: bool,
    active_references: frozenset[str] = frozenset(),
    depth: int = 0,
) -> Any:
    """Synthesize deterministic, credential-free contract examples.

    These fixtures are protocol examples, not schema authority.  They are
    regenerated from the exact committed schema and exercised by the C++
    codec so all 105 methods have minimal and representative forms without a
    second hand-maintained fixture table.
    """

    if schema is True:
        return None
    if schema is False or not isinstance(schema, dict):
        raise GenerationError("cannot synthesize an example for a false/non-object schema")
    if "const" in schema:
        return copy_json(schema["const"])
    if "enum" in schema and schema["enum"]:
        return copy_json(schema["enum"][0])
    if "default" in schema and schema["default"] is not None:
        return copy_json(schema["default"])
    reference = schema.get("$ref")
    if isinstance(reference, str):
        if reference in active_references:
            return None
        return schema_example(
            resolve_schema_reference(root, reference),
            root,
            complete=complete,
            active_references=active_references | {reference},
            depth=depth + 1,
        )
    for keyword in ("oneOf", "anyOf"):
        branches = schema.get(keyword)
        if isinstance(branches, list) and branches:
            preferred = branches
            if depth > 16:
                preferred = sorted(
                    branches,
                    key=lambda branch: 0
                    if isinstance(branch, dict) and branch.get("type") == "null"
                    else 1,
                )
            for branch in preferred:
                try:
                    return schema_example(
                        branch,
                        root,
                        complete=complete,
                        active_references=active_references,
                        depth=depth + 1,
                    )
                except GenerationError:
                    continue
            raise GenerationError(f"cannot synthesize any {keyword} branch")
    all_of = schema.get("allOf")
    if isinstance(all_of, list) and all_of:
        values = [
            schema_example(
                branch,
                root,
                complete=complete,
                active_references=active_references,
                depth=depth + 1,
            )
            for branch in all_of
        ]
        if all(isinstance(value, dict) for value in values):
            merged: dict[str, Any] = {}
            for value in values:
                merged.update(value)
            return merged
        return values[-1]

    schema_type = schema.get("type")
    if isinstance(schema_type, list):
        choices = [value for value in schema_type if value != "null"]
        schema_type = choices[0] if choices else "null"
    if schema_type is None:
        if "properties" in schema or "required" in schema:
            schema_type = "object"
        elif "items" in schema:
            schema_type = "array"
        else:
            return None
    if schema_type == "null":
        return None
    if schema_type == "boolean":
        return False
    if schema_type in {"integer", "number"}:
        minimum = schema.get("minimum", schema.get("exclusiveMinimum", 0))
        if isinstance(minimum, (int, float)) and not isinstance(minimum, bool):
            if "exclusiveMinimum" in schema:
                minimum += 1
            return int(minimum) if schema_type == "integer" else minimum
        return 0
    if schema_type == "string":
        pattern = schema.get("pattern", "")
        if isinstance(pattern, str) and pattern:
            for candidate in ("1", "x", "item-1", "https://example.invalid/"):
                try:
                    if re.fullmatch(pattern, candidate):
                        return candidate
                except re.error:
                    break
        if "uuid" in str(schema.get("format", "")).lower():
            return "00000000-0000-4000-8000-000000000000"
        if schema.get("format") in {"uri", "uri-reference", "url"}:
            return "https://example.invalid/"
        minimum_length = int(schema.get("minLength", 0))
        return "x" * max(1, minimum_length)
    if schema_type == "array":
        count = int(schema.get("minItems", 0))
        if count == 0:
            return []
        item_schema = schema.get("items", {})
        return [
            schema_example(
                item_schema,
                root,
                complete=complete,
                active_references=active_references,
                depth=depth + 1,
            )
            for _ in range(count)
        ]
    if schema_type == "object":
        properties = schema.get("properties", {})
        required = set(schema.get("required", ()))
        names = set(required)
        if complete and isinstance(properties, dict):
            names.update(properties)
        elif isinstance(properties, dict):
            names.update(
                name
                for name, child in properties.items()
                if isinstance(child, dict) and "const" in child
            )
        result: dict[str, Any] = {}
        for name in sorted(names):
            if name in SENSITIVE_RESULT_FIELD_NAMES:
                continue
            child = properties.get(name, {}) if isinstance(properties, dict) else {}
            result[name] = schema_example(
                child,
                root,
                complete=complete,
                active_references=active_references,
                depth=depth + 1,
            )
        return result
    raise GenerationError(f"unsupported example schema type {schema_type!r}")


def generate_golden_fixtures(
    schema: dict[str, Any], manifest: dict[str, Any]
) -> dict[str, Any]:
    def nullable_top_level_forms(value_schema: dict[str, Any]) -> list[dict[str, Any]]:
        def dereference(node: Any, active: frozenset[str] = frozenset()) -> Any:
            if not isinstance(node, dict):
                return node
            reference = node.get("$ref")
            if not isinstance(reference, str) or reference in active:
                return node
            target = resolve_schema_reference(schema, reference)
            return dereference(target, active | {reference})

        def allows_null(node: Any, active: frozenset[int] = frozenset()) -> bool:
            if node is True:
                return True
            if node is False or not isinstance(node, dict):
                return False
            marker = id(node)
            if marker in active:
                return False
            active = active | {marker}
            if "$ref" in node:
                return allows_null(dereference(node), active)
            if node.get("const", object()) is None:
                return True
            enumeration = node.get("enum")
            if isinstance(enumeration, list) and None in enumeration:
                return True
            schema_type = node.get("type")
            if schema_type == "null" or (
                isinstance(schema_type, list) and "null" in schema_type
            ):
                return True
            if isinstance(node.get("oneOf"), list) and any(
                allows_null(branch, active) for branch in node["oneOf"]
            ):
                return True
            if isinstance(node.get("anyOf"), list) and any(
                allows_null(branch, active) for branch in node["anyOf"]
            ):
                return True
            if isinstance(node.get("allOf"), list) and node["allOf"]:
                return all(allows_null(branch, active) for branch in node["allOf"])
            return not any(
                keyword in node
                for keyword in ("type", "const", "enum", "oneOf", "anyOf", "allOf", "not")
            )

        def object_variants(node: Any) -> list[dict[str, Any]]:
            node = dereference(node)
            if not isinstance(node, dict):
                return []
            for keyword in ("oneOf", "anyOf"):
                alternatives = node.get(keyword)
                if isinstance(alternatives, list):
                    return [
                        variant
                        for branch in alternatives
                        for variant in object_variants(branch)
                    ]
            if node.get("type") == "object" or "properties" in node:
                return [node]
            return []

        forms: list[dict[str, Any]] = []
        serialized: set[str] = set()
        for variant in object_variants(value_schema):
            properties = variant.get("properties", {})
            if not isinstance(properties, dict):
                continue
            base = schema_example(variant, schema, complete=False)
            if not isinstance(base, dict):
                continue
            for name, property_schema in properties.items():
                if name in SENSITIVE_RESULT_FIELD_NAMES or not allows_null(
                    property_schema
                ):
                    continue
                candidate = copy_json(base)
                candidate[name] = None
                encoded = json.dumps(candidate, sort_keys=True, separators=(",", ":"))
                if encoded not in serialized:
                    serialized.add(encoded)
                    forms.append(candidate)
        return forms

    definitions = schema["$defs"]
    methods = []
    for method in manifest["methods"]:
        parameter_schema = resolve_schema_reference(schema, method["parameterSchema"])
        result_schema = resolve_schema_reference(schema, method["resultSchema"])
        methods.append(
            {
                "method": method["method"],
                "minimalParams": schema_example(
                    parameter_schema, schema, complete=False
                ),
                "completeParams": schema_example(
                    parameter_schema, schema, complete=True
                ),
                "minimalResult": schema_example(result_schema, schema, complete=False),
                "completeResult": schema_example(result_schema, schema, complete=True),
                "nullableParams": nullable_top_level_forms(parameter_schema),
                "nullableResults": nullable_top_level_forms(result_schema),
                "malformedParams": [],
                "malformedResult": [],
            }
        )
    expanded_events = [
        schema_example(branch, schema, complete=False)
        for branch in definitions["ExpandedFrontendEvent"]["oneOf"]
    ]
    expanded_snapshot = schema_example(
        definitions["ExpandedSnapshot"], schema, complete=True
    )
    expanded_snapshot["state"]["items"] = [
        schema_example(branch, schema, complete=True)
        for branch in definitions["ExpandedThreadItem"]["oneOf"]
    ]
    for item in expanded_snapshot["state"]["items"]:
        item.setdefault("truncated", False)
        item.setdefault("connectionInvalidated", False)
        if not item.get("omittedFields"):
            item.pop("omittedFields", None)
    expanded_snapshot["state"]["pendingRequests"] = [
        schema_example(branch, schema, complete=False)
        for branch in definitions["ExpandedPendingRequest"]["oneOf"]
    ]
    for request in expanded_snapshot["state"]["pendingRequests"]:
        request.setdefault("truncated", False)
        if request["kind"] == "user_input":
            request["questions"] = [
                {
                    "id": "question-1",
                    "header": "Choose a mode",
                    "prompt": "Which safe mode should be used?",
                    "allowsFreeText": True,
                    "isSecret": True,
                    "options": [
                        {
                            "label": "safe",
                            "description": "Use the bounded safe mode.",
                            "futureOptionHint": True,
                        }
                    ],
                    "futureQuestionHint": "preserved",
                }
            ]
            request["autoResolutionMs"] = 60_000
    return {
        "generatedBy": "tools/frontend/generate_frontend_protocol.py",
        "counts": {"methods": 105, "expandedEvents": len(manifest["eventFamilies"])},
        "methods": methods,
        "expandedSnapshot": expanded_snapshot,
        "expandedEvents": expanded_events,
    }


def write_or_check(path: Path, generated: str, check: bool) -> None:
    if check:
        try:
            committed = path.read_text(encoding="utf-8")
        except OSError as error:
            raise GenerationError(f"unable to read {path}: {error}") from error
        if committed != generated:
            raise GenerationError(f"generated artifact is stale: {path}")
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(generated, encoding="utf-8")


def command_generate(arguments: argparse.Namespace) -> None:
    source = load_json(arguments.source)
    manifest = generate_manifest(source)
    header = generate_header(manifest)
    schema = generate_schema(load_json(arguments.schema_template), manifest, source)
    schema_data = generate_schema_data(schema)
    fixtures = generate_golden_fixtures(schema, manifest)
    manifest_text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    schema_text = json.dumps(schema, indent=2, ensure_ascii=False) + "\n"
    fixtures_text = json.dumps(fixtures, indent=2, ensure_ascii=False) + "\n"
    write_or_check(arguments.manifest, manifest_text, arguments.check)
    write_or_check(arguments.header, header, arguments.check)
    write_or_check(arguments.schema, schema_text, arguments.check)
    write_or_check(arguments.schema_data, schema_data, arguments.check)
    write_or_check(arguments.fixtures, fixtures_text, arguments.check)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--source", type=Path, required=True)
    result.add_argument("--manifest", type=Path, required=True)
    result.add_argument("--header", type=Path, required=True)
    result.add_argument("--schema-template", type=Path, required=True)
    result.add_argument("--schema", type=Path, required=True)
    result.add_argument("--schema-data", type=Path, required=True)
    result.add_argument("--fixtures", type=Path, required=True)
    result.add_argument("--check", action="store_true")
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    command_generate(arguments)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GenerationError as error:
        print(f"frontend-protocol: error: {error}", file=sys.stderr)
        raise SystemExit(1)
