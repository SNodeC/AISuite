#!/usr/bin/env python3

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import subprocess
import sys
from pathlib import Path


def load_module(path: Path):
    spec = importlib.util.spec_from_file_location("aisuite_frontend_protocol_generator", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def expect_failure(generator, source: dict, needle: str) -> None:
    try:
        generator.generate_manifest(source)
    except generator.GenerationError as error:
        if needle not in str(error):
            raise AssertionError(f"expected {needle!r}, got {error}") from error
    else:
        raise AssertionError(f"mutation unexpectedly passed: {needle}")


def expect_schema_audit_failure(
    generator, schema: dict, manifest: dict, needle: str
) -> None:
    try:
        generator.audit_runtime_schema_profile(schema, manifest)
    except generator.GenerationError as error:
        if needle not in str(error):
            raise AssertionError(f"expected {needle!r}, got {error}") from error
    else:
        raise AssertionError(f"schema-audit mutation unexpectedly passed: {needle}")


def expect_schema_generation_failure(
    generator,
    template: dict,
    manifest: dict,
    source: dict,
    needle: str,
) -> None:
    try:
        generator.generate_schema(template, manifest, source)
    except generator.GenerationError as error:
        if needle not in str(error):
            raise AssertionError(f"expected {needle!r}, got {error}") from error
    else:
        raise AssertionError(f"schema generation mutation unexpectedly passed: {needle}")


def validate_manifest_contract(generator, source: dict, manifest: dict) -> None:
    rows = generator.validate_source(source)
    methods = manifest.get("methods")
    if not isinstance(methods, list) or len(methods) != 105:
        raise generator.GenerationError("manifest method catalog must contain 105 methods")
    method_names = [row.get("method") for row in methods]
    if len(set(method_names)) != 105:
        raise generator.GenerationError("manifest method strings must be unique")
    if method_names[:15] != list(generator.EXISTING_METHODS):
        raise generator.GenerationError("the original 15 method order/spellings changed")

    expected_native = {row["method"] for row in generator.NATIVE_METHODS}
    actual_native = {
        row.get("method") for row in methods if row.get("frontendNative") is True
    }
    if actual_native != expected_native:
        raise generator.GenerationError("frontend-native method set changed")
    if any(
        row.get("registryKeys")
        for row in methods
        if row.get("method") in expected_native
    ):
        raise generator.GenerationError(
            "frontend-native methods cannot reference registry rows"
        )

    for row in methods:
        method_id = row.get("id")
        if (
            not isinstance(method_id, str)
            or row.get("parameterSchema")
            != generator.schema_name(method_id, "Params")
            or row.get("resultSchema") != generator.schema_name(method_id, "Result")
        ):
            raise generator.GenerationError(
                "every manifest method must have exact schema references"
            )

    provider_source = {
        entry["mappings"][0]: entry
        for entry in rows
        if entry["registryKey"]["category"] == "client_request"
        and entry["stability"] == "stable"
        and len(entry["mappings"]) == 1
    }
    provider_methods = [
        row for row in methods if row.get("category") == "provider_operation"
    ]
    login_source = provider_source.get("account.login.start")
    if login_source is None or login_source.get("parameterShape") != {
        "type": "LoginAccountParams",
        "requiredFields": ["type"],
        "fields": [
            "accessToken",
            "apiKey",
            "appBrand",
            "chatgptAccountId",
            "chatgptPlanType",
            "codexStreamlinedLogin",
            "type",
            "useHostedLoginSuccessPage",
        ],
        "propertyPaths": [
            "accessToken",
            "apiKey",
            "appBrand",
            "chatgptAccountId",
            "chatgptPlanType",
            "codexStreamlinedLogin",
            "type",
            "useHostedLoginSuccessPage",
        ],
    }:
        raise generator.GenerationError(
            "tagged account login union fields changed or were demoted to extensions"
        )
    for row in provider_methods:
        source_row = provider_source.get(row["method"])
        if source_row is None:
            raise generator.GenerationError("manifest provider method lacks registry source")
        if row.get("requiredScopes") != source_row.get("requiredScopes"):
            raise generator.GenerationError(
                "manifest method scopes differ from registry source"
            )
        if row.get("controllerRequired") != source_row.get("controllerRequired"):
            raise generator.GenerationError(
                "manifest controller requirement differs from registry source"
            )
        if row.get("resultType") != source_row["operationContract"].get("resultType"):
            raise generator.GenerationError(
                "manifest result type differs from registry source"
            )
        if row.get("providerReadyRequired") is not True:
            raise generator.GenerationError(
                "provider operation lost its provider-ready requirement"
            )

    runtime_methods = tuple(
        row["method"] for row in methods if row.get("currentlyImplemented") is True
    )
    if runtime_methods != tuple(method_names):
        raise generator.GenerationError("A1.7b runtime must implement all 105 methods")
    legacy_methods = tuple(
        row["method"] for row in methods if row.get("legacyCompatibilityMethod") is True
    )
    if legacy_methods != generator.EXISTING_METHODS:
        raise generator.GenerationError("legacy compatibility method set changed")
    if any(
        row.get("providerReadyRequired") is not (row["category"] in {"provider_operation", "reverse_response"})
        for row in methods
    ):
        raise generator.GenerationError("provider-ready metadata differs from method category")
    generator.validate_runtime_authorization(
        rows, methods, source["defaultRemoteScopes"]
    )

    capabilities = manifest.get("capabilities")
    if not isinstance(capabilities, list):
        raise generator.GenerationError("manifest capability catalog is missing")
    capability_by_key = {row.get("key"): row for row in capabilities}
    if set(capability_by_key) != set(generator.CAPABILITIES):
        raise generator.GenerationError("manifest capability catalog changed")
    implemented_capabilities = {
        key
        for key, row in capability_by_key.items()
        if row.get("implementedByCurrentRuntime") is True
    }
    if any(
        capability_by_key[key].get("implementedByCurrentRuntime") is True
        for key in generator.FUTURE_CAPABILITIES
    ):
        raise generator.GenerationError(
            "A1.7b claims a future capability as implemented"
        )
    if implemented_capabilities != generator.IMPLEMENTED_MECHANISM_CAPABILITIES:
        raise generator.GenerationError(
            "A1.7b mechanism capability set changed"
        )

    pending_requests = manifest.get("pendingRequestMappings")
    expected_pending_kinds = dict(generator.PENDING_REQUEST_KINDS)
    if not isinstance(pending_requests, list) or len(pending_requests) != 10:
        raise generator.GenerationError(
            "manifest pending-request projection must contain ten entries"
        )
    if (
        {row.get("providerMethod") for row in pending_requests}
        != set(expected_pending_kinds)
        or {row.get("kind") for row in pending_requests}
        != set(expected_pending_kinds.values())
    ):
        raise generator.GenerationError(
            "manifest pending-request projection does not bijectively cover the stable request kinds"
        )
    stable_requests = {
        generator.registry_key(row): row
        for row in rows
        if row["registryKey"]["category"] == "server_request"
        and row["stability"] == "stable"
    }
    reverse_methods = {
        row["method"]: row
        for row in methods
        if row["category"] == "reverse_response"
    }
    for mapping in pending_requests:
        source_row = stable_requests.get(mapping.get("registryKey"))
        response_methods = mapping.get("responseMethods")
        if (
            source_row is None
            or mapping.get("kind")
            != expected_pending_kinds[source_row["registryKey"]["name"]]
            or mapping.get("finalExposure") != "DedicatedPendingRequestContract"
            or mapping.get("securityDecision")
            != "ScopeProjectedStateEventApproved"
            or mapping.get("expandedEvent") != "pendingRequests.updated"
            or response_methods != source_row["mappings"]
            or any(method not in reverse_methods for method in response_methods)
            or mapping.get("presentationRequiredScopes") != ["observe"]
            or mapping.get("controllerRequiredForPresentation") is not False
            or mapping.get("responseRequiredScopes")
            != ["control", "sensitive_response"]
            or mapping.get("controllerRequiredForResponse") is not True
            or mapping.get("redactionClass") != "safe_pending_request"
            or mapping.get("capability") != "dedicated_pending_requests"
            or mapping.get("duplicateSuppression")
            != "exactly_one_compatibility_representation_per_connection"
        ):
            raise generator.GenerationError(
                "manifest pending-request projection differs from the reviewed registry response contract"
            )


def expect_manifest_failure(
    generator, source: dict, manifest: dict, needle: str
) -> None:
    try:
        validate_manifest_contract(generator, source, manifest)
    except generator.GenerationError as error:
        if needle not in str(error):
            raise AssertionError(f"expected {needle!r}, got {error}") from error
    else:
        raise AssertionError(f"manifest mutation unexpectedly passed: {needle}")


def expect_authorization_failure(
    generator, source: dict, methods: list[dict], needle: str
) -> None:
    try:
        generator.validate_runtime_authorization(
            source["entries"], methods, source["defaultRemoteScopes"]
        )
    except generator.GenerationError as error:
        if needle not in str(error):
            raise AssertionError(f"expected {needle!r}, got {error}") from error
    else:
        raise AssertionError(f"authorization mutation unexpectedly passed: {needle}")


def validate_source_controller_contract(generator, source: dict) -> None:
    for row in generator.validate_source(source):
        decision = row.get("securityDecision")
        controller = row.get("controllerRequired")
        if decision in {"ControllerRequiredApproved", "PrivilegedScopedApproved"}:
            expected = True
        elif decision in {
            "ObserverReadApproved",
            "ParameterSensitiveApproved",
            "ScopeProjectedStateEventApproved",
            "NotApplicable",
        }:
            expected = False
        elif decision == "ConditionalExplicitEnablementApproved":
            expected = "control" in row.get("requiredScopes", ())
        else:
            continue
        if controller is not expected:
            raise generator.GenerationError(
                "controller requirement does not match security decision"
            )


def expect_source_controller_failure(generator, source: dict) -> None:
    try:
        validate_source_controller_contract(generator, source)
    except generator.GenerationError as error:
        if "controller requirement" not in str(error):
            raise AssertionError(f"unexpected controller mutation failure: {error}") from error
    else:
        raise AssertionError("controller requirement mutation unexpectedly passed")


def validate_exact_dispatch(generator, header: str) -> None:
    if (
        header.count("method.method == value") != 3
        or "starts_with(method.method)" in header
    ):
        raise generator.GenerationError("generated method dispatch must use exact equality")


def expect_dispatch_failure(generator, header: str) -> None:
    try:
        validate_exact_dispatch(generator, header)
    except generator.GenerationError as error:
        if "exact equality" not in str(error):
            raise AssertionError(f"unexpected dispatch mutation failure: {error}") from error
    else:
        raise AssertionError("prefix-dispatch mutation unexpectedly passed")


def validate_additive_schema_contract(
    template: dict, generated: dict, manifest: dict
) -> None:
    definitions = generated["$defs"]
    template_definitions = template["$defs"]
    methods = manifest["methods"]
    method_names = [row["method"] for row in methods]
    branches = definitions["Command"]["allOf"][1]["oneOf"]
    branch_names = [branch["properties"]["method"]["const"] for branch in branches]

    if branch_names != method_names or len(set(branch_names)) != 105:
        raise AssertionError("JSON Schema command branches do not match the 105 methods")
    template_branches = template_definitions["Command"]["allOf"][1]["oneOf"]
    if branches[:15] != template_branches:
        raise AssertionError("additive generation changed an original method schema")

    for row in methods:
        parameter_name = row["parameterSchema"].rsplit("/", 1)[-1]
        result_name = row["resultSchema"].rsplit("/", 1)[-1]
        if parameter_name not in definitions:
            raise AssertionError(f"missing parameter schema for {row['method']}")
        if result_name not in definitions:
            raise AssertionError(f"missing result schema for {row['method']}")

    mutated_legacy_definitions = {"Command", "ErrorCode", "Hello", "Welcome"}
    for name, definition in template_definitions.items():
        if name not in mutated_legacy_definitions and definitions.get(name) != definition:
            raise AssertionError(f"additive generation changed legacy schema $defs/{name}")

    for row, branch in zip(methods[15:], branches[15:], strict=True):
        parameter_ref = branch["properties"]["params"]["$ref"]
        if parameter_ref != row["parameterSchema"]:
            raise AssertionError(f"parameter schema mapping changed for {row['method']}")
    contract_metadata = generated.get("x-aisuite-frontend-contract")
    expected_metadata = {
        "methods": 105,
        "existingMethods": 15,
        "additiveMethods": 90,
        "runtimeAvailableMethods": 90,
        "reviewedIdentities": 234,
        "notificationMappings": 68,
        "threadItemMappings": 18,
        "pendingRequestMappings": 10,
    }
    if contract_metadata != expected_metadata:
        raise AssertionError(f"JSON Schema contract metadata changed: {contract_metadata}")

    capability_names = [row["key"] for row in manifest["capabilities"]]
    if definitions["FrontendCapability"] != {
        "type": "string",
        "enum": capability_names,
    }:
        raise AssertionError("frontend capability discovery schema changed")
    if definitions["FrontendMethod"] != {"type": "string", "enum": method_names}:
        raise AssertionError("frontend method discovery schema changed")

    hello = definitions["Hello"]["allOf"][1]
    if "capabilities" in hello["required"] or hello["properties"]["capabilities"] != {
        "type": "array",
        "items": {"$ref": "#/$defs/FrontendCapability"},
        "uniqueItems": True,
    }:
        raise AssertionError("hello capability negotiation is no longer additive")
    if "authentication" in hello["required"] or hello["properties"].get(
        "authentication"
    ) != {"$ref": "#/$defs/HelloAuthentication"}:
        raise AssertionError("hello bearer authentication is no longer additive")
    if definitions.get("HelloAuthentication") != {
        "type": "object",
        "required": ["scheme", "token"],
        "properties": {
            "scheme": {"const": "bearer"},
            "token": {"type": "string", "minLength": 1, "maxLength": 65536},
        },
        "additionalProperties": False,
    }:
        raise AssertionError("hello bearer authentication schema changed")
    if manifest.get("helloAuthentication") != {
        "optional": True,
        "credentialLocation": "hello.authentication",
        "schemes": ["bearer"],
        "secretFields": ["token"],
        "legacyHelloWithoutCredentialRemainsValid": True,
    }:
        raise AssertionError("hello authentication manifest metadata changed")

    welcome = definitions["Welcome"]["allOf"][1]
    discovery_fields = {
        "capabilities": {"$ref": "#/$defs/CapabilityAdvertisement"},
        "availableMethods": {
            "type": "array",
            "items": {"$ref": "#/$defs/FrontendMethod"},
            "uniqueItems": True,
        },
        "permittedMethods": {
            "type": "array",
            "items": {"$ref": "#/$defs/FrontendMethod"},
            "uniqueItems": True,
        },
        "serverVersion": {"type": "string", "minLength": 1},
    }
    if any(field in welcome["required"] for field in discovery_fields):
        raise AssertionError("welcome discovery fields became required")
    if any(welcome["properties"].get(name) != value for name, value in discovery_fields.items()):
        raise AssertionError("welcome discovery contract changed")

    advertisement = definitions["CapabilityAdvertisement"]
    if advertisement["required"] != ["defined", "implemented", "permitted"]:
        raise AssertionError("capability advertisement completeness changed")
    if set(advertisement["properties"]) != {"defined", "implemented", "permitted"}:
        raise AssertionError("capability advertisement fields changed")

    legacy_error_codes = template_definitions["ErrorCode"]["enum"]
    error_codes = definitions["ErrorCode"]["enum"]
    additive_error_codes = [
        "authentication_required",
        "authentication_failed",
        "origin_rejected",
        "transport_security_required",
        "rate_limited",
    ]
    if (
        error_codes[: len(legacy_error_codes)] != legacy_error_codes
        or error_codes[len(legacy_error_codes) :] != additive_error_codes
        or len(set(error_codes)) != len(error_codes)
    ):
        raise AssertionError("additive frontend error-code contract changed")

    expanded_contract_counts = {
        "threadItems": len(definitions["ExpandedThreadItem"]["oneOf"]),
        "pendingRequests": len(definitions["PendingRequestKind"]["enum"]),
        "events": len(definitions["ExpandedEventType"]["enum"]),
    }
    expected_expanded_counts = {
        "threadItems": 18,
        "pendingRequests": 10,
        "events": len(manifest["eventFamilies"]),
    }
    if expanded_contract_counts != expected_expanded_counts:
        raise AssertionError(
            f"expanded snapshot/event schema counts changed: {expanded_contract_counts}"
        )
    expanded_state = definitions["ExpandedBackendSnapshotState"]
    thread_list = definitions.get("ExpandedThreadListState", {})
    if (
        "threadList" not in expanded_state.get("required", [])
        or expanded_state.get("properties", {}).get("threadList")
        != {"$ref": "#/$defs/ExpandedThreadListState"}
        or thread_list.get("required")
        != ["hasLoadedPage", "complete", "pagesLoaded"]
        or "stamp" not in thread_list.get("properties", {})
    ):
        raise AssertionError("expanded snapshots must carry the authoritative typed thread-list projection")
    thread_list_events = [
        branch
        for branch in definitions["ExpandedFrontendEvent"]["oneOf"]
        if branch["properties"]["type"].get("const") == "threadList.updated"
    ]
    if (
        len(thread_list_events) != 1
        or thread_list_events[0]["properties"]["data"].get("required")
        != ["threadList"]
    ):
        raise AssertionError("threadList.updated must have one exact threadList wrapper")
    if definitions["PendingRequestKind"]["enum"] != [
        mapping["kind"] for mapping in manifest["pendingRequestMappings"]
    ]:
        raise AssertionError(
            "expanded pending-request schema discriminators differ from generated projection metadata"
        )
    pending_question = definitions.get("ExpandedPendingRequestQuestion", {})
    if (
        pending_question.get("required")
        != ["id", "header", "prompt", "allowsFreeText", "isSecret", "options"]
        or "secret" in pending_question.get("properties", {})
        or pending_question.get("properties", {}).get("isSecret")
        != {"type": "boolean"}
        or pending_question.get("properties", {})
        .get("options", {})
        .get("maxItems")
        != 64
    ):
        raise AssertionError(
            "dedicated user-input pending-request question schema changed"
        )
    for field in ("id", "header", "prompt"):
        if pending_question["properties"][field] != {
            "type": "string",
            "maxLength": 16384,
        }:
            raise AssertionError(
                "user-input question strings must preserve provider empty-string semantics while remaining bounded"
            )
    pending_base = definitions.get("ExpandedPendingRequestBase", {})
    if (
        pending_base.get("properties", {}).get("questions", {}).get("maxItems")
        != 64
        or pending_base.get("properties", {}).get("autoResolutionMs")
        != {"$ref": "#/$defs/UInt64"}
    ):
        raise AssertionError(
            "dedicated user-input pending-request bounds changed"
        )
    pending_branches = definitions.get("ExpandedPendingRequest", {}).get(
        "oneOf", []
    )
    user_input_branches = [
        branch
        for branch in pending_branches
        if branch.get("allOf", [{}, {}])[1]
        .get("properties", {})
        .get("kind", {})
        .get("const")
        == "user_input"
    ]
    if (
        len(user_input_branches) != 1
        or user_input_branches[0]["allOf"][1].get("required")
        != ["questions"]
        or any(
            "not" not in branch["allOf"][1]
            for branch in pending_branches
            if branch is not user_input_branches[0]
        )
    ):
        raise AssertionError(
            "only user_input may carry the required dedicated question contract"
        )
    for name in (
        "ExpandedPendingRequest",
        "ExpandedFrontendEvent",
        "ExpandedBackendSnapshotState",
        "ExpandedSnapshot",
    ):
        if name not in definitions:
            raise AssertionError(f"missing additive schema definition {name}")

    def assert_safe_objects(value: object, context: str) -> None:
        if isinstance(value, list):
            for index, child in enumerate(value):
                assert_safe_objects(child, f"{context}/{index}")
            return
        if not isinstance(value, dict):
            return
        schema_type = value.get("type")
        schema_types = (
            {schema_type}
            if isinstance(schema_type, str)
            else set(schema_type or ())
        )
        if "object" in schema_types or "properties" in value or "required" in value:
            if value.get("propertyNames") != definitions["SafeDetailObject"]["propertyNames"]:
                raise AssertionError(f"safe schema object lacks credential-name rejection at {context}")
            if value.get("additionalProperties") is True:
                raise AssertionError(f"safe schema object has unbounded unknown fields at {context}")
        for name, child in value.items():
            assert_safe_objects(child, f"{context}/{name}")

    for row in methods:
        result_name = row["resultSchema"].rsplit("/", 1)[-1]
        assert_safe_objects(definitions[result_name], f"$defs/{result_name}")
    expanded_names = list(definitions)
    expanded_names = expanded_names[expanded_names.index("StateFreshness") :]
    for name in expanded_names:
        assert_safe_objects(definitions[name], f"$defs/{name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app-tool", type=Path, required=True)
    parser.add_argument("--app-manifest", type=Path, required=True)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--schema-template", type=Path, required=True)
    parser.add_argument("--schema", type=Path, required=True)
    parser.add_argument("--schema-data", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--review-identities", type=Path, required=True)
    args = parser.parse_args()

    generator = load_module(args.generator)
    source = json.loads(args.source.read_text(encoding="utf-8"))
    literal_review_identities = json.loads(
        args.review_identities.read_text(encoding="utf-8")
    )
    unresolved_prior = {
        "Unresolved",
        "ExistingOperationSubsetExpansionUnresolved",
        "ExistingGenericContractDedicatedUnresolved",
    }
    actual_review_sets = {"unresolvedBaseline": [], "compatibilityReview": []}
    for row in source["entries"]:
        key = generator.registry_key(row)
        bucket = (
            "unresolvedBaseline"
            if row["priorCompatibilitySecurity"] in unresolved_prior
            else "compatibilityReview"
        )
        actual_review_sets[bucket].append(key)
    actual_review_sets = {
        name: sorted(identities)
        for name, identities in actual_review_sets.items()
    }
    if literal_review_identities != actual_review_sets:
        raise AssertionError(
            "the independent literal 148/86 frontend review identity sets changed"
        )
    if (
        len(literal_review_identities["unresolvedBaseline"]) != 148
        or len(literal_review_identities["compatibilityReview"]) != 86
        or set(literal_review_identities["unresolvedBaseline"])
        & set(literal_review_identities["compatibilityReview"])
    ):
        raise AssertionError("literal frontend review sets must be disjoint 148 + 86")
    manifest = generator.generate_manifest(source)
    committed = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest != committed:
        raise AssertionError("committed frontend manifest is stale")
    generated_header = generator.generate_header(manifest)
    if generated_header != args.header.read_text(encoding="utf-8"):
        raise AssertionError("committed GeneratedProtocol.h is stale")
    validate_exact_dispatch(generator, generated_header)
    schema_template = json.loads(args.schema_template.read_text(encoding="utf-8"))
    generated_schema = generator.generate_schema(schema_template, manifest, source)
    command_exec = generated_schema["$defs"]["CommandExecParams"]["properties"][
        "command"
    ]
    if command_exec.get("minItems") != 1:
        raise AssertionError(
            "command.exec must enforce its documented non-empty argv invariant"
        )
    if generated_schema != json.loads(args.schema.read_text(encoding="utf-8")):
        raise AssertionError("committed frontend JSON Schema is stale")
    if generator.generate_schema_data(generated_schema) != args.schema_data.read_text(
        encoding="utf-8"
    ):
        raise AssertionError("committed GeneratedProtocolSchema.inc is stale")
    generated_fixtures = generator.generate_golden_fixtures(
        generated_schema, manifest
    )
    command_exec_fixture = next(
        row
        for row in generated_fixtures["methods"]
        if row["method"] == "command.exec"
    )
    if not command_exec_fixture["minimalParams"]["command"]:
        raise AssertionError(
            "command.exec generated fixtures must exercise a non-empty argv"
        )
    if generated_fixtures != json.loads(args.fixtures.read_text(encoding="utf-8")):
        raise AssertionError("committed frontend golden fixtures are stale")
    if (
        generated_fixtures["counts"] != {"methods": 105, "expandedEvents": 26}
        or len(generated_fixtures["methods"]) != 105
        or len(generated_fixtures["expandedEvents"]) != 26
    ):
        raise AssertionError("frontend golden fixture coverage changed")
    validate_additive_schema_contract(schema_template, generated_schema, manifest)
    runtime_audit = generator.audit_runtime_schema_profile(
        generated_schema, manifest
    )
    expected_supported_assertions = {
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
    if generator.RUNTIME_ASSERTION_KEYWORDS != expected_supported_assertions:
        raise AssertionError("runtime assertion-keyword support changed")
    expected_used_assertions = expected_supported_assertions - {
        "else",
        "minProperties",
    }
    if set(runtime_audit.assertion_keywords) != expected_used_assertions:
        raise AssertionError(
            f"runtime-reachable assertion keywords changed: {runtime_audit.assertion_keywords}"
        )
    if runtime_audit.structural_keywords != ("$defs",):
        raise AssertionError("runtime structural-keyword profile changed")
    if runtime_audit.annotation_keywords != (
        "$id",
        "$schema",
        "default",
        "description",
        "title",
        "x-aisuite-frontend-contract",
        "x-aisuite-redactionClass",
    ):
        raise AssertionError(
            f"runtime annotation-keyword profile changed: {runtime_audit.annotation_keywords}"
        )
    if set(runtime_audit.numeric_formats) != {
        "int32",
        "int64",
        "uint16",
        "uint32",
        "uint",
        "uint64",
    }:
        raise AssertionError(
            f"runtime numeric-format profile changed: {runtime_audit.numeric_formats}"
        )
    if len(runtime_audit.patterns) != 3:
        raise AssertionError(
            f"runtime pattern count changed: {len(runtime_audit.patterns)}"
        )
    if (
        runtime_audit.unique_item_schema_count != 9
        or runtime_audit.maximum_unique_item_cardinality != 105
        or runtime_audit.maximum_unique_item_comparisons != 5_460
    ):
        raise AssertionError(
            "runtime uniqueItems profile must remain nine bounded schemas, "
            "maximum cardinality 105 and maximum pair count 5,460"
        )

    subprocess.run(
        [
            sys.executable,
            str(args.app_tool),
            "frontend-registry",
            "--manifest",
            str(args.app_manifest),
            "--registry",
            str(args.registry),
            "--output",
            str(args.source),
            "--check",
        ],
        check=True,
    )
    subprocess.run(
        [
            sys.executable,
            str(args.generator),
            "--source",
            str(args.source),
            "--manifest",
            str(args.manifest),
            "--header",
            str(args.header),
            "--schema-template",
            str(args.schema_template),
            "--schema",
            str(args.schema),
            "--schema-data",
            str(args.schema_data),
            "--fixtures",
            str(args.fixtures),
            "--check",
        ],
        check=True,
    )

    counts = manifest["counts"]
    expected_counts = {
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
        "expandedEventFamilies": 26,
    }
    if counts != expected_counts:
        raise AssertionError(f"method/review denominator changed: {counts}")
    command_branches = generated_schema["$defs"]["Command"]["allOf"][1]["oneOf"]
    if len(command_branches) != 105:
        raise AssertionError("JSON Schema does not define all 105 command methods")
    if generated_schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
        raise AssertionError("JSON Schema draft changed")
    if generated_schema["x-aisuite-frontend-contract"]["runtimeAvailableMethods"] != 90:
        raise AssertionError("JSON Schema default runtime availability changed")

    methods = manifest["methods"]
    by_name = {row["method"]: row for row in methods}
    if len(by_name) != 105:
        raise AssertionError("method strings are not unique")
    native = {row["method"] for row in methods if row["frontendNative"]}
    expected_native = {
        "controller.acquire",
        "controller.release",
        "snapshot.get",
        "events.replay",
        "provider.start",
        "provider.stop",
        "provider.restart",
    }
    if native != expected_native:
        raise AssertionError(f"frontend-native set changed: {native}")
    if by_name["thread.injectItems"]["providerMethod"] != "thread/inject_items":
        raise AssertionError("thread/inject_items mapping changed")
    if by_name["mcpServer.oauth.login"]["providerMethod"] != "mcpServer/oauth/login":
        raise AssertionError("MCP OAuth method spelling changed")
    if sum(row["category"] == "provider_operation" for row in methods) != 86:
        raise AssertionError("provider mapping is not 86/86")
    if sum(row["category"] == "reverse_response" for row in methods) != 12:
        raise AssertionError("reverse mapping is not 12/12")
    if sum(row["currentlyImplemented"] for row in methods) != 105:
        raise AssertionError("A1.7b does not implement all 105 methods")
    if tuple(row["method"] for row in methods if row["legacyCompatibilityMethod"]) != generator.EXISTING_METHODS:
        raise AssertionError("the 15-method legacy compatibility set changed")
    authorization = generator.validate_runtime_authorization(
        source["entries"], methods, source["defaultRemoteScopes"]
    )
    expected_authorization = {
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
    if authorization != expected_authorization or manifest.get("authorization") != expected_authorization:
        raise AssertionError("A1.7b authorization totals changed")

    conditional = {row["method"] for row in methods if row["exposure"] == "ConditionallyExposedFrontendMethod"}
    expected_conditional = {
        "fs.getMetadata",
        "fs.readDirectory",
        "fs.readFile",
        "fuzzyFileSearch",
        "fs.watch",
        "fs.unwatch",
        "fs.copy",
        "fs.createDirectory",
        "fs.remove",
        "fs.writeFile",
        "command.exec",
        "command.exec.resize",
        "command.exec.terminate",
        "command.exec.write",
        "thread.shellCommand",
    }
    if conditional != expected_conditional or any(by_name[name]["defaultEnabled"] for name in conditional):
        raise AssertionError("conditional filesystem/command policy changed")
    if manifest["scopeProfiles"]["default_remote"] != ["observe", "control"]:
        raise AssertionError("default remote scope profile changed")
    if "refreshToken true" not in by_name["account.read"]["parameterPolicy"]:
        raise AssertionError("account.read parameter-sensitive policy disappeared")
    implemented_capabilities = {
        capability["key"]
        for capability in manifest["capabilities"]
        if capability["implementedByCurrentRuntime"]
    }
    if implemented_capabilities != generator.IMPLEMENTED_MECHANISM_CAPABILITIES:
        raise AssertionError("the exact A1.7b mechanism/build capabilities changed")

    missing_event_family = copy.deepcopy(source)
    missing_event_family["eventFamilies"].remove("threadList.updated")
    expect_failure(generator, missing_event_family, "26 unique expanded event families")

    duplicate_event_family = copy.deepcopy(source)
    duplicate_event_family["eventFamilies"][-1] = duplicate_event_family["eventFamilies"][0]
    expect_failure(generator, duplicate_event_family, "26 unique expanded event families")

    prefix_pairs = {
        (left, right)
        for left in by_name
        for right in by_name
        if left != right and right.startswith(left + ".")
    }
    required_pairs = {
        ("command.exec", "command.exec.resize"),
        ("command.exec", "command.exec.terminate"),
        ("command.exec", "command.exec.write"),
        ("externalAgentConfig.import", "externalAgentConfig.import.readHistories"),
    }
    if not required_pairs <= prefix_pairs:
        raise AssertionError("expected exact-dispatch prefix pairs disappeared")

    # 1. Removing one identity from the 148-decision baseline is rejected.
    removed_baseline = copy.deepcopy(source)
    removed_baseline["entries"] = [
        row
        for row in removed_baseline["entries"]
        if not (
            row["registryKey"]["category"] == "client_request"
            and row["registryKey"]["name"] == "process/kill"
        )
    ]
    expect_failure(generator, removed_baseline, "234 reviewed entries")

    # 2. Omitting one of the 86 compatibility identities is rejected separately.
    removed_compatibility = copy.deepcopy(source)
    removed_compatibility["entries"] = [
        row
        for row in removed_compatibility["entries"]
        if not (
            row["registryKey"]["category"] == "server_notification"
            and row["registryKey"]["name"] == "error"
        )
    ]
    expect_failure(generator, removed_compatibility, "234 reviewed entries")

    # 3. An applicable reviewed row cannot shrink the denominator via N/A.
    not_applicable = copy.deepcopy(source)
    thread_start = next(
        row
        for row in not_applicable["entries"]
        if row["registryKey"]["name"] == "thread/start"
    )
    thread_start["exposure"] = "NotApplicable"
    expect_failure(generator, not_applicable, "16 reviewed rows")

    # 4. Experimental-only methods remain excluded by policy.
    exposed_experimental = copy.deepcopy(source)
    experimental = next(
        row
        for row in exposed_experimental["entries"]
        if row["registryKey"]["category"] == "client_request"
        and row["registryKey"]["name"] == "process/kill"
    )
    experimental["exposure"] = "DedicatedFrontendMethod"
    expect_failure(generator, exposed_experimental, "36 experimental requests")

    # 5. Arbitrary command execution cannot become default-enabled.
    enabled_command = copy.deepcopy(source)
    command = next(
        row
        for row in enabled_command["entries"]
        if row["registryKey"]["name"] == "command/exec"
    )
    command["defaultEnabled"] = True
    expect_failure(generator, enabled_command, "default-disabled")

    # 6. Filesystem reads cannot become default-enabled either.
    enabled_file = copy.deepcopy(source)
    file_read = next(
        row
        for row in enabled_file["entries"]
        if row["registryKey"]["name"] == "fs/readFile"
    )
    file_read["defaultEnabled"] = True
    expect_failure(generator, enabled_file, "default-disabled")

    # 7. Command-execution scope cannot enter the default remote profile.
    wrong_scopes = copy.deepcopy(source)
    wrong_scopes["defaultRemoteScopes"].append("command_execution")
    expect_failure(generator, wrong_scopes, "default remote scopes")

    # 8. A manifest-only scope drift cannot diverge from the registry source.
    scope_mismatch = copy.deepcopy(manifest)
    thread_list = next(
        row for row in scope_mismatch["methods"] if row["method"] == "thread.list"
    )
    thread_list["requiredScopes"] = ["control"]
    expect_manifest_failure(
        generator, source, scope_mismatch, "scopes differ from registry source"
    )

    # 9. Registry/source controller drift must contradict its reviewed policy.
    controller_mismatch = copy.deepcopy(source)
    thread_list_source = next(
        row
        for row in controller_mismatch["entries"]
        if row["registryKey"]["name"] == "thread/list"
    )
    thread_list_source["controllerRequired"] = True
    expect_source_controller_failure(generator, controller_mismatch)

    # 10. Every manifest method must retain exact parameter/result schemas.
    missing_schema = copy.deepcopy(manifest)
    account_read = next(
        row for row in missing_schema["methods"] if row["method"] == "account.read"
    )
    del account_read["resultSchema"]
    expect_manifest_failure(
        generator, source, missing_schema, "exact schema references"
    )

    # 11. A native service method cannot lose its frontend-native marker.
    missing_native_marker = copy.deepcopy(manifest)
    provider_start = next(
        row
        for row in missing_native_marker["methods"]
        if row["method"] == "provider.start"
    )
    provider_start["frontendNative"] = False
    expect_manifest_failure(
        generator, source, missing_native_marker, "frontend-native method set"
    )

    # 12. Native service methods cannot invent ProtocolSurface registry rows.
    synthetic_native_registry = copy.deepcopy(manifest)
    controller_acquire = next(
        row
        for row in synthetic_native_registry["methods"]
        if row["method"] == "controller.acquire"
    )
    controller_acquire["registryKeys"] = [
        "client_request:ClientRequest:method:controller/acquire"
    ]
    expect_manifest_failure(
        generator,
        source,
        synthetic_native_registry,
        "cannot reference registry rows",
    )

    # 13. Prefix dispatch would misroute command.exec.* and import histories.
    prefix_dispatch_header = generated_header.replace(
        "method.method == value", "value.starts_with(method.method)"
    )
    expect_dispatch_failure(generator, prefix_dispatch_header)

    # 14. Existing v1 method spellings and their order are immutable.
    changed_existing_spelling = copy.deepcopy(source)
    existing_thread_start = next(
        row
        for row in changed_existing_spelling["entries"]
        if row["registryKey"]["name"] == "thread/start"
    )
    existing_thread_start["mappings"] = ["thread.begin"]
    expect_failure(
        generator,
        changed_existing_spelling,
        "original 15 method order/spellings",
    )

    # 15. Every defined method must retain an A1.7b runtime handler.
    runtime_activation = copy.deepcopy(manifest)
    account_read_runtime = next(row for row in runtime_activation["methods"] if row["method"] == "account.read")
    account_read_runtime["currentlyImplemented"] = False
    expect_manifest_failure(
        generator, source, runtime_activation, "implement all 105 methods"
    )

    # 16. Future service/transport/SDK/UI capabilities remain unimplemented.
    future_capability = copy.deepcopy(manifest)
    authentication = next(
        row
        for row in future_capability["capabilities"]
        if row["key"] == "cpp_client_sdk"
    )
    authentication["implementedByCurrentRuntime"] = True
    expect_manifest_failure(
        generator, source, future_capability, "future capability as implemented"
    )

    # 17. Removing one privileged method scope must change neither 53/90 nor
    # the independent owner-registry derivation silently.
    missing_privileged_scope = copy.deepcopy(manifest["methods"])
    account_login = next(row for row in missing_privileged_scope if row["method"] == "account.login.start")
    account_login["requiredScopes"] = ["control"]
    expect_authorization_failure(generator, source, missing_privileged_scope, "53 of 90")

    # 18. The default remote profile cannot acquire a privileged scope.
    privileged_default = copy.deepcopy(source)
    privileged_default["defaultRemoteScopes"].append("account_management")
    expect_authorization_failure(generator, privileged_default, manifest["methods"], "exactly observe and control")

    # 19. An extra owner-registry operation cannot become observer-readable.
    extra_observer = copy.deepcopy(source)
    observer_mutation = next(
        row
        for row in extra_observer["entries"]
        if row["registryKey"]["category"] == "client_request" and row["registryKey"]["name"] == "account/login/start"
    )
    observer_mutation["securityDecision"] = "ObserverReadApproved"
    expect_authorization_failure(generator, extra_observer, manifest["methods"], "26/22/22/15/1")

    # 20. Conditional deployment gates remain absent from default availability.
    enabled_conditional_method = copy.deepcopy(manifest["methods"])
    next(row for row in enabled_conditional_method if row["method"] == "fs.readFile")["defaultEnabled"] = True
    expect_authorization_failure(generator, source, enabled_conditional_method, "denominator must remain 90")

    # 21. Lifecycle methods cannot enter default_remote.
    exposed_lifecycle = copy.deepcopy(manifest["methods"])
    next(row for row in exposed_lifecycle if row["method"] == "provider.start")["requiredScopes"] = ["control"]
    expect_authorization_failure(generator, source, exposed_lifecycle, "53 of 90")

    # 22. Every reverse response/rejection retains a sensitive response scope.
    exposed_reverse = copy.deepcopy(manifest["methods"])
    next(row for row in exposed_reverse if row["method"] == "request.approval.respond")["requiredScopes"] = ["control"]
    expect_authorization_failure(generator, source, exposed_reverse, "53 of 90")

    # 23. A denominator reduction cannot preserve a misleading percentage.
    shrunken_available = copy.deepcopy(manifest["methods"])
    next(row for row in shrunken_available if row["method"] == "thread.list")["defaultEnabled"] = False
    expect_authorization_failure(generator, source, shrunken_available, "denominator must remain 90")

    # 24. Independent registry categories cannot drift while generated method
    # policy remains unchanged.
    mismatched_derivation = copy.deepcopy(source)
    next(
        row
        for row in mismatched_derivation["entries"]
        if row["registryKey"]["category"] == "client_request" and row["registryKey"]["name"] == "thread/start"
    )["securityDecision"] = "ObserverReadApproved"
    expect_authorization_failure(generator, mismatched_derivation, manifest["methods"], "26/22/22/15/1")

    # 25. Provider readiness is generated for all and only 86+12 provider paths.
    missing_provider_ready = copy.deepcopy(manifest["methods"])
    next(row for row in missing_provider_ready if row["method"] == "thread.list")["providerReadyRequired"] = False
    expect_authorization_failure(generator, source, missing_provider_ready, "86 provider operations and 12 reverse")

    # 26. Provider result type metadata is authoritative from the registry.
    wrong_result_type = copy.deepcopy(manifest)
    next(row for row in wrong_result_type["methods"] if row["method"] == "thread.start")["resultType"] = "Thread"
    expect_manifest_failure(generator, source, wrong_result_type, "result type differs from registry source")

    # 27. Runtime implementation does not erase the frozen original 15.
    missing_legacy = copy.deepcopy(manifest)
    next(row for row in missing_legacy["methods"] if row["method"] == "thread.start")["legacyCompatibilityMethod"] = False
    expect_manifest_failure(generator, source, missing_legacy, "legacy compatibility method set changed")

    # 28. The ten dedicated pending-request projections remain a bijection with
    # the stable server-request registry and their exact typed response methods.
    missing_pending_projection = copy.deepcopy(manifest)
    missing_pending_projection["pendingRequestMappings"].pop()
    expect_manifest_failure(
        generator,
        source,
        missing_pending_projection,
        "must contain ten entries",
    )
    wrong_pending_response = copy.deepcopy(manifest)
    wrong_pending_response["pendingRequestMappings"][0]["responseMethods"] = [
        "request.unknown.respond"
    ]
    expect_manifest_failure(
        generator,
        source,
        wrong_pending_response,
        "differs from the reviewed registry response contract",
    )

    try:
        generator.secure_safe_object_extensions(
            {
                "type": "object",
                "properties": {"secret": {"type": "boolean"}},
                "additionalProperties": True,
            }
        )
    except generator.GenerationError as error:
        if "credential-shaped known properties" not in str(error):
            raise AssertionError(
                f"unexpected safe-projection property guard failure: {error}"
            ) from error
    else:
        raise AssertionError(
            "safe generated projections accepted a credential-shaped known property"
        )

    # Runtime-schema assertions are closed over the exact C++ validator profile.
    unsupported_keyword = copy.deepcopy(schema_template)
    unsupported_keyword["$defs"]["ThreadStartParams"]["contains"] = {
        "type": "string"
    }
    expect_schema_generation_failure(
        generator,
        unsupported_keyword,
        manifest,
        source,
        "unsupported assertion keyword 'contains'",
    )

    external_reference = copy.deepcopy(generated_schema)
    external_reference["$defs"]["ThreadStartParams"]["$ref"] = (
        "https://example.invalid/external-schema.json"
    )
    expect_schema_audit_failure(
        generator, external_reference, manifest, "non-local reference"
    )

    unresolved_reference = copy.deepcopy(generated_schema)
    unresolved_reference["$defs"]["ThreadStartParams"]["$ref"] = (
        "#/$defs/DoesNotExist"
    )
    expect_schema_audit_failure(
        generator, unresolved_reference, manifest, "unresolved local reference"
    )

    malformed_escape_reference = copy.deepcopy(generated_schema)
    malformed_escape_reference["$defs"]["ThreadStartParams"]["$ref"] = (
        "#/$defs/~2Malformed"
    )
    expect_schema_audit_failure(
        generator,
        malformed_escape_reference,
        manifest,
        "malformed local reference",
    )

    leading_zero_reference = copy.deepcopy(generated_schema)
    leading_zero_reference["$defs"]["ThreadStartParams"]["$ref"] = (
        "#/$defs/Command/allOf/01"
    )
    expect_schema_audit_failure(
        generator, leading_zero_reference, manifest, "malformed local reference"
    )

    unreviewed_custom_keyword = copy.deepcopy(generated_schema)
    unreviewed_custom_keyword["$defs"]["ThreadStartParams"][
        "x-aisuite-futureAssertion"
    ] = True
    expect_schema_audit_failure(
        generator,
        unreviewed_custom_keyword,
        manifest,
        "unreviewed custom AISuite keyword",
    )

    unsupported_format = copy.deepcopy(generated_schema)
    unsupported_format["$defs"]["ThreadStartParams"]["properties"][
        "futureNumber"
    ] = {"type": "integer", "format": "uint128"}
    expect_schema_audit_failure(
        generator, unsupported_format, manifest, "unsupported numeric format"
    )

    malformed_pattern = copy.deepcopy(generated_schema)
    malformed_pattern["$defs"]["ThreadStartParams"]["pattern"] = "["
    expect_schema_audit_failure(
        generator, malformed_pattern, manifest, "pattern is malformed"
    )

    malformed_assertions = (
        ("$ref", 7, "$ref must be a string"),
        ("allOf", {}, "allOf must be a non-empty array of schemas"),
        ("anyOf", {}, "anyOf must be a non-empty array of schemas"),
        ("oneOf", {}, "oneOf must be a non-empty array of schemas"),
        ("not", [], "not must be an object or boolean schema"),
        ("if", [], "if must be an object or boolean schema"),
        ("then", [], "then must be an object or boolean schema"),
        ("else", [], "else must be an object or boolean schema"),
        ("type", "future", "type has an invalid schema type"),
        ("enum", [], "enum must be a non-empty array"),
        ("properties", [], "properties must be an object"),
        ("propertyNames", [], "propertyNames must be an object or boolean schema"),
        ("additionalProperties", None, "additionalProperties must be an object or boolean schema"),
        ("required", "field", "required must be an array of unique strings"),
        ("minProperties", -1, "minProperties must be a non-negative integer"),
        ("maxProperties", -1, "maxProperties must be a non-negative integer"),
        ("items", [], "items must be an object or boolean schema"),
        ("minItems", -1, "minItems must be a non-negative integer"),
        ("maxItems", -1, "maxItems must be a non-negative integer"),
        ("uniqueItems", "true", "uniqueItems must be a boolean"),
        ("minLength", -1, "minLength must be a non-negative integer"),
        ("maxLength", -1, "maxLength must be a non-negative integer"),
        ("pattern", 7, "pattern must be a string"),
        ("minimum", "zero", "minimum must be a finite number"),
        ("maximum", "one", "maximum must be a finite number"),
        ("format", 7, "format must be a string"),
        (
            "x-aisuite-sensitiveFieldNamesForbidden",
            "secret",
            "x-aisuite-sensitiveFieldNamesForbidden must be an array of unique strings",
        ),
        (
            "x-aisuite-forbiddenNormalizedPropertyNames",
            "secret",
            "x-aisuite-forbiddenNormalizedPropertyNames must be an array of unique strings",
        ),
    )
    for keyword, malformed_value, failure in malformed_assertions:
        malformed = copy.deepcopy(generated_schema)
        malformed["$defs"]["ThreadStartParams"][keyword] = malformed_value
        expect_schema_audit_failure(generator, malformed, manifest, failure)

    unbounded_unique_items = copy.deepcopy(generated_schema)
    unbounded_unique_items["$defs"]["ThreadStartParams"]["properties"][
        "futureCatalog"
    ] = {
        "type": "array",
        "items": {"type": "string"},
        "uniqueItems": True,
    }
    expect_schema_audit_failure(
        generator,
        unbounded_unique_items,
        manifest,
        "unbounded uniqueItems cardinality",
    )

    explicitly_bounded_unique_items = copy.deepcopy(generated_schema)
    explicitly_bounded_unique_items["$defs"]["ThreadStartParams"]["properties"][
        "futureCatalog"
    ] = {
        "type": "array",
        "items": {"type": "string"},
        "maxItems": 32,
        "uniqueItems": True,
    }
    bounded_audit = generator.audit_runtime_schema_profile(
        explicitly_bounded_unique_items, manifest
    )
    if bounded_audit.unique_item_schema_count != 10:
        raise AssertionError("an explicitly bounded uniqueItems schema was not audited")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
