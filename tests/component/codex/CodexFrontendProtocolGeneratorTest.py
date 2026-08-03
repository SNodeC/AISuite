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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app-tool", type=Path, required=True)
    parser.add_argument("--app-manifest", type=Path, required=True)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    args = parser.parse_args()

    generator = load_module(args.generator)
    source = json.loads(args.source.read_text(encoding="utf-8"))
    manifest = generator.generate_manifest(source)
    committed = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest != committed:
        raise AssertionError("committed frontend manifest is stale")
    if generator.generate_header(manifest) != args.header.read_text(encoding="utf-8"):
        raise AssertionError("committed GeneratedProtocol.h is stale")

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
