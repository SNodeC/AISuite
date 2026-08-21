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
    "P1 — Complete reusable SNode.C connect and WebSocket composition",
    "P2 — Build the complete greenfield frontend beside the oracle",
    "P3 — Cut over all applications and transports, then remove the legacy frontend",
]

EXPECTED_FIXED_PHASE_SECTION_SHA256 = "f104860594e7474fdefe59eac2e89de6606b88f5b6114b5855e33379a7e03369"

EXPECTED_DEPENDENCY_GRAPH = """                         P0
                          |
               +----------+----------+
               |                     |
               v                     v
              P1                    P2
           SNode.C               AISuite
               |                     |
               +----------+----------+
                          |
                          v
                         P3
                       AISuite"""

EVIDENCE_CLASSES = {
    "generated-authority-derived",
    "build-or-install-derived",
    "executable-observation",
    "named-test-evidence",
    "owner-approved-declarative-contract",
    "inherited-dependency-evidence",
}

EXPECTED_CLI_SYNTAX = [
    "help",
    "quit",
    "reconnect",
    "snapshot",
    "replay <sequence>",
    "acquire",
    "release",
    "threads",
    "start [--cwd <path>] [--model <model>] [--model-provider <provider>] [--approval-policy <policy>] [--sandbox-mode <mode>] [--ephemeral]",
    "resume <thread-id> [--cwd <path>] [--model <model>] [--model-provider <provider>] [--approval-policy <policy>] [--sandbox-mode <mode>]",
    "new [thread-start-options] -- <prompt>",
    "new <prompt>",
    "read <thread-id>",
    "turn <thread-id> <prompt>",
    "interrupt <thread-id> <turn-id>",
    "raw <json>",
    "watch on",
    "watch off",
]

EXPECTED_PRODUCTION_REDUCTION_ROOTS = [
    "src/ai/openai/codex",
    "src/apps/codex-backend",
    "src/apps/codex-backend-client",
]

REQUIRED_BASELINE_INFRASTRUCTURE = {
    "docs/ai/openai/codex/architecture-reduction/README.md",
    "docs/ai/openai/codex/architecture-reduction/p0-baseline.json",
    "docs/ai/openai/codex/architecture-reduction/p0-baseline.md",
    "tests/policy/codex/CodexArchitectureReductionBaselineTest.py",
    "tests/policy/codex/fixtures/codex-backend-client-help.txt",
    "tools/codex/capture_architecture_baseline.py",
}

SELF_REFERENTIAL_INFRASTRUCTURE = {
    "docs/ai/openai/codex/architecture-reduction/p0-baseline.json",
    "docs/ai/openai/codex/architecture-reduction/p0-baseline.md",
}

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


def validate_evidence(record: Any, location: str) -> None:
    if not isinstance(record, dict):
        fail(f"blocking contract subsection is not an object: {location}")
    evidence = record.get("evidence")
    if not isinstance(evidence, dict):
        fail(f"blocking contract subsection lacks evidence: {location}")
    classes = evidence.get("classes")
    sources = evidence.get("sources")
    if (
        not isinstance(classes, list)
        or not classes
        or classes != sorted(set(classes))
        or not set(classes) <= EVIDENCE_CLASSES
    ):
        fail(f"blocking contract evidence classes are invalid: {location}")
    if (
        not isinstance(sources, list)
        or not sources
        or any(not isinstance(source, str) or not source for source in sources)
    ):
        fail(f"blocking contract evidence sources are absent: {location}")


def validate_aggregate(values: Any, location: str) -> None:
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
    if not isinstance(values, dict) or not aggregate_fields <= set(values):
        fail(f"source metrics are incomplete: {location}")
    if any(
        not isinstance(values[field], int) or values[field] < 0
        for field in aggregate_fields
    ):
        fail(f"source metrics are invalid: {location}")


def normalize_cli_fixture(text: str) -> str:
    lines = text.replace("\r\n", "\n").splitlines()
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines) + "\n"


def cli_syntax_from_help(help_text: str) -> list[str]:
    syntax: list[str] = []
    for line in help_text.splitlines():
        match = re.match(r"^  ([a-z][a-z0-9-]*)(?:\s|$)", line)
        if match:
            syntax.append(line.strip())
        elif syntax and line.startswith("    "):
            syntax[-1] += " " + line.strip()
    return syntax


