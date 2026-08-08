#!/usr/bin/env python3
"""Fast source-oriented policy checks for the immutable Codex P0 baseline."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from typing import Any, Sequence


EXPECTED_PHASES = [
    "P0 — Freeze behavior and architecture baseline",
    "P1 — Correct the library dependency DAG",
    "P2 — Add reusable SNode.C connection/WebSocket composition primitives",
    "P3 — Introduce the shared SNode.C frontend adapter layer",
    "P4 — Replace dual JSON projection with one typed frontend model/occurrence",
    "P5 — Simplify `codex-backend-client`",
    "P6 — Remove compatibility scaffolding and close the reduction",
]

EXPECTED_FIXED_PHASE_SECTION_SHA256 = "705a7bf6d6b9af1d5300846bd8f55edf8e64024a832ddfdf4e926aebef67c301"

EXPECTED_TRANSPORT_IDS = {
    "unix-jsonl",
    "ipv4-jsonl",
    "ipv6-jsonl",
    "ipv4-tls-jsonl",
    "ipv6-tls-jsonl",
    "rfcomm-jsonl",
    "rfcomm-tls-jsonl",
    "websocket-ipv4",
    "websocket-ipv6",
    "wss-ipv4",
    "wss-ipv6",
}

EXPECTED_PROTOCOL = {
    "identity": "snodec.codex-frontend",
    "version": 1,
    "messageKindCount": 8,
    "methodCount": 105,
    "expandedEventFamilyCount": 26,
    "threadItemDiscriminatorCount": 18,
    "scopeCount": 12,
}

EXPECTED_PUBLIC_TARGETS = {
    "AISuite::OpenAICodex": ("ai-openai-codex", "libaisuite-openai-codex.so"),
    "AISuite::OpenAICodexBackend": ("ai-openai-codex-backend", "libaisuite-openai-codex-backend.so"),
    "AISuite::OpenAICodexFrontend": ("ai-openai-codex-frontend", "libaisuite-openai-codex-frontend.so"),
    "AISuite::OpenAICodexFrontendClient": ("ai-openai-codex-frontend-client", "libaisuite-openai-codex-frontend-client.so"),
}

EXPECTED_PRODUCTION_TARGETS = {
    "ai-openai-codex",
    "ai-openai-codex-backend",
    "ai-openai-codex-frontend",
    "ai-openai-codex-frontend-client",
    "codex-backend",
    "codex-backend-client",
    "codex-backend-client-support",
    "codex-backend-runtime-bridge",
    "codex-backend-stream-adapter",
    "codex-backend-web-adapter",
    "codex-backend-websocket-subprotocol",
    "codex-reference-authentication",
}

EXPECTED_ROOTS = {
    "src/ai/openai/codex",
    "src/apps/codex-backend",
    "src/apps/codex-backend-client",
    "tests/component/codex",
    "tests/policy/codex",
    "tools/frontend",
    "tools/codex",
    "docs/ai/openai/codex",
}

EXPECTED_HOTSPOTS = {
    "src/apps/codex-backend/main.cpp",
    "src/apps/codex-backend-client/main.cpp",
    "src/ai/openai/codex/backend/BackendCore.cpp",
    "src/ai/openai/codex/backend/Reducer.cpp",
    "src/ai/openai/codex/frontend/FrontendService.cpp",
    "src/ai/openai/codex/frontend/detail/BackendProjectionBuilder.cpp",
    "src/ai/openai/codex/frontend/client/Client.cpp",
    "src/ai/openai/codex/frontend/client/State.cpp",
    "src/apps/codex-backend-client/CommandDrainController.cpp",
    "src/apps/codex-backend-client/FrontendWebSocketClient.cpp",
}

EXPECTED_TRANSPORT_ROWS = {
    "unix-jsonl": ("Unix JSONL", "codex-backend", "codex-backend-client-unix", "always", "Unix stream", "Unix", "none", "JSONL", None, True, "owner-only pathname; peer credentials where supported; verified-local policy; bearer fallback where required"),
    "ipv4-jsonl": ("IPv4 JSONL", "codex-backend-ipv4", "codex-backend-client-ipv4", "always", "TCP IPv4", "IPv4", "none", "JSONL", None, False, "loopback default; non-loopback plaintext requires explicit insecure override; remote bearer authentication"),
    "ipv6-jsonl": ("IPv6 JSONL", "codex-backend-ipv6", "codex-backend-client-ipv6", "always", "TCP IPv6", "IPv6", "none", "JSONL", None, False, "loopback default; non-loopback plaintext requires explicit insecure override; remote bearer authentication"),
    "ipv4-tls-jsonl": ("IPv4 TLS JSONL", "codex-backend-tls-ipv4", "codex-backend-client-tls-ipv4", "AISUITE_ENABLE_CODEX_FRONTEND_TLS", "TCP IPv4", "IPv4", "TLS", "JSONL", None, False, "TLS plus remote bearer authentication"),
    "ipv6-tls-jsonl": ("IPv6 TLS JSONL", "codex-backend-tls-ipv6", "codex-backend-client-tls-ipv6", "AISUITE_ENABLE_CODEX_FRONTEND_TLS", "TCP IPv6", "IPv6", "TLS", "JSONL", None, False, "TLS plus remote bearer authentication"),
    "rfcomm-jsonl": ("RFCOMM JSONL", "codex-backend-rfcomm", "codex-backend-client-rfcomm", "AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM", "Bluetooth RFCOMM", "RFCOMM", "none", "JSONL", None, False, "remote bearer authentication; Bluetooth pairing is not frontend authentication"),
    "rfcomm-tls-jsonl": ("RFCOMM TLS JSONL", "codex-backend-rfcomm-tls", "codex-backend-client-rfcomm-tls", "AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM", "Bluetooth RFCOMM", "RFCOMM", "TLS", "JSONL", None, False, "TLS plus remote bearer authentication"),
    "websocket-ipv4": ("WebSocket IPv4", "codex-backend-websocket-ipv4", "codex-backend-client-websocket-ipv4", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET", "HTTP/TCP IPv4 upgrade", "IPv4", "none", "WebSocket text message", "codex", False, "origin and WebSocket upgrade policy plus remote bearer authentication"),
    "websocket-ipv6": ("WebSocket IPv6", "codex-backend-websocket-ipv6", "codex-backend-client-websocket-ipv6", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET", "HTTP/TCP IPv6 upgrade", "IPv6", "none", "WebSocket text message", "codex", False, "origin and WebSocket upgrade policy plus remote bearer authentication"),
    "wss-ipv4": ("WSS IPv4", "codex-backend-wss-ipv4", "codex-backend-client-wss-ipv4", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET;AISUITE_ENABLE_CODEX_FRONTEND_TLS", "HTTPS/TCP IPv4 upgrade", "IPv4", "TLS", "WebSocket text message", "codex", False, "TLS, origin and WebSocket upgrade policy, plus remote bearer authentication"),
    "wss-ipv6": ("WSS IPv6", "codex-backend-wss-ipv6", "codex-backend-client-wss-ipv6", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET;AISUITE_ENABLE_CODEX_FRONTEND_TLS", "HTTPS/TCP IPv6 upgrade", "IPv6", "TLS", "WebSocket text message", "codex", False, "TLS, origin and WebSocket upgrade policy, plus remote bearer authentication"),
}

EXPECTED_TRANSPORT_PEER_METADATA = {
    "unix-jsonl": ["transport", "localPeer", "unixUserId"],
    "ipv4-jsonl": ["remote numeric address", "loopback", "transport", "encryption status"],
    "ipv6-jsonl": ["remote numeric address", "loopback", "transport", "encryption status"],
    "ipv4-tls-jsonl": ["remote numeric address", "loopback", "transport", "encryption status"],
    "ipv6-tls-jsonl": ["remote numeric address", "loopback", "transport", "encryption status"],
    "rfcomm-jsonl": ["Bluetooth address and RFCOMM channel", "unencrypted transport fact"],
    "rfcomm-tls-jsonl": ["Bluetooth address and RFCOMM channel", "encrypted transport fact"],
    "websocket-ipv4": ["HTTP peer address", "origin", "unencrypted transport fact"],
    "websocket-ipv6": ["HTTP peer address", "origin", "unencrypted transport fact"],
    "wss-ipv4": ["HTTPS peer address", "origin", "encrypted transport fact"],
    "wss-ipv6": ["HTTPS peer address", "origin", "encrypted transport fact"],
}


def fail(message: str) -> None:
    raise AssertionError(message)


def load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"baseline is not valid UTF-8 JSON: {error}")
    if not isinstance(value, dict):
        fail("baseline root must be an object")
    return value


def validate_shape(baseline: dict[str, Any]) -> None:
    if baseline.get("formatVersion") != 1:
        fail("baseline formatVersion must be 1")
    required = {"provenance", "externalContract", "architectureMeasurements", "ownerLiveEvidence"}
    missing = sorted(required - set(baseline))
    if missing:
        fail(f"baseline is missing required top-level fields: {', '.join(missing)}")

    provenance = baseline["provenance"]
    required_provenance = {
        "repository",
        "sourceBranch",
        "p0BaselineParentSha",
        "pr14MergeCommit",
        "pr14FinalSourceHead",
        "projectVersion",
        "codexSoversion",
        "configuredBuildType",
        "cxxCompiler",
        "cmakeVersion",
        "generator",
        "featureSwitches",
        "enabledOptionalTransportFeatures",
    }
    missing_provenance = sorted(required_provenance - set(provenance))
    if missing_provenance:
        fail(f"baseline provenance is incomplete: {', '.join(missing_provenance)}")
    for key in ("p0BaselineParentSha", "pr14MergeCommit", "pr14FinalSourceHead"):
        if not re.fullmatch(r"[0-9a-f]{40}", str(provenance.get(key, ""))):
            fail(f"provenance {key} is not a full Git SHA")
    if provenance.get("repository") != "SNodeC/AISuite" or provenance.get("sourceBranch") != "master":
        fail("baseline repository/source-branch provenance drifted")
    if provenance.get("p0BaselineParentSha") != "4c0cfbf99667fef64c9fed010d84031248ceaba2":
        fail("actual P0 baseline parent is not the authoritative master baseline")
    if provenance.get("pr14MergeCommit") != "4c0cfbf99667fef64c9fed010d84031248ceaba2":
        fail("PR #14 merge prerequisite drifted")
    if provenance.get("pr14FinalSourceHead") != "d524a6788631680e9fd86bda94ef49337a370d4c":
        fail("PR #14 source-head provenance drifted")
    if provenance.get("projectVersion") != "0.1.0" or provenance.get("codexSoversion") != 2:
        fail("project version or Codex SOVERSION baseline drifted")
    if provenance.get("configuredBuildType") != "Debug":
        fail("P0 baseline was not captured from the feature-complete Debug build")
    switches = provenance.get("featureSwitches", {})
    if not isinstance(switches, dict) or len(switches) != 6 or not all(switches.values()):
        fail("feature-complete P0 switch provenance is incomplete")
    if provenance.get("enabledOptionalTransportFeatures") != ["RFCOMM", "TLS", "WebSocket"]:
        fail("enabled optional transport provenance is incomplete")

    external = baseline["externalContract"]
    if external.get("blocking") is not True:
        fail("externalContract must be blocking")
    protocol = external.get("protocol", {})
    for key, expected in EXPECTED_PROTOCOL.items():
        if protocol.get(key) != expected:
            fail(f"protocol {key} must be {expected!r}")
    if protocol.get("methodOwnershipCounts") != {"native": 7, "provider": 86, "reverse": 12}:
        fail("protocol method ownership split drifted")
    protocol_collections = {
        "messageKinds": "messageKindCount",
        "methods": "methodCount",
        "stableMethodIds": "methodCount",
        "wireMethods": "methodCount",
        "expandedEventFamilies": "expandedEventFamilyCount",
        "threadItemDiscriminators": "threadItemDiscriminatorCount",
        "scopes": "scopeCount",
        "capabilities": "capabilityCount",
    }
    for key, count_key in protocol_collections.items():
        values = protocol.get(key)
        if not isinstance(values, list) or len(values) != protocol.get(count_key):
            fail(f"protocol {key} does not match {count_key}")
        if key not in {"methods", "capabilities"} and len(set(values)) != len(values):
            fail(f"protocol {key} is not a unique stable set")
    if set(protocol.get("capabilityCategories", {})) != {
        "conditional_topology",
        "product",
        "static_mechanism",
    }:
        fail("protocol capability categories are incomplete")
    hashes = protocol.get("authorityCanonicalSha256", {})
    if len(hashes) != 6 or any(not re.fullmatch(r"[0-9a-f]{64}", str(value)) for value in hashes.values()):
        fail("protocol authority SHA-256 inventory is incomplete")

    public_targets = external.get("publicCpp", {}).get("targets", [])
    actual_public = {
        row.get("importedTarget"): row for row in public_targets if isinstance(row, dict)
    }
    if set(actual_public) != set(EXPECTED_PUBLIC_TARGETS):
        fail("public imported-target set drifted")
    required_public_fields = {
        "importedTarget",
        "buildTarget",
        "outputLibrary",
        "targetType",
        "version",
        "soversion",
        "installedPackageExportFile",
    }
    for imported, (build_target, output_library) in EXPECTED_PUBLIC_TARGETS.items():
        row = actual_public[imported]
        if not required_public_fields <= set(row):
            fail(f"public target record is incomplete: {imported}")
        if (row.get("buildTarget"), row.get("outputLibrary")) != (build_target, output_library):
            fail(f"public target mapping drifted: {imported}")
        if (
            row.get("targetType") != "SHARED_LIBRARY"
            or row.get("version") != "0.1.0"
            or row.get("soversion") != 2
            or row.get("installedPackageExportFile") != "lib/cmake/AISuite/AISuiteTargets.cmake"
        ):
            fail(f"public target ABI/package metadata drifted: {imported}")

    architecture = baseline["architectureMeasurements"]
    if architecture.get("blocking") is not False or architecture.get("equalityGate") is not False:
        fail("architectureMeasurements must be explicitly non-blocking and not an equality gate")
    policy = str(architecture.get("comparisonPolicy", "")).lower()
    if "not" not in policy or "equality gate" not in policy:
        fail("architecture measurement policy does not explicitly reject equality gating")

    evidence = baseline["ownerLiveEvidence"]
    if evidence.get("evidenceType") != "owner-reported manual live acceptance":
        fail("owner live evidence is not labeled owner-reported manual live acceptance")
    if evidence.get("reproducedByCodex") is not False:
        fail("owner live evidence must not be represented as reproduced by Codex")
    observations = evidence.get("observations", [])
    if len(observations) != 22 or [row.get("ordinal") for row in observations] != list(range(1, 23)):
        fail("owner live evidence must retain the 22 ordered observations")

    transport = external.get("transport", {})
    rows = transport.get("externalCompositions", [])
    if transport.get("externalCompositionCount") != 11 or len(rows) != 11:
        fail("external transport matrix must contain exactly eleven rows")
    actual_ids = {row.get("id") for row in rows}
    if actual_ids != EXPECTED_TRANSPORT_IDS:
        fail(f"external transport set drifted: {sorted(actual_ids)}")
    row_fields = (
        "name",
        "serverNamedInstance",
        "clientNamedInstance",
        "compiledFeatureSwitch",
        "carrier",
        "addressFamily",
        "encryption",
        "framing",
        "webSocketSubprotocol",
        "defaultEnabled",
        "authenticationMode",
    )
    for row in rows:
        identifier = row["id"]
        if tuple(row.get(field) for field in row_fields) != EXPECTED_TRANSPORT_ROWS[identifier]:
            fail(f"external transport composition/security drifted: {identifier}")
        if row.get("peerMetadataAvailable") != EXPECTED_TRANSPORT_PEER_METADATA[identifier]:
            fail(f"external transport peer-metadata contract drifted: {identifier}")
    in_memory = transport.get("inMemory", {})
    if in_memory.get("classification") != "test-only" or in_memory.get("externalListener") is not False:
        fail("in-memory transport is not separately classified as test-only with no listener")
    if in_memory.get("partOfExternalApplicationTransportSet") is not False:
        fail("in-memory transport was incorrectly included in the eleven external transports")

    source = architecture.get("source", {})
    roots = source.get("roots", {})
    if set(roots) != EXPECTED_ROOTS:
        fail("architecture source-root inventory is incomplete")
    aggregate_fields = {
        "fileCount",
        "trackedPhysicalLines",
        "totalBytes",
        "cCppSourceCount",
        "cCppHeaderCount",
        "pythonCount",
        "cmakeCount",
        "jsonCount",
        "markdownCount",
    }
    for root, categories in roots.items():
        if set(categories) != {"allTracked", "generated", "handWritten"}:
            fail(f"source categories are incomplete for {root}")
        for category, values in categories.items():
            if not isinstance(values, dict) or not aggregate_fields <= set(values):
                fail(f"source metrics are incomplete for {root}/{category}")
            if any(not isinstance(values[field], int) or values[field] < 0 for field in aggregate_fields):
                fail(f"source metrics are invalid for {root}/{category}")
    hotspots = source.get("hotspots", {})
    if set(hotspots) != EXPECTED_HOTSPOTS:
        fail("complexity hotspot inventory is incomplete")
    for path, values in hotspots.items():
        if set(values) != {"trackedBytes", "trackedPhysicalLines"}:
            fail(f"hotspot measurements are incomplete: {path}")

    cmake = architecture.get("cmake", {})
    target_rows = cmake.get("targets", [])
    target_names = {row.get("name") for row in target_rows if isinstance(row, dict)}
    if not EXPECTED_PRODUCTION_TARGETS <= target_names:
        fail("CMake File API target inventory is incomplete")
    target_fields = {
        "name",
        "type",
        "classification",
        "sourceDirectory",
        "codemodelSourceEntryCount",
        "buildOutputs",
        "resolvedInProjectBuildDependencies",
        "resolvedLinkLibraryNames",
        "compileDefinitions",
        "installed",
        "installDestinations",
    }
    if any(not target_fields <= set(row) for row in target_rows):
        fail("CMake File API target record is incomplete")
    if not isinstance(cmake.get("resolvedInProjectDependencyEdges"), list):
        fail("CMake dependency-edge inventory is missing")

    installed = architecture.get("installed", {})
    installed_headers = installed.get("installedPublicHeaders", {})
    if set(installed_headers.get("groups", {})) != {"core", "backend", "frontend", "frontendClient"}:
        fail("installed public-header groups are incomplete")
    if installed_headers.get("totalCount") != sum(
        group.get("count", -1) for group in installed_headers["groups"].values()
    ):
        fail("installed public-header total is inconsistent")
    target_measurements = installed.get("publicTargetMeasurements", [])
    if {row.get("importedTarget") for row in target_measurements} != set(EXPECTED_PUBLIC_TARGETS):
        fail("installed public-target measurements are incomplete")
    binary_fields = {
        "fileSizeBytes",
        "soname",
        "neededLibraries",
        "exportedDynamicSymbolCount",
        "exportedDynamicSymbolNameSetSha256",
    }
    if any(not binary_fields <= set(row) for row in target_measurements):
        fail("installed public-target binary measurements are incomplete")
    libraries = installed.get("installedLibraries", [])
    expected_libraries = {value[1] for value in EXPECTED_PUBLIC_TARGETS.values()} | {
        "libaisuite-codex-backend-runtime.so",
        "libsnodec-websocket-codex-server.so",
    }
    if {row.get("name") for row in libraries} != expected_libraries:
        fail("installed Codex shared-library inventory is incomplete")
    if any(not binary_fields <= set(row) for row in libraries):
        fail("installed shared-library binary measurements are incomplete")
    executables = installed.get("installedExecutables", [])
    if {row.get("name") for row in executables} != {"codex-backend", "codex-backend-client"}:
        fail("installed executable inventory is incomplete")
    if any(not {"fileSizeBytes", "neededLibraries"} <= set(row) for row in executables):
        fail("installed executable measurements are incomplete")

    tests = architecture.get("tests", {})
    test_rows = tests.get("tests", [])
    test_names = {row.get("name") for row in test_rows if isinstance(row, dict)}
    if tests.get("registered") != len(test_rows) or not test_rows:
        fail("registered CTest inventory is missing or inconsistent")
    suite = tests.get("ordinarySuite", {})
    if suite.get("total") != tests.get("registered") or suite.get("failed") != 0:
        fail("ordinary CTest result inventory is inconsistent or failing")
    if suite.get("passed", 0) + suite.get("failed", 0) + suite.get("skipped", 0) != suite.get("total"):
        fail("ordinary CTest result counts do not add up")
    if set(suite.get("testNames", [])) != test_names:
        fail("CTest result and registered-name sets differ")
    transport_coverage = tests.get("transportCoverage", [])
    if {row.get("transportId") for row in transport_coverage} != EXPECTED_TRANSPORT_IDS:
        fail("transport coverage inventory is incomplete")
    for row in transport_coverage:
        automated = row.get("automatedTests", [])
        if not automated or not set(automated) <= test_names:
            fail(f"transport coverage names unregistered tests: {row.get('transportId')}")


def validate_no_machine_paths_or_secrets(value: Any) -> None:
    posix_absolute = re.compile(r"(?:^|[\s='\":])/(?!/)")
    windows_absolute = re.compile(r"(?:^|[\s='\"])[A-Za-z]:[\\/]")
    secret_key = re.compile(r"(?:bearer|credential|secret|token|password).*(?:file|path)$", re.IGNORECASE)

    def walk(node: Any, location: str) -> None:
        if isinstance(node, dict):
            for key, child in node.items():
                if secret_key.search(str(key)):
                    fail(f"secret-bearing key found at {location}.{key}")
                walk(child, f"{location}.{key}")
        elif isinstance(node, list):
            for index, child in enumerate(node):
                walk(child, f"{location}[{index}]")
        elif isinstance(node, str) and (posix_absolute.search(node) or windows_absolute.search(node)):
            fail(f"absolute or host-specific path found at {location}: {node}")

    walk(value, "baseline")


def validate_roadmap(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")
    headings = re.findall(r"^### (P[0-9]+ — .+)$", text, flags=re.MULTILINE)
    if headings != EXPECTED_PHASES:
        fail(f"fixed P0–P6 phase headings are absent, duplicated, renamed, or reordered: {headings}")
    for phase in EXPECTED_PHASES:
        if text.count(f"### {phase}") != 1:
            fail(f"roadmap phase does not appear exactly once as a normative heading: {phase}")
    if re.search(r"^### P7\b", text, flags=re.MULTILINE):
        fail("roadmap must not create P7")
    fixed = re.search(
        r"^## Fixed phases\n(.*?)^## Phase dependencies",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if fixed is None:
        fail("roadmap fixed-phase scope section is missing")
    fingerprint = hashlib.sha256(fixed.group(1).encode("utf-8")).hexdigest()
    if fingerprint != EXPECTED_FIXED_PHASE_SECTION_SHA256:
        fail("fixed P0–P6 repository/scope text changed")


def run_tool(arguments: Sequence[str], source_dir: pathlib.Path, expected: int) -> str:
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "LANG": "C", "PYTHONDONTWRITEBYTECODE": "1"})
    completed = subprocess.run(
        [sys.executable, "-B", *arguments],
        cwd=source_dir,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode != expected:
        fail(
            f"tool returned {completed.returncode}, expected {expected}: "
            f"stdout={completed.stdout!r} stderr={completed.stderr!r}"
        )
    return completed.stdout


def validate_tool_policy(
    source_dir: pathlib.Path,
    tool: pathlib.Path,
    baseline_path: pathlib.Path,
    baseline: dict[str, Any],
) -> None:
    run_tool(
        [str(tool), "verify-contract", "--source-dir", str(source_dir), "--baseline", str(baseline_path)],
        source_dir,
        0,
    )
    with tempfile.TemporaryDirectory(prefix="aisuite-p0-policy-") as temporary:
        root = pathlib.Path(temporary)
        architecture_change = copy.deepcopy(baseline)
        source_metrics = architecture_change["architectureMeasurements"]["source"]
        source_metrics["policyMutationProbe"] = 1
        current = root / "architecture-change.json"
        current.write_text(json.dumps(architecture_change, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        output = run_tool(
            [str(tool), "compare", "--baseline", str(baseline_path), "--current", str(current)],
            source_dir,
            0,
        )
        report = json.loads(output)
        if report.get("changedExternalContracts"):
            fail("architecture-only mutation was incorrectly treated as external-contract drift")

        external_change = copy.deepcopy(baseline)
        external_change["externalContract"]["protocol"]["identity"] = "invalid.drift"
        current.write_text(json.dumps(external_change, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        run_tool(
            [str(tool), "compare", "--baseline", str(baseline_path), "--current", str(current)],
            source_dir,
            1,
        )


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--source-dir", type=pathlib.Path, required=True)
    result.add_argument("--tool", type=pathlib.Path, required=True)
    result.add_argument("--baseline", type=pathlib.Path, required=True)
    result.add_argument("--roadmap", type=pathlib.Path, required=True)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    arguments = parser().parse_args(argv)
    source_dir = arguments.source_dir.resolve()
    baseline_path = arguments.baseline.resolve()
    baseline = load_json(baseline_path)
    validate_shape(baseline)
    validate_no_machine_paths_or_secrets(baseline)
    validate_roadmap(arguments.roadmap.resolve())
    validate_tool_policy(source_dir, arguments.tool.resolve(), baseline_path, baseline)
    print("Codex architecture-reduction P0 baseline policy passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
