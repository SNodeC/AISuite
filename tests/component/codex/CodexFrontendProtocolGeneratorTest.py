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
        "runtimeAvailableMethods": 15,
        "reviewedIdentities": 234,
        "notificationMappings": 68,
        "threadItemMappings": 18,
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
    for name in (
        "ExpandedPendingRequest",
        "ExpandedFrontendEvent",
        "ExpandedBackendSnapshotState",
        "ExpandedSnapshot",
    ):
        if name not in definitions:
            raise AssertionError(f"missing additive schema definition {name}")


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
    manifest = generator.generate_manifest(source)
    committed = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest != committed:
        raise AssertionError("committed frontend manifest is stale")
    generated_header = generator.generate_header(manifest)
    if generated_header != args.header.read_text(encoding="utf-8"):
        raise AssertionError("committed GeneratedProtocol.h is stale")
    schema_template = json.loads(args.schema_template.read_text(encoding="utf-8"))
    generated_schema = generator.generate_schema(schema_template, manifest, source)
    if generated_schema != json.loads(args.schema.read_text(encoding="utf-8")):
        raise AssertionError("committed frontend JSON Schema is stale")
    if generator.generate_schema_data(generated_schema) != args.schema_data.read_text(
        encoding="utf-8"
    ):
        raise AssertionError("committed GeneratedProtocolSchema.inc is stale")
    generated_fixtures = generator.generate_golden_fixtures(
        generated_schema, manifest
    )
    if generated_fixtures != json.loads(args.fixtures.read_text(encoding="utf-8")):
        raise AssertionError("committed frontend golden fixtures are stale")
    if (
        generated_fixtures["counts"] != {"methods": 105, "expandedEvents": 25}
        or len(generated_fixtures["methods"]) != 105
        or len(generated_fixtures["expandedEvents"]) != 25
    ):
        raise AssertionError("frontend golden fixture coverage changed")
    validate_additive_schema_contract(schema_template, generated_schema, manifest)

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
        "currentRuntimeMethods": 15,
        "reviewedIdentities": 234,
        "notifications": 68,
        "threadItems": 18,
    }
    if counts != expected_counts:
        raise AssertionError(f"method/review denominator changed: {counts}")
    command_branches = generated_schema["$defs"]["Command"]["allOf"][1]["oneOf"]
    if len(command_branches) != 105:
        raise AssertionError("JSON Schema does not define all 105 command methods")
    if generated_schema.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
        raise AssertionError("JSON Schema draft changed")
    if generated_schema["x-aisuite-frontend-contract"]["runtimeAvailableMethods"] != 15:
        raise AssertionError("JSON Schema claims additive runtime availability")

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
    if sum(row["currentlyImplemented"] for row in methods) != 15:
        raise AssertionError("A1.7a activated an additive method")
    if any(row["currentlyImplemented"] for row in methods[15:]):
        raise AssertionError("one of the 90 additive methods is runtime-active")

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
    if any(
        capability["implementedByCurrentRuntime"]
        for capability in manifest["capabilities"]
        if capability["key"] in generator.FUTURE_CAPABILITIES
    ):
        raise AssertionError("a future capability is claimed as implemented")

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

    removed = copy.deepcopy(source)
    removed["entries"].pop()
    expect_failure(generator, removed, "234 reviewed entries")
    wrong_scopes = copy.deepcopy(source)
    wrong_scopes["defaultRemoteScopes"].append("command_execution")
    expect_failure(generator, wrong_scopes, "default remote scopes")
    exposed_experimental = copy.deepcopy(source)
    experimental = next(row for row in exposed_experimental["entries"] if row["stability"] == "experimental_only")
    experimental["exposure"] = "DedicatedFrontendMethod"
    expect_failure(generator, exposed_experimental, "36 experimental requests")
    enabled_command = copy.deepcopy(source)
    command = next(row for row in enabled_command["entries"] if row["registryKey"]["name"] == "command/exec")
    command["defaultEnabled"] = True
    expect_failure(generator, enabled_command, "default-disabled")
    enabled_file = copy.deepcopy(source)
    file_read = next(row for row in enabled_file["entries"] if row["registryKey"]["name"] == "fs/readFile")
    file_read["defaultEnabled"] = True
    expect_failure(generator, enabled_file, "default-disabled")
    not_applicable = copy.deepcopy(source)
    thread_start = next(row for row in not_applicable["entries"] if row["registryKey"]["name"] == "thread/start")
    thread_start["exposure"] = "NotApplicable"
    expect_failure(generator, not_applicable, "16 reviewed rows")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
