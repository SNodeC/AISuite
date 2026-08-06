#!/usr/bin/env python3
"""Generate and verify the reviewed C++ Frontend SDK binding inventory."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any, NamedTuple


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "docs/ai/openai/codex/frontend-protocol-v1.manifest.json"
SCHEMA = ROOT / "docs/ai/openai/codex/frontend-protocol-v1.schema.json"
BINDINGS = ROOT / "tools/frontend/cpp-client-bindings.json"
PROVIDER_DESCRIPTORS = ROOT / "src/ai/openai/codex/detail/ClientOperationCodecDescriptors.inc"
PROVIDER_OPERATIONS = ROOT / "src/ai/openai/codex/backend/internal/ProviderOperations.inc"
CODEC_HEADERS = ROOT / "src/ai/openai/codex/detail"
OUTPUT = ROOT / "src/ai/openai/codex/frontend/client/GeneratedBindings.h"
CLIENT_OUTPUT = ROOT / "src/ai/openai/codex/frontend/client"
FACADE_IMPLEMENTATION = CLIENT_OUTPUT / "GeneratedFacades.cpp"

FORMAT_VERSION = 2

CLIENT_NAMESPACE = "::ai::openai::codex::frontend::client"
FRONTEND_NAMESPACE = "::ai::openai::codex::frontend"
TYPED_NAMESPACE = "::ai::openai::codex::typed"
CODEX_DETAIL_NAMESPACE = "::ai::openai::codex::detail"
CLIENT_DETAIL_NAMESPACE = f"{CLIENT_NAMESPACE}::detail"

NATIVE_BINDINGS: dict[str, dict[str, str]] = {
    "ControllerAcquire": {
        "facade": "Controller",
        "operation": "acquire",
        "parameterType": f"{TYPED_NAMESPACE}::Unit",
        "parameterHeader": "ai/openai/codex/typed/Results.h",
        "resultType": f"{CLIENT_NAMESPACE}::ControllerResult",
        "resultHeader": "ai/openai/codex/frontend/client/Controller.h",
        "parameterEncoder": f"{CLIENT_DETAIL_NAMESPACE}::encodeUnitParams",
        "resultDecoder": f"{CLIENT_DETAIL_NAMESPACE}::decodeControllerResult",
    },
    "ControllerRelease": {
        "facade": "Controller",
        "operation": "release",
        "parameterType": f"{TYPED_NAMESPACE}::Unit",
        "parameterHeader": "ai/openai/codex/typed/Results.h",
        "resultType": f"{CLIENT_NAMESPACE}::ControllerResult",
        "resultHeader": "ai/openai/codex/frontend/client/Controller.h",
        "parameterEncoder": f"{CLIENT_DETAIL_NAMESPACE}::encodeUnitParams",
        "resultDecoder": f"{CLIENT_DETAIL_NAMESPACE}::decodeControllerResult",
    },
    "SnapshotGet": {
        "facade": "Synchronization",
        "operation": "snapshot",
        "parameterType": f"{TYPED_NAMESPACE}::Unit",
        "parameterHeader": "ai/openai/codex/typed/Results.h",
        "resultType": f"{CLIENT_NAMESPACE}::SynchronizationResult",
        "resultHeader": "ai/openai/codex/frontend/client/Synchronization.h",
        "parameterEncoder": f"{CLIENT_DETAIL_NAMESPACE}::encodeUnitParams",
        "resultDecoder": f"{CLIENT_DETAIL_NAMESPACE}::decodeSnapshotSynchronizationResult",
    },
    "EventsReplay": {
        "facade": "Synchronization",
        "operation": "replay",
        "parameterType": f"{FRONTEND_NAMESPACE}::SequenceNumber",
        "parameterHeader": "ai/openai/codex/frontend/Protocol.h",
        "resultType": f"{CLIENT_NAMESPACE}::SynchronizationResult",
        "resultHeader": "ai/openai/codex/frontend/client/Synchronization.h",
        "parameterEncoder": f"{CLIENT_DETAIL_NAMESPACE}::encodeEventsReplayParams",
        "resultDecoder": f"{CLIENT_DETAIL_NAMESPACE}::decodeReplaySynchronizationResult",
    },
    "ProviderStart": {
        "facade": "Provider",
        "operation": "start",
        "parameterType": f"{TYPED_NAMESPACE}::Unit",
        "parameterHeader": "ai/openai/codex/typed/Results.h",
        "resultType": f"{TYPED_NAMESPACE}::Unit",
        "resultHeader": "ai/openai/codex/typed/Results.h",
        "parameterEncoder": f"{CLIENT_DETAIL_NAMESPACE}::encodeUnitParams",
        "resultDecoder": f"{CLIENT_DETAIL_NAMESPACE}::decodeUnitResult",
    },
    "ProviderStop": {
        "facade": "Provider",
        "operation": "stop",
        "parameterType": f"{TYPED_NAMESPACE}::Unit",
        "parameterHeader": "ai/openai/codex/typed/Results.h",
        "resultType": f"{TYPED_NAMESPACE}::Unit",
        "resultHeader": "ai/openai/codex/typed/Results.h",
        "parameterEncoder": f"{CLIENT_DETAIL_NAMESPACE}::encodeUnitParams",
        "resultDecoder": f"{CLIENT_DETAIL_NAMESPACE}::decodeUnitResult",
    },
    "ProviderRestart": {
        "facade": "Provider",
        "operation": "restart",
        "parameterType": f"{TYPED_NAMESPACE}::Unit",
        "parameterHeader": "ai/openai/codex/typed/Results.h",
        "resultType": f"{TYPED_NAMESPACE}::Unit",
        "resultHeader": "ai/openai/codex/typed/Results.h",
        "parameterEncoder": f"{CLIENT_DETAIL_NAMESPACE}::encodeUnitParams",
        "resultDecoder": f"{CLIENT_DETAIL_NAMESPACE}::decodeUnitResult",
    },
}

PROJECTED_PROVIDER_RESULTS = {
    "ThreadStart": "ThreadStartResult",
    "ThreadResume": "ThreadResumeResult",
    "ThreadList": "ThreadListResult",
    "ThreadRead": "ThreadReadResult",
    "TurnStart": "TurnStartResult",
}

REVERSE_METHODS = {
    "ApprovalRespond": "respond",
    "UserInputRespond": "respond",
    "AuthenticationRespond": "respond",
    "UnknownRequestRespond": "respond",
    "UnknownRequestReject": "reject",
    "ApplyPatchApprovalRespond": "respond",
    "AttestationRespond": "respond",
    "DynamicToolRespond": "respond",
    "ExecCommandApprovalRespond": "respond",
    "KnownRequestReject": "reject",
    "McpElicitationRespond": "respond",
    "PermissionsApprovalRespond": "respond",
}

FACADE_TYPED_HEADERS = {
    "Accounts": "ai/openai/codex/typed/Accounts.h",
    "Apps": "ai/openai/codex/typed/Apps.h",
    "Commands": "ai/openai/codex/typed/Commands.h",
    "Configuration": "ai/openai/codex/typed/Configuration.h",
    "ExternalAgents": "ai/openai/codex/typed/ExternalAgents.h",
    "Feedback": "ai/openai/codex/typed/Feedback.h",
    "Filesystem": "ai/openai/codex/typed/Filesystem.h",
    "Hooks": "ai/openai/codex/typed/Hooks.h",
    "Marketplace": "ai/openai/codex/typed/Marketplace.h",
    "Mcp": "ai/openai/codex/typed/Mcp.h",
    "Models": "ai/openai/codex/typed/Models.h",
    "PermissionProfiles": "ai/openai/codex/typed/PermissionProfiles.h",
    "Plugins": "ai/openai/codex/typed/Plugins.h",
    "Reviews": "ai/openai/codex/typed/Reviews.h",
    "Skills": "ai/openai/codex/typed/Skills.h",
    "Threads": "ai/openai/codex/typed/Threads.h",
    "Turns": "ai/openai/codex/typed/Turns.h",
    "WindowsSandbox": "ai/openai/codex/typed/WindowsSandbox.h",
}

REQUIRED_FIELDS = {
    "methodId",
    "facade",
    "operation",
    "category",
    "parameterType",
    "parameterHeader",
    "resultType",
    "resultHeader",
    "parameterEncoder",
    "resultDecoder",
    "sensitive",
}


class ProviderCodecDescriptor(NamedTuple):
    provider_method: str
    parameter_type: str
    result_type: str


class ProviderOperationDescriptor(NamedTuple):
    facade: str
    operation: str
    result_type: str
    provider_method: str


def load_json(path: pathlib.Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def provider_descriptors() -> dict[str, ProviderCodecDescriptor]:
    pattern = re.compile(
        r'CODEX_CLIENT_OPERATION_CODEC_DESCRIPTOR\(\s*SurfaceCategory::ClientRequest,\s*'
        r'"ClientRequest",\s*"method",\s*"(?P<provider>[^\"]+)",\s*'
        r'ClientRequestTarget::(?P<id>[A-Za-z0-9_]+),\s*'
        r'"ClientRequestTarget::[A-Za-z0-9_]+",\s*'
        r'"(?P<parameter>[A-Za-z0-9_]+)",\s*"(?P<result>[A-Za-z0-9_]+)"'
    )
    descriptors: dict[str, ProviderCodecDescriptor] = {}
    for match in pattern.finditer(PROVIDER_DESCRIPTORS.read_text(encoding="utf-8")):
        descriptors[match.group("id")] = ProviderCodecDescriptor(
            match.group("provider"), match.group("parameter"), match.group("result")
        )
    if len(descriptors) != 86:
        raise ValueError(f"expected 86 provider codec descriptors, got {len(descriptors)}")
    return descriptors


def provider_operations() -> dict[str, ProviderOperationDescriptor]:
    pattern = re.compile(
        r"CODEX_BACKEND_PROVIDER_OPERATION(?:_EMPTY)?\s*\(\s*"
        r"(?P<id>[A-Za-z0-9_]+)\s*,\s*(?P<result>[A-Za-z0-9_:]+)\s*,\s*"
        r"(?P<accessor>[A-Za-z0-9_]+)\s*,\s*(?P<operation>[A-Za-z0-9_]+)\s*,\s*"
        r"[A-Za-z0-9_:]+\s*,\s*(?:true|false)\s*,\s*\"(?P<provider>[^\"]+)\"\s*\)",
        re.DOTALL,
    )
    operations: dict[str, ProviderOperationDescriptor] = {}
    for match in pattern.finditer(PROVIDER_OPERATIONS.read_text(encoding="utf-8")):
        accessor = match.group("accessor")
        facade = accessor[0].upper() + accessor[1:]
        operations[match.group("id")] = ProviderOperationDescriptor(
            facade, match.group("operation"), match.group("result"), match.group("provider")
        )
    if len(operations) != 86:
        raise ValueError(f"expected 86 provider operation declarations, got {len(operations)}")
    return operations


def parameter_encoder_headers() -> dict[str, str]:
    encoders: dict[str, str] = {}
    pattern = re.compile(r"\bencode(?P<type>[A-Za-z0-9_]+Params)\s*\(")
    for path in CODEC_HEADERS.glob("*Codec.h"):
        for match in pattern.finditer(path.read_text(encoding="utf-8")):
            encoder = f"encode{match.group('type')}"
            header = path.relative_to(ROOT / "src").as_posix()
            previous = encoders.setdefault(encoder, header)
            if previous != header:
                raise ValueError(f"parameter encoder {encoder} is declared by both {previous} and {header}")
    return encoders


def validate_provider_schema_authority(method: dict[str, Any],
                                       descriptor: ProviderCodecDescriptor,
                                       operation: ProviderOperationDescriptor,
                                       schema: dict[str, Any]) -> None:
    method_id = method["id"]
    expected_parameter_schema = f"#/$defs/{method_id}Params"
    expected_result_schema = f"#/$defs/{method_id}Result"
    for field, expected in (
        ("parameterSchema", expected_parameter_schema),
        ("resultSchema", expected_result_schema),
    ):
        if method.get(field) != expected:
            raise ValueError(
                f"{method_id} frontend {field} disagrees with its method-specific schema identity: "
                f"expected {expected!r}, got {method.get(field)!r}"
            )

    definitions = schema.get("$defs")
    if not isinstance(definitions, dict):
        raise ValueError("frontend schema lacks an object $defs authority")
    parameter_schema_name = expected_parameter_schema.rsplit("/", 1)[-1]
    result_schema_name = expected_result_schema.rsplit("/", 1)[-1]
    if parameter_schema_name not in definitions:
        raise ValueError(f"{method_id} frontend parameter schema definition {parameter_schema_name} is missing")
    if result_schema_name not in definitions:
        raise ValueError(f"{method_id} frontend result schema definition {result_schema_name} is missing")

    if method.get("backendCommand") != method_id:
        raise ValueError(
            f"{method_id} frontend backendCommand disagrees with the provider binding: "
            f"got {method.get('backendCommand')!r}"
        )
    if method.get("providerMethod") != descriptor.provider_method or method.get("providerMethod") != operation.provider_method:
        raise ValueError(
            f"{method_id} provider method disagrees across frontend manifest, codec descriptor, and provider operation: "
            f"{method.get('providerMethod')!r}, {descriptor.provider_method!r}, {operation.provider_method!r}"
        )
    if descriptor.result_type != operation.result_type or method.get("resultType") != descriptor.result_type:
        raise ValueError(
            f"{method_id} result type disagrees across frontend manifest, codec descriptor, and provider operation: "
            f"{method.get('resultType')!r}, {descriptor.result_type!r}, {operation.result_type!r}"
        )

    parameter_schema = definitions[parameter_schema_name]
    result_schema = definitions[result_schema_name]
    if not isinstance(parameter_schema, dict) or not isinstance(result_schema, dict):
        raise ValueError(f"{method_id} frontend parameter/result schema definitions must be objects")

    projected = method_id in PROJECTED_PROVIDER_RESULTS
    if projected:
        if not method.get("legacyCompatibilityMethod") or descriptor.result_type == "Unit":
            raise ValueError(f"{method_id} projected client result is not a non-Unit legacy provider projection")
    elif descriptor.result_type != "Unit" and result_schema.get("title") != descriptor.result_type:
        raise ValueError(
            f"{method_id} frontend result schema title disagrees with typed result {descriptor.result_type!r}: "
            f"got {result_schema.get('title')!r}"
        )

    if descriptor.parameter_type == "Unit":
        if parameter_schema.get("type") != "object":
            raise ValueError(f"{method_id} Unit parameter schema must remain an object")
    elif not method.get("legacyCompatibilityMethod") and parameter_schema.get("title") != descriptor.parameter_type:
        raise ValueError(
            f"{method_id} frontend parameter schema title disagrees with typed parameter {descriptor.parameter_type!r}: "
            f"got {parameter_schema.get('title')!r}"
        )

    if descriptor.result_type == "Unit" and result_schema.get("type") != "object":
        raise ValueError(f"{method_id} Unit result schema must remain an object")


def provider_binding(method: dict[str, Any],
                     descriptors: dict[str, ProviderCodecDescriptor],
                     operations: dict[str, ProviderOperationDescriptor],
                     encoders: dict[str, str],
                     schema: dict[str, Any]) -> dict[str, Any]:
    method_id = method["id"]
    if method_id not in descriptors or method_id not in operations:
        raise ValueError(f"provider authority is missing {method_id}")
    descriptor = descriptors[method_id]
    operation_descriptor = operations[method_id]
    validate_provider_schema_authority(method, descriptor, operation_descriptor, schema)
    parameter_name = descriptor.parameter_type
    upstream_result_name = descriptor.result_type
    facade = operation_descriptor.facade
    operation = operation_descriptor.operation
    if facade not in FACADE_TYPED_HEADERS:
        raise ValueError(f"provider binding {method_id} has unknown facade {facade}")

    parameter_type = f"{TYPED_NAMESPACE}::{parameter_name}"
    parameter_header = (
        "ai/openai/codex/typed/Results.h" if parameter_name == "Unit" else FACADE_TYPED_HEADERS[facade]
    )
    if parameter_name == "Unit":
        parameter_encoder = f"{CLIENT_DETAIL_NAMESPACE}::encodeUnitParams"
    else:
        encoder_name = f"encode{parameter_name}"
        if encoder_name not in encoders:
            raise ValueError(f"provider binding {method_id} lacks encoder {encoder_name}")
        parameter_encoder = f"{CODEX_DETAIL_NAMESPACE}::{encoder_name}"

    if method_id in PROJECTED_PROVIDER_RESULTS:
        result_name = PROJECTED_PROVIDER_RESULTS[method_id]
        result_type = f"{CLIENT_NAMESPACE}::{result_name}"
        result_header = "ai/openai/codex/frontend/client/Results.h"
        result_decoder = f"{CLIENT_DETAIL_NAMESPACE}::decode{result_name}"
    elif upstream_result_name == "Unit":
        result_type = f"{TYPED_NAMESPACE}::Unit"
        result_header = "ai/openai/codex/typed/Results.h"
        result_decoder = f"{CLIENT_DETAIL_NAMESPACE}::decodeUnitResult"
    else:
        result_type = f"{TYPED_NAMESPACE}::{upstream_result_name}"
        result_header = FACADE_TYPED_HEADERS[facade]
        result_decoder = (
            f"{CODEX_DETAIL_NAMESPACE}::decodeClientOperationResultAs<"
            f"{result_type}>@ClientRequestTarget::{method_id}"
        )

    return {
        "methodId": method_id,
        "facade": facade,
        "operation": operation,
        "category": "provider",
        "parameterType": parameter_type,
        "parameterHeader": parameter_header,
        "resultType": result_type,
        "resultHeader": result_header,
        "parameterEncoder": parameter_encoder,
        "resultDecoder": result_decoder,
        "sensitive": method_id == "AccountLoginStart",
    }


def native_binding(method: dict[str, Any]) -> dict[str, Any]:
    method_id = method["id"]
    try:
        fields = NATIVE_BINDINGS[method_id]
    except KeyError as error:
        raise ValueError(f"native binding authority is missing {method_id}") from error
    return {"methodId": method_id, "category": "native", **fields, "sensitive": False}


def reverse_binding(method: dict[str, Any]) -> dict[str, Any]:
    method_id = method["id"]
    try:
        operation = REVERSE_METHODS[method_id]
    except KeyError as error:
        raise ValueError(f"reverse binding authority is missing {method_id}") from error
    params_name = f"{method_id}Params"
    return {
        "methodId": method_id,
        "facade": "Requests",
        "operation": operation,
        "category": "reverse",
        "parameterType": f"{CLIENT_NAMESPACE}::{params_name}",
        "parameterHeader": "ai/openai/codex/frontend/client/Types.h",
        "resultType": f"{TYPED_NAMESPACE}::Unit",
        "resultHeader": "ai/openai/codex/typed/Results.h",
        "parameterEncoder": f"{CLIENT_DETAIL_NAMESPACE}::encode{params_name}",
        "resultDecoder": f"{CLIENT_DETAIL_NAMESPACE}::decodeUnitResult",
        "sensitive": True,
    }


def expected_bindings() -> tuple[dict[str, Any], list[dict[str, Any]]]:
    manifest = load_json(MANIFEST)
    schema = load_json(SCHEMA)
    descriptors = provider_descriptors()
    operations = provider_operations()
    encoders = parameter_encoder_headers()
    projected_methods = {
        method["id"]
        for method in manifest["methods"]
        if method["category"] == "provider_operation"
        and method["legacyCompatibilityMethod"]
        and method["resultType"] != "Unit"
    }
    if projected_methods != set(PROJECTED_PROVIDER_RESULTS):
        raise ValueError(
            "projected provider result bindings disagree with the legacy non-Unit frontend result inventory: "
            f"expected {sorted(projected_methods)}, got {sorted(PROJECTED_PROVIDER_RESULTS)}"
        )
    bindings: list[dict[str, Any]] = []
    for method in manifest["methods"]:
        if method["frontendNative"]:
            binding = native_binding(method)
        elif method["category"] == "reverse_response":
            binding = reverse_binding(method)
        elif method["category"] == "provider_operation":
            binding = provider_binding(method, descriptors, operations, encoders, schema)
        else:
            raise ValueError(f"unsupported binding category for {method['id']}")
        bindings.append(binding)
    return manifest, bindings


def validate(bindings: list[dict[str, Any]]) -> tuple[dict[str, Any], dict[str, int]]:
    manifest, expected = expected_bindings()
    expected_by_id = {binding["methodId"]: binding for binding in expected}
    seen: set[str] = set()
    counts = {"native": 0, "provider": 0, "reverse": 0}
    for binding in bindings:
        missing = REQUIRED_FIELDS - binding.keys()
        if missing:
            raise ValueError(f"binding is missing {sorted(missing)}")
        method_id = binding["methodId"]
        if method_id not in expected_by_id:
            raise ValueError(f"unknown MethodId {method_id}")
        if method_id in seen:
            raise ValueError(f"duplicate MethodId {method_id}")
        seen.add(method_id)
        if not isinstance(binding["sensitive"], bool):
            raise ValueError(f"{method_id} sensitivity must be boolean")
        if "generated::MethodParameters" in binding["parameterType"]:
            raise ValueError(f"{method_id} normal public parameter type uses a generated JSON wrapper")
        if "generated::MethodResult" in binding["resultType"]:
            raise ValueError(f"{method_id} normal public result type uses a generated JSON wrapper")
        expected_binding = expected_by_id[method_id]
        for field in sorted(REQUIRED_FIELDS - {"methodId"}):
            if binding[field] != expected_binding[field]:
                raise ValueError(
                    f"{method_id} has wrong {field}: expected {expected_binding[field]!r}, got {binding[field]!r}"
                )
        counts[binding["category"]] += 1
        for header_field in ("parameterHeader", "resultHeader"):
            if not (ROOT / "src" / binding[header_field]).is_file():
                raise ValueError(f"{method_id} references missing public header {binding[header_field]}")

    missing_ids = set(expected_by_id) - seen
    if missing_ids:
        raise ValueError(f"missing MethodIds: {sorted(missing_ids)}")
    if counts != {"native": 7, "provider": 86, "reverse": 12}:
        raise ValueError(f"wrong binding category counts: {counts}")
    if sum(binding["facade"] == "Requests" for binding in bindings) != 12:
        raise ValueError("Requests must own exactly all 12 reverse bindings")
    if any(binding["category"] == "reverse" and binding["facade"] != "Requests" for binding in bindings):
        raise ValueError("every reverse binding must belong to Requests")
    if any(binding["facade"] == "Requests" and binding["category"] != "reverse" for binding in bindings):
        raise ValueError("Requests may own only reverse bindings")
    if len(bindings) != 105:
        raise ValueError(f"expected 105 bindings, got {len(bindings)}")
    return manifest, counts


def cpp_string(value: str) -> str:
    return json.dumps(value)


def render(bindings: list[dict[str, Any]], counts: dict[str, int]) -> str:
    headers = sorted(
        {binding["parameterHeader"] for binding in bindings}
        | {binding["resultHeader"] for binding in bindings}
        | {f"ai/openai/codex/frontend/client/{binding['facade']}.h" for binding in bindings}
    )
    facades = sorted({binding["facade"] for binding in bindings})
    lines = [
        "// Generated by tools/frontend/generate_cpp_frontend_client.py. Do not edit.",
        "#ifndef AI_OPENAI_CODEX_FRONTEND_CLIENT_GENERATEDBINDINGS_H",
        "#define AI_OPENAI_CODEX_FRONTEND_CLIENT_GENERATEDBINDINGS_H",
        "",
    ]
    for header in headers:
        lines.append(f'#include "{header}"')
    lines.extend(["", "#include <array>", "#include <cstddef>", "#include <string_view>", "#include <type_traits>", ""])
    lines.append(f"namespace {CLIENT_NAMESPACE.removeprefix('::')} {{")
    for facade in facades:
        lines.append(f"    class {facade};")
    lines.extend(["}", "", "// clang-format off", f"namespace {CLIENT_NAMESPACE.removeprefix('::')}::generated {{", ""])
    lines.extend(
        [
            "    enum class BindingCategory { Native, Provider, Reverse };",
            "",
            "    struct BindingMetadata {",
            "        frontend::generated::MethodId method;",
            "        std::string_view facade;",
            "        std::string_view operation;",
            "        BindingCategory category;",
            "        std::string_view parameterType;",
            "        std::string_view parameterHeader;",
            "        std::string_view resultType;",
            "        std::string_view resultHeader;",
            "        std::string_view parameterEncoder;",
            "        std::string_view resultDecoder;",
            "        bool sensitive;",
            "    };",
            "",
            "    template <frontend::generated::MethodId Method>",
            "    struct BindingTraits;",
            "",
            "    template <typename T>",
            "    struct IsGeneratedJsonWrapper : std::false_type {};",
            "    template <frontend::generated::MethodId Method>",
            "    struct IsGeneratedJsonWrapper<frontend::generated::MethodParameters<Method>> : std::true_type {};",
            "    template <frontend::generated::MethodId Method>",
            "    struct IsGeneratedJsonWrapper<frontend::generated::MethodResult<Method>> : std::true_type {};",
            "",
        ]
    )
    category_cpp = {"native": "Native", "provider": "Provider", "reverse": "Reverse"}
    for binding in bindings:
        method = binding["methodId"]
        facade_type = f"{CLIENT_NAMESPACE}::{binding['facade']}"
        handler_type = (
            f"{CLIENT_NAMESPACE}::DoneHandler"
            if binding["resultType"] == f"{TYPED_NAMESPACE}::Unit"
            else f"{CLIENT_NAMESPACE}::CompletionHandler<{binding['resultType']}>"
        )
        member_parameters = (
            handler_type
            if binding["parameterType"] == f"{TYPED_NAMESPACE}::Unit"
            else f"{binding['parameterType']}, {handler_type}"
        )
        member_type = f"{CLIENT_NAMESPACE}::Submission ({facade_type}::*)({member_parameters})"
        lines.extend(
            [
                f"    template <> struct BindingTraits<frontend::generated::MethodId::{method}> {{",
                f"        using Facade = {facade_type};",
                f"        using Parameter = {binding['parameterType']};",
                f"        using Result = {binding['resultType']};",
                f"        static constexpr frontend::generated::MethodId Method = frontend::generated::MethodId::{method};",
                f"        static constexpr BindingCategory Category = BindingCategory::{category_cpp[binding['category']]};",
                f"        static constexpr bool Sensitive = {'true' if binding['sensitive'] else 'false'};",
                "    };",
                f"    static_assert(!IsGeneratedJsonWrapper<typename BindingTraits<frontend::generated::MethodId::{method}>::Parameter>::value);",
                f"    static_assert(!IsGeneratedJsonWrapper<typename BindingTraits<frontend::generated::MethodId::{method}>::Result>::value);",
                f"    using BindingMember_{method} = {member_type};",
                f"    static_assert(std::is_same_v<decltype(static_cast<BindingMember_{method}>(&{facade_type}::{binding['operation']})), BindingMember_{method}>);",
            ]
        )
    lines.extend(["", f"    inline constexpr std::array<BindingMetadata, {len(bindings)}> AllBindings{{{{"])
    for binding in bindings:
        lines.append(
            "        {frontend::generated::MethodId::%s, %s, %s, BindingCategory::%s, %s, %s, %s, %s, %s, %s, %s},"
            % (
                binding["methodId"],
                cpp_string(binding["facade"]),
                cpp_string(binding["operation"]),
                category_cpp[binding["category"]],
                cpp_string(binding["parameterType"]),
                cpp_string(binding["parameterHeader"]),
                cpp_string(binding["resultType"]),
                cpp_string(binding["resultHeader"]),
                cpp_string(binding["parameterEncoder"]),
                cpp_string(binding["resultDecoder"]),
                "true" if binding["sensitive"] else "false",
            )
        )
    lines.extend(
        [
            "    }};",
            "",
            f"    inline constexpr std::size_t NativeBindingCount = {counts['native']};",
            f"    inline constexpr std::size_t ProviderBindingCount = {counts['provider']};",
            f"    inline constexpr std::size_t ReverseBindingCount = {counts['reverse']};",
            "    inline constexpr std::size_t RequestsBindingCount = 12;",
            "    static_assert(AllBindings.size() == 105);",
            "    static_assert(NativeBindingCount == 7);",
            "    static_assert(ProviderBindingCount == 86);",
            "    static_assert(ReverseBindingCount == 12);",
            "    static_assert(RequestsBindingCount == ReverseBindingCount);",
            "",
            "    [[nodiscard]] constexpr const BindingMetadata* bindingMetadata(frontend::generated::MethodId method) noexcept {",
            "        for (const BindingMetadata& binding : AllBindings) {",
            "            if (binding.method == method) {",
            "                return &binding;",
            "            }",
            "        }",
            "        return nullptr;",
            "    }",
            "",
            "    [[nodiscard]] constexpr bool bindingIsSensitive(frontend::generated::MethodId method) noexcept {",
            "        const BindingMetadata* binding = bindingMetadata(method);",
            "        return binding != nullptr && binding->sensitive;",
            "    }",
            "",
            f"}} // namespace {CLIENT_NAMESPACE.removeprefix('::')}::generated",
            "// clang-format on",
            "",
            "#endif // AI_OPENAI_CODEX_FRONTEND_CLIENT_GENERATEDBINDINGS_H",
            "",
        ]
    )
    return "\n".join(lines)


def public_handler_type(binding: dict[str, Any]) -> str:
    return (
        "DoneHandler"
        if binding["resultType"] == f"{TYPED_NAMESPACE}::Unit"
        else f"CompletionHandler<{binding['resultType']}>"
    )


def public_parameter_list(binding: dict[str, Any]) -> str:
    handler = public_handler_type(binding)
    if binding["parameterType"] == f"{TYPED_NAMESPACE}::Unit":
        return f"{handler} handler"
    return f"{binding['parameterType']} parameters, {handler} handler"


def render_facade(facade: str, bindings: list[dict[str, Any]]) -> str:
    """Render one pointer-sized, declaration-only, domain-typed facade."""
    guard = f"AI_OPENAI_CODEX_FRONTEND_CLIENT_{facade.upper()}_H"
    facade_bindings = [binding for binding in bindings if binding["facade"] == facade]
    headers = sorted(
        {
            "ai/openai/codex/frontend/client/Export.h",
            "ai/openai/codex/frontend/client/Results.h",
        }
        | {binding["parameterHeader"] for binding in facade_bindings}
        | {binding["resultHeader"] for binding in facade_bindings}
    )
    lines = [
        "// Generated by tools/frontend/generate_cpp_frontend_client.py. Do not edit.",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
    ]
    for header in headers:
        lines.append(f'#include "{header}"')
    lines.extend(
        [
            "",
            "// clang-format off",
            f"namespace {CLIENT_NAMESPACE.removeprefix('::')} {{",
            "    class Client;",
            "",
            f"    class AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_EXPORT {facade} {{",
            "    public:",
            f"        {facade}(const {facade}&) = delete;",
            f"        {facade}({facade}&&) = delete;",
            f"        {facade}& operator=(const {facade}&) = delete;",
            f"        {facade}& operator=({facade}&&) = delete;",
            "",
        ]
    )
    for binding in facade_bindings:
        lines.append(f"        [[nodiscard]] Submission {binding['operation']}({public_parameter_list(binding)});")
    lines.extend(
        [
            "",
            "    private:",
            "        friend class Client;",
            f"        explicit {facade}(Client& owner) noexcept",
            "            : client(&owner) {",
            "        }",
            "        Client* client;",
            "    };",
            f"}} // namespace {CLIENT_NAMESPACE.removeprefix('::')}",
            "// clang-format on",
            "",
            f"#endif // {guard}",
            "",
        ]
    )
    return "\n".join(lines)


def result_decoder_expression(binding: dict[str, Any]) -> str:
    method_id = binding["methodId"]
    result_type = binding["resultType"]
    decoder = binding["resultDecoder"]
    standard_prefix = f"{CODEX_DETAIL_NAMESPACE}::decodeClientOperationResultAs<{result_type}>@ClientRequestTarget::"
    if decoder.startswith(standard_prefix):
        target = decoder.removeprefix(standard_prefix)
        if target != method_id:
            raise ValueError(f"{method_id} result decoder targets {target}")
        return (
            f"decodeProviderResult<frontend::generated::MethodId::{method_id}, "
            f"::ai::openai::codex::detail::ClientRequestTarget::{target}, {result_type}>(result, error)"
        )
    if decoder.startswith(f"{CLIENT_DETAIL_NAMESPACE}::decode"):
        return (
            f"decodeBoundResult<frontend::generated::MethodId::{method_id}, {result_type}>("
            f"result, {decoder}, error)"
        )
    raise ValueError(f"{method_id} has unsupported generated result decoder {decoder}")


def render_facade_implementation(bindings: list[dict[str, Any]]) -> str:
    native_bindings = [binding for binding in bindings if binding["category"] == "native"]
    generated_bindings = [binding for binding in bindings if binding["category"] != "native"]
    if len(native_bindings) != 7:
        raise ValueError(f"expected 7 native binding codec guards, got {len(native_bindings)}")
    if len(generated_bindings) != 98:
        raise ValueError(f"expected 98 generated domain facade definitions, got {len(generated_bindings)}")

    encoder_headers = parameter_encoder_headers()
    headers = {
        "ai/openai/codex/detail/ClientOperationCodec.h",
        "ai/openai/codex/detail/ProtocolSurfaceRegistry.h",
        "ai/openai/codex/frontend/GeneratedProtocol.h",
        "ai/openai/codex/frontend/client/Client.h",
        "ai/openai/codex/frontend/client/detail/BoundOperation.h",
        "ai/openai/codex/frontend/client/detail/OperationCodecs.h",
    }
    for binding in generated_bindings:
        headers.add(f"ai/openai/codex/frontend/client/{binding['facade']}.h")
        encoder = binding["parameterEncoder"]
        if encoder.startswith(f"{CODEX_DETAIL_NAMESPACE}::"):
            encoder_name = encoder.removeprefix(f"{CODEX_DETAIL_NAMESPACE}::")
            try:
                headers.add(encoder_headers[encoder_name])
            except KeyError as error:
                raise ValueError(f"{binding['methodId']} has no declaration header for {encoder}") from error
        elif not encoder.startswith(f"{CLIENT_DETAIL_NAMESPACE}::"):
            raise ValueError(f"{binding['methodId']} has unsupported generated parameter encoder {encoder}")

    lines = [
        "// Generated by tools/frontend/generate_cpp_frontend_client.py. Do not edit.",
    ]
    for header in sorted(headers):
        lines.append(f'#include "{header}"')
    lines.extend(
        [
            "",
            "#include <optional>",
            "#include <string>",
            "#include <type_traits>",
            "#include <utility>",
            "#include <variant>",
            "",
            "// clang-format off",
            f"namespace {CLIENT_NAMESPACE.removeprefix('::')} {{",
            "    namespace {",
        ]
    )
    for binding in native_bindings:
        method_id = binding["methodId"]
        lines.extend(
            [
                f"        [[maybe_unused]] constexpr auto NativeParameterEncoder_{method_id} = &{binding['parameterEncoder']};",
                f"        [[maybe_unused]] constexpr auto NativeResultDecoder_{method_id} = &{binding['resultDecoder']};",
                f"        static_assert(std::is_pointer_v<std::remove_cv_t<decltype(NativeParameterEncoder_{method_id})>>);",
                f"        static_assert(std::is_pointer_v<std::remove_cv_t<decltype(NativeResultDecoder_{method_id})>>);",
            ]
        )
    lines.extend(
        [
            "",
            "        [[nodiscard]] Submission parameterEncodingFailure() noexcept {",
            "            Error error;",
            "            error.origin = ErrorOrigin::Client;",
            "            error.clientCode = ClientErrorCode::SerializationFailed;",
            '            error.message = "typed frontend parameters could not be encoded";',
            "            return Submission{std::nullopt, std::move(error)};",
            "        }",
            "",
            "        template <typename Parameters, typename Encoder>",
            "        [[nodiscard]] std::optional<frontend::Json> encodeParameters(const Parameters& parameters, Encoder encoder) noexcept {",
            "            try {",
            "                std::string error;",
            "                return encoder(parameters, error);",
            "            } catch (...) {",
            "                return std::nullopt;",
            "            }",
            "        }",
            "",
            "        template <frontend::generated::MethodId Method, typename Result, typename Decoder>",
            "        [[nodiscard]] std::optional<Result> decodeBoundResult(",
            "            const frontend::generated::CompleteCommandResult& result, Decoder decoder, std::string& error) noexcept {",
            "            try {",
            "                const auto* generated = std::get_if<frontend::generated::MethodResult<Method>>(&result);",
            "                if (generated == nullptr) {",
            '                    error = "frontend result did not match the submitted generated MethodId";',
            "                    return std::nullopt;",
            "                }",
            "                return decoder(generated->value, error);",
            "            } catch (...) {",
            '                error = "frontend result decoder failed";',
            "                return std::nullopt;",
            "            }",
            "        }",
            "",
            "        template <frontend::generated::MethodId Method, ::ai::openai::codex::detail::ClientRequestTarget Target, typename Result>",
            "        [[nodiscard]] std::optional<Result> decodeProviderResult(",
            "            const frontend::generated::CompleteCommandResult& result, std::string& error) noexcept {",
            "            return decodeBoundResult<Method, Result>(",
            "                result,",
            "                [](const frontend::Json& value, std::string& decoderError) {",
            "                    return ::ai::openai::codex::detail::decodeClientOperationResultAs<Result>(",
            "                        Target, value, std::nullopt, decoderError);",
            "                },",
            "                error);",
            "        }",
            "    } // namespace",
            "",
        ]
    )

    for binding in generated_bindings:
        method_id = binding["methodId"]
        parameter_expression = (
            f"{TYPED_NAMESPACE}::Unit{{}}"
            if binding["parameterType"] == f"{TYPED_NAMESPACE}::Unit"
            else "parameters"
        )
        decoder_expression = result_decoder_expression(binding)
        encoder_expression = (
            f"[](const {binding['parameterType']}& value, std::string& error) {{ "
            f"return {binding['parameterEncoder']}(value, error); }}"
        )
        submission_lines = (
            [
                "        return client->submitReverseBound(",
                "            parameters.pendingRequestId,",
            ]
            if binding["category"] == "reverse"
            else ["        return client->submitBound("]
        )
        lines.extend(
            [
                f"    Submission {binding['facade']}::{binding['operation']}({public_parameter_list(binding)}) {{",
                f"        std::optional<frontend::Json> encoded = encodeParameters({parameter_expression}, {encoder_expression});",
                "        if (!encoded) {",
                "            return parameterEncodingFailure();",
                "        }",
                f"        using Parameters = frontend::generated::MethodParameters<frontend::generated::MethodId::{method_id}>;",
                f"        using Result = {binding['resultType']};",
            ]
            + submission_lines
            + [
                "            frontend::generated::CompleteCommandParameters{Parameters{std::move(*encoded)}},",
                "            detail::bindCompletion<Result>(",
                "                std::move(handler),",
                "                [](const frontend::generated::CompleteCommandResult& result, std::string& error) noexcept {",
                f"                    return {decoder_expression};",
                "                }));",
                "    }",
                "",
            ]
        )

    lines.extend(
        [
            f"}} // namespace {CLIENT_NAMESPACE.removeprefix('::')}",
            "// clang-format on",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bootstrap", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    if args.bootstrap:
        _, bootstrap_bindings = expected_bindings()
        BINDINGS.write_text(
            json.dumps({"formatVersion": FORMAT_VERSION, "bindings": bootstrap_bindings}, indent=2) + "\n",
            encoding="utf-8",
        )

    authority = load_json(BINDINGS)
    if authority.get("formatVersion") != FORMAT_VERSION:
        raise ValueError(f"expected C++ client binding format version {FORMAT_VERSION}")
    bindings = authority["bindings"]
    _, counts = validate(bindings)
    rendered = render(bindings, counts)

    generated_facades = sorted(
        {binding["facade"] for binding in bindings}
        - {"Controller", "Provider", "Synchronization"}
    )
    facade_outputs = {
        CLIENT_OUTPUT / f"{facade}.h": render_facade(facade, bindings)
        for facade in generated_facades
    }
    facade_implementation = render_facade_implementation(bindings)

    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != rendered:
            print(f"stale generated C++ frontend client bindings: {OUTPUT}", file=sys.stderr)
            return 1
        for path, contents in facade_outputs.items():
            if not path.exists() or path.read_text(encoding="utf-8") != contents:
                print(f"stale generated C++ frontend client facade: {path}", file=sys.stderr)
                return 1
        if not FACADE_IMPLEMENTATION.exists() or FACADE_IMPLEMENTATION.read_text(encoding="utf-8") != facade_implementation:
            print(f"stale generated C++ frontend client facade definitions: {FACADE_IMPLEMENTATION}", file=sys.stderr)
            return 1
        return 0

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(rendered, encoding="utf-8")
    for path, contents in facade_outputs.items():
        path.write_text(contents, encoding="utf-8")
    FACADE_IMPLEMENTATION.write_text(facade_implementation, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
