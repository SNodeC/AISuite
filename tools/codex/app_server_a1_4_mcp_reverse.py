#!/usr/bin/env python3
"""Freeze and verify the Codex A1.4 MCP and reverse-request batch.

The audit is intentionally independent from later production registry
promotions.  It anchors the predecessor state to the requested AISuite base,
regenerates the exact transitive closure from the pinned stable schema graph,
and records the public API and ABI-sensitive append plan before production
implementation starts.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, Sequence

sys.dont_write_bytecode = True

import app_server_a1_4 as native_a14
import app_server_a1_shared as shared
import app_server_fixtures as fixtures
import app_server_surface as surface


FORMAT_VERSION = 1
CODEX_VERSION = "codex-cli 0.144.6"
UPSTREAM_TAG = "rust-v0.144.6"
UPSTREAM_SOURCE_COMMIT = "5d1fbf26c43abc65a203928b2e31561cb039e06d"
EXPECTED_BASE_SHA = "0c3a5838359eb283aca67840325ce6019345b462"
EXPECTED_BASE_TREE = "f86196b41d695f7165dca6a80ec017a8b9166de1"
EXPECTED_PREDECESSOR_SHA = "ad15fd20c29e7478f28506ccf8190c069752f0aa"
EXPECTED_PREDECESSOR_TREE = "eea5e735e4ef4781d9377e5462be7dc752aa596a"
EXPECTED_CLEAN_SNODEC_SHA = "77415c71a87fb7955e9a050bedaca02b65754324"
EXPECTED_CLEAN_SNODEC_TREE = "2d39c334f12c308828936656c820447bfcc38d47"
EXPECTED_PROVENANCE_SHA = "d18b231a1d2ec2235fd6f204786b0a761cc24ff5"
EXPECTED_PROVENANCE_TREE = "88a63edc985a851b2b76b0c56df19fae74ea8069"
EXPECTED_SOVERSION = 1
EXPECTED_STABLE_SCHEMA_AGGREGATE = (
    "cee1ac3bcaf95e5fcdcf07499c7e6b00fc423b90c670ea3380f1799434b72add"
)

EXPECTED_VARIANT_HASHES = {
    "CanonicalServerNotification": (
        "7fe40be1301e549a4fe9f4c16af7732518a0c1e9926e76c1d106a89d24f1bcf1"
    ),
    "Event": (
        "90e092711c907b8f1597c23fb1d1d1873112046ffeb22fa478fa3654a77280aa"
    ),
    "TypedServerRequest": (
        "c57cb003b80cbf03dc221b965ec6cc9ab127e7682d1ba72683f7c38586c71129"
    ),
}
EXPECTED_VARIANT_SIZES = {
    "CanonicalServerNotification": 57,
    "Event": 59,
    "TypedServerRequest": 8,
}
EXPECTED_SOURCE_BLOBS = {
    "src/ai/openai/codex/typed/Events.h": (
        "c6a433d9f95cb4cf051d0e548948f5289ea31231"
    ),
    "src/ai/openai/codex/typed/ServerRequests.h": (
        "51263844aabc0c2f45d58625e14ea56d0c2417ff"
    ),
    "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc": (
        "93305acb0777d50786a59c4f82795281eeb434c3"
    ),
    "CMakeLists.txt": "c42d83c37f8f7fd2afde82af7807444e3e8b453d",
}

EXPECTED_SCHEMA_HASHES = {
    "definitions": (
        "0a5c654ddc722ac6b77186d9f6c72c13ee624d0631922a6e46fab499be63b534"
    ),
    "seed_definitions": (
        "5729a24beb174fa602b09a00b45f5207a9260a5f9a4d275eb5ae7e3493d13df5"
    ),
    "schema_paths": (
        "992e887ffeb4437fd11de8cd720ee7b4f61d9fc118fa786c0c247e698a3c971a"
    ),
    "object_policies": (
        "6b484a6267bb123a0bbe350bae8d4603a7d91d14247672d61bd92a0f26c537fe"
    ),
    "identity_reachability": (
        "c07f83929323140654c26183f33841adf327fd119e9320af5d2fac62fa72c9e8"
    ),
}
EXPECTED_PR_A_IDENTITY_HASH = (
    "a6813700fc5b6e1afa12cc4fcf4dd30df5ffd86ac9588f1a5eaeb3cf589d4213"
)

SIX_COMMIT_SUBJECTS = (
    "Adopt the cleaned SNode.C dependency",
    "Freeze Codex A1.4 MCP and reverse-request scope",
    "Complete Codex MCP client operations and notifications",
    "Complete Codex attestation and dynamic-tool requests",
    "Complete Codex user-input and MCP elicitation requests",
    "Close and verify Codex A1.4 MCP and reverse requests",
)

CLIENT_REQUESTS = (
    (
        "mcpServer/oauth/login",
        "McpServerOauthLoginParams",
        "McpServerOauthLoginResponse",
    ),
    (
        "mcpServer/resource/read",
        "McpResourceReadParams",
        "McpResourceReadResponse",
    ),
    (
        "mcpServer/tool/call",
        "McpServerToolCallParams",
        "McpServerToolCallResponse",
    ),
    (
        "mcpServerStatus/list",
        "ListMcpServerStatusParams",
        "ListMcpServerStatusResponse",
    ),
)

SERVER_NOTIFICATIONS = (
    (
        "mcpServer/oauthLogin/completed",
        "McpServerOauthLoginCompletedNotification",
    ),
    (
        "mcpServer/startupStatus/updated",
        "McpServerStatusUpdatedNotification",
    ),
)

SERVER_REQUESTS = (
    (
        "attestation/generate",
        "AttestationGenerateParams",
        "AttestationGenerateResponse",
        "AttestationGenerateRequest",
        8,
        4,
    ),
    (
        "item/tool/call",
        "DynamicToolCallParams",
        "DynamicToolCallResponse",
        "DynamicToolCallRequest",
        9,
        4,
    ),
    (
        "item/tool/requestUserInput",
        "ToolRequestUserInputParams",
        "ToolRequestUserInputResponse",
        "UserInputRequest",
        2,
        5,
    ),
    (
        "mcpServer/elicitation/request",
        "McpServerElicitationRequestParams",
        "McpServerElicitationRequestResponse",
        "McpServerElicitationRequest",
        10,
        5,
    ),
)

ELICITATION_MODES = ("form", "openai/form", "url")

PR_C_IDENTITIES = (
    ("client_request", "ClientRequest", "method", "windowsSandbox/readiness"),
    ("client_request", "ClientRequest", "method", "windowsSandbox/setupStart"),
    (
        "server_notification",
        "ServerNotification",
        "method",
        "deprecationNotice",
    ),
    ("server_notification", "ServerNotification", "method", "process/exited"),
    (
        "server_notification",
        "ServerNotification",
        "method",
        "process/outputDelta",
    ),
    (
        "server_notification",
        "ServerNotification",
        "method",
        "remoteControl/status/changed",
    ),
    (
        "server_notification",
        "ServerNotification",
        "method",
        "serverRequest/resolved",
    ),
    ("server_notification", "ServerNotification", "method", "warning"),
    (
        "server_notification",
        "ServerNotification",
        "method",
        "windows/worldWritableWarning",
    ),
    (
        "server_notification",
        "ServerNotification",
        "method",
        "windowsSandbox/setupCompleted",
    ),
)

INHERITED_PARTIALS = (
    ("client_notification", "ClientNotification", "method", "initialized"),
    ("client_request", "ClientRequest", "method", "initialize"),
    ("server_notification", "ServerNotification", "method", "error"),
)

EXPECTED_CLOSURE = {
    "seed_definitions": 18,
    "reachable_named_definitions": 55,
    "definition_namespaces": {"legacy": 34, "v2": 21},
    "schema_paths": 204,
    "schema_path_kinds": {
        "array_element": 22,
        "map_value": 3,
        "property": 179,
    },
    "required_properties": 87,
    "optional_properties": 92,
    "nullable_paths": 97,
    "default_bearing_paths": 3,
    "arrays": 22,
    "map_values": 3,
    "object_nodes": 48,
    "open_objects": 33,
    "closed_objects": 12,
    "schema_valued_additional_properties": 3,
    "opaque_json_paths": 24,
    "integer_number_formats": {
        "double": 3,
        "int64": 2,
        "uint32": 3,
        "uint64": 5,
    },
    "minimum_bearing_paths": 8,
    "maximum_bearing_paths": 0,
    "sensitive_paths": 53,
    "union_families": 1,
    "union_alternatives": 3,
    "reaching_root_identities": 13,
    "root_definition_reachability": 124,
    "elicitation_reaching_roots": 1,
}

EXPECTED_DEFAULTS = (
    (
        "#/definitions/ToolRequestUserInputParams/properties/autoResolutionMs",
        None,
    ),
    (
        "#/definitions/ToolRequestUserInputQuestion/properties/isOther",
        False,
    ),
    (
        "#/definitions/ToolRequestUserInputQuestion/properties/isSecret",
        False,
    ),
)

NOTIFICATION_APPENDS = (
    {
        "method": "mcpServer/oauthLogin/completed",
        "type": "McpServerOauthLoginCompletedNotification",
        "canonical_index": 57,
        "event_index": 59,
    },
    {
        "method": "mcpServer/startupStatus/updated",
        "type": "McpServerStatusUpdatedNotification",
        "canonical_index": 58,
        "event_index": 60,
    },
)

REQUEST_APPENDS = (
    {
        "method": "attestation/generate",
        "type": "AttestationGenerateRequest",
        "index": 8,
    },
    {
        "method": "item/tool/call",
        "type": "DynamicToolCallRequest",
        "index": 9,
    },
    {
        "method": "mcpServer/elicitation/request",
        "type": "McpServerElicitationRequest",
        "index": 10,
    },
)


@dataclass(frozen=True, order=True)
class Diagnostic:
    code: str
    location: str
    message: str


class AuditError(RuntimeError):
    def __init__(self, diagnostics: Sequence[Diagnostic]) -> None:
        self.diagnostics = tuple(diagnostics)
        self.codes = tuple(sorted({row.code for row in diagnostics}))
        super().__init__(
            "; ".join(
                f"{row.code} at {row.location}: {row.message}"
                for row in diagnostics
            )
        )


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


def _git_blob(repo_root: Path, revision: str, path: str) -> str:
    return subprocess.run(
        ("git", "show", f"{revision}:{path}"),
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ).stdout


def _git_blob_id(repo_root: Path, revision: str, path: str) -> str:
    return _run(repo_root, "git", "rev-parse", f"{revision}:{path}")


def _load(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected object-valued JSON: {path}")
    return value


def _render(value: Mapping[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _sha256(value: Any) -> str:
    encoded = json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _key(
    category: str,
    domain: str,
    discriminator_field: str,
    name: str,
) -> dict[str, str]:
    return {
        "category": category,
        "domain": domain,
        "discriminator_field": discriminator_field,
        "name": name,
    }


def _key_tuple(value: Mapping[str, Any]) -> tuple[str, str, str, str]:
    return (
        str(value.get("category", "")),
        str(value.get("domain", "")),
        str(value.get("discriminator_field", "")),
        str(value.get("name", "")),
    )


def _row_key(row: Mapping[str, Any]) -> tuple[str, str, str, str]:
    key = row.get("protocol_surface_key", {})
    return _key_tuple(key if isinstance(key, Mapping) else {})


def _variant(source: str, alias: str) -> list[str]:
    match = re.search(
        rf"using\s+{re.escape(alias)}\s*=\s*std::variant<(?P<body>.*?)>;",
        source,
        flags=re.DOTALL,
    )
    if match is None:
        raise ValueError(f"unable to locate variant {alias}")
    alternatives = [
        re.sub(r"\s+", "", value)
        for value in match.group("body").split(",")
        if value.strip()
    ]
    if not alternatives or any(
        re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value) is None
        for value in alternatives
    ):
        raise ValueError(f"{alias} is not a flat named-alternative variant")
    return alternatives


def _scope_identity_rows() -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for name, params, result in CLIENT_REQUESTS:
        rows.append(
            {
                "protocol_surface_key": _key(
                    "client_request", "ClientRequest", "method", name
                ),
                "parameter_type": params,
                "result_type": result,
                "result_kind": "Concrete",
                "payload_type": None,
                "start_status": "NotImplemented",
                "promotion_commit": 3,
            }
        )
    for name, payload in SERVER_NOTIFICATIONS:
        rows.append(
            {
                "protocol_surface_key": _key(
                    "server_notification",
                    "ServerNotification",
                    "method",
                    name,
                ),
                "parameter_type": None,
                "result_type": None,
                "result_kind": "NotApplicable",
                "payload_type": payload,
                "start_status": "NotImplemented",
                "promotion_commit": 3,
            }
        )
    for name, params, result, _public_type, _index, commit in SERVER_REQUESTS:
        rows.append(
            {
                "protocol_surface_key": _key(
                    "server_request", "ServerRequest", "method", name
                ),
                "parameter_type": params,
                "result_type": result,
                "result_kind": "Concrete",
                "payload_type": None,
                "start_status": (
                    "Partial"
                    if name == "item/tool/requestUserInput"
                    else "NotImplemented"
                ),
                "promotion_commit": commit,
            }
        )
    for mode in ELICITATION_MODES:
        rows.append(
            {
                "protocol_surface_key": _key(
                    "tagged_union_discriminator",
                    "McpServerElicitationRequestParams",
                    "mode",
                    mode,
                ),
                "parameter_type": None,
                "result_type": None,
                "result_kind": "NotApplicable",
                "payload_type": None,
                "start_status": "NotImplemented",
                "promotion_commit": 5,
            }
        )
    return sorted(rows, key=_row_key)


def _public_api() -> dict[str, Any]:
    return {
        "header": "ai/openai/codex/typed/Mcp.h",
        "client_accessors": (
            "Mcp& Client::mcp() noexcept",
            "const Mcp& Client::mcp() const noexcept",
        ),
        "facade": {
            "type": "Mcp",
            "submission_type": "AppServerClient::RawProtocol::Submission",
            "handler_aliases": {
                "StartOauthLoginResultHandler": (
                    "std::function<void(const "
                    "OperationResult<McpServerOauthLoginResponse>&)>"
                ),
                "ReadResourceResultHandler": (
                    "std::function<void(const "
                    "OperationResult<McpResourceReadResponse>&)>"
                ),
                "CallToolResultHandler": (
                    "std::function<void(const "
                    "OperationResult<McpServerToolCallResponse>&)>"
                ),
                "ListServersResultHandler": (
                    "std::function<void(const "
                    "OperationResult<ListMcpServerStatusResponse>&)>"
                ),
            },
            "methods": (
                "Submission startOauthLogin(McpServerOauthLoginParams params, "
                "StartOauthLoginResultHandler handler)",
                "Submission readResource(McpResourceReadParams params, "
                "ReadResourceResultHandler handler)",
                "Submission callTool(McpServerToolCallParams params, "
                "CallToolResultHandler handler)",
                "Submission listServers(ListMcpServerStatusParams params, "
                "ListServersResultHandler handler)",
            ),
            "raw_method_escape_hatches": (),
        },
        "example": (
            "client.typed().mcp().listServers(params, "
            "[](const auto& result) { /* handle typed result */ });"
        ),
        "lifecycle": (
            "Every method submits through RawProtocol and returns immediately; "
            "completion is delivered asynchronously through OperationResult<T>."
        ),
        "client_pimpl_members_after_change": 1,
        "app_server_client_members_added": 0,
    }


def _reverse_request_api() -> dict[str, Any]:
    requests = [
        {
            "method": name,
            "public_type": public_type,
            "canonical_params": params,
            "canonical_response": response,
            "variant_index": index,
        }
        for name, params, response, public_type, index, _commit in SERVER_REQUESTS
    ]
    return {
        "header": "ai/openai/codex/typed/ServerRequests.h",
        "request_types": requests,
        "respond_methods": (
            "SendResult respond(const AttestationGenerateRequest& request, "
            "AttestationGenerateResponse response)",
            "SendResult respond(const DynamicToolCallRequest& request, "
            "DynamicToolCallResponse response)",
            "SendResult respond(const UserInputRequest& request, "
            "ToolRequestUserInputResponse response)",
            "SendResult respond(const UserInputRequest& request, "
            "std::vector<UserInputAnswer> answers)",
            "SendResult respond(const McpServerElicitationRequest& request, "
            "McpServerElicitationRequestResponse response)",
        ),
        "reject_methods": (
            "SendResult reject(const AttestationGenerateRequest& request, "
            "ProtocolError error)",
            "SendResult reject(const DynamicToolCallRequest& request, "
            "ProtocolError error)",
            "SendResult reject(const UserInputRequest& request, "
            "ProtocolError error)",
            "SendResult reject(const McpServerElicitationRequest& request, "
            "ProtocolError error)",
        ),
        "elicitation_action_factories": (
            "McpServerElicitationAction::accept()",
            "McpServerElicitationAction::decline()",
            "McpServerElicitationAction::cancel()",
        ),
        "elicitation_response_fields": {
            "action": "McpServerElicitationAction",
            "content": "OptionalNullable<Json>",
            "meta": "OptionalNullable<Json>",
        },
        "user_input_compatibility": {
            "projection_types": (
                "UserInputRequest",
                "UserInputQuestion",
                "UserInputOption",
                "UserInputAnswer",
            ),
            "canonical_view": (
                "ToolRequestUserInputParams UserInputRequest::canonicalParams"
            ),
            "canonical_diagnostics": (
                "std::vector<DecodeDiagnostic> UserInputRequest::diagnostics"
            ),
        },
    }


def _stages() -> list[dict[str, Any]]:
    return [
        {
            "commit": 3,
            "subject": SIX_COMMIT_SUBJECTS[2],
            "identities": [
                _key("client_request", "ClientRequest", "method", name)
                for name, _params, _result in CLIENT_REQUESTS
            ]
            + [
                _key(
                    "server_notification",
                    "ServerNotification",
                    "method",
                    name,
                )
                for name, _payload in SERVER_NOTIFICATIONS
            ],
            "promotions": {"NotImplemented_to_Complete": 6},
            "native_a1_4": {
                "Complete": 39,
                "Partial": 1,
                "NotImplemented": 16,
            },
            "global": {
                "Complete": 319,
                "Partial": 4,
                "NotImplemented": 16,
                "NotApplicable": 48,
            },
        },
        {
            "commit": 4,
            "subject": SIX_COMMIT_SUBJECTS[3],
            "identities": [
                _key(
                    "server_request",
                    "ServerRequest",
                    "method",
                    name,
                )
                for name, *_rest in SERVER_REQUESTS[:2]
            ],
            "promotions": {"NotImplemented_to_Complete": 2},
            "native_a1_4": {
                "Complete": 41,
                "Partial": 1,
                "NotImplemented": 14,
            },
            "global": {
                "Complete": 321,
                "Partial": 4,
                "NotImplemented": 14,
                "NotApplicable": 48,
            },
        },
        {
            "commit": 5,
            "subject": SIX_COMMIT_SUBJECTS[4],
            "identities": [
                _key(
                    "server_request",
                    "ServerRequest",
                    "method",
                    "item/tool/requestUserInput",
                ),
                _key(
                    "server_request",
                    "ServerRequest",
                    "method",
                    "mcpServer/elicitation/request",
                ),
            ]
            + [
                _key(
                    "tagged_union_discriminator",
                    "McpServerElicitationRequestParams",
                    "mode",
                    mode,
                )
                for mode in ELICITATION_MODES
            ],
            "promotions": {
                "NotImplemented_to_Complete": 4,
                "Partial_to_Complete": 1,
            },
            "native_a1_4": {
                "Complete": 46,
                "Partial": 0,
                "NotImplemented": 10,
            },
            "global": {
                "Complete": 326,
                "Partial": 3,
                "NotImplemented": 10,
                "NotApplicable": 48,
            },
        },
    ]


def _root_contracts() -> list[dict[str, Any]]:
    roots: list[dict[str, Any]] = []
    for name, params, response in CLIENT_REQUESTS:
        roots.append(
            {
                "protocol_surface_key": _key(
                    "client_request", "ClientRequest", "method", name
                ),
                "seeds": (
                    ("request_params", params),
                    ("successful_response", response),
                ),
            }
        )
    for name, payload in SERVER_NOTIFICATIONS:
        roots.append(
            {
                "protocol_surface_key": _key(
                    "server_notification",
                    "ServerNotification",
                    "method",
                    name,
                ),
                "seeds": (("notification_params", payload),),
            }
        )
    for name, params, response, _public_type, _index, _commit in SERVER_REQUESTS:
        roots.append(
            {
                "protocol_surface_key": _key(
                    "server_request", "ServerRequest", "method", name
                ),
                "seeds": (
                    ("request_params", params),
                    ("successful_response", response),
                ),
            }
        )
    for mode in ELICITATION_MODES:
        roots.append(
            {
                "protocol_surface_key": _key(
                    "tagged_union_discriminator",
                    "McpServerElicitationRequestParams",
                    "mode",
                    mode,
                ),
                "seeds": (
                    (
                        "registered_union_family",
                        "McpServerElicitationRequestParams",
                    ),
                ),
            }
        )
    return sorted(
        roots,
        key=lambda row: _key_tuple(row["protocol_surface_key"]),
    )


def _elicitation_union(
    nodes: Mapping[fixtures.DefinitionId, Any],
) -> dict[str, Any]:
    definition = fixtures.DefinitionId(
        "legacy", "McpServerElicitationRequestParams"
    )
    schema = nodes[definition]
    branches = schema.get("oneOf")
    if not isinstance(branches, list):
        raise ValueError("McpServerElicitationRequestParams lacks oneOf")
    branch_rows: list[dict[str, Any]] = []
    for index, branch in enumerate(branches):
        if not isinstance(branch, Mapping):
            raise ValueError(f"elicitation branch {index} is not an object")
        properties = branch.get("properties", {})
        if not isinstance(properties, Mapping):
            raise ValueError(f"elicitation branch {index} lacks properties")
        mode_schema = properties.get("mode", {})
        values = (
            mode_schema.get("enum", [])
            if isinstance(mode_schema, Mapping)
            else []
        )
        if (
            not isinstance(values, list)
            or len(values) != 1
            or not isinstance(values[0], str)
        ):
            raise ValueError(f"elicitation branch {index} has malformed mode")
        required = set(branch.get("required", []))
        branch_rows.append(
            {
                "alternative": values[0],
                "branch_index": index,
                "required_fields": sorted(required),
                "optional_fields": sorted(set(properties) - required),
                "nullable_fields": sorted(
                    name
                    for name, child in properties.items()
                    if native_a14._nullable(child)
                ),
                "intentionally_opaque_fields": sorted(
                    name
                    for name, child in properties.items()
                    if native_a14._value_kind(child) == "opaque_json"
                ),
                "additional_properties": branch.get(
                    "additionalProperties", "allowed_by_default"
                ),
                "property_schemas": [
                    {
                        "field": name,
                        "required": name in required,
                        "nullable": native_a14._nullable(child),
                        "value_kind": native_a14._value_kind(child),
                        "default_present": (
                            isinstance(child, Mapping)
                            and "default" in child
                        ),
                    }
                    for name, child in sorted(properties.items())
                ],
            }
        )
    outer_properties = schema.get("properties", {})
    outer_required = set(schema.get("required", []))
    if not isinstance(outer_properties, Mapping):
        raise ValueError("elicitation outer properties are malformed")
    user_input = nodes[
        fixtures.DefinitionId("legacy", "ToolRequestUserInputParams")
    ]
    user_input_properties = user_input.get("properties", {})
    user_input_has_mode = (
        isinstance(user_input_properties, Mapping)
        and "mode" in user_input_properties
    )
    return {
        "owner": "McpServerElicitationRequestParams",
        "definition": definition.to_json(),
        "discriminator": "mode",
        "known_alternatives": branch_rows,
        "public_variant_order": (
            "McpElicitationForm",
            "McpElicitationOpenAiForm",
            "McpElicitationUrl",
            "UnknownMcpElicitation",
        ),
        "outer_required_fields": sorted(outer_required),
        "outer_optional_fields": sorted(
            set(outer_properties) - outer_required
        ),
        "outer_nullable_fields": sorted(
            name
            for name, child in outer_properties.items()
            if native_a14._nullable(child)
        ),
        "outer_additional_properties": schema.get(
            "additionalProperties", "allowed_by_default"
        ),
        "reaching_roots": (
            _key(
                "server_request",
                "ServerRequest",
                "method",
                "mcpServer/elicitation/request",
            ),
        ),
        "future_unknown": {
            "variant_index": 3,
            "preserves_mode": True,
            "preserves_raw_json": True,
            "diagnostic_kind": "UnknownDiscriminator",
            "diagnostic_severity": "ForwardCompatibility",
            "fatal": False,
        },
        "malformed_known": {
            "preserves_raw_json": True,
            "diagnostic_kind": "MalformedKnownPayload",
            "diagnostic_severity": "ProtocolWarning",
            "classified_as_future_unknown": False,
            "transport_disconnect": False,
            "typed_request_fallback": "UnknownServerRequest",
        },
        "tool_user_input_has_mode": user_input_has_mode,
    }


def _schema_closure(arguments: argparse.Namespace) -> dict[str, Any]:
    draft07 = fixtures.load_draft07(arguments.draft07_validator)
    catalog = fixtures.SchemaCatalog(arguments.schema_root, draft07)
    aggregate = catalog.load(
        arguments.schema_root
        / "stable/codex_app_server_protocol.schemas.json"
    )
    nodes, edges = fixtures.definition_graph(aggregate)
    seeds: dict[fixtures.DefinitionId, list[dict[str, Any]]] = defaultdict(list)
    identity_definitions: dict[
        tuple[str, str, str, str], set[fixtures.DefinitionId]
    ] = {}

    for root in _root_contracts():
        surface_key = root["protocol_surface_key"]
        key_tuple = _key_tuple(surface_key)
        reached: set[fixtures.DefinitionId] = set()
        for role, type_identity in root["seeds"]:
            definition = fixtures.locate_definition_for_type(
                catalog, nodes, type_identity
            )
            if definition is None:
                raise ValueError(f"missing schema root for {type_identity}")
            association = {
                "role": role,
                "surface_key": surface_key,
            }
            if association not in seeds[definition]:
                seeds[definition].append(association)
            reached.update(
                fixtures.transitive_definitions((definition,), edges)
            )
        identity_definitions[key_tuple] = reached

    closure = set(fixtures.transitive_definitions(seeds.keys(), edges))
    paths, objects = native_a14.collect_schema_paths(nodes, closure)
    path_kinds = Counter(str(row["schema_node_kind"]) for row in paths)
    for kind in ("array_element", "map_value", "property"):
        path_kinds.setdefault(kind, 0)
    formats = Counter(
        str(row["integer_format"])
        for row in paths
        if row["integer_format"] is not None
    )
    policies = Counter(
        (
            "schema"
            if row["additional_properties"] == "schema"
            else (
                "closed"
                if row["additional_properties"] is False
                else "open"
            )
        )
        for row in objects
    )
    union = _elicitation_union(nodes)
    identity_reachability = [
        {
            "protocol_surface_key": _key(*key),
            "reachable_definition_count": len(identity_definitions[key]),
        }
        for key in sorted(identity_definitions)
    ]
    seed_rows = [
        {
            "definition": definition.to_json(),
            "associations": sorted(
                associations,
                key=lambda row: (
                    row["role"],
                    _key_tuple(row["surface_key"]),
                ),
            ),
        }
        for definition, associations in sorted(seeds.items())
    ]
    definition_rows = [
        {
            "definition": definition.to_json(),
            "direct_dependencies": [
                dependency.to_json()
                for dependency in sorted(edges[definition] & closure)
            ],
            "schema_sha256": shared.sha256_json(nodes[definition]),
        }
        for definition in sorted(closure)
    ]
    defaults = tuple(
        (row["schema_path"], row["default"])
        for row in paths
        if row["default_present"]
    )
    counts = {
        "seed_definitions": len(seeds),
        "reachable_named_definitions": len(closure),
        "definition_namespaces": dict(
            sorted(Counter(row.namespace for row in closure).items())
        ),
        "schema_paths": len(paths),
        "schema_path_kinds": dict(sorted(path_kinds.items())),
        "required_properties": sum(
            row["schema_node_kind"] == "property" and bool(row["required"])
            for row in paths
        ),
        "optional_properties": sum(
            row["schema_node_kind"] == "property" and bool(row["optional"])
            for row in paths
        ),
        "nullable_paths": sum(bool(row["nullable"]) for row in paths),
        "default_bearing_paths": sum(
            bool(row["default_present"]) for row in paths
        ),
        "arrays": path_kinds["array_element"],
        "map_values": path_kinds["map_value"],
        "object_nodes": len(objects),
        "open_objects": policies["open"],
        "closed_objects": policies["closed"],
        "schema_valued_additional_properties": policies["schema"],
        "opaque_json_paths": sum(
            bool(row["intentionally_opaque_json"]) for row in paths
        ),
        "integer_number_formats": dict(sorted(formats.items())),
        "minimum_bearing_paths": sum(
            row["minimum"] is not None for row in paths
        ),
        "maximum_bearing_paths": sum(
            row["maximum"] is not None for row in paths
        ),
        "sensitive_paths": sum(bool(row["sensitive"]) for row in paths),
        "union_families": 1,
        "union_alternatives": len(union["known_alternatives"]),
        "reaching_root_identities": len(identity_reachability),
        "root_definition_reachability": sum(
            row["reachable_definition_count"]
            for row in identity_reachability
        ),
        "elicitation_reaching_roots": len(union["reaching_roots"]),
    }
    return {
        "authority": {
            "stable_aggregate_sha256": EXPECTED_STABLE_SCHEMA_AGGREGATE,
            "derivation": (
                "Regenerated directly from the pinned stable aggregate "
                "definition graph; predecessor closure evidence is not an input."
            ),
        },
        "counts": counts,
        "defaults": [
            {"schema_path": path, "default": value}
            for path, value in defaults
        ],
        "seed_definitions": seed_rows,
        "definitions": definition_rows,
        "schema_paths": paths,
        "object_policies": objects,
        "identity_reachable_definition_counts": identity_reachability,
        "elicitation_union": union,
        "integrity": {
            "definitions_sha256": _sha256(definition_rows),
            "seed_definitions_sha256": _sha256(seed_rows),
            "schema_paths_sha256": _sha256(paths),
            "object_policies_sha256": _sha256(objects),
            "identity_reachability_sha256": _sha256(identity_reachability),
        },
    }


def _status_counts(rows: Sequence[Mapping[str, Any]]) -> dict[str, int]:
    counts = Counter(str(row["typed_schema_status"]) for row in rows)
    return {
        status: counts.get(status, 0)
        for status in (
            "Complete",
            "Partial",
            "NotImplemented",
            "NotApplicable",
        )
    }


def _registry_start(
    repo_root: Path,
    predecessor_plan: Mapping[str, Any],
) -> dict[str, Any]:
    registry_text = _git_blob(
        repo_root,
        EXPECTED_PREDECESSOR_SHA,
        "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc",
    )
    rows = surface.parse_registry_data_text(
        registry_text,
        f"{EXPECTED_BASE_SHA}:ProtocolSurfaceRegistryData.inc",
    )
    by_key = {
        (
            row["category"],
            row["domain"],
            row["discriminator_field"],
            row["name"],
        ): row
        for row in rows
    }
    native_rows = [row for row in rows if row["a1_slice"] == "A1.4"]
    partial_rows = [
        {
            "protocol_surface_key": _key(
                row["category"],
                row["domain"],
                row["discriminator_field"],
                row["name"],
            ),
            "module": row["typed_module"],
            "slice": row["a1_slice"],
            "status": row["typed_schema_status"],
        }
        for row in rows
        if row["typed_schema_status"] == "Partial"
    ]
    scope_statuses = [
        {
            "protocol_surface_key": row["protocol_surface_key"],
            "status": by_key[_row_key(row)]["typed_schema_status"],
        }
        for row in _scope_identity_rows()
    ]
    pr_a_keys = sorted(
        (
            row["protocol_surface_key"]
            for row in predecessor_plan["scope"]["identities"]
        ),
        key=_key_tuple,
    )
    pr_a_rows = [
        {
            "protocol_surface_key": key,
            "status": by_key[_key_tuple(key)]["typed_schema_status"],
        }
        for key in pr_a_keys
    ]
    pr_c_rows = [
        {
            "protocol_surface_key": _key(*key),
            "status": by_key[key]["typed_schema_status"],
        }
        for key in PR_C_IDENTITIES
    ]
    inventory_rows = [
        row for row in rows if row["a1_slice"] == "InventoryOnly"
    ]
    global_counts = _status_counts(rows)
    global_counts["Total"] = len(rows)
    native_counts = _status_counts(native_rows)
    native_counts.pop("NotApplicable")
    native_counts["Total"] = len(native_rows)
    return {
        "global": global_counts,
        "native_a1_4": native_counts,
        "partials": sorted(
            partial_rows,
            key=lambda row: _key_tuple(row["protocol_surface_key"]),
        ),
        "scope_statuses": scope_statuses,
        "pr_a_complete": pr_a_rows,
        "pr_a_identity_sha256": _sha256(pr_a_keys),
        "pr_c_unchanged": pr_c_rows,
        "inventory_only": {
            "count": len(inventory_rows),
            "status_counts": _status_counts(inventory_rows),
        },
    }


def _variant_start(repo_root: Path) -> dict[str, Any]:
    events_source = _git_blob(
        repo_root,
        EXPECTED_PREDECESSOR_SHA,
        "src/ai/openai/codex/typed/Events.h",
    )
    requests_source = _git_blob(
        repo_root,
        EXPECTED_PREDECESSOR_SHA,
        "src/ai/openai/codex/typed/ServerRequests.h",
    )
    variants = {
        "CanonicalServerNotification": _variant(
            events_source, "CanonicalServerNotification"
        ),
        "Event": _variant(events_source, "Event"),
        "TypedServerRequest": _variant(
            requests_source, "TypedServerRequest"
        ),
    }
    return {
        alias: {
            "size": len(alternatives),
            "sha256": _sha256(alternatives),
            "alternatives": [
                {"index": index, "type": type_name}
                for index, type_name in enumerate(alternatives)
            ],
        }
        for alias, alternatives in variants.items()
    }


def _architecture(repo_root: Path) -> dict[str, Any]:
    # This is predecessor evidence, not a hash pin on the production work
    # that follows the freeze. Read it from the immutable Commit-1 tree so the
    # audit remains reproducible while Commits 3-5 extend the same lifecycle.
    protocol = _git_blob(
        repo_root,
        EXPECTED_PREDECESSOR_SHA,
        "src/ai/openai/codex/AppServerClient.cpp",
    )
    client = _git_blob(
        repo_root,
        EXPECTED_PREDECESSOR_SHA,
        "src/ai/openai/codex/typed/Client.h",
    )
    return {
        "transport_instances": protocol.count(
            "std::unique_ptr<detail::Transport> transport;"
        ),
        "raw_protocol_instances": protocol.count("RawProtocol rawProtocol;"),
        "json_rpc_request_id_allocators": protocol.count(
            "std::int64_t nextRequestId = 0;"
        ),
        "client_pending_operation_maps": protocol.count(
            "std::map<std::int64_t, PendingRequest> pendingRequests;"
        ),
        "server_request_occurrence_registries": protocol.count(
            "std::map<ServerRequestId, ServerRequest> pendingServerRequests;"
        ),
        "transport_generations": protocol.count(
            "\n        std::uint64_t connectionGeneration = 0;"
        ),
        "server_request_token_allocators": protocol.count(
            "std::uint64_t nextServerRequestToken = 1;"
        ),
        "typed_notification_dispatchers": protocol.count(
            "RawProtocol::NotificationHandler typedNotificationDispatcher;"
        ),
        "typed_server_request_dispatchers": protocol.count(
            "RawProtocol::ServerRequestHandler typedServerRequestDispatcher;"
        ),
        "typed_client_pimpl_pointers": client.count(
            "std::unique_ptr<Impl> impl;"
        ),
        "protocol_source_sha256": _sha256_text(protocol),
        "client_header_sha256": _sha256_text(client),
    }


def _expected_architecture_counts() -> dict[str, int]:
    return {
        "transport_instances": 1,
        "raw_protocol_instances": 1,
        "json_rpc_request_id_allocators": 1,
        "client_pending_operation_maps": 1,
        "server_request_occurrence_registries": 1,
        "transport_generations": 1,
        "server_request_token_allocators": 1,
        "typed_notification_dispatchers": 1,
        "typed_server_request_dispatchers": 1,
        "typed_client_pimpl_pointers": 1,
    }


def _dependency_authority(
    ownership: Mapping[str, Any],
) -> dict[str, Any]:
    readiness = ownership.get("cutover_readiness", {})
    normal = ownership.get("snodec_normal_dependency", {})
    provenance = ownership.get("snodec_source_authority", {})
    return {
        "cutover_readiness": {
            "ready": readiness.get("ready"),
            "snodec_cutover_performed": readiness.get(
                "snodec_cutover_performed"
            ),
            "normal_dependency_commit": readiness.get(
                "normal_dependency_commit"
            ),
            "normal_dependency_tree": readiness.get(
                "normal_dependency_tree"
            ),
            "extraction_provenance_commit": readiness.get(
                "extraction_provenance_commit"
            ),
            "extraction_provenance_tree": readiness.get(
                "extraction_provenance_tree"
            ),
        },
        "normal_dependency": {
            "repository": normal.get("repository"),
            "commit": normal.get("commit"),
            "tree": normal.get("tree"),
            "role": normal.get("role"),
        },
        "extraction_provenance": {
            "repository": provenance.get("repository"),
            "commit": provenance.get("commit"),
            "tree": provenance.get("tree"),
            "role": "read-only extraction provenance; not built or linked",
        },
    }


def _expected_dependency_authority() -> dict[str, Any]:
    return {
        "cutover_readiness": {
            "ready": True,
            "snodec_cutover_performed": True,
            "normal_dependency_commit": EXPECTED_CLEAN_SNODEC_SHA,
            "normal_dependency_tree": EXPECTED_CLEAN_SNODEC_TREE,
            "extraction_provenance_commit": EXPECTED_PROVENANCE_SHA,
            "extraction_provenance_tree": EXPECTED_PROVENANCE_TREE,
        },
        "normal_dependency": {
            "repository": "https://github.com/SNodeC/snode.c",
            "commit": EXPECTED_CLEAN_SNODEC_SHA,
            "tree": EXPECTED_CLEAN_SNODEC_TREE,
            "role": (
                "normal AISuite compilation and linking through one installed "
                "SNode.C prefix"
            ),
        },
        "extraction_provenance": {
            "repository": "https://github.com/SNodeC/snode.c",
            "commit": EXPECTED_PROVENANCE_SHA,
            "tree": EXPECTED_PROVENANCE_TREE,
            "role": "read-only extraction provenance; not built or linked",
        },
    }


def _elicitation_plan() -> dict[str, Any]:
    return {
        "owner": "McpServerElicitationRequestParams",
        "discriminator": "mode",
        "known_order": ELICITATION_MODES,
        "public_variant_order": (
            "McpElicitationForm",
            "McpElicitationOpenAiForm",
            "McpElicitationUrl",
            "UnknownMcpElicitation",
        ),
        "future_unknown_index": 3,
        "tool_user_input_owner": False,
        "tool_user_input_discriminator": None,
        "unknown_future_policy": (
            "preserve mode and raw JSON; emit ForwardCompatibility; nonfatal"
        ),
        "malformed_known_policy": (
            "preserve raw JSON; emit MalformedKnownPayload at ProtocolWarning; "
            "do not classify as future unknown and do not disconnect"
        ),
    }


def build_reports(
    arguments: argparse.Namespace,
) -> tuple[dict[str, Any], dict[str, Any]]:
    repo_root = arguments.repo_root.resolve()
    base_tree = _run(
        repo_root,
        "git",
        "show",
        "-s",
        "--format=%T",
        EXPECTED_BASE_SHA,
    )
    predecessor_tree = _run(
        repo_root,
        "git",
        "show",
        "-s",
        "--format=%T",
        EXPECTED_PREDECESSOR_SHA,
    )
    cmake = (repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
    soversion_match = re.search(
        r"set\(AISUITE_CODEX_SOVERSION\s+(\d+)\)", cmake
    )
    if soversion_match is None:
        raise ValueError("unable to parse Codex SOVERSION")
    schema_provenance = _load(arguments.schema_provenance)
    release = schema_provenance["upstream"]["release"]
    predecessor_plan = _load(arguments.predecessor_plan)
    ownership = _load(arguments.ownership_evidence)
    registry_start = _registry_start(repo_root, predecessor_plan)
    variants = _variant_start(repo_root)
    architecture = _architecture(repo_root)
    closure = _schema_closure(arguments)

    start = {
        "format_version": FORMAT_VERSION,
        "generated_notice": (
            "Generated Codex A1.4 MCP/reverse-request start evidence; "
            "do not edit."
        ),
        "actual_base": {
            "sha": EXPECTED_BASE_SHA,
            "tree": base_tree,
        },
        "immediate_predecessor": {
            "sha": EXPECTED_PREDECESSOR_SHA,
            "tree": predecessor_tree,
        },
        "protocol_authority": {
            "codex_version": schema_provenance["codex_version"],
            "upstream_tag": release["tag"],
            "upstream_source_commit": release["source_commit_sha"],
            "stable_schema_aggregate_sha256": schema_provenance[
                "schema_trees"
            ]["stable"]["aggregate_sha256"],
        },
        "dependency_authority": _dependency_authority(ownership),
        "project": {
            "codex_soversion": int(soversion_match.group(1)),
        },
        "registry_start": registry_start,
        "predecessor_variants": variants,
        "predecessor_source_blobs": {
            path: _git_blob_id(
                repo_root, EXPECTED_PREDECESSOR_SHA, path
            )
            for path in EXPECTED_SOURCE_BLOBS
        },
        "architecture_start": architecture,
    }

    plan = {
        "format_version": FORMAT_VERSION,
        "generated_notice": (
            "Generated frozen six-commit plan for Codex A1.4 MCP and "
            "reverse requests; do not edit."
        ),
        "base": {
            "sha": EXPECTED_BASE_SHA,
            "tree": EXPECTED_BASE_TREE,
        },
        "immediate_predecessor": {
            "sha": EXPECTED_PREDECESSOR_SHA,
            "tree": EXPECTED_PREDECESSOR_TREE,
        },
        "six_commit_subjects": SIX_COMMIT_SUBJECTS,
        "scope": {
            "identities": _scope_identity_rows(),
            "identity_count": 13,
            "taxonomy": {
                "client_requests": 4,
                "server_notifications": 2,
                "server_requests": 4,
                "tagged_union_alternatives": 3,
            },
            "client_result_contracts": {"Concrete": 4, "Unit": 0},
            "no_registry_promotion_commits": (1, 2, 6),
            "excluded": {
                "pr_c": [_key(*key) for key in PR_C_IDENTITIES],
                "inherited_a1_0": [
                    _key(*key) for key in INHERITED_PARTIALS
                ],
                "inventory_only_count": 48,
            },
        },
        "schema_closure": closure,
        "stages": _stages(),
        "public_api": _public_api(),
        "reverse_request_api": _reverse_request_api(),
        "notification_append": {
            "predecessor_sizes": {
                "CanonicalServerNotification": 57,
                "Event": 59,
            },
            "mapping": NOTIFICATION_APPENDS,
            "final_sizes": {
                "CanonicalServerNotification": 59,
                "Event": 61,
            },
        },
        "request_variant": {
            "predecessor_size": 8,
            "preserved_indices": {
                "UserInputRequest": 2,
                "UnknownServerRequest": 4,
            },
            "appends": REQUEST_APPENDS,
            "final_size": 11,
        },
        "elicitation_union": _elicitation_plan(),
        "architecture": {
            **_expected_architecture_counts(),
            "blocking_api_calls": 0,
            "polling_loops": 0,
            "sleeps": 0,
            "default_worker_threads": 0,
            "second_lifecycle_engines": 0,
            "backend_product_expansion": False,
            "frontend_product_expansion": False,
        },
        "commit_2_boundary": {
            "production_implementation": False,
            "registry_promotions": 0,
            "variant_changes": 0,
            "descriptor_changes": 0,
            "codec_changes": 0,
            "installed_public_headers_added": 0,
            "api_signatures_frozen_as_evidence_only": True,
        },
        "integrity": {
            "codex_soversion": EXPECTED_SOVERSION,
            "normal_snodec_commit": EXPECTED_CLEAN_SNODEC_SHA,
            "extraction_provenance_commit": EXPECTED_PROVENANCE_SHA,
            "pr_c_identity_count": 10,
            "inherited_a1_0_identity_count": 3,
            "inventory_only_identity_count": 48,
        },
    }
    return (
        json.loads(json.dumps(start, ensure_ascii=False)),
        json.loads(json.dumps(plan, ensure_ascii=False)),
    )


def report_diagnostics(
    start: Mapping[str, Any],
    plan: Mapping[str, Any],
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []

    def require(
        condition: bool,
        code: str,
        location: str,
        message: str,
    ) -> None:
        if not condition:
            diagnostics.append(Diagnostic(code, location, message))

    require(
        start.get("actual_base")
        == {"sha": EXPECTED_BASE_SHA, "tree": EXPECTED_BASE_TREE}
        and start.get("immediate_predecessor")
        == {
            "sha": EXPECTED_PREDECESSOR_SHA,
            "tree": EXPECTED_PREDECESSOR_TREE,
        }
        and plan.get("base")
        == {"sha": EXPECTED_BASE_SHA, "tree": EXPECTED_BASE_TREE}
        and plan.get("immediate_predecessor")
        == {
            "sha": EXPECTED_PREDECESSOR_SHA,
            "tree": EXPECTED_PREDECESSOR_TREE,
        },
        "McpReversePredecessorEvidenceDrift",
        "$.actual_base",
        "AISuite base SHA/tree changed",
    )
    require(
        start.get("protocol_authority")
        == {
            "codex_version": CODEX_VERSION,
            "upstream_tag": UPSTREAM_TAG,
            "upstream_source_commit": UPSTREAM_SOURCE_COMMIT,
            "stable_schema_aggregate_sha256": (
                EXPECTED_STABLE_SCHEMA_AGGREGATE
            ),
        },
        "McpReversePredecessorEvidenceDrift",
        "$.protocol_authority",
        "Codex protocol or stable schema authority changed",
    )
    require(
        start.get("dependency_authority")
        == _expected_dependency_authority(),
        "McpReverseDependencyAuthorityMismatch",
        "$.dependency_authority",
        "cleaned dependency and historical provenance roles changed",
    )
    require(
        start.get("project", {}).get("codex_soversion")
        == EXPECTED_SOVERSION
        and plan.get("integrity", {}).get("codex_soversion")
        == EXPECTED_SOVERSION,
        "McpReverseSOVERSIONDrift",
        "$.project.codex_soversion",
        "Codex SOVERSION changed",
    )
    require(
        start.get("predecessor_source_blobs") == EXPECTED_SOURCE_BLOBS,
        "McpReversePredecessorEvidenceDrift",
        "$.predecessor_source_blobs",
        "base source blob identities changed",
    )

    registry = start.get("registry_start", {})
    require(
        registry.get("global")
        == {
            "Complete": 313,
            "Partial": 4,
            "NotImplemented": 22,
            "NotApplicable": 48,
            "Total": 387,
        }
        and registry.get("native_a1_4")
        == {
            "Complete": 33,
            "Partial": 1,
            "NotImplemented": 22,
            "Total": 56,
        },
        "McpReverseRegistryStartMismatch",
        "$.registry_start",
        "global or native A1.4 start arithmetic changed",
    )
    expected_partials = {
        (*key, "Common", "A1.0", "Partial")
        for key in INHERITED_PARTIALS
    } | {
        (
            "server_request",
            "ServerRequest",
            "method",
            "item/tool/requestUserInput",
            "IntegrationsAndLongTail",
            "A1.4",
            "Partial",
        )
    }
    actual_partials = {
        (
            *_key_tuple(row.get("protocol_surface_key", {})),
            row.get("module"),
            row.get("slice"),
            row.get("status"),
        )
        for row in registry.get("partials", [])
        if isinstance(row, Mapping)
    }
    require(
        actual_partials == expected_partials,
        "McpReverseRegistryStartMismatch",
        "$.registry_start.partials",
        "partial identity or ownership set changed",
    )
    scope_statuses = registry.get("scope_statuses", [])
    expected_statuses = {
        _row_key(row): row["start_status"]
        for row in _scope_identity_rows()
    }
    actual_statuses = {
        _row_key(row): row.get("status")
        for row in scope_statuses
        if isinstance(row, Mapping)
    }
    require(
        actual_statuses == expected_statuses,
        "McpReverseFalseComplete",
        "$.registry_start.scope_statuses",
        "a scoped identity has a false or changed start status",
    )
    pr_a_rows = registry.get("pr_a_complete", [])
    pr_a_keys = [
        row.get("protocol_surface_key", {})
        for row in pr_a_rows
        if isinstance(row, Mapping)
    ]
    require(
        len(pr_a_rows) == 33
        and all(row.get("status") == "Complete" for row in pr_a_rows)
        and _sha256(sorted(pr_a_keys, key=_key_tuple))
        == EXPECTED_PR_A_IDENTITY_HASH,
        "McpReversePredecessorEvidenceDrift",
        "$.registry_start.pr_a_complete",
        "PR-A identity completion changed",
    )
    require(
        registry.get("pr_a_identity_sha256")
        == EXPECTED_PR_A_IDENTITY_HASH,
        "McpReversePredecessorEvidenceDrift",
        "$.registry_start.pr_a_identity_sha256",
        "PR-A identity hash changed",
    )
    pr_c_rows = registry.get("pr_c_unchanged", [])
    require(
        [
            _row_key(row)
            for row in pr_c_rows
            if isinstance(row, Mapping)
        ]
        == list(PR_C_IDENTITIES)
        and all(row.get("status") == "NotImplemented" for row in pr_c_rows),
        "McpReversePrCScopeLeak",
        "$.registry_start.pr_c_unchanged",
        "PR-C identity start status changed",
    )
    require(
        registry.get("inventory_only")
        == {
            "count": 48,
            "status_counts": {
                "Complete": 0,
                "Partial": 0,
                "NotImplemented": 0,
                "NotApplicable": 48,
            },
        },
        "McpReverseInventoryScopeLeak",
        "$.registry_start.inventory_only",
        "InventoryOnly denominator or NotApplicable status changed",
    )

    variants = start.get("predecessor_variants", {})
    for alias in (
        "CanonicalServerNotification",
        "Event",
        "TypedServerRequest",
    ):
        row = variants.get(alias, {})
        alternatives = row.get("alternatives", [])
        types = [
            alternative.get("type")
            for alternative in alternatives
            if isinstance(alternative, Mapping)
        ]
        require(
            row.get("size") == EXPECTED_VARIANT_SIZES[alias]
            and row.get("sha256") == EXPECTED_VARIANT_HASHES[alias]
            and _sha256(types) == EXPECTED_VARIANT_HASHES[alias]
            and all(
                alternative.get("index") == index
                for index, alternative in enumerate(alternatives)
                if isinstance(alternative, Mapping)
            ),
            "McpReverseVariantBaseIndexMismatch",
            f"$.predecessor_variants.{alias}",
            f"{alias} predecessor type/index mapping changed",
        )
    typed_types = [
        row.get("type")
        for row in variants.get("TypedServerRequest", {}).get(
            "alternatives", []
        )
        if isinstance(row, Mapping)
    ]
    require(
        len(typed_types) == 8
        and typed_types[2] == "UserInputRequest"
        and typed_types[4] == "UnknownServerRequest",
        "McpReverseRequestAppendIndexMismatch",
        "$.predecessor_variants.TypedServerRequest",
        "UserInputRequest or UnknownServerRequest predecessor index changed",
    )

    architecture_start = start.get("architecture_start", {})
    require(
        all(
            architecture_start.get(name) == value
            for name, value in _expected_architecture_counts().items()
        ),
        "McpReverseArchitectureMismatch",
        "$.architecture_start",
        "one-RawProtocol or one-occurrence-lifecycle evidence changed",
    )

    scope = plan.get("scope", {})
    identities = scope.get("identities", [])
    expected_rows = _scope_identity_rows()
    expected_keys = {_row_key(row) for row in expected_rows}
    actual_keys = {
        _row_key(row)
        for row in identities
        if isinstance(row, Mapping)
    }
    require(
        identities == expected_rows
        and scope.get("identity_count") == 13
        and scope.get("taxonomy")
        == {
            "client_requests": 4,
            "server_notifications": 2,
            "server_requests": 4,
            "tagged_union_alternatives": 3,
        },
        "McpReverseIdentitySetMismatch",
        "$.scope",
        "exact 13-identity set or 4/2/4/3 taxonomy changed",
    )
    extras = actual_keys - expected_keys
    require(
        not (extras & set(PR_C_IDENTITIES)),
        "McpReversePrCScopeLeak",
        "$.scope.identities",
        "a PR-C identity leaked into MCP/reverse scope",
    )
    require(
        not (extras & set(INHERITED_PARTIALS)),
        "McpReverseInheritedScopeLeak",
        "$.scope.identities",
        "an inherited A1.0 identity leaked into MCP/reverse scope",
    )
    require(
        scope.get("client_result_contracts")
        == {"Concrete": 4, "Unit": 0}
        and all(
            row.get("result_kind") == expected.get("result_kind")
            and row.get("parameter_type") == expected.get("parameter_type")
            and row.get("result_type") == expected.get("result_type")
            for row, expected in zip(identities, expected_rows)
        )
        if len(identities) == len(expected_rows)
        else False,
        "McpReverseResultContractMismatch",
        "$.scope.identities",
        "request/result contract changed",
    )
    actual_mode_rows = [
        row
        for row in identities
        if isinstance(row, Mapping)
        and _row_key(row)[3] in ELICITATION_MODES
    ]
    require(
        len(actual_mode_rows) == 3
        and all(
            _row_key(row)[0:3]
            == (
                "tagged_union_discriminator",
                "McpServerElicitationRequestParams",
                "mode",
            )
            for row in actual_mode_rows
        ),
        "McpReverseElicitationOwnershipMismatch",
        "$.scope.identities",
        "elicitation alternatives moved into the user-input model",
    )
    require(
        all(
            row.get("start_status")
            == expected_statuses.get(_row_key(row))
            for row in identities
            if isinstance(row, Mapping)
        ),
        "McpReverseFalseComplete",
        "$.scope.identities",
        "a scoped row claims false predecessor completion",
    )
    require(
        all(
            row.get("promotion_commit")
            == {
                **{
                    (
                        "client_request",
                        "ClientRequest",
                        "method",
                        name,
                    ): 3
                    for name, _params, _result in CLIENT_REQUESTS
                },
                **{
                    (
                        "server_notification",
                        "ServerNotification",
                        "method",
                        name,
                    ): 3
                    for name, _payload in SERVER_NOTIFICATIONS
                },
                **{
                    (
                        "server_request",
                        "ServerRequest",
                        "method",
                        name,
                    ): commit
                    for (
                        name,
                        _params,
                        _result,
                        _public,
                        _index,
                        commit,
                    ) in SERVER_REQUESTS
                },
                **{
                    (
                        "tagged_union_discriminator",
                        "McpServerElicitationRequestParams",
                        "mode",
                        mode,
                    ): 5
                    for mode in ELICITATION_MODES
                },
            }.get(_row_key(row))
            for row in identities
            if isinstance(row, Mapping)
        )
        and scope.get("no_registry_promotion_commits") == [1, 2, 6],
        "McpReversePromotionStageMismatch",
        "$.scope.identities",
        "identity promotion moved to the wrong commit",
    )

    closure = plan.get("schema_closure", {})
    integrity = closure.get("integrity", {})
    require(
        closure.get("counts") == EXPECTED_CLOSURE,
        "McpReverseSchemaClosureMismatch",
        "$.schema_closure.counts",
        "18/55/204 closure or schema taxonomy changed",
    )
    require(
        tuple(
            (row.get("schema_path"), row.get("default"))
            for row in closure.get("defaults", [])
            if isinstance(row, Mapping)
        )
        == EXPECTED_DEFAULTS,
        "McpReverseSchemaClosureMismatch",
        "$.schema_closure.defaults",
        "default-bearing schema paths changed",
    )
    closure_hash_contracts = (
        (
            "definitions",
            "definitions_sha256",
            "definitions",
        ),
        (
            "seed_definitions",
            "seed_definitions_sha256",
            "seed_definitions",
        ),
        (
            "schema_paths",
            "schema_paths_sha256",
            "schema_paths",
        ),
        (
            "object_policies",
            "object_policies_sha256",
            "object_policies",
        ),
        (
            "identity_reachable_definition_counts",
            "identity_reachability_sha256",
            "identity_reachability",
        ),
    )
    for rows_name, integrity_name, expected_name in closure_hash_contracts:
        rows = closure.get(rows_name, [])
        require(
            integrity.get(integrity_name)
            == EXPECTED_SCHEMA_HASHES[expected_name]
            and _sha256(rows) == EXPECTED_SCHEMA_HASHES[expected_name],
            "McpReverseSchemaClosureMismatch",
            f"$.schema_closure.{rows_name}",
            f"exact {rows_name} closure changed",
        )
    union_schema = closure.get("elicitation_union", {})
    require(
        union_schema.get("owner")
        == "McpServerElicitationRequestParams"
        and union_schema.get("discriminator") == "mode"
        and [
            row.get("alternative")
            for row in union_schema.get("known_alternatives", [])
            if isinstance(row, Mapping)
        ]
        == list(ELICITATION_MODES)
        and union_schema.get("tool_user_input_has_mode") is False,
        "McpReverseElicitationOwnershipMismatch",
        "$.schema_closure.elicitation_union",
        "schema-derived elicitation ownership or order changed",
    )

    require(
        plan.get("stages") == _stages(),
        "McpReverseStageArithmeticMismatch",
        "$.stages",
        "Commit 3-5 ownership or registry arithmetic changed",
    )
    require(
        plan.get("six_commit_subjects") == list(SIX_COMMIT_SUBJECTS),
        "McpReversePromotionStageMismatch",
        "$.six_commit_subjects",
        "six-commit history contract changed",
    )
    require(
        plan.get("public_api")
        == json.loads(json.dumps(_public_api(), ensure_ascii=False))
        and plan.get("reverse_request_api")
        == json.loads(json.dumps(_reverse_request_api(), ensure_ascii=False)),
        "McpReversePublicApiMismatch",
        "$.public_api",
        "frozen application-facing API signature changed",
    )
    require(
        plan.get("notification_append")
        == {
            "predecessor_sizes": {
                "CanonicalServerNotification": 57,
                "Event": 59,
            },
            "mapping": list(NOTIFICATION_APPENDS),
            "final_sizes": {
                "CanonicalServerNotification": 59,
                "Event": 61,
            },
        },
        "McpReverseNotificationAppendIndexMismatch",
        "$.notification_append",
        "notification append order or final size changed",
    )
    require(
        plan.get("request_variant")
        == {
            "predecessor_size": 8,
            "preserved_indices": {
                "UserInputRequest": 2,
                "UnknownServerRequest": 4,
            },
            "appends": list(REQUEST_APPENDS),
            "final_size": 11,
        },
        "McpReverseRequestAppendIndexMismatch",
        "$.request_variant",
        "TypedServerRequest append order or preserved index changed",
    )
    require(
        plan.get("elicitation_union")
        == json.loads(json.dumps(_elicitation_plan(), ensure_ascii=False)),
        "McpReverseElicitationOwnershipMismatch",
        "$.elicitation_union",
        "elicitation union ownership or malformed/future policy changed",
    )
    expected_architecture = {
        **_expected_architecture_counts(),
        "blocking_api_calls": 0,
        "polling_loops": 0,
        "sleeps": 0,
        "default_worker_threads": 0,
        "second_lifecycle_engines": 0,
        "backend_product_expansion": False,
        "frontend_product_expansion": False,
    }
    require(
        plan.get("architecture") == expected_architecture,
        "McpReverseArchitectureMismatch",
        "$.architecture",
        "non-blocking one-lifecycle architecture contract changed",
    )
    require(
        plan.get("commit_2_boundary")
        == {
            "production_implementation": False,
            "registry_promotions": 0,
            "variant_changes": 0,
            "descriptor_changes": 0,
            "codec_changes": 0,
            "installed_public_headers_added": 0,
            "api_signatures_frozen_as_evidence_only": True,
        },
        "McpReverseFalseComplete",
        "$.commit_2_boundary",
        "Commit 2 contains or claims production completion",
    )
    return sorted(set(diagnostics))


def validate_reports(
    start: Mapping[str, Any],
    plan: Mapping[str, Any],
) -> None:
    diagnostics = report_diagnostics(start, plan)
    if diagnostics:
        raise AuditError(diagnostics)


def write_or_check(
    path: Path,
    value: Mapping[str, Any],
    *,
    check: bool,
) -> None:
    rendered = _render(value)
    if check:
        if not path.is_file() or path.read_text(encoding="utf-8") != rendered:
            raise AuditError(
                (
                    Diagnostic(
                        "McpReversePredecessorEvidenceDrift",
                        str(path),
                        "checked generated evidence is stale",
                    ),
                )
            )
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(rendered, encoding="utf-8")


def parser() -> argparse.ArgumentParser:
    repo = Path(__file__).resolve().parents[2]
    evidence = repo / "tools/codex/app-server-evidence/0.144.6"
    schema_root = repo / "tools/codex/app-server-schema/0.144.6"
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("command", choices=("generate", "check"))
    result.add_argument("--repo-root", type=Path, default=repo)
    result.add_argument("--schema-root", type=Path, default=schema_root)
    result.add_argument(
        "--draft07-validator",
        type=Path,
        default=repo / "tools/codex/draft07.py",
    )
    result.add_argument(
        "--schema-provenance",
        type=Path,
        default=schema_root / "PROVENANCE.json",
    )
    result.add_argument(
        "--predecessor-plan",
        type=Path,
        default=evidence / "a1-4-user-integrations-batch-plan.json",
    )
    result.add_argument(
        "--ownership-evidence",
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
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    try:
        start, plan = build_reports(arguments)
        validate_reports(start, plan)
        check = arguments.command == "check"
        write_or_check(arguments.start_state, start, check=check)
        write_or_check(arguments.batch_plan, plan, check=check)
    except (
        AuditError,
        OSError,
        ValueError,
        KeyError,
        subprocess.CalledProcessError,
        fixtures.FixtureError,
        surface.SurfaceError,
    ) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