def validate_shape(baseline: dict[str, Any], source_dir: pathlib.Path) -> None:
    if baseline.get("formatVersion") != 1:
        fail("baseline formatVersion must be 1")
    required = {
        "formatVersion",
        "provenance",
        "externalContract",
        "architectureMeasurements",
        "ownerLiveEvidence",
    }
    if set(baseline) != required:
        fail(
            "baseline must retain provenance and exactly three contract/evidence "
            f"categories: {sorted(baseline)}"
        )

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
        "dependencyProvenance",
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
    snodec_provenance = provenance.get("dependencyProvenance", {}).get("snodec", {})
    if (
        snodec_provenance.get("repository") != "SNodeC/snode.c"
        or not re.fullmatch(
            r"[0-9a-f]{40}", str(snodec_provenance.get("exactCommit", ""))
        )
        or not re.fullmatch(
            r"[0-9]+(?:\.[0-9]+){1,3}", str(snodec_provenance.get("version", ""))
        )
        or "checked-out dependency source" not in str(
            snodec_provenance.get("source", "")
        )
    ):
        fail("exact SNode.C repository/SHA/version/source provenance is incomplete")

    external = baseline["externalContract"]
    if external.get("blocking") is not True:
        fail("externalContract must be blocking")
    protocol = external.get("protocol", {})
    validate_evidence(protocol, "externalContract.protocol")
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

    public_cpp = external.get("publicCpp", {})
    validate_evidence(public_cpp, "externalContract.publicCpp")
    public_targets = public_cpp.get("requiredTargets", [])
    actual_public = {
        row.get("importedTarget"): row for row in public_targets if isinstance(row, dict)
    }
    if set(actual_public) != set(EXPECTED_PUBLIC_TARGETS):
        fail("required public imported-target set drifted")
    if public_cpp.get("requiredImportedTargetNames") != sorted(
        EXPECTED_PUBLIC_TARGETS
    ):
        fail("required imported-target name inventory is inconsistent")
    if (
        public_cpp.get("additiveTargetsPermitted") is not True
        or public_cpp.get("additivePublicHeadersPermitted") is not True
        or public_cpp.get("additiveAbiCompatibleSymbolsPermitted") is not True
    ):
        fail("compatible additive targets, headers, and symbols must be permitted")
    required_public_fields = {
        "importedTarget",
        "outputLibrary",
        "targetType",
        "version",
        "soversion",
        "installedPackageExportFile",
        "requiredInstalledPublicHeaderPaths",
    }
    required_header_union: set[str] = set()
    for imported, (_build_target, output_library) in EXPECTED_PUBLIC_TARGETS.items():
        row = actual_public[imported]
        if not required_public_fields <= set(row):
            fail(f"public target record is incomplete: {imported}")
        if row.get("outputLibrary") != output_library:
            fail(f"public target mapping drifted: {imported}")
        if (
            row.get("targetType") != "SHARED_LIBRARY"
            or row.get("version") != "0.4.0"
            or row.get("soversion") != 5
            or row.get("installedPackageExportFile") != "lib/cmake/AISuite/AISuiteTargets.cmake"
        ):
            fail(f"public target ABI/package metadata drifted: {imported}")
        headers = row.get("requiredInstalledPublicHeaderPaths")
        if (
            not isinstance(headers, list)
            or not headers
            or headers != sorted(set(headers))
            or any(
                not str(path).startswith("include/aisuite/ai/openai/codex/")
                for path in headers
            )
        ):
            fail(f"required public-header paths are incomplete: {imported}")
        required_header_union.update(headers)
    if public_cpp.get("requiredInstalledPublicHeaderPaths") != sorted(
        required_header_union
    ):
        fail("required installed public-header union is inconsistent")
    compatibility = " ".join(public_cpp.get("compatibility", [])).lower()
    if "does not claim to prove complete c++ api/abi compatibility" not in compatibility:
        fail("public C++ contract overstates the baseline tool's API/ABI proof")
    nonblocking_public = set(public_cpp.get("nonBlockingMeasurements", []))
    if not {
        "exported-symbol count and fingerprint",
        "NEEDED libraries",
        "binary size",
        "total public-header count",
        "internal and public dependency edges",
        "internal CMake build-target names",
    } <= nonblocking_public:
        fail("public C++ binary/dependency/header measurements are not non-blocking")

    architecture = baseline["architectureMeasurements"]
    if architecture.get("blocking") is not False or architecture.get("equalityGate") is not False:
        fail("architectureMeasurements must be explicitly non-blocking and not an equality gate")
    policy = str(architecture.get("comparisonPolicy", "")).lower()
    if "not" not in policy or "equality gate" not in policy:
        fail("architecture measurement policy does not explicitly reject equality gating")
    if "composite score" not in policy:
        fail("architecture measurement policy does not reject a composite score")
    determinism = architecture.get("determinism", {})
    if (
        determinism.get("unchangedCompleteCaptureEqualityRequired") is not True
        or determinism.get("capturePerformsTwoIndependentNormalizations") is not True
        or determinism.get("volatileHostObservationFields") != []
    ):
        fail("two unchanged complete captures are not required to match exactly")
    snodec_measurement = architecture.get("dependencies", {}).get("snodec", {})
    if any(
        snodec_measurement.get(key) != snodec_provenance.get(key)
        for key in ("repository", "exactCommit", "version", "source")
    ):
        fail("SNode.C provenance and architecture dependency evidence disagree")
    defaults = snodec_measurement.get("configuredConnectionDefaults", {})
    if (
        defaults.get("classification")
        != "inherited configurable SNode.C behavior"
        or defaults.get("readInactivityTimeoutSeconds") != 60
        or defaults.get("writeInactivityTimeoutSeconds") != 60
        or not {
            "CMakeCache.txt:SNODEC_READ_TIMEOUT",
            "CMakeCache.txt:SNODEC_WRITE_TIMEOUT",
            "src/net/CMakeLists.txt",
        }
        <= set(defaults.get("authorities", []))
    ):
        fail("configured SNode.C timeout provenance/evidence is incomplete")
    dependency_policy = str(
        architecture.get("dependencies", {}).get("comparisonPolicy", "")
    ).lower()
    if "reported" not in dependency_policy or "equality-gating" not in dependency_policy:
        fail("SNode.C revision changes must be reported without equality gating")

    application_cli = external.get("applicationCli", {})
    validate_evidence(application_cli, "externalContract.applicationCli")
    if application_cli.get("commandSyntax") != EXPECTED_CLI_SYNTAX:
        fail("application CLI command syntax drifted")
    executable_help = application_cli.get("executableHelp", {})
    fixture_relative = executable_help.get("fixturePath")
    if fixture_relative != "tests/policy/codex/fixtures/codex-backend-client-help.txt":
        fail("CLI executable-help fixture authority is incorrect")
    fixture_path = source_dir / fixture_relative
    try:
        fixture = normalize_cli_fixture(fixture_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError) as error:
        fail(f"CLI executable-help fixture is unavailable: {error}")
    fixture_hash = hashlib.sha256(fixture.encode("utf-8")).hexdigest()
    if (
        executable_help.get("normalizedHelpSha256") != fixture_hash
        or executable_help.get("normalizedHelpLineCount")
        != len(fixture.splitlines())
        or executable_help.get("normalizedHelpByteCount")
        != len(fixture.encode("utf-8"))
        or executable_help.get("commandSyntax") != EXPECTED_CLI_SYNTAX
        or cli_syntax_from_help(fixture) != EXPECTED_CLI_SYNTAX
        or executable_help.get("commandVocabulary")
        != sorted({syntax.split()[0] for syntax in EXPECTED_CLI_SYNTAX})
    ):
        fail("canonical CLI fixture and captured executable evidence disagree")

    semantics = external.get("backendAndFrontendSemantics", {})
    validate_evidence(semantics, "externalContract.backendAndFrontendSemantics")
    semantic_contract = semantics.get("contract", {})
    if set(semantic_contract) != {
        "appServerBorder",
        "backendCore",
        "frontendService",
        "frontendClientSdk",
        "synchronization",
        "projection",
        "controller",
    }:
        fail("backend/frontend semantic-contract boundaries are incomplete")

    oracle = external.get("replacementCompatibilityOracle", {})
    validate_evidence(oracle, "externalContract.replacementCompatibilityOracle")
    if (
        "temporary executable differential oracle" not in str(
            oracle.get("classification", "")
        )
        or "BackendCore" not in str(oracle.get("serverComparisonBorder", ""))
        or "canonical Frontend Protocol v1 output"
        not in str(oracle.get("serverComparisonBorder", ""))
        or "canonical Frontend Protocol v1 input"
        not in str(oracle.get("clientComparisonBorder", ""))
        or "immutable State" not in str(oracle.get("clientComparisonBorder", ""))
        or not str(oracle.get("productionCutoverPhase", "")).startswith("P3 ")
        or not str(oracle.get("legacyRemovalPhase", "")).startswith("P3")
        or not str(oracle.get("finalComparisonPhase", "")).startswith("P3 ")
        or not str(oracle.get("greenfieldConstructionPhase", "")).startswith("P2 ")
    ):
        fail("greenfield replacement differential-oracle borders are incomplete")
    required_domains = set(oracle.get("requiredDomains", []))
    for domain_fragment in (
        "all 105 methods",
        "all 26 expanded event families",
        "all 18 ThreadItem discriminators",
        "all 12 scopes",
        "capability negotiation plus Hello and Welcome",
        "replay and live Snapshot",
        "equal-sequence expanded event groups plus lower/higher/gapped sequence handling",
        "authentication and authorization failures",
        "item content accumulation and truncation",
        "queue and backpressure terminal behavior",
    ):
        if domain_fragment not in required_domains:
            fail(f"differential-oracle domain is missing: {domain_fragment}")
    if "differences in stable Frontend Protocol semantics fail" not in str(
        oracle.get("normalizationPolicy", "")
    ):
        fail("differential normalization is not narrowly constrained")
    legacy_oracle = architecture.get("legacyImplementationOracle", {})
    if (
        "temporary executable differential oracle"
        not in str(legacy_oracle.get("classification", ""))
        or "P3" not in str(legacy_oracle.get("removalGate", ""))
        or legacy_oracle.get("sourceIdentityAuthority")
        != "tracked path presence plus raw-content SHA-256"
        or legacy_oracle.get("identityStatus")
        != "all configured P0 oracle source identities are present and tracked"
    ):
        fail("current server/client implementation is not classified as a temporary oracle")
    source_identities = legacy_oracle.get("sourceIdentities", {})
    if set(source_identities) != {"client", "server"}:
        fail("legacy oracle source identities must distinguish client and server")
    identity_rows = [
        row
        for rows in source_identities.values()
        if isinstance(rows, list)
        for row in rows
    ]
    if (
        len(identity_rows) < 2
        or any(
            not isinstance(row, dict)
            or row.get("present") is not True
            or row.get("tracked") is not True
            or not re.fullmatch(r"[0-9a-f]{64}", str(row.get("sha256", "")))
            for row in identity_rows
        )
    ):
        fail("legacy oracle identities must be derived from tracked P0 source files")

    evidence = baseline["ownerLiveEvidence"]
    if evidence.get("evidenceType") != "owner-reported manual live acceptance":
        fail("owner live evidence is not labeled owner-reported manual live acceptance")
    if evidence.get("reproducedByCodex") is not False:
        fail("owner live evidence must not be represented as reproduced by Codex")
    observations = evidence.get("observations", [])
    if len(observations) != 22 or [row.get("ordinal") for row in observations] != list(range(1, 23)):
        fail("owner live evidence must retain the 22 ordered observations")

    transport = external.get("transport", {})
    validate_evidence(transport, "externalContract.transport")
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
        validate_evidence(row, f"externalContract.transport.{identifier}")
        if tuple(row.get(field) for field in row_fields) != EXPECTED_TRANSPORT_ROWS[identifier]:
            fail(f"external transport composition/security drifted: {identifier}")
        if row.get("peerMetadataAvailable") != EXPECTED_TRANSPORT_PEER_METADATA[identifier]:
            fail(f"external transport peer-metadata contract drifted: {identifier}")
        field_sources = row.get("evidence", {}).get("fieldSources", {})
        required_field_sources = {
            "namedInstancesAndDefaultState",
            "compiledFeatureSwitch",
            "carrierFamilyEncryptionFraming",
            "authenticationAndPeerPolicy",
        }
        if set(field_sources) != required_field_sources or any(
            not isinstance(values, list) or not values
            for values in field_sources.values()
        ):
            fail(f"transport row lacks per-field evidence: {identifier}")
    in_memory = transport.get("inMemory", {})
    if in_memory.get("classification") != "test-only" or in_memory.get("externalListener") is not False:
        fail("in-memory transport is not separately classified as test-only with no listener")
    if in_memory.get("partOfExternalApplicationTransportSet") is not False:
        fail("in-memory transport was incorrectly included in the eleven external transports")
    validate_evidence(in_memory, "externalContract.transport.inMemory")

    inherited = external.get("inheritedSNodeCConnectionDefaults", {})
    validate_evidence(
        inherited, "externalContract.inheritedSNodeCConnectionDefaults"
    )
    if (
        inherited.get("classification")
        != "inherited configurable SNode.C behavior, not hard-coded AISuite constants"
        or inherited.get("readInactivityTimeoutSeconds") != 60
        or inherited.get("writeInactivityTimeoutSeconds") != 60
        or "architectureMeasurements.dependencies.snodec"
        not in inherited.get("evidence", {}).get("sources", [])
    ):
        fail("inherited SNode.C timeout evidence is incomplete or misclassified")

    source = architecture.get("source", {})
    roots = source.get("roots", {})
    if set(roots) != EXPECTED_ROOTS:
        fail("architecture source-root inventory is incomplete")
    for root, categories in roots.items():
        if set(categories) != {"allTracked", "generated", "handWritten"}:
            fail(f"source categories are incomplete for {root}")
        for category, values in categories.items():
            validate_aggregate(values, f"{root}/{category}")
    scopes = source.get("scopeClassification", {})
    if set(scopes) != {
        "productionReductionSubject",
        "permanentCompatibilityAndConformanceSupport",
        "baselineInfrastructure",
    }:
        fail("source measurement scope classification is incomplete")
    production_scope = scopes["productionReductionSubject"]
    if production_scope.get("roots") != EXPECTED_PRODUCTION_REDUCTION_ROOTS:
        fail("production/reduction-subject roots drifted")
    validate_aggregate(
        production_scope.get("metrics"), "productionReductionSubject"
    )
    support_scope = scopes["permanentCompatibilityAndConformanceSupport"]
    if "non-production support" not in str(support_scope.get("classification", "")):
        fail("permanent compatibility/conformance support is misclassified")
    validate_aggregate(
        support_scope.get("metrics"), "permanentCompatibilityAndConformanceSupport"
    )
    infrastructure = scopes["baselineInfrastructure"]
    classified_paths = set(infrastructure.get("classifiedPaths", []))
    measured_paths = set(infrastructure.get("measuredPaths", []))
    excluded_paths = set(infrastructure.get("excludedFromEncodedMetrics", []))
    if not REQUIRED_BASELINE_INFRASTRUCTURE <= classified_paths:
        fail("P0 baseline-infrastructure path classification is incomplete")
    if source.get("baselineInfrastructurePaths") != sorted(classified_paths):
        fail("baseline-infrastructure path inventories disagree")
    if (
        not SELF_REFERENTIAL_INFRASTRUCTURE <= excluded_paths
        or measured_paths & excluded_paths
        or measured_paths | excluded_paths != classified_paths
    ):
        fail("self-referential P0 infrastructure is not excluded from encoded metrics")
    validate_aggregate(infrastructure.get("metrics"), "baselineInfrastructure")
    if infrastructure["metrics"].get("fileCount") != len(measured_paths):
        fail("baseline-infrastructure aggregate includes an unmeasured/self file")
    if "P0-only non-production infrastructure" not in str(
        infrastructure.get("classification", "")
    ):
        fail("baseline infrastructure is not separated from frontend reduction")
    hotspots = source.get("hotspots", {})
    if set(hotspots) != EXPECTED_HOTSPOTS:
        fail("complexity hotspot inventory is incomplete")
    for path, values in hotspots.items():
        if (
            set(values) != {"present", "trackedBytes", "trackedPhysicalLines"}
            or values.get("present") is not True
            or not isinstance(values.get("trackedBytes"), int)
            or not isinstance(values.get("trackedPhysicalLines"), int)
        ):
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
    observed_header_paths = {
        path
        for group in installed_headers["groups"].values()
        for path in group.get("paths", [])
    }
    if not required_header_union <= observed_header_paths:
        fail("installed inventory is missing a required existing public header")
    observed_public_targets = installed.get("observedPublicImportedTargets", [])
    observed_target_names = {
        row.get("importedTarget")
        for row in observed_public_targets
        if isinstance(row, dict)
    }
    if not set(EXPECTED_PUBLIC_TARGETS) <= observed_target_names:
        fail("installed inventory is missing a required existing public target")
    if any(
        not {
            "importedTarget",
            "targetType",
            "publicLinkInterfaceDependencies",
        }
        <= set(row)
        for row in observed_public_targets
    ):
        fail("observed installed public-target record is incomplete")
    target_measurements = installed.get("publicTargetMeasurements", [])
    if {row.get("importedTarget") for row in target_measurements} != set(EXPECTED_PUBLIC_TARGETS):
        fail("installed public-target measurements are incomplete")
    measured_by_imported = {
        row.get("importedTarget"): row for row in target_measurements
    }
    if any(
        measured_by_imported[imported].get("buildTarget") != build_target
        for imported, (build_target, _output) in EXPECTED_PUBLIC_TARGETS.items()
    ):
        fail("P0 CMake build-target measurements drifted")
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
    contract_coverage = tests.get("contractCoverage", {})
    if contract_coverage.get("missingP0ReferenceTests") != []:
        fail("committed P0 contract mapping has missing reference tests")
    if any(
        not row.get("tests") or row.get("missingP0ReferenceTests") != []
        for row in contract_coverage.get("automated", [])
    ):
        fail("committed P0 automated contract mapping is incomplete")
    transport_coverage = tests.get("transportCoverage", [])
    if {row.get("transportId") for row in transport_coverage} != EXPECTED_TRANSPORT_IDS:
        fail("transport coverage inventory is incomplete")
    for row in transport_coverage:
        automated = row.get("automatedTests", [])
        if (
            not automated
            or not set(automated) <= test_names
            or row.get("missingP0ReferenceTests") != []
        ):
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


