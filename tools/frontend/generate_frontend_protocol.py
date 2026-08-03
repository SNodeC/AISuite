#!/usr/bin/env python3

"""Generate the additive Codex Frontend Protocol v1 contract artifacts.

The sole provider-inventory input is the committed registry export produced by
``tools/codex/app_server_surface.py frontend-registry``.  This generator never
parses vendored schema, Rust, TypeScript, or an installed Codex binary.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Sequence


class GenerationError(RuntimeError):
    pass


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

FUTURE_CAPABILITIES = frozenset(
    {
        "authenticated_frontend",
        "scope_projected_state",
        "provider_lifecycle",
        "multi_transport",
        "cpp_client_sdk",
        "typescript_client_sdk",
        "browser_ui",
        "qt_ui",
    }
)

EVENT_FAMILIES = (
    "provider.updated",
    "controller.updated",
    "sessions.updated",
    "thread.upserted",
    "thread.removed",
    "turn.upserted",
    "item.upserted",
    "item.content.updated",
    "pendingRequests.updated",
    "account.updated",
    "models.updated",
    "configuration.updated",
    "process.updated",
    "filesystemWatch.updated",
    "fuzzySearch.updated",
    "reviews.updated",
    "integrations.updated",
    "plugins.updated",
    "skills.updated",
    "mcp.updated",
    "platform.updated",
    "notice.added",
    "activity.updated",
    "capacity.updated",
    "diagnostics.updated",
)

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
                "exposure": row["exposure"],
                "securityDecision": row["securityDecision"],
                "requiredScopes": scopes,
                "controllerRequired": row["controllerRequired"],
                "defaultEnabled": row["defaultEnabled"],
                "currentlyImplemented": mappings[0] in EXISTING_METHODS,
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
                "exposure": "DedicatedFrontendMethod",
                "securityDecision": native["security"],
                "requiredScopes": native["scopes"],
                "controllerRequired": native["controllerRequired"],
                "defaultEnabled": True,
                "currentlyImplemented": existing,
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
                "exposure": "DedicatedFrontendMethod",
                "securityDecision": "PrivilegedScopedApproved",
                "requiredScopes": ["control", "unknown_request_response" if unknown else "sensitive_response"],
                "controllerRequired": True,
                "defaultEnabled": True,
                "currentlyImplemented": existing,
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
    if sum(row["currentlyImplemented"] for row in methods) != 15:
        raise GenerationError("A1.7a runtime availability must remain exactly 15")
    if any(row["currentlyImplemented"] for row in methods[15:]):
        raise GenerationError("an additive method was activated before A1.7b")
    return methods


def generate_manifest(source: dict[str, Any]) -> dict[str, Any]:
    rows = validate_source(source)
    methods = method_manifest(source)
    capabilities = [
        {
            "key": key,
            "defined": True,
            "implementedByCurrentRuntime": key in {"method_discovery", "security_scopes"},
            "futurePhase": (
                "A1.7b" if key in {"authenticated_frontend", "scope_projected_state", "provider_lifecycle", "multi_transport"}
                else "A1.7c" if key in {"cpp_client_sdk", "qt_ui"}
                else "A1.7d" if key in {"typescript_client_sdk", "browser_ui"}
                else "A1.7a-contract"
            ),
        }
        for key in CAPABILITIES
    ]
    if any(item["implementedByCurrentRuntime"] for item in capabilities if item["key"] in FUTURE_CAPABILITIES):
        raise GenerationError("A1.7a claims a future capability as implemented")
    notifications = [
        {
            "registryKey": registry_key(row),
            "legacyContract": row["compatibilityBehavior"],
            "expandedMappings": row["mappings"],
            "requiredScopes": row["requiredScopes"],
            "redactionClass": row["redactionClass"],
            "duplicateSuppression": "choose legacy or expanded per connection; never both",
        }
        for row in rows
        if row["registryKey"]["category"] == "server_notification"
    ]
    items = [
        {
            "registryKey": registry_key(row),
            "legacyContract": row["compatibilityBehavior"],
            "expandedMappings": row["mappings"],
            "requiredScopes": row["requiredScopes"],
            "redactionClass": row["redactionClass"],
            "duplicateSuppression": "choose legacy or expanded per connection; never both",
        }
        for row in rows
        if row["registryKey"]["category"] == "item_discriminator" and row["registryKey"]["domain"] == "ThreadItem"
    ]
    if len(notifications) != 68 or len(items) != 18:
        raise GenerationError("notification/item mappings must be 68/18")
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
            "currentRuntimeMethods": 15,
            "reviewedIdentities": 234,
            "notifications": 68,
            "threadItems": 18,
        },
        "scopeProfiles": {
            "default_remote": ["observe", "control"],
            "local_trusted": list(SCOPE_ENUM),
        },
        "capabilities": capabilities,
        "eventFamilies": list(EVENT_FAMILIES),
        "methods": methods,
        "notificationMappings": notifications,
        "threadItemMappings": items,
        "nonExposedOrNotApplicable": excluded,
        "reviewedContracts": rows,
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


def generate_header(manifest: dict[str, Any]) -> str:
    methods = manifest["methods"]
    lines = [
        "/* Generated by tools/frontend/generate_frontend_protocol.py. Do not edit. */",
        "#ifndef AI_OPENAI_CODEX_FRONTEND_GENERATEDPROTOCOL_H",
        "#define AI_OPENAI_CODEX_FRONTEND_GENERATEDPROTOCOL_H",
        "",
        '#include "Security.h"',
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <optional>",
        "#include <span>",
        "#include <string_view>",
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
            "        std::string_view exposure;",
            "        std::string_view securityDecision;",
            "        std::span<const FrontendScope> requiredScopes;",
            "        bool controllerRequired;",
            "        bool defaultEnabled;",
            "        bool currentlyImplemented;",
            "        bool observerAvailability;",
            "        bool sensitiveResult;",
            "        bool largeResult;",
            "        std::string_view compatibilityStatus;",
            "        std::string_view capability;",
            "        std::string_view implementationPhase;",
            "        std::string_view parameterPolicy;",
            "    };",
            "",
            "    struct CapabilityMetadata { Capability id; std::string_view key; bool defined; bool implementedByCurrentRuntime; };",
            "    struct ContractMetadata { std::string_view registryKey; std::string_view exposure; std::string_view securityDecision; std::string_view mappings; std::string_view redactionClass; std::string_view compatibilityBehavior; bool controllerRequired; bool defaultEnabled; };",
            "",
        ]
    )
    for row in methods:
        name = row["id"]
        keys = ", ".join(q(key) for key in row["registryKeys"])
        scopes = ", ".join(f"FrontendScope::{SCOPE_ENUM[scope]}" for scope in row["requiredScopes"])
        lines.append(f"    inline constexpr std::array<std::string_view, {len(row['registryKeys'])}> {name}RegistryKeys{{{keys}}};")
        lines.append(f"    inline constexpr std::array<FrontendScope, {len(row['requiredScopes'])}> {name}Scopes{{{scopes}}};")
    lines.extend(["", f"    inline constexpr std::array<MethodMetadata, {len(methods)}> AllMethods{{{{"])
    for row in methods:
        lines.append(
            "        {MethodId::%s, %s, MethodCategory::%s, %s, %sRegistryKeys, %s, %s, %s, %s, %s, %s, %s, %sScopes, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s},"
            % (
                row["id"], q(row["method"]), category_cpp(row["category"]), str(row["frontendNative"]).lower(), row["id"],
                q(row["genericContractKey"]), q(row["backendCommand"]), q(row["serviceAction"]), q(row["parameterSchema"]), q(row["resultSchema"]),
                q(row["exposure"]), q(row["securityDecision"]), row["id"], str(row["controllerRequired"]).lower(), str(row["defaultEnabled"]).lower(),
                str(row["currentlyImplemented"]).lower(), str(row["observerAvailability"]).lower(), str(row["sensitiveResult"]).lower(), str(row["largeResult"]).lower(),
                q(row["compatibilityStatus"]), q(row["capability"]), q(row["implementationPhase"]), q(row["parameterPolicy"]),
            )
        )
    lines.extend(["    }};", ""])
    capabilities = manifest["capabilities"]
    lines.append(f"    inline constexpr std::array<CapabilityMetadata, {len(capabilities)}> AllCapabilities{{{{")
    for capability in capabilities:
        lines.append(
            f"        {{Capability::{cpp_id(capability['key'])}, {q(capability['key'])}, true, {str(capability['implementedByCurrentRuntime']).lower()}}},"
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
            "    consteval std::size_t countCategory(MethodCategory category) { std::size_t count = 0; for (const auto& method : AllMethods) count += method.category == category; return count; }",
            "    consteval std::size_t countNative() { std::size_t count = 0; for (const auto& method : AllMethods) count += method.frontendNative; return count; }",
            "    consteval std::size_t countImplemented() { std::size_t count = 0; for (const auto& method : AllMethods) count += method.currentlyImplemented; return count; }",
            "    consteval bool uniqueMethods() { for (std::size_t i = 0; i < AllMethods.size(); ++i) for (std::size_t j = i + 1; j < AllMethods.size(); ++j) if (AllMethods[i].method == AllMethods[j].method) return false; return true; }",
            "",
            "    inline constexpr std::size_t MethodCount = AllMethods.size();",
            "    inline constexpr std::size_t FrontendNativeMethodCount = countNative();",
            "    inline constexpr std::size_t NonNativeMethodCount = MethodCount - FrontendNativeMethodCount;",
            "    inline constexpr std::size_t ExistingMethodCount = countImplemented();",
            "    inline constexpr std::size_t AdditiveMethodCount = MethodCount - ExistingMethodCount;",
            "    inline constexpr std::size_t ProviderOperationMethodCount = countCategory(MethodCategory::ProviderOperation);",
            "    inline constexpr std::size_t ReverseMethodCount = countCategory(MethodCategory::ReverseResponse);",
            "    inline constexpr std::size_t ProviderLifecycleMethodCount = countCategory(MethodCategory::ProviderLifecycle);",
            "    inline constexpr std::size_t ReviewedIdentityCount = AllReviewedContracts.size();",
            "",
            "    static_assert(MethodCount == 105);",
            "    static_assert(FrontendNativeMethodCount == 7);",
            "    static_assert(NonNativeMethodCount == 98);",
            "    static_assert(ExistingMethodCount == 15);",
            "    static_assert(AdditiveMethodCount == 90);",
            "    static_assert(ProviderOperationMethodCount == 86);",
            "    static_assert(ReverseMethodCount == 12);",
            "    static_assert(ProviderLifecycleMethodCount == 3);",
            "    static_assert(ReviewedIdentityCount == 234);",
            "    static_assert(uniqueMethods());",
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
            "",
        ]
    )
    return "\n".join(lines)


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
    manifest_text = json.dumps(manifest, indent=2, ensure_ascii=False) + "\n"
    write_or_check(arguments.manifest, manifest_text, arguments.check)
    write_or_check(arguments.header, header, arguments.check)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--source", type=Path, required=True)
    result.add_argument("--manifest", type=Path, required=True)
    result.add_argument("--header", type=Path, required=True)
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