def phase_body(text: str, phase: int) -> str:
    match = re.search(
        rf"^### P{phase} — .*?\n(.*?)(?=^### P{phase + 1} — |^## Phase dependencies)",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if match is None:
        fail(f"roadmap P{phase} body is missing")
    return match.group(1)


def require_phrases(text: str, phrases: Sequence[str], location: str) -> None:
    normalized_text = re.sub(r"\s+", " ", text)
    for phrase in phrases:
        if re.sub(r"\s+", " ", phrase) not in normalized_text:
            fail(f"{location} is missing fixed text: {phrase!r}")


def validate_phase_headings(text: str) -> None:
    headings = re.findall(r"^### (P[0-9]+ — .+)$", text, flags=re.MULTILINE)
    if headings != EXPECTED_PHASES:
        fail(
            "fixed P0–P3 phase headings are absent, duplicated, renamed, or "
            f"reordered: {headings}"
        )
    for phase in EXPECTED_PHASES:
        if text.count(f"### {phase}") != 1:
            fail(f"roadmap phase does not appear exactly once as a normative heading: {phase}")
    if re.search(r"^### P[4-7]\b", text, flags=re.MULTILINE):
        fail("roadmap must contain no P4, P5, P6, or P7 phase")


def validate_roadmap_text(text: str) -> None:
    validate_phase_headings(text)
    fixed = re.search(
        r"^## Final fixed phases\n(.*?)^## Phase dependencies and repository ownership",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if fixed is None:
        fail("roadmap fixed-phase scope section is missing")
    fingerprint = hashlib.sha256(fixed.group(1).encode("utf-8")).hexdigest()
    if fingerprint != EXPECTED_FIXED_PHASE_SECTION_SHA256:
        fail("fixed P0–P3 repository/scope text changed")

    graph = re.search(
        r"^## Phase dependencies and repository ownership\n\n```text\n(.*?)\n```",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if graph is None or graph.group(1) != EXPECTED_DEPENDENCY_GRAPH:
        fail("fixed P1/P2 parallel and P3 convergence graph drifted")
    dependency_rules = (
        "- P1 and P2 may proceed in parallel after P0 is merged.",
        "- P2 is transport-neutral and does not require P1.",
        "- P3 requires the merged results of both P1 and P2.",
        "- Only P1 modifies SNode.C.",
        "- P0, P2, and P3 modify AISuite.",
        "- `codex-ui` is not modified by P0–P3.",
    )
    for rule in dependency_rules:
        if re.sub(r"\s+", " ", text).count(re.sub(r"\s+", " ", rule)) != 1:
            fail(f"fixed dependency/ownership rule is absent or duplicated: {rule}")

    p0 = phase_body(text, 0)
    require_phrases(
        p0,
        (
            "Repository: AISuite",
            "retain the current frontend implementation as a temporary differential oracle",
            "P0 does not require the current internal architecture to survive",
            "does not implement P1, P2, or P3",
        ),
        "P0",
    )
    p1 = phase_body(text, 1)
    require_phrases(
        p1,
        (
            "Repository: SNode.C",
            "retain `SocketClient::connect()` as the public explicit connection operation",
            "do not add `connectOnce()`",
            "do not add a public `reconnect()` method",
            "permit a later explicit `connect()` on the same configured `SocketClient`",
            "preserve configured automatic retry and automatic reconnect",
            "prevent stale timers, callbacks, status handlers, or connection receivers",
            "`SocketClient` destruction or ordinary scope exit must not implicitly cancel an active shared asynchronous flow",
            "direct per-upgrade WebSocket server factory/context composition",
            "direct per-request WebSocket client factory/context composition",
            "P1 is a separate SNode.C pull request and is not implemented by P0",
        ),
        "P1",
    )
    p2 = phase_body(text, 2)
    require_phrases(
        p2,
        (
            "Repository: AISuite",
            "Prerequisite: P0 merged",
            "P2 may proceed in parallel with P1",
            "one shared Frontend Protocol authority/boundary",
            "one new transport-neutral frontend server core",
            "one new transport-neutral frontend client core",
            "permanent typed frontend model/occurrence design",
            "permanent snapshot/live/replay projection authority",
            "complete differential validation between the old and new implementations",
            "not depend on the old frontend server implementation",
            "depend on neither the old nor the new frontend server, `BackendCore`, nor SNode.C transport modules",
            "production applications continue using the old implementation throughout P2",
            "No old production implementation is deleted in P2",
        ),
        "P2",
    )
    if text.count("`connectOnce()`") != 1:
        fail("connectOnce must appear exactly once and only as a P1 prohibition")
    if text.count("do not add a public `reconnect()` method") != 1:
        fail("a public SNode.C reconnect API must be prohibited exactly once")

    p3 = phase_body(text, 3)
    require_phrases(
        p3,
        (
            "Prerequisites: P1 merged and P2 merged",
            "Repository: AISuite",
            "all server/client transport composition",
            "all eleven transports",
            "use the merged P1 SNode.C primitives",
            "delete both old frontend implementations",
            "temporary oracle build paths",
            "The old server/client code may be deleted only after",
            "perform the final comparison against P0",
        ),
        "P3",
    )

    require_phrases(
        text,
        (
            "There is no separate AISuite dependency-DAG cleanup phase and no preparatory protocol-extraction phase",
            "The old frontend server and client are temporary executable oracles through P2 and until P3 closure",
            "Broad “ignore differing JSON” normalization is forbidden",
            "A1.7c-2 follows P3",
            "`codex-ui` remains untouched throughout P0–P3",
            "P0–P3 do not implement the Qt UI",
            "There is no P4, P5, P6, or P7",
        ),
        "roadmap oracle/A1.7c-2 policy",
    )


def validate_roadmap(path: pathlib.Path, source_dir: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8")
    validate_roadmap_text(text)

    for stale_phase in (4, 5, 6):
        mutated = text.replace(
            "## Phase dependencies and repository ownership",
            f"### P{stale_phase} — Obsolete roadmap phase\n\n"
            "This stale phase must be rejected.\n\n"
            "## Phase dependencies and repository ownership",
            1,
        )
        try:
            validate_phase_headings(mutated)
        except AssertionError:
            pass
        else:
            fail(f"stale P{stale_phase} roadmap mutation was accepted")

    root_readme = (source_dir / "README.md").read_text(encoding="utf-8")
    require_phrases(
        root_readme,
        (
            "P0–P3 architecture-reduction roadmap",
            "P3 production cutover and legacy deletion, and finally A1.7c-2",
            "`codex-ui` remains untouched throughout P0–P3",
            "P0–P3 do not implement the Qt UI",
        ),
        "root README delivery order",
    )
    forbidden = ("A1.7c-2 immediately follows", "no additional PR is inserted")
    for markdown in (source_dir / "docs/ai/openai/codex").rglob("*.md"):
        content = markdown.read_text(encoding="utf-8")
        for phrase in forbidden:
            if phrase in content:
                fail(
                    f"obsolete A1.7c-2 ordering remains in "
                    f"{markdown.relative_to(source_dir).as_posix()}"
                )


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
    run_tool([str(tool), "self-test"], source_dir, 0)
    validation_output = run_tool(
        [
            str(tool),
            "validate-baseline",
            "--source-dir",
            str(source_dir),
            "--baseline",
            str(baseline_path),
        ],
        source_dir,
        0,
    )
    if (
        "source-only baseline validation passed" not in validation_output
        or "build/install/executable contract portions were not freshly verified"
        not in validation_output
    ):
        fail("source-only validation overstates build/install/executable verification")

    with tempfile.TemporaryDirectory(prefix="aisuite-p0-policy-") as temporary:
        root = pathlib.Path(temporary)

        def compare_capture(
            name: str, capture: dict[str, Any], expected: int
        ) -> dict[str, Any]:
            current = root / f"{name}.json"
            current.write_text(
                json.dumps(capture, sort_keys=True, indent=2) + "\n",
                encoding="utf-8",
            )
            output = run_tool(
                [
                    str(tool),
                    "compare",
                    "--baseline",
                    str(baseline_path),
                    "--current",
                    str(current),
                ],
                source_dir,
                expected,
            )
            try:
                report = json.loads(output)
            except json.JSONDecodeError as error:
                fail(f"compare did not emit JSON for {name}: {error}")
            if not isinstance(report, dict):
                fail(f"compare report is not an object for {name}")
            return report

        architecture_change = copy.deepcopy(baseline)
        source_metrics = architecture_change["architectureMeasurements"]["source"]
        source_metrics["policyMutationProbe"] = 1
        report = compare_capture("architecture-change", architecture_change, 0)
        if report.get("changedExternalContracts"):
            fail("architecture-only mutation was incorrectly treated as external-contract drift")

        legacy_reduction = copy.deepcopy(baseline)
        legacy_architecture = legacy_reduction["architectureMeasurements"]
        removed_private_target = "codex-backend-runtime-bridge"
        legacy_architecture["cmake"]["targets"] = [
            row
            for row in legacy_architecture["cmake"]["targets"]
            if row.get("name") != removed_private_target
        ]
        legacy_architecture["cmake"]["resolvedInProjectDependencyEdges"] = [
            edge
            for edge in legacy_architecture["cmake"][
                "resolvedInProjectDependencyEdges"
            ]
            if removed_private_target not in edge
        ]
        removed_private_library = "libaisuite-codex-backend-runtime.so"
        legacy_architecture["installed"]["installedLibraries"] = [
            row
            for row in legacy_architecture["installed"]["installedLibraries"]
            if row.get("name") != removed_private_library
        ]
        removed_hotspot = (
            "src/apps/codex-backend-client/FrontendWebSocketClient.cpp"
        )
        legacy_architecture["source"]["hotspots"][removed_hotspot] = {
            "present": False,
            "trackedBytes": None,
            "trackedPhysicalLines": None,
        }
        oracle_row = legacy_architecture["legacyImplementationOracle"][
            "sourceIdentities"
        ]["server"][0]
        removed_oracle_path = oracle_row["path"]
        oracle_row.clear()
        oracle_row.update(
            {"path": removed_oracle_path, "present": False, "tracked": False}
        )
        legacy_architecture["legacyImplementationOracle"][
            "identityStatus"
        ] = "one or more configured P0 oracle source identities are absent or untracked"
        report = compare_capture("legacy-reduction", legacy_reduction, 0)
        oracle_status = report.get("legacyImplementationOracleStatus", {})
        if (
            report.get("changedExternalContracts")
            or removed_private_target not in report.get("removedTargets", [])
            or removed_private_library not in report.get("removedBinaries", [])
            or removed_oracle_path
            not in oracle_status.get("missingSourceIdentities", [])
        ):
            fail("legacy architecture removal was not reported as non-blocking")

        dependency_change = copy.deepcopy(baseline)
        dependency_change["provenance"]["dependencyProvenance"]["snodec"][
            "exactCommit"
        ] = "0" * 40
        dependency_change["architectureMeasurements"]["dependencies"]["snodec"][
            "exactCommit"
        ] = "0" * 40
        report = compare_capture("snodec-change", dependency_change, 0)
        if (
            report.get("changedExternalContracts")
            or report.get("snodecDependencyChange", {}).get("changed") is not True
            or report.get("snodecDependencyChange", {}).get("blocking") is not False
        ):
            fail("SNode.C dependency change was not reported as non-blocking")

        additive = copy.deepcopy(baseline)
        public_cpp = additive["externalContract"]["publicCpp"]
        added_target = "AISuite::OpenAICodexFrontendProtocolProbe"
        added_header = (
            "include/aisuite/ai/openai/codex/frontend/ProtocolProbe.h"
        )
        public_cpp["requiredTargets"].append(
            {
                "importedTarget": added_target,
                "buildTarget": "ai-openai-codex-frontend-protocol-probe",
                "outputLibrary": "libaisuite-openai-codex-frontend-protocol-probe.so",
                "targetType": "SHARED_LIBRARY",
                "version": "0.1.0",
                "soversion": 2,
                "installedPackageExportFile": "lib/cmake/AISuite/AISuiteTargets.cmake",
                "requiredInstalledPublicHeaderPaths": [added_header],
            }
        )
        public_cpp["requiredTargets"].sort(key=lambda row: row["importedTarget"])
        public_cpp["requiredImportedTargetNames"].append(added_target)
        public_cpp["requiredImportedTargetNames"].sort()
        public_cpp["requiredInstalledPublicHeaderPaths"].append(added_header)
        public_cpp["requiredInstalledPublicHeaderPaths"].sort()
        installed = additive["architectureMeasurements"]["installed"]
        installed["observedPublicImportedTargets"].append(
            {
                "importedTarget": added_target,
                "targetType": "SHARED_LIBRARY",
                "publicLinkInterfaceDependencies": [],
            }
        )
        installed["observedPublicImportedTargets"].sort(
            key=lambda row: row["importedTarget"]
        )
        header_group = installed["installedPublicHeaders"]["groups"]["frontend"]
        header_group["paths"].append(added_header)
        header_group["paths"].sort()
        header_group["count"] += 1
        installed["installedPublicHeaders"]["totalCount"] += 1
        report = compare_capture("additive-target-header", additive, 0)
        if (
            report.get("changedExternalContracts")
            or report.get("addedPublicTargets") != [added_target]
            or report.get("addedPublicHeaders") != [added_header]
        ):
            fail("additive public target/header compatibility semantics are incorrect")

        removed_target = copy.deepcopy(baseline)
        missing_target = sorted(EXPECTED_PUBLIC_TARGETS)[0]
        removed_public = removed_target["externalContract"]["publicCpp"]
        removed_public["requiredTargets"] = [
            row
            for row in removed_public["requiredTargets"]
            if row["importedTarget"] != missing_target
        ]
        removed_public["requiredImportedTargetNames"] = [
            name
            for name in removed_public["requiredImportedTargetNames"]
            if name != missing_target
        ]
        removed_installed = removed_target["architectureMeasurements"]["installed"]
        removed_installed["observedPublicImportedTargets"] = [
            row
            for row in removed_installed["observedPublicImportedTargets"]
            if row["importedTarget"] != missing_target
        ]
        report = compare_capture("required-target-removal", removed_target, 1)
        if missing_target not in report.get("missingRequiredPublicTargets", []):
            fail("required public-target removal was not reported as blocking")

        removed_header = copy.deepcopy(baseline)
        missing_header = removed_header["externalContract"]["publicCpp"][
            "requiredInstalledPublicHeaderPaths"
        ][0]
        removed_header["externalContract"]["publicCpp"][
            "requiredInstalledPublicHeaderPaths"
        ].remove(missing_header)
        for row in removed_header["externalContract"]["publicCpp"][
            "requiredTargets"
        ]:
            if missing_header in row["requiredInstalledPublicHeaderPaths"]:
                row["requiredInstalledPublicHeaderPaths"].remove(missing_header)
        removed_header_inventory = removed_header["architectureMeasurements"][
            "installed"
        ]["installedPublicHeaders"]
        for group in removed_header_inventory["groups"].values():
            if missing_header in group["paths"]:
                group["paths"].remove(missing_header)
                group["count"] -= 1
                removed_header_inventory["totalCount"] -= 1
        report = compare_capture("required-header-removal", removed_header, 1)
        if missing_header not in report.get("missingRequiredPublicHeaders", []):
            fail("required public-header removal was not reported as blocking")

        external_change = copy.deepcopy(baseline)
        external_change["externalContract"]["protocol"]["identity"] = "invalid.drift"
        report = compare_capture("protocol-drift", external_change, 1)
        if not any(
            "protocol" in change
            for change in report.get("changedExternalContracts", [])
        ):
            fail("external protocol mutation was not reported as blocking drift")


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
    validate_shape(baseline, source_dir)
    validate_no_machine_paths_or_secrets(baseline)
    validate_roadmap(arguments.roadmap.resolve(), source_dir)
    validate_tool_policy(source_dir, arguments.tool.resolve(), baseline_path, baseline)
    print("Codex architecture-reduction P0 baseline policy passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
