#!/usr/bin/env python3
"""Capture and compare the immutable AISuite Codex P0 baseline.

Only Python's standard library is used.  Protocol facts come from the reviewed
JSON authorities, target facts come from the CMake File API, and binary facts
come from the installed ELF artifacts.  Architecture measurements are always
diagnostic; only external-contract drift is blocking.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from typing import Any, Iterable, Mapping, Sequence


FORMAT_VERSION = 1
REPOSITORY = "SNodeC/AISuite"
SOURCE_BRANCH = "master"
PR14_MERGE_COMMIT = "4c0cfbf99667fef64c9fed010d84031248ceaba2"
PR14_SOURCE_HEAD = "d524a6788631680e9fd86bda94ef49337a370d4c"

BASELINE_RELATIVE = pathlib.PurePosixPath(
    "docs/ai/openai/codex/architecture-reduction/p0-baseline.json"
)
ROADMAP_RELATIVE = pathlib.PurePosixPath(
    "docs/ai/openai/codex/architecture-reduction/README.md"
)
CLI_HELP_FIXTURE_RELATIVE = pathlib.PurePosixPath(
    "tests/policy/codex/fixtures/codex-backend-client-help.txt"
)

AUTHORITY_FILES = {
    "protocolSchema": "docs/ai/openai/codex/frontend-protocol-v1.schema.json",
    "protocolManifest": "docs/ai/openai/codex/frontend-protocol-v1.manifest.json",
    "generatedFixture": "tests/component/codex/fixtures/frontend-protocol-v1.generated.json",
    "reducerConformanceFixture": "tests/component/codex/fixtures/frontend-client-reducer/conformance.json",
    "ownerReviewedRegistry": "tools/frontend/frontend-registry-source.json",
    "cppClientBindings": "tools/frontend/cpp-client-bindings.json",
}

MEASUREMENT_ROOTS = (
    "src/ai/openai/codex",
    "src/apps/codex-backend",
    "src/apps/codex-backend-client",
    "tests/component/codex",
    "tests/policy/codex",
    "tools/frontend",
    "tools/codex",
    "docs/ai/openai/codex",
)

PRODUCTION_REDUCTION_ROOTS = (
    "src/ai/openai/codex",
    "src/apps/codex-backend",
    "src/apps/codex-backend-client",
)

BASELINE_INFRASTRUCTURE = frozenset(
    {
        "docs/ai/openai/codex/architecture-reduction/README.md",
        "docs/ai/openai/codex/architecture-reduction/p0-baseline.json",
        "docs/ai/openai/codex/architecture-reduction/p0-baseline.md",
        "tests/policy/codex/CodexArchitectureReductionBaselineTest.py",
        "tests/policy/codex/fixtures/codex-backend-client-help.txt",
        "tools/codex/capture_architecture_baseline.py",
    }
)

# These generated reports are classified as P0 infrastructure but deliberately
# excluded from every encoded byte/line metric.  In particular, the baseline
# JSON can never measure itself.
SELF_REFERENTIAL_BASELINE_INFRASTRUCTURE = frozenset(
    {
        "docs/ai/openai/codex/architecture-reduction/p0-baseline.json",
        "docs/ai/openai/codex/architecture-reduction/p0-baseline.md",
    }
)

EVIDENCE_CLASSES = frozenset(
    {
        "generated-authority-derived",
        "build-or-install-derived",
        "executable-observation",
        "named-test-evidence",
        "owner-approved-declarative-contract",
        "inherited-dependency-evidence",
    }
)

GENERATED_ARTIFACTS = frozenset(
    {
        "src/ai/openai/codex/frontend/GeneratedProtocol.h",
        "src/ai/openai/codex/frontend/GeneratedProtocolSchema.inc",
        "src/ai/openai/codex/frontend/client/GeneratedBindings.h",
        "src/ai/openai/codex/frontend/client/GeneratedFacades.cpp",
        "tests/component/codex/fixtures/frontend-protocol-v1.generated.json",
        "tests/component/codex/fixtures/frontend-client-reducer/conformance.json",
    }
)

HOTSPOTS = (
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
)

# These tracked implementation files identify the P0 server/client oracle at
# stable source borders.  Their hashes are architecture measurements only: a
# future change or removal is reported, never treated as external drift.
LEGACY_ORACLE_SOURCE_IDENTITIES = {
    "server": (
        "src/ai/openai/codex/frontend/FrontendService.cpp",
        "src/ai/openai/codex/frontend/EventCoalescer.cpp",
        "src/ai/openai/codex/frontend/EventJournal.cpp",
        "src/ai/openai/codex/frontend/UpdateBatch.cpp",
        "src/ai/openai/codex/frontend/detail/BackendProjectionBuilder.cpp",
    ),
    "client": (
        "src/ai/openai/codex/frontend/client/Client.cpp",
        "src/ai/openai/codex/frontend/client/State.cpp",
    ),
}

PUBLIC_TARGETS = (
    {
        "importedTarget": "AISuite::OpenAICodex",
        "buildTarget": "ai-openai-codex",
        "outputLibrary": "libaisuite-openai-codex.so",
        "headerGroup": "core",
    },
    {
        "importedTarget": "AISuite::OpenAICodexBackend",
        "buildTarget": "ai-openai-codex-backend",
        "outputLibrary": "libaisuite-openai-codex-backend.so",
        "headerGroup": "backend",
    },
    {
        "importedTarget": "AISuite::OpenAICodexFrontend",
        "buildTarget": "ai-openai-codex-frontend",
        "outputLibrary": "libaisuite-openai-codex-frontend.so",
        "headerGroup": "frontend",
    },
    {
        "importedTarget": "AISuite::OpenAICodexFrontendClient",
        "buildTarget": "ai-openai-codex-frontend-client",
        "outputLibrary": "libaisuite-openai-codex-frontend-client.so",
        "headerGroup": "frontendClient",
    },
)

FEATURE_SWITCHES = (
    "AISUITE_BUILD_APPS",
    "AISUITE_BUILD_TESTS",
    "AISUITE_BUILD_CODEX_FRONTEND_CLIENT",
    "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET",
    "AISUITE_ENABLE_CODEX_FRONTEND_TLS",
    "AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM",
)

EXPECTED_PROTOCOL = {
    "identity": "snodec.codex-frontend",
    "version": 1,
    "messageKinds": 8,
    "methods": 105,
    "native": 7,
    "provider": 86,
    "reverse": 12,
    "expandedEventFamilies": 26,
    "threadItemDiscriminators": 18,
    "scopes": 12,
}

EXPECTED_TRANSPORT_CONTRACT = {
    "unix-jsonl": ("Unix JSONL", "codex-backend", "codex-backend-client-unix", "always", "Unix stream", "Unix", "none", "JSONL", None, True),
    "ipv4-jsonl": ("IPv4 JSONL", "codex-backend-ipv4", "codex-backend-client-ipv4", "always", "TCP IPv4", "IPv4", "none", "JSONL", None, False),
    "ipv6-jsonl": ("IPv6 JSONL", "codex-backend-ipv6", "codex-backend-client-ipv6", "always", "TCP IPv6", "IPv6", "none", "JSONL", None, False),
    "ipv4-tls-jsonl": ("IPv4 TLS JSONL", "codex-backend-tls-ipv4", "codex-backend-client-tls-ipv4", "AISUITE_ENABLE_CODEX_FRONTEND_TLS", "TCP IPv4", "IPv4", "TLS", "JSONL", None, False),
    "ipv6-tls-jsonl": ("IPv6 TLS JSONL", "codex-backend-tls-ipv6", "codex-backend-client-tls-ipv6", "AISUITE_ENABLE_CODEX_FRONTEND_TLS", "TCP IPv6", "IPv6", "TLS", "JSONL", None, False),
    "rfcomm-jsonl": ("RFCOMM JSONL", "codex-backend-rfcomm", "codex-backend-client-rfcomm", "AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM", "Bluetooth RFCOMM", "RFCOMM", "none", "JSONL", None, False),
    "rfcomm-tls-jsonl": ("RFCOMM TLS JSONL", "codex-backend-rfcomm-tls", "codex-backend-client-rfcomm-tls", "AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM", "Bluetooth RFCOMM", "RFCOMM", "TLS", "JSONL", None, False),
    "websocket-ipv4": ("WebSocket IPv4", "codex-backend-websocket-ipv4", "codex-backend-client-websocket-ipv4", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET", "HTTP/TCP IPv4 upgrade", "IPv4", "none", "WebSocket text message", "codex", False),
    "websocket-ipv6": ("WebSocket IPv6", "codex-backend-websocket-ipv6", "codex-backend-client-websocket-ipv6", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET", "HTTP/TCP IPv6 upgrade", "IPv6", "none", "WebSocket text message", "codex", False),
    "wss-ipv4": ("WSS IPv4", "codex-backend-wss-ipv4", "codex-backend-client-wss-ipv4", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET;AISUITE_ENABLE_CODEX_FRONTEND_TLS", "HTTPS/TCP IPv4 upgrade", "IPv4", "TLS", "WebSocket text message", "codex", False),
    "wss-ipv6": ("WSS IPv6", "codex-backend-wss-ipv6", "codex-backend-client-wss-ipv6", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET;AISUITE_ENABLE_CODEX_FRONTEND_TLS", "HTTPS/TCP IPv6 upgrade", "IPv6", "TLS", "WebSocket text message", "codex", False),
}

EXPECTED_TRANSPORT_SECURITY = {
    "unix-jsonl": ("owner-only pathname; peer credentials where supported; verified-local policy; bearer fallback where required", ("transport", "localPeer", "unixUserId")),
    "ipv4-jsonl": ("loopback default; non-loopback plaintext requires explicit insecure override; remote bearer authentication", ("remote numeric address", "loopback", "transport", "encryption status")),
    "ipv6-jsonl": ("loopback default; non-loopback plaintext requires explicit insecure override; remote bearer authentication", ("remote numeric address", "loopback", "transport", "encryption status")),
    "ipv4-tls-jsonl": ("TLS plus remote bearer authentication", ("remote numeric address", "loopback", "transport", "encryption status")),
    "ipv6-tls-jsonl": ("TLS plus remote bearer authentication", ("remote numeric address", "loopback", "transport", "encryption status")),
    "rfcomm-jsonl": ("remote bearer authentication; Bluetooth pairing is not frontend authentication", ("Bluetooth address and RFCOMM channel", "unencrypted transport fact")),
    "rfcomm-tls-jsonl": ("TLS plus remote bearer authentication", ("Bluetooth address and RFCOMM channel", "encrypted transport fact")),
    "websocket-ipv4": ("origin and WebSocket upgrade policy plus remote bearer authentication", ("HTTP peer address", "origin", "unencrypted transport fact")),
    "websocket-ipv6": ("origin and WebSocket upgrade policy plus remote bearer authentication", ("HTTP peer address", "origin", "unencrypted transport fact")),
    "wss-ipv4": ("TLS, origin and WebSocket upgrade policy, plus remote bearer authentication", ("HTTPS peer address", "origin", "encrypted transport fact")),
    "wss-ipv6": ("TLS, origin and WebSocket upgrade policy, plus remote bearer authentication", ("HTTPS peer address", "origin", "encrypted transport fact")),
}

CLI_SYNTAX = (
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
)

LIFECYCLE_CONTRACT = {
    "commandLifetime": [
        "malformed local input ends only that command",
        "local pre-acceptance rejection ends or defers only that command",
        "normal Response(ok=false) ends only that command",
        "command failure does not close a valid connection",
        "command failure does not terminate the process",
    ],
    "physicalConnectionLifetime": [
        "transport failure closes the physical attachment",
        "fatal protocol or state failure closes the physical attachment",
        "pending accepted operations complete or fail exactly once",
        "retained SDK State becomes stale",
        "the process remains alive in Disconnected",
    ],
    "processLifetime": [
        "quit may terminate the process",
        "Ctrl-C or framework shutdown may terminate the process",
        "true stdin EOF after deterministic drain may terminate the process",
        "unrecoverable application infrastructure failure may terminate the process",
        "ordinary command failure may not terminate the process",
    ],
    "reconnect": [
        "physical reconnect is explicit",
        "there is no automatic physical reconnect",
        "there is no command retry or carry-over after attachment failure",
        "there is no automatic controller reacquisition",
        "the same SDK Client survives sequential physical connections",
        "valid continuity may use replay; invalid continuity uses a snapshot",
    ],
    "commandQueue": {
        "defaultMaximumCommands": 256,
        "defaultMaximumRetainedCommandBytes": 16 * 1024 * 1024,
        "zeroMeansZero": True,
        "overflowPolicy": "reject newest; never silently evict older entries",
        "disconnectedPolicy": "do not retain remote commands for later execution",
    },
    "eofDrain": [
        "accepted and queued work drains deterministically",
        "ordinary failures accumulate final nonzero status",
        "later queued commands still execute",
        "accepted commands are never retried",
    ],
    "stdin": [
        "EAGAIN and EWOULDBLOCK are not EOF",
        "only read() == 0 is EOF",
        "regular-file stdin remains rejected",
        "piped stdin remains supported",
    ],
    "excluded": [
        "raw asynchronous terminal-line redraw is not a P0 contract",
    ],
}

SEMANTIC_CONTRACT = {
    "appServerBorder": [
        "AppServerClient remains the typed provider boundary",
        "frontend clients never connect directly to the App Server",
        "raw App Server JSON is not exposed through Frontend Protocol v1",
    ],
    "backendCore": [
        "one authoritative backend state",
        "typed commands, events, and results",
        "bounded state and snapshots",
        "frontend sessions and observer subscriptions",
        "provider lifecycle and recovery",
        "controller and session facts",
    ],
    "frontendService": [
        "transport-neutral complete JSON object boundary",
        "authentication, authorization, and scope projection",
        "controller serialization and bounded outbound queues",
        "event journal, replay, snapshots, and live Snapshot barriers",
        "batching, coalescing, and capability advertisement",
    ],
    "frontendClientSdk": [
        "transport-neutral Client, Connection, and immutable State",
        "one Client survives sequential physical attachments",
        "at most one active physical attachment",
        "SDK owns Hello, authentication placement, IDs, correlation, synchronization, replay cursor, and state reduction",
        "application owns concrete physical transports",
    ],
    "synchronization": [
        "initial and explicit snapshot and replay remain supported",
        "live Snapshot while Ready is a transactional authoritative replacement",
        "live Snapshot does not fabricate another sync.complete",
        "pending command correlation survives a live Snapshot",
        "lower sequence is rejected",
        "equal sequence is accepted authoritatively",
        "higher sequence advances the durable cursor",
    ],
    "projection": [
        "exact entity identity is preserved without unrelated first or last substitution",
        "compact threadList.updated remains",
        "a thread-list page emits each returned identity once",
        "snapshots, live delivery, and replay share the reviewed item projector",
        "backend item spellings never become frontend discriminators",
        "accumulated content uses accumulated-value semantics",
        "producer and consumer validate expanded events",
    ],
    "controller": [
        "every new connection begins as observer",
        "controller acquisition is explicit",
        "controller ownership is not restored after reconnect",
        "disconnect releases controller ownership",
    ],
}


class BaselineError(RuntimeError):
    """Actionable deterministic capture failure."""


def _run(
    arguments: Sequence[str],
    *,
    cwd: pathlib.Path,
    description: str,
) -> str:
    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "LANG": "C"})
    try:
        completed = subprocess.run(
            list(arguments),
            cwd=cwd,
            env=environment,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except FileNotFoundError as error:
        raise BaselineError(f"{description}: required program is unavailable: {arguments[0]}") from error
    except subprocess.CalledProcessError as error:
        detail = error.stderr.strip() or error.stdout.strip() or f"exit status {error.returncode}"
        raise BaselineError(f"{description} failed: {detail}") from error
    return completed.stdout


def _run_observation(
    arguments: Sequence[str],
    *,
    cwd: pathlib.Path,
    description: str,
    accepted_returncodes: frozenset[int],
    input_text: str | None = None,
) -> str:
    """Run a deterministic executable observation with narrowly accepted exits."""

    environment = os.environ.copy()
    environment.update({"LC_ALL": "C", "LANG": "C"})
    try:
        completed = subprocess.run(
            list(arguments),
            cwd=cwd,
            env=environment,
            input=input_text,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except FileNotFoundError as error:
        raise BaselineError(
            f"{description}: required program is unavailable: {arguments[0]}"
        ) from error
    if completed.returncode not in accepted_returncodes:
        detail = completed.stdout.strip() or f"exit status {completed.returncode}"
        raise BaselineError(
            f"{description} returned {completed.returncode}, expected "
            f"{sorted(accepted_returncodes)}: {detail}"
        )
    return completed.stdout.replace("\r\n", "\n")


def _load_json(path: pathlib.Path, description: str) -> Any:
    if not path.is_file():
        raise BaselineError(f"{description} is missing: {path}")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise BaselineError(f"{description} is not valid UTF-8 JSON: {path}: {error}") from error


def _canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _canonical_json_sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(_canonical_json_bytes(_load_json(path, "JSON authority"))).hexdigest()


def _sha256_lines(values: Iterable[str]) -> str:
    normalized = "".join(f"{value}\n" for value in sorted(set(values)))
    return hashlib.sha256(normalized.encode("utf-8")).hexdigest()


def _require_directory(path: pathlib.Path, description: str) -> pathlib.Path:
    resolved = path.resolve()
    if not resolved.is_dir():
        raise BaselineError(f"{description} is missing or not a directory: {path}")
    return resolved


def _source_file(source_dir: pathlib.Path, relative: str | pathlib.PurePosixPath) -> pathlib.Path:
    path = source_dir / pathlib.PurePosixPath(relative)
    if not path.is_file():
        raise BaselineError(f"required repository file is missing: {relative}")
    return path


def _ownership(method: Mapping[str, Any]) -> str:
    if method.get("frontendNative") is True:
        return "native"
    if method.get("category") == "reverse_response":
        return "reverse"
    return "provider"


def _derive_protocol(source_dir: pathlib.Path) -> dict[str, Any]:
    registry = _load_json(
        _source_file(source_dir, AUTHORITY_FILES["ownerReviewedRegistry"]),
        "owner-reviewed frontend registry",
    )
    manifest = _load_json(
        _source_file(source_dir, AUTHORITY_FILES["protocolManifest"]),
        "generated frontend protocol manifest",
    )
    fixture = _load_json(
        _source_file(source_dir, AUTHORITY_FILES["generatedFixture"]),
        "generated frontend fixture",
    )

    methods = manifest.get("methods")
    capabilities = manifest.get("capabilities")
    if not isinstance(methods, list) or not isinstance(capabilities, list):
        raise BaselineError("protocol manifest lacks method or capability authorities")

    method_rows = sorted(
        (
            {
                "id": str(row["id"]),
                "method": str(row["method"]),
                "ownership": _ownership(row),
            }
            for row in methods
        ),
        key=lambda row: row["id"],
    )
    if len({row["id"] for row in method_rows}) != len(method_rows):
        raise BaselineError("protocol manifest contains duplicate stable method IDs")
    if len({row["method"] for row in method_rows}) != len(method_rows):
        raise BaselineError("protocol manifest contains duplicate wire method names")

    ownership_counts = collections.Counter(row["ownership"] for row in method_rows)
    thread_items = sorted(
        str(row["registryKey"]).rsplit(":", 1)[-1]
        for row in manifest.get("threadItemMappings", [])
    )
    event_families = sorted(str(value) for value in registry.get("eventFamilies", []))
    scopes = sorted(str(value) for value in registry.get("scopeStrings", []))
    message_kinds = sorted(str(value) for value in manifest.get("messageKinds", []))
    capability_rows = sorted(
        (
            {
                "key": str(row["key"]),
                "category": str(row["category"]),
                "staticRuntimeImplementation": bool(row.get("implementedByCurrentRuntime")),
            }
            for row in capabilities
        ),
        key=lambda row: row["key"],
    )
    capability_categories = {
        category: sorted(row["key"] for row in capability_rows if row["category"] == category)
        for category in sorted({row["category"] for row in capability_rows})
    }
    feature_complete_implemented = sorted(
        {
            row["key"]
            for row in capability_rows
            if row["category"] == "static_mechanism" and row["staticRuntimeImplementation"]
        }
        | {"multi_transport", "cpp_client_sdk"}
    )

    derived = {
        "identity": registry.get("protocolIdentity"),
        "version": registry.get("protocolVersion"),
        "messageKindCount": len(message_kinds),
        "messageKinds": message_kinds,
        "methodCount": len(method_rows),
        "methodOwnershipCounts": {
            key: ownership_counts.get(key, 0) for key in ("native", "provider", "reverse")
        },
        "methods": method_rows,
        "stableMethodIds": sorted(row["id"] for row in method_rows),
        "wireMethods": sorted(row["method"] for row in method_rows),
        "expandedEventFamilyCount": len(event_families),
        "expandedEventFamilies": event_families,
        "threadItemDiscriminatorCount": len(thread_items),
        "threadItemDiscriminators": thread_items,
        "scopeCount": len(scopes),
        "scopes": scopes,
        "capabilityCount": len(capability_rows),
        "capabilities": capability_rows,
        "capabilityCategories": capability_categories,
        "featureCompleteRuntimeCapabilityTruth": {
            "definedCount": len(capability_rows),
            "implementedCount": len(feature_complete_implemented),
            "permittedCount": len(feature_complete_implemented),
            "implemented": feature_complete_implemented,
            "notImplemented": sorted(
                row["key"] for row in capability_rows if row["key"] not in feature_complete_implemented
            ),
            "derivation": "13 generated static mechanisms plus topology-derived multi_transport and build-derived cpp_client_sdk",
        },
        "authorityCanonicalSha256": {
            name: _canonical_json_sha256(_source_file(source_dir, relative))
            for name, relative in sorted(AUTHORITY_FILES.items())
        },
        "authorityPaths": {name: relative for name, relative in sorted(AUTHORITY_FILES.items())},
        "evidence": {
            "classes": ["generated-authority-derived"],
            "sources": sorted(AUTHORITY_FILES.values()),
            "currentnessChecks": [
                "CodexFrontendProtocolGeneratedArtifactsGuardTest",
                "CodexFrontendClientBindingsCurrentnessTest",
            ],
        },
    }

    checks = {
        "identity": derived["identity"],
        "version": derived["version"],
        "messageKinds": derived["messageKindCount"],
        "methods": derived["methodCount"],
        "native": derived["methodOwnershipCounts"]["native"],
        "provider": derived["methodOwnershipCounts"]["provider"],
        "reverse": derived["methodOwnershipCounts"]["reverse"],
        "expandedEventFamilies": derived["expandedEventFamilyCount"],
        "threadItemDiscriminators": derived["threadItemDiscriminatorCount"],
        "scopes": derived["scopeCount"],
    }
    differences = {
        key: {"expected": expected, "actual": checks.get(key)}
        for key, expected in EXPECTED_PROTOCOL.items()
        if checks.get(key) != expected
    }
    if differences:
        raise BaselineError(
            "derived protocol values disagree with the owner-approved P0 expectations: "
            + json.dumps(differences, sort_keys=True)
        )
    if manifest.get("protocolIdentity") != derived["identity"] or manifest.get("protocolVersion") != derived["version"]:
        raise BaselineError("registry and generated manifest disagree on protocol identity/version")
    if fixture.get("counts", {}).get("methods") != len(method_rows):
        raise BaselineError("generated fixture method count disagrees with the manifest")
    if fixture.get("counts", {}).get("expandedEvents") != len(event_families):
        raise BaselineError("generated fixture event-family count disagrees with the registry")
    return derived


def _run_currentness_checks(source_dir: pathlib.Path) -> None:
    python = sys.executable
    _run(
        (
            python,
            "-B",
            "tools/codex/app_server_surface.py",
            "frontend-registry",
            "--manifest",
            "tools/codex/app-server-surface/0.144.6.json",
            "--registry",
            "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc",
            "--output",
            AUTHORITY_FILES["ownerReviewedRegistry"],
            "--check",
        ),
        cwd=source_dir,
        description="frontend registry currentness check",
    )
    _run(
        (
            python,
            "-B",
            "tools/frontend/generate_frontend_protocol.py",
            "--source",
            AUTHORITY_FILES["ownerReviewedRegistry"],
            "--manifest",
            AUTHORITY_FILES["protocolManifest"],
            "--header",
            "src/ai/openai/codex/frontend/GeneratedProtocol.h",
            "--schema-template",
            "tools/frontend/frontend-protocol-v1.schema.template.json",
            "--schema",
            AUTHORITY_FILES["protocolSchema"],
            "--schema-data",
            "src/ai/openai/codex/frontend/GeneratedProtocolSchema.inc",
            "--fixtures",
            AUTHORITY_FILES["generatedFixture"],
            "--check",
        ),
        cwd=source_dir,
        description="frontend protocol generated-authority currentness check",
    )
    _run(
        (python, "-B", "tools/frontend/generate_cpp_frontend_client.py", "--check"),
        cwd=source_dir,
        description="C++ frontend client binding currentness check",
    )


def _cache_reply(reply_dir: pathlib.Path, index: Mapping[str, Any]) -> dict[str, Any]:
    entry = index.get("reply", {}).get("cache-v2")
    if not isinstance(entry, dict) or not isinstance(entry.get("jsonFile"), str):
        raise BaselineError("CMake File API cache-v2 reply is absent; query it before configuring")
    payload = _load_json(reply_dir / entry["jsonFile"], "CMake File API cache reply")
    return {str(row["name"]): row.get("value") for row in payload.get("entries", [])}


def _file_api(build_dir: pathlib.Path) -> tuple[pathlib.Path, dict[str, Any], dict[str, Any], dict[str, Any]]:
    reply_dir = build_dir / ".cmake/api/v1/reply"
    if not reply_dir.is_dir():
        raise BaselineError("CMake File API reply is missing; create codemodel-v2, cache-v2, and toolchains-v1 queries before configure")
    indexes = sorted(reply_dir.glob("index-*.json"))
    if not indexes:
        raise BaselineError("CMake File API index is missing")
    index = _load_json(indexes[-1], "CMake File API index")
    reply = index.get("reply", {})
    codemodel_entry = reply.get("codemodel-v2")
    toolchains_entry = reply.get("toolchains-v1")
    if not isinstance(codemodel_entry, dict) or not isinstance(codemodel_entry.get("jsonFile"), str):
        raise BaselineError("CMake File API codemodel-v2 reply is absent")
    if not isinstance(toolchains_entry, dict) or not isinstance(toolchains_entry.get("jsonFile"), str):
        raise BaselineError("CMake File API toolchains-v1 reply is absent")
    codemodel = _load_json(reply_dir / codemodel_entry["jsonFile"], "CMake codemodel")
    toolchains = _load_json(reply_dir / toolchains_entry["jsonFile"], "CMake toolchains")
    cache = _cache_reply(reply_dir, index)
    return reply_dir, index, codemodel, {"toolchains": toolchains, "cache": cache}


def _bool_cache(cache: Mapping[str, Any], key: str) -> bool:
    value = str(cache.get(key, "")).upper()
    if value in {"ON", "TRUE", "YES", "1"}:
        return True
    if value in {"OFF", "FALSE", "NO", "0"}:
        return False
    raise BaselineError(f"configured cache value is absent or not boolean: {key}")


def _normalize_source_path(value: str, source_dir: pathlib.Path) -> str:
    candidate = pathlib.Path(value)
    if candidate.is_absolute():
        try:
            candidate = candidate.resolve().relative_to(source_dir)
        except ValueError as error:
            raise BaselineError(f"CMake target source path escapes the repository: {value}") from error
    return candidate.as_posix()


def _target_inventory(
    source_dir: pathlib.Path,
    build_dir: pathlib.Path,
    reply_dir: pathlib.Path,
    codemodel: Mapping[str, Any],
) -> list[dict[str, Any]]:
    configurations = codemodel.get("configurations", [])
    if len(configurations) != 1:
        raise BaselineError("P0 requires exactly one configured CMake codemodel configuration")
    configuration = configurations[0]
    target_refs = configuration.get("targets", [])
    names_by_id = {str(row["id"]): str(row["name"]) for row in target_refs}
    rows: list[dict[str, Any]] = []
    for reference in target_refs:
        name = str(reference["name"])
        payload = _load_json(reply_dir / str(reference["jsonFile"]), f"CMake target {name}")
        source_path = _normalize_source_path(str(payload.get("paths", {}).get("source", "")), source_dir)
        is_production = any(
            source_path == root or source_path.startswith(root + "/")
            for root in PRODUCTION_REDUCTION_ROOTS
        )
        if not is_production:
            continue
        dependencies = sorted(
            {
                names_by_id[str(dependency["id"])]
                for dependency in payload.get("dependencies", [])
                if str(dependency.get("id")) in names_by_id
            }
        )
        definitions = sorted(
            {
                str(definition["define"])
                for group in payload.get("compileGroups", [])
                for definition in group.get("defines", [])
                if "/" not in str(definition.get("define", ""))
            }
        )
        artifacts: list[str] = []
        for artifact in payload.get("artifacts", []):
            artifact_path = pathlib.Path(str(artifact["path"]))
            if artifact_path.is_absolute():
                try:
                    artifact_path = artifact_path.resolve().relative_to(build_dir)
                except ValueError as error:
                    raise BaselineError(f"CMake target artifact escapes the build tree: {artifact['path']}") from error
            artifacts.append(artifact_path.as_posix())
        artifacts.sort()
        install = payload.get("install") or {}
        destinations = sorted(
            {
                str(destination.get("path", ""))
                for destination in install.get("destinations", [])
                if destination.get("path")
            }
        )
        if name.endswith("-test-support"):
            classification = "test-support"
        elif name in {spec["buildTarget"] for spec in PUBLIC_TARGETS}:
            classification = "public-library"
        elif name in {"codex-backend", "codex-backend-client"}:
            classification = "installed-application"
        elif name == "ai-openai-codex-frontend-client-objects":
            classification = "production-implementation-helper"
        else:
            classification = "application-private-adapter-or-support"
        resolved_link_libraries: set[str] = set()
        for fragment in payload.get("link", {}).get("commandFragments", []):
            if fragment.get("role") != "libraries":
                continue
            value = str(fragment.get("fragment", ""))
            if not value or value.startswith("-Wl,-rpath"):
                continue
            if "/" in value and " " not in value:
                value = pathlib.PurePosixPath(value).name
            resolved_link_libraries.add(value)
        rows.append(
            {
                "name": name,
                "type": str(payload.get("type")),
                "classification": classification,
                "sourceDirectory": source_path,
                "codemodelSourceEntryCount": len(payload.get("sources", [])),
                "buildOutputs": artifacts,
                "resolvedInProjectBuildDependencies": dependencies,
                "resolvedLinkLibraryNames": sorted(resolved_link_libraries),
                "compileDefinitions": definitions,
                "installed": bool(destinations),
                "installDestinations": destinations,
            }
        )
    rows.sort(key=lambda row: row["name"])
    return rows


def _toolchain_provenance(index: Mapping[str, Any], context: Mapping[str, Any]) -> dict[str, Any]:
    toolchains = context["toolchains"].get("toolchains", [])
    cxx = next((row for row in toolchains if row.get("language") == "CXX"), None)
    if not isinstance(cxx, dict):
        raise BaselineError("CMake File API does not contain a CXX toolchain")
    compiler = cxx.get("compiler", {})
    cmake = index.get("cmake", {})
    generator = cmake.get("generator", {})
    cache = context["cache"]
    return {
        "projectVersion": str(cache.get("CMAKE_PROJECT_VERSION", "")),
        "configuredBuildType": str(cache.get("CMAKE_BUILD_TYPE", "")),
        "cxxCompiler": {
            "id": str(compiler.get("id", "")),
            "version": str(compiler.get("version", "")),
        },
        "cmakeVersion": str(cmake.get("version", {}).get("string", "")),
        "generator": str(generator.get("name", "")),
        "featureSwitches": {key: _bool_cache(cache, key) for key in FEATURE_SWITCHES},
        "enabledOptionalTransportFeatures": sorted(
            feature
            for feature, key in (
                ("RFCOMM", "AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM"),
                ("TLS", "AISUITE_ENABLE_CODEX_FRONTEND_TLS"),
                ("WebSocket", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET"),
            )
            if _bool_cache(cache, key)
        ),
    }


def _physical_lines(data: bytes) -> int:
    if not data:
        return 0
    return data.count(b"\n") + (0 if data.endswith(b"\n") else 1)


def _file_kinds(path: str) -> collections.Counter[str]:
    pure = pathlib.PurePosixPath(path)
    suffix = pure.suffix.lower()
    result: collections.Counter[str] = collections.Counter()
    if suffix in {".c", ".cc", ".cpp", ".cxx"}:
        result["cCppSourceCount"] += 1
    if suffix in {".h", ".hh", ".hpp", ".hxx", ".inc"}:
        result["cCppHeaderCount"] += 1
    if suffix == ".py":
        result["pythonCount"] += 1
    if pure.name == "CMakeLists.txt" or suffix == ".cmake":
        result["cmakeCount"] += 1
    if suffix == ".json":
        result["jsonCount"] += 1
    if suffix in {".md", ".markdown"}:
        result["markdownCount"] += 1
    return result


def _read_cmake_cache(build_dir: pathlib.Path, description: str) -> dict[str, str]:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        raise BaselineError(f"{description} CMakeCache.txt is missing")
    result: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("//", "#")) or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        key, separator, _type = key_and_type.partition(":")
        if separator:
            result[key] = value
    return result


def _normalize_snodec_repository(remote: str) -> str:
    normalized = remote.strip().removesuffix(".git").replace(":", "/")
    if not normalized.endswith("/SNodeC/snode.c"):
        raise BaselineError(
            "SNode.C checkout origin is not the required SNodeC/snode.c repository"
        )
    return "SNodeC/snode.c"


def _snodec_dependency_provenance(
    source_dir: pathlib.Path,
    build_dir: pathlib.Path,
    aisuite_cache: Mapping[str, Any],
) -> dict[str, Any]:
    source_dir = _require_directory(source_dir, "SNode.C source directory")
    build_dir = _require_directory(build_dir, "SNode.C build directory")
    commit = _run(
        ("git", "rev-parse", "HEAD"),
        cwd=source_dir,
        description="SNode.C exact dependency commit",
    ).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise BaselineError(f"SNode.C dependency commit is not a full SHA: {commit}")
    repository = _normalize_snodec_repository(
        _run(
            ("git", "remote", "get-url", "origin"),
            cwd=source_dir,
            description="SNode.C dependency repository",
        )
    )
    cache = _read_cmake_cache(build_dir, "SNode.C build")
    configured_source = pathlib.Path(cache.get("CMAKE_HOME_DIRECTORY", ""))
    if not configured_source.is_absolute() or configured_source.resolve() != source_dir:
        raise BaselineError(
            "SNode.C configured build does not use the supplied --snodec-source-dir"
        )
    version = cache.get("CMAKE_PROJECT_VERSION", "")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){1,3}", version):
        raise BaselineError(f"configured SNode.C version is invalid: {version!r}")
    try:
        read_timeout = int(cache["SNODEC_READ_TIMEOUT"])
        write_timeout = int(cache["SNODEC_WRITE_TIMEOUT"])
    except (KeyError, ValueError) as error:
        raise BaselineError(
            "configured SNode.C read/write inactivity defaults are missing or invalid"
        ) from error
    snodec_prefix = pathlib.Path(cache.get("CMAKE_INSTALL_PREFIX", ""))
    configured_package = pathlib.Path(str(aisuite_cache.get("snodec_DIR", "")))
    if not snodec_prefix.is_absolute() or not configured_package.is_absolute():
        raise BaselineError("SNode.C install-prefix/package provenance is incomplete")
    try:
        configured_package.resolve().relative_to(snodec_prefix.resolve())
    except ValueError as error:
        raise BaselineError(
            "AISuite was not configured from the supplied SNode.C build install prefix"
        ) from error
    version_files = sorted(configured_package.glob("*ConfigVersion.cmake"))
    if len(version_files) != 1:
        raise BaselineError(
            f"expected one installed SNode.C ConfigVersion file, found {len(version_files)}"
        )
    match = re.search(
        r'set\(PACKAGE_VERSION\s+"([0-9]+(?:\.[0-9]+){1,3})"\)',
        version_files[0].read_text(encoding="utf-8"),
    )
    if match is None or match.group(1) != version:
        raise BaselineError(
            "configured and installed SNode.C package versions do not match"
        )
    return {
        "repository": repository,
        "exactCommit": commit,
        "version": version,
        "source": "checked-out dependency source used for the configured build and install",
        "configuredConnectionDefaults": {
            "classification": "inherited configurable SNode.C behavior",
            "readInactivityTimeoutSeconds": read_timeout,
            "writeInactivityTimeoutSeconds": write_timeout,
            "authorities": [
                "CMakeCache.txt:SNODEC_READ_TIMEOUT",
                "CMakeCache.txt:SNODEC_WRITE_TIMEOUT",
                "src/net/CMakeLists.txt",
                "src/net/config/ConfigConnection.cpp",
                "src/core/socket/stream/SocketConnection.hpp",
            ],
        },
    }


def _aggregate_files(source_dir: pathlib.Path, paths: Sequence[str]) -> dict[str, int]:
    totals: collections.Counter[str] = collections.Counter(
        {
            "fileCount": 0,
            "trackedPhysicalLines": 0,
            "totalBytes": 0,
            "cCppSourceCount": 0,
            "cCppHeaderCount": 0,
            "pythonCount": 0,
            "cmakeCount": 0,
            "jsonCount": 0,
            "markdownCount": 0,
        }
    )
    for relative in paths:
        data = _source_file(source_dir, relative).read_bytes()
        totals["fileCount"] += 1
        totals["trackedPhysicalLines"] += _physical_lines(data)
        totals["totalBytes"] += len(data)
        totals.update(_file_kinds(relative))
    return dict(totals)


def _source_measurements(source_dir: pathlib.Path) -> dict[str, Any]:
    output = _run(
        ("git", "ls-files", "-z", "--", *MEASUREMENT_ROOTS),
        cwd=source_dir,
        description="tracked source inventory",
    )
    tracked = sorted(path for path in output.split("\0") if path)
    tracked_set = set(tracked)
    missing_infrastructure = sorted(path for path in BASELINE_INFRASTRUCTURE if path not in tracked_set)
    if missing_infrastructure:
        raise BaselineError(
            "required P0 baseline infrastructure is not tracked: "
            + ", ".join(missing_infrastructure)
        )

    roots: dict[str, Any] = {}
    for root in MEASUREMENT_ROOTS:
        files = [
            path
            for path in tracked
            if (path == root or path.startswith(root + "/"))
            and path not in BASELINE_INFRASTRUCTURE
        ]
        generated = [path for path in files if path in GENERATED_ARTIFACTS]
        handwritten = [path for path in files if path not in GENERATED_ARTIFACTS]
        roots[root] = {
            "allTracked": _aggregate_files(source_dir, files),
            "generated": _aggregate_files(source_dir, generated),
            "handWritten": _aggregate_files(source_dir, handwritten),
        }
    missing_generated = sorted(path for path in GENERATED_ARTIFACTS if path not in tracked_set)
    if missing_generated:
        raise BaselineError(f"required generated artifacts are not tracked: {', '.join(missing_generated)}")
    hotspots: dict[str, Any] = {}
    for relative in HOTSPOTS:
        if relative in tracked_set and (source_dir / relative).is_file():
            data = _source_file(source_dir, relative).read_bytes()
            hotspots[relative] = {
                "present": True,
                "trackedBytes": len(data),
                "trackedPhysicalLines": _physical_lines(data),
            }
        else:
            hotspots[relative] = {
                "present": False,
                "trackedBytes": None,
                "trackedPhysicalLines": None,
            }
    production_paths = sorted(
        path
        for path in tracked
        if any(path == root or path.startswith(root + "/") for root in PRODUCTION_REDUCTION_ROOTS)
    )
    support_paths = sorted(
        path
        for path in tracked
        if path not in production_paths and path not in BASELINE_INFRASTRUCTURE
    )
    infrastructure_paths = sorted(BASELINE_INFRASTRUCTURE)
    measured_infrastructure_paths = sorted(
        BASELINE_INFRASTRUCTURE - SELF_REFERENTIAL_BASELINE_INFRASTRUCTURE
    )
    return {
        "inventoryAuthority": "git ls-files",
        "lineMetric": "tracked physical lines",
        "scopeClassification": {
            "productionReductionSubject": {
                "roots": list(PRODUCTION_REDUCTION_ROOTS),
                "metrics": _aggregate_files(source_dir, production_paths),
            },
            "permanentCompatibilityAndConformanceSupport": {
                "classification": "tracked non-production support excluding P0-only infrastructure",
                "metrics": _aggregate_files(source_dir, support_paths),
            },
            "baselineInfrastructure": {
                "classification": "P0-only non-production infrastructure; reported separately from frontend reduction",
                "classifiedPaths": infrastructure_paths,
                "measuredPaths": measured_infrastructure_paths,
                "excludedFromEncodedMetrics": sorted(
                    SELF_REFERENTIAL_BASELINE_INFRASTRUCTURE
                ),
                "metrics": _aggregate_files(source_dir, measured_infrastructure_paths),
            },
        },
        "roots": roots,
        "generatedArtifactPaths": sorted(GENERATED_ARTIFACTS),
        "baselineInfrastructurePaths": infrastructure_paths,
        "hotspots": hotspots,
    }


def _legacy_oracle_measurement(source_dir: pathlib.Path) -> dict[str, Any]:
    configured_paths = sorted(
        path
        for paths in LEGACY_ORACLE_SOURCE_IDENTITIES.values()
        for path in paths
    )
    tracked_output = _run(
        ("git", "ls-files", "-z", "--", *configured_paths),
        cwd=source_dir,
        description="legacy frontend oracle source identity inventory",
    )
    tracked = {path for path in tracked_output.split("\0") if path}
    groups: dict[str, list[dict[str, Any]]] = {}
    all_present_and_tracked = True
    for group, paths in sorted(LEGACY_ORACLE_SOURCE_IDENTITIES.items()):
        records: list[dict[str, Any]] = []
        for relative in sorted(paths):
            path = source_dir / relative
            present = path.is_file()
            is_tracked = relative in tracked
            all_present_and_tracked = (
                all_present_and_tracked and present and is_tracked
            )
            record: dict[str, Any] = {
                "path": relative,
                "present": present,
                "tracked": is_tracked,
            }
            if present:
                data = path.read_bytes()
                record.update(
                    {
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "trackedBytes": len(data),
                        "trackedPhysicalLines": _physical_lines(data),
                    }
                )
            records.append(record)
        groups[group] = records
    return {
        "classification": "temporary executable differential oracle; not a design dependency of the greenfield cores",
        "productionStatusAtP0": "active behavioral reference",
        "sourceIdentityAuthority": "tracked path presence plus raw-content SHA-256",
        "sourceIdentities": groups,
        "identityStatus": (
            "all configured P0 oracle source identities are present and tracked"
            if all_present_and_tracked
            else "one or more configured P0 oracle source identities are absent or untracked"
        ),
        "removalGate": "P3 only after P2 differential closure plus transport, package, compatibility, and owner-live acceptance",
    }


def _readelf_dynamic(path: pathlib.Path, source_dir: pathlib.Path) -> tuple[str | None, list[str]]:
    output = _run(("readelf", "-d", str(path)), cwd=source_dir, description=f"ELF metadata for {path.name}")
    soname: str | None = None
    needed: list[str] = []
    for line in output.splitlines():
        match = re.search(r"\((SONAME|NEEDED)\).*\[([^]]+)]", line)
        if not match:
            continue
        if match.group(1) == "SONAME":
            soname = match.group(2)
        else:
            needed.append(match.group(2))
    return soname, sorted(set(needed))


def _exported_symbols(path: pathlib.Path, source_dir: pathlib.Path) -> tuple[int, str]:
    output = _run(
        ("nm", "-D", "--defined-only", "--format=posix", str(path)),
        cwd=source_dir,
        description=f"dynamic symbols for {path.name}",
    )
    symbols = sorted({line.split()[0].split("@", 1)[0] for line in output.splitlines() if line.split()})
    return len(symbols), _sha256_lines(symbols)


def _find_installed_file(install_dir: pathlib.Path, name: str) -> pathlib.Path:
    matches = sorted(path for path in install_dir.rglob(name) if path.is_file() or path.is_symlink())
    if len(matches) != 1:
        raise BaselineError(f"expected one installed {name}, found {len(matches)}")
    resolved = matches[0].resolve()
    try:
        resolved.relative_to(install_dir)
    except ValueError as error:
        raise BaselineError(f"installed artifact resolves outside the install tree: {name}") from error
    return resolved


def _installed_headers(install_dir: pathlib.Path) -> dict[str, list[str]]:
    include = install_dir / "include/aisuite/ai/openai/codex"
    if not include.is_dir():
        raise BaselineError("installed Codex public-header tree is missing")
    all_headers = sorted(
        path.relative_to(install_dir).as_posix()
        for path in include.rglob("*")
        if path.is_file() and path.suffix.lower() in {".h", ".hh", ".hpp", ".hxx", ".inc"}
    )
    groups = {"core": [], "backend": [], "frontend": [], "frontendClient": []}
    marker = "include/aisuite/ai/openai/codex/"
    for path in all_headers:
        tail = path[len(marker) :]
        if tail.startswith("backend/"):
            groups["backend"].append(path)
        elif tail.startswith("frontend/client/"):
            groups["frontendClient"].append(path)
        elif tail.startswith("frontend/"):
            groups["frontend"].append(path)
        else:
            groups["core"].append(path)
    return groups


def _validate_install_manifest(build_dir: pathlib.Path, install_dir: pathlib.Path) -> None:
    manifest = build_dir / "install_manifest.txt"
    if not manifest.is_file():
        raise BaselineError("CMake install_manifest.txt is missing; install the configured P0 build before capture")
    expected: set[str] = set()
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if not line:
            continue
        installed = pathlib.Path(line)
        try:
            relative = installed.relative_to(install_dir).as_posix()
        except ValueError as error:
            raise BaselineError(
                "install manifest contains a path outside the supplied install prefix; use exactly one fresh P0 prefix"
            ) from error
        expected.add(relative)
    actual = {
        path.relative_to(install_dir).as_posix()
        for path in install_dir.rglob("*")
        if path.is_file() or path.is_symlink()
    }
    if expected != actual:
        stale = sorted(actual - expected)
        missing = sorted(expected - actual)
        raise BaselineError(
            "install tree does not exactly match CMake install_manifest.txt; "
            f"stale={stale[:20]} missing={missing[:20]}"
        )


def _export_link_interfaces(
    source_dir: pathlib.Path,
    install_dir: pathlib.Path,
    cmake_package_hints: Mapping[str, Any],
) -> tuple[str, dict[str, list[str]], list[dict[str, Any]]]:
    matches = sorted(install_dir.rglob("AISuiteTargets.cmake"))
    if len(matches) != 1:
        raise BaselineError(f"expected one installed AISuiteTargets.cmake, found {len(matches)}")
    export_file = matches[0]
    with tempfile.TemporaryDirectory(prefix="aisuite-p0-export-query-") as temporary:
        root = pathlib.Path(temporary)
        cmake_lists = root / "CMakeLists.txt"
        result_file = root / "interfaces.txt"
        cmake_lists.write_text(
            "\n".join(
                (
                    "cmake_minimum_required(VERSION 3.18)",
                    "project(AISuiteP0InstalledTargetQuery LANGUAGES CXX)",
                    "find_package(AISuite CONFIG REQUIRED PATHS \"${AISUITE_PREFIX}\" NO_DEFAULT_PATH)",
                    "get_property(_targets DIRECTORY PROPERTY IMPORTED_TARGETS)",
                    "list(FILTER _targets INCLUDE REGEX \"^AISuite::\")",
                    "list(SORT _targets)",
                    "set(_result \"\")",
                    "foreach(_target IN LISTS _targets)",
                    "  get_target_property(_links \"${_target}\" INTERFACE_LINK_LIBRARIES)",
                    "  if(_links STREQUAL \"_links-NOTFOUND\")",
                    "    set(_links \"\")",
                    "  endif()",
                    "  get_target_property(_type \"${_target}\" TYPE)",
                    "  string(REPLACE \";\" \"|\" _links \"${_links}\")",
                    "  string(APPEND _result \"${_target}\\t${_type}\\t${_links}\\n\")",
                    "endforeach()",
                    "file(WRITE \"${AISUITE_RESULT_FILE}\" \"${_result}\")",
                    "",
                )
            ),
            encoding="utf-8",
        )
        command = [
            "cmake",
            "-S",
            str(root),
            "-B",
            str(root / "build"),
            f"-DAISUITE_PREFIX={install_dir}",
            f"-DAISUITE_RESULT_FILE={result_file}",
        ]
        for key in ("snodec_DIR", "nlohmann_json_DIR"):
            value = cmake_package_hints.get(key)
            if value and not str(value).endswith("-NOTFOUND"):
                command.append(f"-D{key}={value}")
        _run(
            command,
            cwd=source_dir,
            description="installed AISuite imported-target query",
        )
        interfaces: dict[str, list[str]] = {}
        observed_targets: list[dict[str, Any]] = []
        for line in result_file.read_text(encoding="utf-8").splitlines():
            fields = line.split("\t", 2)
            if len(fields) != 3:
                raise BaselineError("installed target query produced malformed output")
            target, target_type, encoded = fields
            dependencies = []
            for dependency in encoded.split("|") if encoded else []:
                if dependency.startswith("$<LINK_ONLY:") and dependency.endswith(">"):
                    dependency = dependency[len("$<LINK_ONLY:") : -1]
                if dependency:
                    dependencies.append(dependency)
            interfaces[target] = sorted(set(dependencies))
            observed_targets.append(
                {
                    "importedTarget": target,
                    "targetType": target_type,
                    "publicLinkInterfaceDependencies": interfaces[target],
                }
            )
    expected_targets = {str(spec["importedTarget"]) for spec in PUBLIC_TARGETS}
    missing = sorted(expected_targets - set(interfaces))
    if missing:
        raise BaselineError(
            "installed target query is missing required public targets: "
            + ", ".join(missing)
        )
    return (
        export_file.relative_to(install_dir).as_posix(),
        interfaces,
        sorted(observed_targets, key=lambda row: row["importedTarget"]),
    )


def _binary_strings(path: pathlib.Path, source_dir: pathlib.Path) -> set[str]:
    output = _run(("strings", "-a", str(path)), cwd=source_dir, description=f"strings for {path.name}")
    return {line.strip() for line in output.splitlines() if line.strip()}


def _installed_inventory(
    source_dir: pathlib.Path,
    install_dir: pathlib.Path,
    project_version: str,
    cmake_package_hints: Mapping[str, Any],
) -> tuple[dict[str, Any], list[dict[str, Any]], dict[str, set[str]]]:
    header_groups = _installed_headers(install_dir)
    export_file, link_interfaces, observed_public_targets = _export_link_interfaces(
        source_dir, install_dir, cmake_package_hints
    )
    observed_target_by_name = {
        str(row["importedTarget"]): row for row in observed_public_targets
    }
    libraries: list[dict[str, Any]] = []
    public_records: list[dict[str, Any]] = []
    soversion_values: set[str] = set()
    for spec in PUBLIC_TARGETS:
        logical_name = str(spec["outputLibrary"])
        binary = _find_installed_file(install_dir, logical_name)
        version_prefix = logical_name + "."
        if not binary.name.startswith(version_prefix):
            raise BaselineError(
                f"installed shared library lacks a VERSION suffix: {binary.name}"
            )
        installed_version = binary.name[len(version_prefix) :]
        if installed_version != project_version:
            raise BaselineError(
                f"installed VERSION for {logical_name} is {installed_version}, "
                f"but the configured project version is {project_version}"
            )
        soname, needed = _readelf_dynamic(binary, source_dir)
        if not soname:
            raise BaselineError(f"installed shared library has no SONAME: {logical_name}")
        symbol_count, symbol_hash = _exported_symbols(binary, source_dir)
        soname_prefix = logical_name + "."
        if not soname.startswith(soname_prefix):
            raise BaselineError(f"unexpected SONAME for {logical_name}: {soname}")
        soversion_text = soname[len(soname_prefix) :]
        soversion: int | str = int(soversion_text) if soversion_text.isdigit() else soversion_text
        soversion_values.add(str(soversion))
        headers = header_groups[str(spec["headerGroup"])]
        observed_target = observed_target_by_name[str(spec["importedTarget"])]
        if observed_target.get("targetType") != "SHARED_LIBRARY":
            raise BaselineError(
                f"installed target {spec['importedTarget']} has unexpected type "
                f"{observed_target.get('targetType')}"
            )
        libraries.append(
            {
                "name": logical_name,
                "classification": "public-library",
                "importedTarget": spec["importedTarget"],
                "fileSizeBytes": binary.stat().st_size,
                "soname": soname,
                "neededLibraries": needed,
                "exportedDynamicSymbolCount": symbol_count,
                "exportedDynamicSymbolNameSetSha256": symbol_hash,
                "installedPublicHeaderCount": len(headers),
                "installedPublicHeaderPaths": headers,
            }
        )
        public_records.append(
            {
                "importedTarget": spec["importedTarget"],
                "outputLibrary": logical_name,
                "targetType": observed_target["targetType"],
                "version": installed_version,
                "soversion": soversion,
                "installedPackageExportFile": export_file,
                "requiredInstalledPublicHeaderPaths": headers,
            }
        )
    known_private_build_targets = {
        "libaisuite-codex-backend-runtime.so": "codex-backend-runtime-bridge",
        "libsnodec-websocket-codex-server.so": "codex-backend-websocket-subprotocol",
    }
    public_library_names = {str(spec["outputLibrary"]) for spec in PUBLIC_TARGETS}
    additional_logical_libraries = sorted(
        {
            path.name
            for path in install_dir.rglob("*.so")
            if (path.is_file() or path.is_symlink())
            and path.name not in public_library_names
        }
    )
    for logical_name in additional_logical_libraries:
        binary = _find_installed_file(install_dir, logical_name)
        soname, needed = _readelf_dynamic(binary, source_dir)
        if not soname:
            raise BaselineError(
                f"installed additional shared library has no SONAME: {logical_name}"
            )
        symbol_count, symbol_hash = _exported_symbols(binary, source_dir)
        record = {
            "name": logical_name,
            "classification": (
                "application-private-runtime-library"
                if logical_name in known_private_build_targets
                else "additional-installed-shared-library"
            ),
            "fileSizeBytes": binary.stat().st_size,
            "soname": soname,
            "neededLibraries": needed,
            "exportedDynamicSymbolCount": symbol_count,
            "exportedDynamicSymbolNameSetSha256": symbol_hash,
            "installedPublicHeaderCount": 0,
            "installedPublicHeaderPaths": [],
        }
        if logical_name in known_private_build_targets:
            record["buildTarget"] = known_private_build_targets[logical_name]
        libraries.append(record)
    if len(soversion_values) != 1:
        raise BaselineError("public Codex libraries do not share one SOVERSION")

    executables: list[dict[str, Any]] = []
    binary_strings: dict[str, set[str]] = {}
    for name in ("codex-backend", "codex-backend-client"):
        binary = _find_installed_file(install_dir, name)
        soname, needed = _readelf_dynamic(binary, source_dir)
        if soname is not None:
            raise BaselineError(f"installed executable unexpectedly has a SONAME: {name}")
        executables.append(
            {
                "name": name,
                "fileSizeBytes": binary.stat().st_size,
                "neededLibraries": needed,
            }
        )
        binary_strings[name] = _binary_strings(binary, source_dir)

    installed_files = sorted(
        path.relative_to(install_dir).as_posix()
        for path in install_dir.rglob("*")
        if path.is_file() or path.is_symlink()
    )
    package_files = sorted(
        path for path in installed_files if "/cmake/AISuite/" in path or path.startswith("lib/cmake/AISuite/")
    )
    inventory = {
        "installTreeClassification": "temporary P0 install prefix; paths normalized relative to the prefix",
        "installedFiles": installed_files,
        "installedLibraries": sorted(libraries, key=lambda row: row["name"]),
        "installedExecutables": executables,
        "installedPublicHeaders": {
            "totalCount": sum(len(paths) for paths in header_groups.values()),
            "groups": {
                name: {"count": len(paths), "paths": paths}
                for name, paths in sorted(header_groups.items())
            },
        },
        "installedCMakePackageFiles": package_files,
        "observedPublicImportedTargets": observed_public_targets,
        "publicLinkInterfaces": link_interfaces,
    }
    return inventory, sorted(public_records, key=lambda row: row["importedTarget"]), binary_strings


def _normalize_cli_help(output: str) -> str:
    lines = output.replace("\r\n", "\n").splitlines()
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines) + "\n"


def _cli_syntax_from_help(help_text: str) -> list[str]:
    syntax: list[str] = []
    for line in help_text.splitlines():
        match = re.match(r"^  ([a-z][a-z0-9-]*)(?:\s|$)", line)
        if match:
            syntax.append(line.strip())
        elif syntax and line.startswith("    "):
            syntax[-1] += " " + line.strip()
    return syntax


def _installed_cli_evidence(
    source_dir: pathlib.Path,
    install_dir: pathlib.Path,
) -> dict[str, Any]:
    fixture_path = _source_file(source_dir, CLI_HELP_FIXTURE_RELATIVE)
    expected = _normalize_cli_help(fixture_path.read_text(encoding="utf-8"))
    client = _find_installed_file(install_dir, "codex-backend-client")
    observed = _normalize_cli_help(
        _run_observation(
            (str(client), "--log-level", "2", "--quiet=true"),
            cwd=source_dir,
            description="installed codex-backend-client interactive help observation",
            accepted_returncodes=frozenset({0}),
            input_text="help\nquit\n",
        )
    )
    if observed != expected:
        raise BaselineError(
            "installed codex-backend-client interactive help differs from the canonical fixture"
        )
    syntax = _cli_syntax_from_help(observed)
    if syntax != list(CLI_SYNTAX):
        raise BaselineError(
            "installed codex-backend-client command syntax differs from the P0 contract"
        )
    return {
        "fixturePath": CLI_HELP_FIXTURE_RELATIVE.as_posix(),
        "normalizedHelpSha256": hashlib.sha256(observed.encode("utf-8")).hexdigest(),
        "normalizedHelpLineCount": len(observed.splitlines()),
        "normalizedHelpByteCount": len(observed.encode("utf-8")),
        "commandSyntax": syntax,
        "commandVocabulary": sorted({line.split()[0] for line in syntax}),
    }


def _configured_instance_observation(
    source_dir: pathlib.Path,
    executable: pathlib.Path,
) -> dict[str, bool]:
    output = _run_observation(
        (str(executable), "--log-level", "2", "--quiet=true", "--help=expanded"),
        cwd=source_dir,
        description=f"installed {executable.name} named-instance observation",
        accepted_returncodes=frozenset({2}),
    )
    if "Instances (disabled):" not in output:
        raise BaselineError(
            f"installed {executable.name} help did not expose disabled instances"
        )
    instances: dict[str, bool] = {}
    for line in output.splitlines():
        match = re.fullmatch(
            r"([a-z0-9-]+) \[OPTIONS\] \[SECTIONS\]( DISABLED)?", line
        )
        if match:
            instances[match.group(1)] = match.group(2) is None
    return dict(sorted(instances.items()))


def _installed_transport_observation(
    source_dir: pathlib.Path,
    install_dir: pathlib.Path,
) -> dict[str, dict[str, bool]]:
    return {
        "serverInstances": _configured_instance_observation(
            source_dir, _find_installed_file(install_dir, "codex-backend")
        ),
        "clientInstances": _configured_instance_observation(
            source_dir, _find_installed_file(install_dir, "codex-backend-client")
        ),
    }


def _installed_timeout_observation(
    source_dir: pathlib.Path,
    install_dir: pathlib.Path,
) -> dict[str, int]:
    client = _find_installed_file(install_dir, "codex-backend-client")
    output = _run_observation(
        (str(client), "codex-backend-client-unix", "connection", "--help"),
        cwd=source_dir,
        description="installed SNode.C connection-timeout help observation",
        accepted_returncodes=frozenset({2}),
    )
    result: dict[str, int] = {}
    for key, option in (
        ("readInactivityTimeoutSeconds", "read-timeout"),
        ("writeInactivityTimeoutSeconds", "write-timeout"),
    ):
        match = re.search(rf"--{option}[^\n]*\[([0-9]+)\]", output)
        if match is None:
            raise BaselineError(
                f"installed connection help did not expose --{option} default"
            )
        result[key] = int(match.group(1))
    return result


def _transport_matrix(
    binary_strings: Mapping[str, set[str]],
    instance_observation: Mapping[str, Mapping[str, bool]],
    feature_switches: Mapping[str, bool],
) -> list[dict[str, Any]]:
    rows = [
        ("unix-jsonl", "Unix JSONL", "codex-backend", "codex-backend-client-unix", "always", "Unix stream", "Unix", False, "JSONL", True,
         "owner-only pathname; peer credentials where supported; verified-local policy; bearer fallback where required", ["transport", "localPeer", "unixUserId"],
         ["CodexFrontendNativeTransportTest", "CodexBackendUnixAcceptanceTest", "CodexBackendClientUnixAcceptanceTest", "CodexBackendClientRealBackendAcceptanceTest", "CodexBackendClientThreadWorkflowAcceptanceTest"]),
        ("ipv4-jsonl", "IPv4 JSONL", "codex-backend-ipv4", "codex-backend-client-ipv4", "always", "TCP IPv4", "IPv4", False, "JSONL", False,
         "loopback default; non-loopback plaintext requires explicit insecure override; remote bearer authentication", ["remote numeric address", "loopback", "transport", "encryption status"],
         ["CodexFrontendNativeTransportTest", "CodexBackendClientIpv4AcceptanceTest"]),
        ("ipv6-jsonl", "IPv6 JSONL", "codex-backend-ipv6", "codex-backend-client-ipv6", "always", "TCP IPv6", "IPv6", False, "JSONL", False,
         "loopback default; non-loopback plaintext requires explicit insecure override; remote bearer authentication", ["remote numeric address", "loopback", "transport", "encryption status"],
         ["CodexFrontendNativeTransportTest", "CodexBackendClientIpv6AcceptanceTest"]),
        ("ipv4-tls-jsonl", "IPv4 TLS JSONL", "codex-backend-tls-ipv4", "codex-backend-client-tls-ipv4", "AISUITE_ENABLE_CODEX_FRONTEND_TLS", "TCP IPv4", "IPv4", True, "JSONL", False,
         "TLS plus remote bearer authentication", ["remote numeric address", "loopback", "transport", "encryption status"],
         ["CodexFrontendNativeTransportTest", "CodexBackendClientTlsIpv4AcceptanceTest"]),
        ("ipv6-tls-jsonl", "IPv6 TLS JSONL", "codex-backend-tls-ipv6", "codex-backend-client-tls-ipv6", "AISUITE_ENABLE_CODEX_FRONTEND_TLS", "TCP IPv6", "IPv6", True, "JSONL", False,
         "TLS plus remote bearer authentication", ["remote numeric address", "loopback", "transport", "encryption status"],
         ["CodexFrontendNativeTransportTest", "CodexBackendClientTlsIpv6AcceptanceTest"]),
        ("rfcomm-jsonl", "RFCOMM JSONL", "codex-backend-rfcomm", "codex-backend-client-rfcomm", "AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM", "Bluetooth RFCOMM", "RFCOMM", False, "JSONL", False,
         "remote bearer authentication; Bluetooth pairing is not frontend authentication", ["Bluetooth address and RFCOMM channel", "unencrypted transport fact"],
         ["CodexFrontendNativeTransportTest", "CodexBackendUnixCliCompatibilityTest", "CodexBackendClientTransportCompositionTest", "CodexBackendClientAuthenticationTest"]),
        ("rfcomm-tls-jsonl", "RFCOMM TLS JSONL", "codex-backend-rfcomm-tls", "codex-backend-client-rfcomm-tls", "AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM", "Bluetooth RFCOMM", "RFCOMM", True, "JSONL", False,
         "TLS plus remote bearer authentication", ["Bluetooth address and RFCOMM channel", "encrypted transport fact"],
         ["CodexFrontendNativeTransportTest", "CodexBackendUnixCliCompatibilityTest", "CodexBackendClientTransportCompositionTest", "CodexBackendClientAuthenticationTest"]),
        ("websocket-ipv4", "WebSocket IPv4", "codex-backend-websocket-ipv4", "codex-backend-client-websocket-ipv4", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET", "HTTP/TCP IPv4 upgrade", "IPv4", False, "WebSocket text message", False,
         "origin and WebSocket upgrade policy plus remote bearer authentication", ["HTTP peer address", "origin", "unencrypted transport fact"],
         ["CodexFrontendWebHttpIntegrationTest", "CodexFrontendWebSocketIntegrationTest", "CodexBackendClientWebSocketIpv4AcceptanceTest"]),
        ("websocket-ipv6", "WebSocket IPv6", "codex-backend-websocket-ipv6", "codex-backend-client-websocket-ipv6", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET", "HTTP/TCP IPv6 upgrade", "IPv6", False, "WebSocket text message", False,
         "origin and WebSocket upgrade policy plus remote bearer authentication", ["HTTP peer address", "origin", "unencrypted transport fact"],
         ["CodexBackendClientWebSocketIpv6AcceptanceTest"]),
        ("wss-ipv4", "WSS IPv4", "codex-backend-wss-ipv4", "codex-backend-client-wss-ipv4", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET;AISUITE_ENABLE_CODEX_FRONTEND_TLS", "HTTPS/TCP IPv4 upgrade", "IPv4", True, "WebSocket text message", False,
         "TLS, origin and WebSocket upgrade policy, plus remote bearer authentication", ["HTTPS peer address", "origin", "encrypted transport fact"],
         ["CodexFrontendWebSocketTlsIntegrationTest", "CodexBackendClientWssIpv4AcceptanceTest"]),
        ("wss-ipv6", "WSS IPv6", "codex-backend-wss-ipv6", "codex-backend-client-wss-ipv6", "AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET;AISUITE_ENABLE_CODEX_FRONTEND_TLS", "HTTPS/TCP IPv6 upgrade", "IPv6", True, "WebSocket text message", False,
         "TLS, origin and WebSocket upgrade policy, plus remote bearer authentication", ["HTTPS peer address", "origin", "encrypted transport fact"],
         ["CodexBackendClientWssIpv6AcceptanceTest"]),
    ]
    server_strings = binary_strings.get("codex-backend", set())
    client_strings = binary_strings.get("codex-backend-client", set())
    observed_servers = instance_observation.get("serverInstances", {})
    observed_clients = instance_observation.get("clientInstances", {})
    result: list[dict[str, Any]] = []
    for identifier, name, server, client, feature, carrier, family, encrypted, framing, enabled, authentication, peer, tests in rows:
        if server not in server_strings:
            raise BaselineError(f"installed codex-backend does not contain configured named instance {server}")
        if client not in client_strings:
            raise BaselineError(f"installed codex-backend-client does not contain configured named instance {client}")
        if server not in observed_servers or client not in observed_clients:
            raise BaselineError(
                f"installed executable help does not expose transport instances {server}/{client}"
            )
        if bool(observed_servers[server]) != enabled or bool(observed_clients[client]) != enabled:
            raise BaselineError(
                f"installed executable help default state drifted for {identifier}"
            )
        for switch in feature.split(";"):
            if switch != "always" and feature_switches.get(switch) is not True:
                raise BaselineError(
                    f"transport {identifier} requires enabled configured switch {switch}"
                )
        ipv6 = family == "IPv6"
        tls = encrypted and family != "RFCOMM"
        rfcomm = family == "RFCOMM"
        result.append(
            {
                "id": identifier,
                "name": name,
                "serverNamedInstance": server,
                "clientNamedInstance": client,
                "compiledFeatureSwitch": feature,
                "carrier": carrier,
                "addressFamily": family,
                "encryption": "TLS" if encrypted else "none",
                "framing": framing,
                "webSocketSubprotocol": "codex" if "WebSocket" in framing else None,
                "defaultEnabled": enabled,
                "authenticationMode": authentication,
                "peerMetadataAvailable": peer,
                "automatedTestRequirements": {
                    "ipv6HostSupport": ipv6,
                    "tlsMaterial": tls,
                    "physicalRfcommHardware": False,
                    "codexExecutable": False,
                    "credentialsOrQuota": False,
                },
                "manualFullExchangeRequirements": {
                    "physicalRfcommHardware": rfcomm,
                    "tlsMaterial": encrypted,
                    "credentialsOrQuota": False,
                },
                "coverageKey": identifier,
                "configuredBinaryAuthorityVerified": True,
                "automatedTestsAtP0": sorted(tests),
                "evidence": {
                    "classes": [
                        "build-or-install-derived",
                        "executable-observation",
                        "named-test-evidence",
                        "owner-approved-declarative-contract",
                    ],
                    "sources": sorted(
                        {
                            "configured CMake cache feature switches",
                            "installed codex-backend --help=expanded",
                            "installed codex-backend-client --help=expanded",
                            *tests,
                            "owner-approved P0 transport contract",
                        }
                    ),
                    "fieldSources": {
                        "namedInstancesAndDefaultState": [
                            "installed codex-backend --help=expanded",
                            "installed codex-backend-client --help=expanded",
                        ],
                        "compiledFeatureSwitch": [
                            "configured CMake cache feature switches"
                        ],
                        "carrierFamilyEncryptionFraming": sorted(
                            {
                                "CodexFrontendNativeTransportTest",
                                "CodexBackendClientTransportCompositionTest",
                                *[
                                    test
                                    for test in tests
                                    if "WebSocket" in test or "Wss" in test or "Tls" in test
                                ],
                            }
                        ),
                        "authenticationAndPeerPolicy": sorted(
                            {
                                "CodexFrontendReferenceAuthenticationTest",
                                "CodexBackendClientAuthenticationTest",
                                "owner-approved P0 transport contract",
                            }
                        ),
                    },
                },
            }
        )
    return result


def _parse_ctest_inventory(path: pathlib.Path | None, build_dir: pathlib.Path, source_dir: pathlib.Path) -> dict[str, Any]:
    if path is None:
        raw = _run(
            ("ctest", "--test-dir", str(build_dir), "--show-only=json-v1"),
            cwd=source_dir,
            description="CTest JSON inventory",
        )
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError as error:
            raise BaselineError(f"CTest returned invalid JSON inventory: {error}") from error
        authority = "ctest --show-only=json-v1"
    else:
        payload = _load_json(path, "CTest JSON inventory")
        authority = "supplied CTest JSON inventory"
    tests: list[dict[str, Any]] = []
    label_counts: collections.Counter[str] = collections.Counter()
    for test in payload.get("tests", []):
        labels: list[str] = []
        for prop in test.get("properties", []):
            if prop.get("name") == "LABELS":
                value = prop.get("value", [])
                labels = sorted(str(item) for item in (value if isinstance(value, list) else [value]))
        label_counts.update(labels)
        tests.append({"name": str(test["name"]), "labels": labels})
    tests.sort(key=lambda row: row["name"])
    if not tests:
        raise BaselineError("CTest inventory contains no registered tests")
    return {
        "authority": authority,
        "registered": len(tests),
        "tests": tests,
        "labelCounts": dict(sorted(label_counts.items())),
    }


def _parse_ctest_results(path: pathlib.Path, parallelism: int) -> dict[str, Any]:
    if not path.is_file():
        raise BaselineError(f"CTest JUnit results are missing: {path}")
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as error:
        raise BaselineError(f"CTest JUnit results are invalid: {path}: {error}") from error
    suites = [root] if root.tag == "testsuite" else list(root.findall("testsuite"))
    cases = [case for suite in suites for case in suite.findall("testcase")]
    if not cases:
        raise BaselineError("CTest JUnit results contain no test cases")
    failed = 0
    skipped = 0
    skip_reasons: list[dict[str, str]] = []
    for case in cases:
        failure = case.find("failure")
        error_node = case.find("error")
        skipped_node = case.find("skipped")
        if failure is not None or error_node is not None:
            failed += 1
        if skipped_node is not None:
            skipped += 1
            system_out = (case.findtext("system-out") or "").strip()
            skip_line = next(
                (line.strip() for line in system_out.splitlines() if line.strip().startswith("SKIP:")),
                "",
            )
            reason = skip_line or skipped_node.get("message") or (skipped_node.text or "").strip() or "CTest reported skipped"
            skip_reasons.append({"test": str(case.get("name", "")), "reason": reason})
    total = len(cases)
    duration = sum(float(suite.get("time", "0") or 0.0) for suite in suites)
    return {
        "resultAuthority": "CTest ordinary-suite JUnit output",
        "total": total,
        "passed": total - failed - skipped,
        "failed": failed,
        "skipped": skipped,
        "durationSeconds": round(duration, 3),
        "parallelism": parallelism,
        "skipReasons": sorted(skip_reasons, key=lambda row: row["test"]),
        "testNames": sorted(str(case.get("name", "")) for case in cases),
    }


def _coverage_mapping(test_names: set[str]) -> dict[str, Any]:
    mappings: dict[str, list[str]] = {
        "generated frontend protocol authority/currentness": ["CodexFrontendProtocolGeneratedArtifactsGuardTest"],
        "C++ frontend client binding currentness": ["CodexFrontendClientBindingsCurrentnessTest", "CodexFrontendClientGeneratorTest"],
        "frontend codec and schema validation": ["CodexFrontendProtocolV1Test", "CodexFrontendCompleteCodecTest", "CodexFrontendSchemaValidatorTest"],
        "frontend projection and exact identity/item normalization": ["CodexFrontendProjectionTest", "CodexFrontendBackendMappingTest", "CodexFrontendServiceClientIntegrationTest"],
        "FrontendService behavior": ["CodexFrontendServiceTest"],
        "service-to-SDK integration": ["CodexFrontendServiceClientIntegrationTest"],
        "client lifecycle and correlation": ["CodexFrontendClientLifecycleTest"],
        "client synchronization, replay, and live Snapshot": ["CodexFrontendClientSynchronizationTest"],
        "command, connection, process lifetime and EOF drain": ["CodexBackendClientCommandTest", "CodexBackendClientCommandDrainTest", "CodexBackendClientStdinReaderTest"],
        "manual reconnect composition": ["CodexBackendClientTransportCompositionTest", "CodexBackendClientIpv4AcceptanceTest", "CodexBackendClientWebSocketIpv4AcceptanceTest"],
        "Unix acceptance": ["CodexBackendUnixAcceptanceTest", "CodexBackendClientUnixAcceptanceTest", "CodexBackendClientRealBackendAcceptanceTest"],
        "IPv4 and IPv6 acceptance": ["CodexFrontendNativeTransportTest", "CodexBackendClientIpv4AcceptanceTest", "CodexBackendClientIpv6AcceptanceTest"],
        "TLS acceptance": ["CodexBackendClientTlsIpv4AcceptanceTest", "CodexBackendClientTlsIpv6AcceptanceTest"],
        "WebSocket and WSS acceptance": ["CodexFrontendWebSocketIntegrationTest", "CodexFrontendWebSocketTlsIntegrationTest", "CodexBackendClientWebSocketIpv4AcceptanceTest", "CodexBackendClientWebSocketIpv6AcceptanceTest", "CodexBackendClientWssIpv4AcceptanceTest", "CodexBackendClientWssIpv6AcceptanceTest"],
        "authentication and authorization": ["CodexFrontendReferenceAuthenticationTest", "CodexBackendClientAuthenticationTest", "CodexFrontendAuthorizationMetadataTest"],
        "installed typed consumer": ["AISuiteInstalledConsumerTest"],
        "public-header policy": ["CodexPublicHeaderPolicyTest"],
        "public-header self-containment": ["CodexPublicHeaderSelfContainmentTest"],
        "symbol visibility": ["CodexFrontendClientSymbolVisibilityTest"],
        "dependency policy": ["AISuiteCodexFrontendDependencyPolicyTest"],
        "binary package": ["AISuiteBinaryPackageTest"],
        "source package": ["AISuiteSourcePackageTest"],
    }
    missing = sorted(
        {
            name
            for names in mappings.values()
            for name in names
            if name not in test_names
        }
    )
    return {
        "automated": [
            {
                "contract": contract,
                "tests": sorted(name for name in names if name in test_names),
                "missingP0ReferenceTests": sorted(
                    name for name in names if name not in test_names
                ),
            }
            for contract, names in sorted(mappings.items())
        ],
        "missingP0ReferenceTests": missing,
        "manualOrInherited": [
            {
                "contract": "owner live interaction with the real Codex App Server",
                "classification": "owner-reported manual live acceptance; credentials/quota; not rerun by P0 tooling",
            },
            {
                "contract": "physical RFCOMM exchange",
                "classification": "hardware-limited; build/configuration/factory behavior is automated, physical radio exchange is not",
            },
            {
                "contract": "SNode.C 60-second read/write inactivity defaults",
                "classification": "inherited configurable SNode.C behavior; recorded rather than duplicated as an AISuite constant",
            },
            {
                "contract": "CLI semantics not fully expressible in executable help, including presentation-only watch, queue defaults, and regular-file stdin rejection",
                "classification": "interactive help and syntax have executable-fixture currentness evidence; remaining semantics are owner-approved and mapped to adjacent parser, queue, presenter, transport-selection, and stdin tests",
            },
        ],
    }


def _external_contract(
    protocol: Mapping[str, Any],
    public_targets: Sequence[Mapping[str, Any]],
    cli_evidence: Mapping[str, Any],
    transports: Sequence[Mapping[str, Any]],
    timeout_defaults: Mapping[str, int],
) -> dict[str, Any]:
    # Test names are evidence, not an external contract.  Keep their per-row
    # mapping in architectureMeasurements and remove it from blocking rows.
    blocking_transports = []
    for row in transports:
        evidence_fields = {
            "automatedTestsAtP0",
            "automatedTestRequirements",
            "manualFullExchangeRequirements",
            "coverageKey",
            "configuredBinaryAuthorityVerified",
        }
        blocking = {key: value for key, value in row.items() if key not in evidence_fields}
        blocking_transports.append(blocking)
    required_header_paths = sorted(
        {
            path
            for target in public_targets
            for path in target.get("requiredInstalledPublicHeaderPaths", [])
        }
    )
    return {
        "blocking": True,
        "changePolicy": "P1-P3 preserve this section unless the owner explicitly approves a contract change",
        "protocol": dict(protocol),
        "publicCpp": {
            "evidence": {
                "classes": ["build-or-install-derived", "named-test-evidence"],
                "sources": [
                    "CMake File API codemodel v2",
                    "installed AISuite CMake package query",
                    "installed public-header tree",
                    "AISuiteInstalledConsumerTest",
                    "CodexPublicHeaderPolicyTest",
                    "CodexPublicHeaderSelfContainmentTest",
                    "CodexFrontendClientSymbolVisibilityTest",
                    "AISuiteCodexFrontendDependencyPolicyTest",
                ],
            },
            "requiredTargets": list(public_targets),
            "requiredImportedTargetNames": sorted(
                str(target["importedTarget"]) for target in public_targets
            ),
            "requiredInstalledPublicHeaderPaths": required_header_paths,
            "additiveTargetsPermitted": True,
            "additivePublicHeadersPermitted": True,
            "additiveAbiCompatibleSymbolsPermitted": True,
            "mechanicallyVerifiedPackageIdentity": [
                "required imported target names",
                "required output library identities",
                "target type",
                "VERSION and SOVERSION compatibility",
                "installed package/export availability",
                "required installed public-header paths",
            ],
            "dedicatedCompatibilityEvidence": [
                "installed consumer",
                "public-header policy",
                "public-header self-containment",
                "symbol visibility",
                "dependency policy",
                "existing source/ABI compatibility tests where present",
            ],
            "nonBlockingMeasurements": [
                "exported-symbol count and fingerprint",
                "NEEDED libraries",
                "binary size",
                "total public-header count",
                "internal and public dependency edges",
                "internal CMake build-target names",
            ],
            "compatibility": [
                "required existing imported target names remain available",
                "required existing output library names remain available",
                "required existing public-header paths remain available",
                "public API source compatibility remains a hard project requirement",
                "public ABI compatibility remains a hard project requirement",
                "SOVERSION remains unchanged unless separately owner-approved",
                "additional targets, headers, and ABI-compatible symbols are permitted",
                "the baseline tool does not claim to prove complete C++ API/ABI compatibility",
            ],
        },
        "applicationCli": {
            "evidence": {
                "classes": [
                    "executable-observation",
                    "named-test-evidence",
                    "owner-approved-declarative-contract",
                ],
                "sources": [
                    "installed codex-backend-client interactive help",
                    CLI_HELP_FIXTURE_RELATIVE.as_posix(),
                    "CodexBackendClientCommandTest",
                    "CodexBackendClientCommandDrainTest",
                    "CodexBackendClientPresenterTest",
                    "CodexBackendClientStdinReaderTest",
                    "owner-approved P0 lifecycle contract",
                ],
            },
            "parserAuthority": "src/apps/codex-backend-client/CommandParser.cpp",
            "executableHelp": dict(cli_evidence),
            "commandSyntax": list(CLI_SYNTAX),
            "startOptions": ["--cwd", "--model", "--model-provider", "--approval-policy", "--sandbox-mode", "--ephemeral"],
            "resumeOptions": ["--cwd", "--model", "--model-provider", "--approval-policy", "--sandbox-mode"],
            "newSeparatorBehavior": "with thread-start options, '--' is required and everything after it is the prompt; without options the separator may be omitted",
            "rawRestrictions": "one known generated Frontend Protocol method with object params; no caller ID, Hello, unknown method, or raw App Server message",
            "watchSemantics": "presentation-only; state and synchronization continue",
            "lifecycle": LIFECYCLE_CONTRACT,
        },
        "backendAndFrontendSemantics": {
            "evidence": {
                "classes": [
                    "named-test-evidence",
                    "owner-approved-declarative-contract",
                ],
                "sources": [
                    "CodexFrontendProjectionTest",
                    "CodexFrontendServiceTest",
                    "CodexFrontendServiceClientIntegrationTest",
                    "CodexFrontendClientLifecycleTest",
                    "CodexFrontendClientSynchronizationTest",
                    "owner-approved P0 semantic contract",
                ],
            },
            "contract": SEMANTIC_CONTRACT,
        },
        "replacementCompatibilityOracle": {
            "evidence": {
                "classes": [
                    "generated-authority-derived",
                    "named-test-evidence",
                    "owner-approved-declarative-contract",
                ],
                "sources": [
                    AUTHORITY_FILES["generatedFixture"],
                    AUTHORITY_FILES["reducerConformanceFixture"],
                    "CodexFrontendProjectionTest",
                    "CodexFrontendSchemaValidatorTest",
                    "CodexFrontendServiceClientIntegrationTest",
                    "owner-reported live acceptance",
                ],
            },
            "classification": "current implementation is a temporary executable differential oracle through P3 closure, not an architectural dependency of the greenfield frontend",
            "serverComparisonBorder": "equivalent BackendCore typed inputs/state/occurrences -> old and greenfield server cores -> canonical Frontend Protocol v1 output",
            "clientComparisonBorder": "equivalent canonical Frontend Protocol v1 input -> old and greenfield client cores -> public results/errors/immutable State/callbacks",
            "requiredDomains": [
                "all 105 methods",
                "all 26 expanded event families",
                "all 18 ThreadItem discriminators",
                "all 12 scopes",
                "capability negotiation plus Hello and Welcome",
                "snapshot and initial synchronization",
                "replay and live Snapshot",
                "equal-sequence expanded event groups plus lower/higher/gapped sequence handling",
                "controller and session behavior",
                "authentication and authorization failures",
                "scope projection and redaction/information ceilings",
                "pending reverse requests and command errors",
                "malformed protocol and unknown/future safe containment",
                "item content accumulation and truncation",
                "queue and backpressure terminal behavior",
            ],
            "startingCorpus": {
                "generatedProtocolFixture": AUTHORITY_FILES["generatedFixture"],
                "reducerConformanceFixture": AUTHORITY_FILES[
                    "reducerConformanceFixture"
                ],
                "canonicalSha256": {
                    "generatedProtocolFixture": protocol[
                        "authorityCanonicalSha256"
                    ]["generatedFixture"],
                    "reducerConformanceFixture": protocol[
                        "authorityCanonicalSha256"
                    ]["reducerConformanceFixture"],
                },
                "tests": [
                    "CodexFrontendProjectionTest",
                    "CodexFrontendSchemaValidatorTest",
                    "CodexFrontendServiceClientIntegrationTest",
                ],
                "manualEvidence": "owner-reported live acceptance",
            },
            "normalizationPolicy": "only narrow reviewed and documented normalization is permitted; differences in stable Frontend Protocol semantics fail",
            "greenfieldConstructionPhase": "P2 builds both transport-neutral cores beside the old implementations and completes differential validation while production remains on the old implementation",
            "productionCutoverPhase": "P3 cuts over all applications and all eleven external transports",
            "legacyRemovalPhase": "P3 after P2 differential closure and the transport, package, compatibility, and owner-live deletion gates pass",
            "finalComparisonPhase": "P3 compares the reduced production architecture with P0",
        },
        "transport": {
            "evidence": {
                "classes": [
                    "build-or-install-derived",
                    "executable-observation",
                    "named-test-evidence",
                    "owner-approved-declarative-contract",
                ],
                "sources": [
                    "configured CMake cache",
                    "installed executable expanded help",
                    "registered native/WebSocket/TLS/authentication/acceptance tests",
                    "owner-approved P0 transport contract",
                ],
            },
            "externalCompositionCount": len(blocking_transports),
            "externalCompositions": blocking_transports,
            "inMemory": {
                "classification": "test-only",
                "externalListener": False,
                "partOfExternalApplicationTransportSet": False,
                "evidence": {
                    "classes": ["named-test-evidence"],
                    "sources": [
                        "CodexFrontendServiceClientIntegrationTest",
                        "CodexFrontendClientLifecycleTest",
                    ],
                },
            },
            "setPolicy": "the eleven external compositions and their semantics are frozen; internal adapter count is a reduction metric",
        },
        "inheritedSNodeCConnectionDefaults": {
            "evidence": {
                "classes": [
                    "executable-observation",
                    "inherited-dependency-evidence",
                    "owner-approved-declarative-contract",
                ],
                "sources": [
                    "architectureMeasurements.dependencies.snodec",
                    "installed codex-backend-client connection --help",
                    "owner-reported live acceptance observation 13-18",
                ],
            },
            "classification": "inherited configurable SNode.C behavior, not hard-coded AISuite constants",
            "readInactivityTimeoutSeconds": timeout_defaults[
                "readInactivityTimeoutSeconds"
            ],
            "writeInactivityTimeoutSeconds": timeout_defaults[
                "writeInactivityTimeoutSeconds"
            ],
            "behavior": [
                "absence of relevant I/O for the configured timeout may close the physical connection",
                "the close is a transport disconnect, not a Frontend Protocol failure",
                "codex-backend-client remains alive in Disconnected",
                "the user may explicitly reconnect",
                "no automatic reconnect, command retry, or controller restoration occurs",
            ],
        },
    }


def _complexity_interpretation() -> dict[str, Any]:
    return {
        "referenceScope": "qualitative interpretation of the P0 implementation retained as comparison context; current presence/removal is derived by source, target, binary, and oracle-identity measurements",
        "classificationPolicy": "qualitative architectural judgment; no composite complexity score",
        "classes": [
            {
                "class": "necessary domain complexity",
                "observations": ["typed provider operations", "BackendCore state and recovery", "controller/session facts"],
            },
            {
                "class": "necessary protocol complexity",
                "observations": ["authentication/authorization", "bounded synchronization/replay", "command correlation", "eleven external transport semantics"],
            },
            {
                "class": "accidental representation complexity",
                "observations": ["dual legacy/expanded canonical representation", "arbitrary JSON positions", "event coalescer/journal/batch machinery"],
            },
            {
                "class": "accidental transport-composition complexity",
                "observations": ["server and client instantiation matrices", "duplicate JSONL framers", "separate native/WebSocket lifecycle stacks", "global runtime bridges"],
            },
            {
                "class": "accidental application-lifecycle complexity",
                "observations": ["reconnect configuration copying", "attempt-generation machinery", "broad main.cpp and CommandDrainController responsibilities"],
            },
        ],
        "representationStages": {
            "evidenceKind": "architectural judgment",
            "majorStageCount": 5,
            "stages": ["typed App Server values", "typed BackendCore state/occurrence", "legacy canonical JSON occurrence", "expanded frontend wire JSON", "typed SDK State"],
        },
        "dualCanonicalRepresentations": {"observedCount": 2, "names": ["legacy", "expanded"]},
        "duplicateJsonlFramers": [
            "src/apps/codex-backend/JsonLineFramer.cpp",
            "src/apps/codex-backend-client/JsonLineFramer.cpp",
        ],
        "runtimeBridges": [
            "src/apps/codex-backend/FrontendRuntimeBridge.cpp",
            "src/apps/codex-backend-client/FrontendWebSocketClient.cpp",
        ],
        "responsibilityJudgments": {
            "src/apps/codex-backend/main.cpp": ["process/config bootstrap", "provider/backend/service construction", "authentication", "eleven server compositions", "runtime bridge", "listener lifecycle", "shutdown"],
            "src/apps/codex-backend-client/main.cpp": ["process/config bootstrap", "SDK callbacks", "eleven client compositions", "authentication", "attempt/reconnect supervision", "WebSocket bridge", "stdin/command drain", "shutdown"],
            "src/apps/codex-backend-client/CommandDrainController.cpp": ["command admission", "bounded queueing", "SDK submission deferral", "compound new workflow", "reconnect gating", "EOF drain", "disconnect accounting", "final process status"],
        },
    }


def _owner_live_evidence() -> dict[str, Any]:
    observations = [
        "clean initial connection and synchronization",
        "distinct thread identities",
        "compact threadList.updated",
        "controller acquisition",
        "successful real thread.start",
        "successful real turn.start",
        "userMessage",
        "agentMessage",
        "agent content equal to the requested short answer",
        "completed agent item",
        "completed turn",
        "subsequent commands on the same connection",
        "expected SNode.C inactivity timeout",
        "client process remained alive in Disconnected",
        "local help remained available",
        "remote commands were rejected locally while Disconnected",
        "explicit reconnect succeeded",
        "reconnect used replay when continuity was available",
        "controller ownership was not restored",
        "permission_denied remained a nonfatal command error",
        "explicit reacquisition succeeded",
        "another real turn returned: Codex working",
    ]
    return {
        "evidenceType": "owner-reported manual live acceptance",
        "reportedBy": "owner",
        "reproducedByCodex": False,
        "date": "2026-08-08",
        "transport": "Unix JSONL",
        "realProvider": "Codex App Server",
        "observations": [{"ordinal": index + 1, "observation": value} for index, value in enumerate(observations)],
        "exclusions": [
            "accidental terminal input concatenation is not classified as a software failure",
            "terminal prompt redraw is not part of P0",
        ],
    }


def _baseline_parent(source_dir: pathlib.Path, explicit: str | None) -> str:
    if explicit:
        value = explicit.strip()
    else:
        value = _run(
            ("git", "merge-base", "HEAD", "refs/remotes/origin/master"),
            cwd=source_dir,
            description="P0 baseline parent resolution",
        ).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise BaselineError(f"baseline parent is not a full Git SHA: {value}")
    return value


def _validate_no_machine_paths_or_secrets(value: Any) -> None:
    forbidden_key = re.compile(r"(?:bearer|credential|secret|token|password).*(?:file|path)$", re.IGNORECASE)
    posix_absolute_path = re.compile(r"(?:^|[\s='\":])/(?!/)")
    windows_absolute_path = re.compile(r"(?:^|[\s='\"])[A-Za-z]:[\\/]")

    def walk(node: Any, location: str) -> None:
        if isinstance(node, dict):
            for key, child in node.items():
                if forbidden_key.search(str(key)):
                    raise BaselineError(f"secret-bearing field is forbidden at {location}.{key}")
                walk(child, f"{location}.{key}")
        elif isinstance(node, list):
            for index, child in enumerate(node):
                walk(child, f"{location}[{index}]")
        elif isinstance(node, str) and (
            posix_absolute_path.search(node) or windows_absolute_path.search(node)
        ):
            raise BaselineError(f"absolute or host-specific path retained at {location}: {node}")

    walk(value, "baseline")


def _write_json(path: pathlib.Path, value: Any) -> None:
    _validate_no_machine_paths_or_secrets(value)
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    path.write_text(encoded, encoding="utf-8")


def _build_capture_document(arguments: argparse.Namespace) -> dict[str, Any]:
    source_dir = _require_directory(arguments.source_dir, "source directory")
    build_dir = _require_directory(arguments.build_dir, "build directory")
    install_dir = _require_directory(arguments.install_dir, "install directory")
    reply_dir, index, codemodel, context = _file_api(build_dir)
    codemodel_paths = codemodel.get("paths", {})
    if pathlib.Path(str(codemodel_paths.get("source", ""))).resolve() != source_dir:
        raise BaselineError("CMake codemodel source path does not match --source-dir")
    if pathlib.Path(str(codemodel_paths.get("build", ""))).resolve() != build_dir:
        raise BaselineError("CMake codemodel build path does not match --build-dir")
    provenance = _toolchain_provenance(index, context)
    if provenance["configuredBuildType"] != "Debug":
        raise BaselineError(
            f"P0 capture requires configured build type Debug, got {provenance['configuredBuildType']!r}"
        )
    if not all(provenance["featureSwitches"].values()):
        disabled = sorted(key for key, enabled in provenance["featureSwitches"].items() if not enabled)
        raise BaselineError(f"P0 capture requires the feature-complete build; disabled switches: {', '.join(disabled)}")
    _run_currentness_checks(source_dir)
    protocol = _derive_protocol(source_dir)
    snodec_dependency = _snodec_dependency_provenance(
        arguments.snodec_source_dir,
        arguments.snodec_build_dir,
        context["cache"],
    )
    target_inventory = _target_inventory(source_dir, build_dir, reply_dir, codemodel)
    _validate_install_manifest(build_dir, install_dir)
    installed, public_targets, binary_strings = _installed_inventory(
        source_dir, install_dir, provenance["projectVersion"], context["cache"]
    )
    library_by_name = {row["name"]: row for row in installed["installedLibraries"]}
    installed["publicTargetMeasurements"] = []
    for contract in public_targets:
        output_library = str(contract["outputLibrary"])
        build_candidates = [
            row
            for row in target_inventory
            if any(
                pathlib.PurePosixPath(output).name == output_library
                or pathlib.PurePosixPath(output).name.startswith(
                    output_library + "."
                )
                for output in row["buildOutputs"]
            )
        ]
        if len(build_candidates) != 1:
            raise BaselineError(
                f"expected one CMake build target producing {output_library}, "
                f"found {len(build_candidates)}"
            )
        build_record = build_candidates[0]
        binary_record = library_by_name[output_library]
        installed["publicTargetMeasurements"].append(
            {
                **contract,
                "buildTarget": build_record["name"],
                "publicLinkInterfaceDependencies": installed["publicLinkInterfaces"].get(
                    str(contract["importedTarget"]), []
                ),
                "resolvedInProjectBuildDependencies": build_record[
                    "resolvedInProjectBuildDependencies"
                ],
                "resolvedLinkLibraryNames": build_record["resolvedLinkLibraryNames"],
                "codemodelSourceEntryCount": build_record["codemodelSourceEntryCount"],
                "installedPublicHeaderCount": binary_record["installedPublicHeaderCount"],
                "installedPublicHeaderPaths": binary_record["installedPublicHeaderPaths"],
                "fileSizeBytes": binary_record["fileSizeBytes"],
                "soname": binary_record["soname"],
                "neededLibraries": binary_record["neededLibraries"],
                "exportedDynamicSymbolCount": binary_record["exportedDynamicSymbolCount"],
                "exportedDynamicSymbolNameSetSha256": binary_record[
                    "exportedDynamicSymbolNameSetSha256"
                ],
            }
        )
    installed["publicTargetMeasurements"].sort(key=lambda row: row["importedTarget"])
    cli_evidence = _installed_cli_evidence(source_dir, install_dir)
    transport_observation = _installed_transport_observation(source_dir, install_dir)
    timeout_observation = _installed_timeout_observation(source_dir, install_dir)
    configured_timeouts = snodec_dependency["configuredConnectionDefaults"]
    for key in (
        "readInactivityTimeoutSeconds",
        "writeInactivityTimeoutSeconds",
    ):
        if timeout_observation[key] != configured_timeouts[key]:
            raise BaselineError(
                f"installed executable and configured SNode.C disagree on {key}"
            )
    transports_with_coverage = _transport_matrix(
        binary_strings,
        transport_observation,
        provenance["featureSwitches"],
    )
    inventory_path = arguments.ctest_inventory.resolve() if arguments.ctest_inventory else None
    tests = _parse_ctest_inventory(inventory_path, build_dir, source_dir)
    results = _parse_ctest_results(arguments.ctest_results.resolve(), arguments.ctest_parallelism)
    if tests["registered"] != results["total"]:
        raise BaselineError(
            f"CTest inventory/result count mismatch: {tests['registered']} registered versus {results['total']} results"
        )
    inventory_names = sorted(row["name"] for row in tests["tests"])
    if inventory_names != results["testNames"]:
        raise BaselineError("CTest inventory and result test-name sets differ")
    if results["failed"] != 0:
        raise BaselineError(f"ordinary CTest baseline has {results['failed']} failed tests; zero is required")
    inventory_name_set = set(inventory_names)
    coverage = _coverage_mapping(inventory_name_set)
    transport_coverage = [
        {
            "transportId": row["id"],
            "automatedTests": sorted(
                name
                for name in row["automatedTestsAtP0"]
                if name in inventory_name_set
            ),
            "missingP0ReferenceTests": sorted(
                name
                for name in row["automatedTestsAtP0"]
                if name not in inventory_name_set
            ),
            "automatedTestRequirements": row["automatedTestRequirements"],
            "manualFullExchangeRequirements": row["manualFullExchangeRequirements"],
        }
        for row in transports_with_coverage
    ]
    baseline_parent = _baseline_parent(source_dir, arguments.baseline_parent)
    provenance.update(
        {
            "repository": REPOSITORY,
            "sourceBranch": SOURCE_BRANCH,
            "p0BaselineParentSha": baseline_parent,
            "pr14MergeCommit": PR14_MERGE_COMMIT,
            "pr14FinalSourceHead": PR14_SOURCE_HEAD,
            "codexSoversion": public_targets[0]["soversion"],
            "dependencyProvenance": {
                "snodec": {
                    key: value
                    for key, value in snodec_dependency.items()
                    if key != "configuredConnectionDefaults"
                }
            },
        }
    )
    architecture = {
        "blocking": False,
        "equalityGate": False,
        "comparisonPolicy": "report changes individually; reductions are desired where appropriate, and architecture metrics must not be used as equality gates or a composite score",
        "determinism": {
            "unchangedCompleteCaptureEqualityRequired": True,
            "capturePerformsTwoIndependentNormalizations": True,
            "volatileHostObservationFields": [],
        },
        "source": _source_measurements(source_dir),
        "dependencies": {
            "snodec": snodec_dependency,
            "comparisonPolicy": "SNode.C SHA/version/default changes are reported without equality-gating the dependency revision; inherited behavior changes remain visible",
        },
        "cmake": {
            "authority": "CMake File API codemodel v2",
            "targets": target_inventory,
            "targetCountsByClassification": dict(
                sorted(collections.Counter(target["classification"] for target in target_inventory).items())
            ),
            "productionAndBuildSupportTargetCount": sum(
                target["classification"] != "test-support" for target in target_inventory
            ),
            "resolvedInProjectDependencyEdges": sorted(
                [target["name"], dependency]
                for target in target_inventory
                for dependency in target["resolvedInProjectBuildDependencies"]
            ),
        },
        "installed": installed,
        "tests": {
            **tests,
            "ordinarySuite": results,
            "contractCoverage": coverage,
            "transportCoverage": transport_coverage,
        },
        "complexityInterpretation": _complexity_interpretation(),
        "legacyImplementationOracle": _legacy_oracle_measurement(source_dir),
        "finalComparisonOwner": "P3 cutover and legacy deletion",
        "finalComparisonDimensions": [
            "public protocol equality",
            "public API and ABI compatibility",
            "transport and security behavior equality",
            "lifecycle behavior equality",
            "target DAG and client-to-server dependency reduction",
            "complete greenfield server/client differential equivalence and legacy-oracle removal",
            "production and adapter target-count changes",
            "application main.cpp tracked-physical-line changes",
            "duplicate framer and runtime bridge removal",
            "configuration-copy and attempt-generation reduction",
            "arbitrary JSON representation reduction",
            "public-header and binary-size changes",
            "dynamic dependency changes",
            "test count and duration changes",
        ],
    }
    baseline = {
        "formatVersion": FORMAT_VERSION,
        "provenance": provenance,
        "externalContract": _external_contract(
            protocol,
            public_targets,
            cli_evidence,
            transports_with_coverage,
            timeout_observation,
        ),
        "architectureMeasurements": architecture,
        "ownerLiveEvidence": _owner_live_evidence(),
    }
    return baseline


def capture(arguments: argparse.Namespace) -> int:
    first = _build_capture_document(arguments)
    second = _build_capture_document(arguments)
    if _canonical_json_bytes(first) != _canonical_json_bytes(second):
        changes = _deep_differences(first, second)
        raise BaselineError(
            "two unchanged complete captures were not deterministic: "
            + "; ".join(changes[:20])
        )
    _write_json(arguments.output.resolve(), first)
    print(f"captured normalized P0 baseline: {arguments.output}")
    return 0


def _validate_evidence(record: Mapping[str, Any], location: str) -> None:
    evidence = record.get("evidence")
    if not isinstance(evidence, dict):
        raise BaselineError(f"blocking contract subsection lacks evidence at {location}")
    classes = evidence.get("classes")
    sources = evidence.get("sources")
    if (
        not isinstance(classes, list)
        or not classes
        or not set(classes) <= EVIDENCE_CLASSES
        or classes != sorted(set(classes))
    ):
        raise BaselineError(f"blocking contract evidence classes are invalid at {location}")
    if not isinstance(sources, list) or not sources or any(not str(value) for value in sources):
        raise BaselineError(f"blocking contract evidence sources are absent at {location}")


def _validate_external_shape(contract: Mapping[str, Any]) -> None:
    if contract.get("blocking") is not True:
        raise BaselineError("externalContract must be explicitly blocking")
    protocol = contract.get("protocol", {})
    _validate_evidence(protocol, "externalContract.protocol")
    expected_protocol_counts = {
        "identity": EXPECTED_PROTOCOL["identity"],
        "version": EXPECTED_PROTOCOL["version"],
        "messageKindCount": EXPECTED_PROTOCOL["messageKinds"],
        "methodCount": EXPECTED_PROTOCOL["methods"],
        "expandedEventFamilyCount": EXPECTED_PROTOCOL["expandedEventFamilies"],
        "threadItemDiscriminatorCount": EXPECTED_PROTOCOL["threadItemDiscriminators"],
        "scopeCount": EXPECTED_PROTOCOL["scopes"],
    }
    for key, expected in expected_protocol_counts.items():
        if protocol.get(key) != expected:
            raise BaselineError(f"external protocol {key} must be {expected!r}")
    if protocol.get("methodOwnershipCounts") != {
        "native": EXPECTED_PROTOCOL["native"],
        "provider": EXPECTED_PROTOCOL["provider"],
        "reverse": EXPECTED_PROTOCOL["reverse"],
    }:
        raise BaselineError("external protocol method ownership counts are incomplete or incorrect")
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
        value = protocol.get(key)
        if not isinstance(value, list) or len(value) != protocol.get(count_key):
            raise BaselineError(f"external protocol {key} does not match {count_key}")
        if key not in {"methods", "capabilities"} and len(set(value)) != len(value):
            raise BaselineError(f"external protocol {key} is not a unique stable set")
    if set(protocol.get("capabilityCategories", {})) != {
        "conditional_topology",
        "product",
        "static_mechanism",
    }:
        raise BaselineError("external protocol capability categories are incomplete")
    authority_hashes = protocol.get("authorityCanonicalSha256", {})
    authority_paths = protocol.get("authorityPaths", {})
    if set(authority_hashes) != set(AUTHORITY_FILES) or authority_paths != {
        name: path for name, path in sorted(AUTHORITY_FILES.items())
    }:
        raise BaselineError("external protocol authority inventory is incomplete")
    if any(not re.fullmatch(r"[0-9a-f]{64}", str(value)) for value in authority_hashes.values()):
        raise BaselineError("external protocol authority fingerprint is not SHA-256")

    public_cpp = contract.get("publicCpp", {})
    _validate_evidence(public_cpp, "externalContract.publicCpp")
    target_rows = public_cpp.get("requiredTargets", [])
    expected_targets = {str(row["importedTarget"]): row for row in PUBLIC_TARGETS}
    if not isinstance(target_rows, list) or {
        row.get("importedTarget") for row in target_rows if isinstance(row, dict)
    } != set(expected_targets):
        raise BaselineError("required public C++ imported-target set is incomplete")
    if public_cpp.get("requiredImportedTargetNames") != sorted(expected_targets):
        raise BaselineError("required imported-target name inventory is inconsistent")
    if (
        public_cpp.get("additiveTargetsPermitted") is not True
        or public_cpp.get("additivePublicHeadersPermitted") is not True
        or public_cpp.get("additiveAbiCompatibleSymbolsPermitted") is not True
    ):
        raise BaselineError("public C++ contract does not permit compatible additions")
    required_target_fields = {
        "importedTarget",
        "outputLibrary",
        "targetType",
        "version",
        "soversion",
        "installedPackageExportFile",
        "requiredInstalledPublicHeaderPaths",
    }
    for row in target_rows:
        if not isinstance(row, dict) or not required_target_fields <= set(row):
            raise BaselineError("public C++ target record is incomplete")
        expected = expected_targets[str(row["importedTarget"])]
        if row.get("outputLibrary") != expected["outputLibrary"]:
            raise BaselineError(f"public C++ target mapping drifted: {row.get('importedTarget')}")
        if (
            row.get("targetType") != "SHARED_LIBRARY"
            or row.get("version") != "0.3.0"
            or row.get("soversion") != 4
            or row.get("installedPackageExportFile") != "lib/cmake/AISuite/AISuiteTargets.cmake"
        ):
            raise BaselineError(f"public C++ target metadata is incomplete: {row.get('importedTarget')}")
        headers = row.get("requiredInstalledPublicHeaderPaths")
        if (
            not isinstance(headers, list)
            or not headers
            or headers != sorted(set(headers))
            or any(not str(path).startswith("include/aisuite/ai/openai/codex/") for path in headers)
        ):
            raise BaselineError(
                f"required public-header paths are incomplete: {row.get('importedTarget')}"
            )
    required_headers = sorted(
        {
            path
            for row in target_rows
            for path in row["requiredInstalledPublicHeaderPaths"]
        }
    )
    if public_cpp.get("requiredInstalledPublicHeaderPaths") != required_headers:
        raise BaselineError("required public-header union is inconsistent")

    application_cli = contract.get("applicationCli", {})
    _validate_evidence(application_cli, "externalContract.applicationCli")
    help_evidence = application_cli.get("executableHelp", {})
    if (
        help_evidence.get("fixturePath") != CLI_HELP_FIXTURE_RELATIVE.as_posix()
        or help_evidence.get("commandSyntax") != list(CLI_SYNTAX)
        or not re.fullmatch(
            r"[0-9a-f]{64}", str(help_evidence.get("normalizedHelpSha256", ""))
        )
    ):
        raise BaselineError("installed CLI executable evidence is incomplete")

    semantics = contract.get("backendAndFrontendSemantics", {})
    _validate_evidence(semantics, "externalContract.backendAndFrontendSemantics")
    if semantics.get("contract") != SEMANTIC_CONTRACT:
        raise BaselineError("backend/frontend semantic contract is incomplete")

    oracle = contract.get("replacementCompatibilityOracle", {})
    _validate_evidence(oracle, "externalContract.replacementCompatibilityOracle")
    domains = oracle.get("requiredDomains", [])
    if not isinstance(domains, list) or len(domains) < 10:
        raise BaselineError("replacement differential-oracle domains are incomplete")
    if (
        not str(oracle.get("greenfieldConstructionPhase", "")).startswith(
            "P2 builds both transport-neutral cores"
        )
        or not str(oracle.get("productionCutoverPhase", "")).startswith(
            "P3 cuts over all applications and all eleven external transports"
        )
        or not str(oracle.get("legacyRemovalPhase", "")).startswith("P3 after")
        or oracle.get("finalComparisonPhase")
        != "P3 compares the reduced production architecture with P0"
    ):
        raise BaselineError("replacement-oracle roadmap phases are incorrect")

    transport = contract.get("transport", {})
    _validate_evidence(transport, "externalContract.transport")
    rows = transport.get("externalCompositions", [])
    if transport.get("externalCompositionCount") != 11 or len(rows) != 11:
        raise BaselineError("external transport matrix must contain exactly eleven compositions")
    if {row.get("id") for row in rows if isinstance(row, dict)} != set(EXPECTED_TRANSPORT_CONTRACT):
        raise BaselineError("external transport matrix IDs are incomplete or not unique")
    transport_fields = (
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
    )
    for row in rows:
        _validate_evidence(row, f"externalContract.transport.{row.get('id')}")
        expected = EXPECTED_TRANSPORT_CONTRACT[str(row["id"])]
        actual = tuple(row.get(field) for field in transport_fields)
        if actual != expected:
            raise BaselineError(f"external transport identity/composition drifted: {row['id']}")
        expected_authentication, expected_peer = EXPECTED_TRANSPORT_SECURITY[str(row["id"])]
        if row.get("authenticationMode") != expected_authentication:
            raise BaselineError(f"external transport authentication mode drifted: {row['id']}")
        if tuple(row.get("peerMetadataAvailable", [])) != expected_peer:
            raise BaselineError(f"external transport peer metadata drifted: {row['id']}")
    in_memory = transport.get("inMemory", {})
    if (
        in_memory.get("classification") != "test-only"
        or in_memory.get("externalListener") is not False
        or in_memory.get("partOfExternalApplicationTransportSet") is not False
    ):
        raise BaselineError("in-memory transport is not classified separately as test-only")
    _validate_evidence(in_memory, "externalContract.transport.inMemory")

    inherited = contract.get("inheritedSNodeCConnectionDefaults", {})
    _validate_evidence(
        inherited, "externalContract.inheritedSNodeCConnectionDefaults"
    )
    if (
        inherited.get("classification")
        != "inherited configurable SNode.C behavior, not hard-coded AISuite constants"
        or not isinstance(inherited.get("readInactivityTimeoutSeconds"), int)
        or not isinstance(inherited.get("writeInactivityTimeoutSeconds"), int)
    ):
        raise BaselineError("inherited SNode.C timeout contract is incomplete")


def _without_evidence(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: _without_evidence(child)
            for key, child in sorted(value.items())
            if key != "evidence"
        }
    if isinstance(value, list):
        return [_without_evidence(child) for child in value]
    return value


def _external_contract_differences(expected: Any, current: Any) -> list[str]:
    if not isinstance(expected, dict) or not isinstance(current, dict):
        return _deep_differences(_without_evidence(expected), _without_evidence(current))

    differences: list[str] = []
    for section in sorted(set(expected) | set(current)):
        if section == "publicCpp":
            continue
        differences.extend(
            f"{section}.{change}"
            for change in _deep_differences(
                _without_evidence(expected.get(section)),
                _without_evidence(current.get(section)),
            )
        )

    before_public = expected.get("publicCpp", {})
    after_public = current.get("publicCpp", {})
    before_targets = {
        str(row.get("importedTarget")): row
        for row in before_public.get("requiredTargets", [])
        if isinstance(row, dict) and row.get("importedTarget")
    }
    after_targets = {
        str(row.get("importedTarget")): row
        for row in after_public.get("requiredTargets", [])
        if isinstance(row, dict) and row.get("importedTarget")
    }
    for name in sorted(before_targets):
        if name not in after_targets:
            differences.append(f"publicCpp.requiredTargets.{name}: missing")
            continue
        before_row = {
            key: value
            for key, value in _without_evidence(before_targets[name]).items()
            if key != "requiredInstalledPublicHeaderPaths"
        }
        after_row = {
            key: value
            for key, value in _without_evidence(after_targets[name]).items()
            if key != "requiredInstalledPublicHeaderPaths"
        }
        differences.extend(
            f"publicCpp.requiredTargets.{name}.{change}"
            for change in _deep_differences(before_row, after_row)
        )
        missing_headers = sorted(
            set(before_targets[name].get("requiredInstalledPublicHeaderPaths", []))
            - set(after_targets[name].get("requiredInstalledPublicHeaderPaths", []))
        )
        differences.extend(
            f"publicCpp.requiredTargets.{name}.requiredInstalledPublicHeaderPaths: missing {path}"
            for path in missing_headers
        )
    missing_union = sorted(
        set(before_public.get("requiredInstalledPublicHeaderPaths", []))
        - set(after_public.get("requiredInstalledPublicHeaderPaths", []))
    )
    differences.extend(
        f"publicCpp.requiredInstalledPublicHeaderPaths: missing {path}"
        for path in missing_union
    )
    policy_keys = {
        key
        for key in before_public
        if key
        not in {
            "evidence",
            "requiredTargets",
            "requiredImportedTargetNames",
            "requiredInstalledPublicHeaderPaths",
        }
    }
    differences.extend(
        f"publicCpp.{change}"
        for change in _deep_differences(
            {key: _without_evidence(before_public.get(key)) for key in policy_keys},
            {key: _without_evidence(after_public.get(key)) for key in policy_keys},
        )
    )
    return sorted(differences)


def _load_and_validate_baseline(
    source_dir: pathlib.Path, baseline_path: pathlib.Path
) -> dict[str, Any]:
    baseline = _load_json(baseline_path.resolve(), "committed P0 baseline")
    if baseline.get("formatVersion") != FORMAT_VERSION:
        raise BaselineError(
            f"unsupported baseline formatVersion: {baseline.get('formatVersion')}"
        )
    required = {"provenance", "externalContract", "architectureMeasurements", "ownerLiveEvidence"}
    if required - set(baseline):
        raise BaselineError("baseline is missing required provenance or category fields")
    _validate_external_shape(baseline["externalContract"])
    architecture = baseline.get("architectureMeasurements", {})
    if architecture.get("blocking") is not False or architecture.get("equalityGate") is not False:
        raise BaselineError(
            "architecture measurements must be explicitly non-blocking and not an equality gate"
        )
    comparison_dimensions = architecture.get("finalComparisonDimensions")
    if (
        architecture.get("finalComparisonOwner") != "P3 cutover and legacy deletion"
        or not isinstance(comparison_dimensions, list)
        or not comparison_dimensions
        or "p6ComparisonDimensions" in architecture
    ):
        raise BaselineError(
            "architecture measurements must use the phase-neutral final comparison model owned by P3"
        )
    dependency = baseline.get("provenance", {}).get("dependencyProvenance", {}).get("snodec", {})
    if (
        dependency.get("repository") != "SNodeC/snode.c"
        or not re.fullmatch(r"[0-9a-f]{40}", str(dependency.get("exactCommit", "")))
        or not str(dependency.get("version", ""))
    ):
        raise BaselineError("baseline SNode.C dependency provenance is incomplete")
    _validate_no_machine_paths_or_secrets(baseline)
    return baseline


def validate_baseline(arguments: argparse.Namespace) -> int:
    source_dir = _require_directory(arguments.source_dir, "source directory")
    baseline = _load_and_validate_baseline(source_dir, arguments.baseline)
    _run_currentness_checks(source_dir)
    current_protocol = _derive_protocol(source_dir)
    expected_protocol = baseline["externalContract"]["protocol"]
    changes = _deep_differences(
        _without_evidence(expected_protocol), _without_evidence(current_protocol)
    )
    if changes:
        print("source authority drift detected:", file=sys.stderr)
        for change in changes[:100]:
            print(f"  protocol.{change}", file=sys.stderr)
        return 1
    print(
        "source-only baseline validation passed; build/install/executable contract portions were not freshly verified"
    )
    return 0


def verify_contract(arguments: argparse.Namespace) -> int:
    source_dir = _require_directory(arguments.source_dir, "source directory")
    baseline = _load_and_validate_baseline(source_dir, arguments.baseline)
    _run_currentness_checks(source_dir)
    protocol = _derive_protocol(source_dir)
    expected = baseline["externalContract"]
    build_dir = _require_directory(arguments.build_dir, "build directory")
    install_dir = _require_directory(arguments.install_dir, "install directory")
    _reply_dir, index, codemodel, context = _file_api(build_dir)
    codemodel_paths = codemodel.get("paths", {})
    if pathlib.Path(str(codemodel_paths.get("source", ""))).resolve() != source_dir:
        raise BaselineError("CMake codemodel source path does not match --source-dir")
    if pathlib.Path(str(codemodel_paths.get("build", ""))).resolve() != build_dir:
        raise BaselineError("CMake codemodel build path does not match --build-dir")
    build_provenance = _toolchain_provenance(index, context)
    if build_provenance["configuredBuildType"] != "Debug":
        raise BaselineError("verify-contract requires the configured P0 Debug build")
    if not all(build_provenance["featureSwitches"].values()):
        disabled = sorted(
            key for key, enabled in build_provenance["featureSwitches"].items() if not enabled
        )
        raise BaselineError(
            "verify-contract requires the feature-complete build; disabled switches: "
            + ", ".join(disabled)
        )
    snodec_dependency = _snodec_dependency_provenance(
        arguments.snodec_source_dir,
        arguments.snodec_build_dir,
        context["cache"],
    )
    _validate_install_manifest(build_dir, install_dir)
    installed, public_targets, binary_strings = _installed_inventory(
        source_dir,
        install_dir,
        build_provenance["projectVersion"],
        context["cache"],
    )
    cli_evidence = _installed_cli_evidence(source_dir, install_dir)
    transport_observation = _installed_transport_observation(source_dir, install_dir)
    timeout_observation = _installed_timeout_observation(source_dir, install_dir)
    configured_timeouts = snodec_dependency["configuredConnectionDefaults"]
    for key in ("readInactivityTimeoutSeconds", "writeInactivityTimeoutSeconds"):
        if timeout_observation[key] != configured_timeouts[key]:
            raise BaselineError(
                f"installed executable and configured SNode.C disagree on {key}"
            )
    transports = _transport_matrix(
        binary_strings,
        transport_observation,
        build_provenance["featureSwitches"],
    )
    current = _external_contract(
        protocol, public_targets, cli_evidence, transports, timeout_observation
    )
    changes = _external_contract_differences(expected, current)
    if changes:
        print("external-contract drift detected:", file=sys.stderr)
        for change in changes[:100]:
            print(f"  {change}", file=sys.stderr)
        return 1
    baseline_dependency = baseline["provenance"]["dependencyProvenance"]["snodec"]
    dependency_changed = any(
        baseline_dependency.get(key) != snodec_dependency.get(key)
        for key in ("exactCommit", "version")
    )
    observed_names = {
        row["importedTarget"] for row in installed["observedPublicImportedTargets"]
    }
    required_names = set(expected["publicCpp"]["requiredImportedTargetNames"])
    print("full build/install/executable external contract matches the committed P0 baseline")
    print(
        "SNode.C dependency: "
        f"P0={baseline_dependency['exactCommit']} current={snodec_dependency['exactCommit']} "
        f"changed={'yes' if dependency_changed else 'no'}"
    )
    added_targets = sorted(observed_names - required_names)
    if added_targets:
        print("additive public targets observed: " + ", ".join(added_targets))
    return 0


def _flatten(value: Any, prefix: str = "") -> dict[str, Any]:
    output: dict[str, Any] = {}
    if isinstance(value, dict):
        for key in sorted(value):
            child = f"{prefix}.{key}" if prefix else str(key)
            output.update(_flatten(value[key], child))
    elif isinstance(value, list):
        for index, child_value in enumerate(value):
            output.update(_flatten(child_value, f"{prefix}[{index}]"))
    else:
        output[prefix] = value
    return output


def _deep_differences(expected: Any, actual: Any) -> list[str]:
    left = _flatten(expected)
    right = _flatten(actual)
    differences: list[str] = []
    for key in sorted(set(left) | set(right)):
        if key not in left:
            differences.append(f"{key}: added {right[key]!r}")
        elif key not in right:
            differences.append(f"{key}: removed (was {left[key]!r})")
        elif left[key] != right[key]:
            differences.append(f"{key}: {left[key]!r} -> {right[key]!r}")
    return differences


def _named_rows(value: Mapping[str, Any], path: Sequence[str], name_key: str) -> dict[str, Mapping[str, Any]]:
    node: Any = value
    for key in path:
        node = node.get(key, {}) if isinstance(node, dict) else {}
    if not isinstance(node, list):
        return {}
    return {str(row[name_key]): row for row in node if isinstance(row, dict) and name_key in row}


def _numeric_metric_changes(before: Mapping[str, Any], after: Mapping[str, Any]) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    left = _flatten(before)
    right = _flatten(after)
    reduced: list[dict[str, Any]] = []
    increased: list[dict[str, Any]] = []
    for key in sorted(set(left) & set(right)):
        old = left[key]
        new = right[key]
        if isinstance(old, bool) or isinstance(new, bool):
            continue
        if isinstance(old, (int, float)) and isinstance(new, (int, float)) and old != new:
            row = {"metric": key, "p0": old, "current": new, "delta": round(new - old, 6)}
            (reduced if new < old else increased).append(row)
    return reduced, increased


def _observed_public_targets(architecture: Mapping[str, Any]) -> set[str]:
    return {
        str(row["importedTarget"])
        for row in architecture.get("installed", {}).get(
            "observedPublicImportedTargets", []
        )
        if isinstance(row, dict) and row.get("importedTarget")
    }


def _observed_public_headers(architecture: Mapping[str, Any]) -> set[str]:
    groups = (
        architecture.get("installed", {})
        .get("installedPublicHeaders", {})
        .get("groups", {})
    )
    return {
        str(path)
        for group in groups.values()
        if isinstance(group, dict)
        for path in group.get("paths", [])
    }


def _legacy_oracle_identities(
    architecture: Mapping[str, Any],
) -> dict[str, Mapping[str, Any]]:
    groups = architecture.get("legacyImplementationOracle", {}).get(
        "sourceIdentities", {}
    )
    if not isinstance(groups, dict):
        return {}
    return {
        str(row["path"]): row
        for rows in groups.values()
        if isinstance(rows, list)
        for row in rows
        if isinstance(row, dict) and row.get("path")
    }


def compare(arguments: argparse.Namespace) -> int:
    baseline = _load_json(arguments.baseline.resolve(), "immutable P0 baseline")
    current = _load_json(arguments.current.resolve(), "current architecture capture")
    if baseline.get("formatVersion") != FORMAT_VERSION or current.get("formatVersion") != FORMAT_VERSION:
        raise BaselineError("compare requires formatVersion 1 captures")
    for description, value in (("baseline", baseline), ("current capture", current)):
        missing_categories = {
            "provenance",
            "externalContract",
            "architectureMeasurements",
            "ownerLiveEvidence",
        } - set(value)
        if missing_categories:
            raise BaselineError(
                f"{description} is missing required categories: {', '.join(sorted(missing_categories))}"
            )
        architecture = value["architectureMeasurements"]
        missing_measurements = {"source", "cmake", "installed", "tests"} - set(architecture)
        if missing_measurements:
            raise BaselineError(
                f"{description} is missing required architecture measurements: "
                + ", ".join(sorted(missing_measurements))
            )
    external_changes = _external_contract_differences(
        baseline.get("externalContract"), current.get("externalContract")
    )
    external_sections = sorted(set(baseline.get("externalContract", {})) | set(current.get("externalContract", {})))
    changed_sections = sorted({change.split(".", 1)[0].split(":", 1)[0] for change in external_changes})
    unchanged_sections = [section for section in external_sections if section not in changed_sections]
    before_arch = baseline.get("architectureMeasurements", {})
    after_arch = current.get("architectureMeasurements", {})
    required_public_targets = set(
        baseline["externalContract"]["publicCpp"]["requiredImportedTargetNames"]
    )
    required_public_headers = set(
        baseline["externalContract"]["publicCpp"][
            "requiredInstalledPublicHeaderPaths"
        ]
    )
    before_public_targets = _observed_public_targets(before_arch)
    after_public_targets = _observed_public_targets(after_arch)
    before_public_headers = _observed_public_headers(before_arch)
    after_public_headers = _observed_public_headers(after_arch)
    missing_required_targets = sorted(required_public_targets - after_public_targets)
    missing_required_headers = sorted(required_public_headers - after_public_headers)
    external_changes.extend(
        f"publicCpp.requiredTargets.{name}: missing from installed target inventory"
        for name in missing_required_targets
        if not any(name in change for change in external_changes)
    )
    external_changes.extend(
        f"publicCpp.requiredInstalledPublicHeaderPaths: missing {path} from installed header inventory"
        for path in missing_required_headers
        if not any(path in change for change in external_changes)
    )
    external_changes = sorted(set(external_changes))
    changed_sections = sorted(
        {change.split(".", 1)[0].split(":", 1)[0] for change in external_changes}
    )
    unchanged_sections = [
        section for section in external_sections if section not in changed_sections
    ]
    reduced, increased = _numeric_metric_changes(before_arch, after_arch)

    before_targets = _named_rows(before_arch, ("cmake", "targets"), "name")
    after_targets = _named_rows(after_arch, ("cmake", "targets"), "name")
    before_edges = {
        tuple(edge) for edge in before_arch.get("cmake", {}).get("resolvedInProjectDependencyEdges", [])
    }
    after_edges = {
        tuple(edge) for edge in after_arch.get("cmake", {}).get("resolvedInProjectDependencyEdges", [])
    }
    before_libraries = _named_rows(before_arch, ("installed", "installedLibraries"), "name")
    after_libraries = _named_rows(after_arch, ("installed", "installedLibraries"), "name")
    before_executables = _named_rows(before_arch, ("installed", "installedExecutables"), "name")
    after_executables = _named_rows(after_arch, ("installed", "installedExecutables"), "name")
    before_binaries = {**before_libraries, **before_executables}
    after_binaries = {**after_libraries, **after_executables}
    header_changes = []
    binary_changes = []
    dependency_changes = []
    exported_symbol_fingerprint_changes = []
    for name in sorted(set(before_binaries) & set(after_binaries)):
        old = before_binaries[name]
        new = after_binaries[name]
        if old.get("installedPublicHeaderCount") != new.get("installedPublicHeaderCount"):
            header_changes.append({"library": name, "p0": old.get("installedPublicHeaderCount"), "current": new.get("installedPublicHeaderCount")})
        if old.get("fileSizeBytes") != new.get("fileSizeBytes"):
            binary_changes.append({"binary": name, "p0": old.get("fileSizeBytes"), "current": new.get("fileSizeBytes")})
        if old.get("exportedDynamicSymbolNameSetSha256") != new.get("exportedDynamicSymbolNameSetSha256"):
            exported_symbol_fingerprint_changes.append(
                {
                    "library": name,
                    "p0": old.get("exportedDynamicSymbolNameSetSha256"),
                    "current": new.get("exportedDynamicSymbolNameSetSha256"),
                }
            )
        old_needed = set(old.get("neededLibraries", []))
        new_needed = set(new.get("neededLibraries", []))
        if old_needed != new_needed:
            dependency_changes.append(
                {
                    "binary": name,
                    "added": sorted(new_needed - old_needed),
                    "removed": sorted(old_needed - new_needed),
                }
            )
    before_tests = before_arch.get("tests", {})
    after_tests = after_arch.get("tests", {})
    before_flat = _flatten(before_arch)
    after_flat = _flatten(after_arch)
    before_numeric_paths = {
        key for key, value in before_flat.items() if isinstance(value, (int, float)) and not isinstance(value, bool)
    }
    after_numeric_paths = {
        key for key, value in after_flat.items() if isinstance(value, (int, float)) and not isinstance(value, bool)
    }
    before_dependency = (
        baseline.get("provenance", {}).get("dependencyProvenance", {}).get("snodec", {})
    )
    after_dependency = (
        current.get("provenance", {}).get("dependencyProvenance", {}).get("snodec", {})
    )
    before_production = (
        before_arch.get("source", {})
        .get("scopeClassification", {})
        .get("productionReductionSubject", {})
        .get("metrics", {})
    )
    after_production = (
        after_arch.get("source", {})
        .get("scopeClassification", {})
        .get("productionReductionSubject", {})
        .get("metrics", {})
    )
    before_infrastructure = (
        before_arch.get("source", {})
        .get("scopeClassification", {})
        .get("baselineInfrastructure", {})
    )
    after_infrastructure = (
        after_arch.get("source", {})
        .get("scopeClassification", {})
        .get("baselineInfrastructure", {})
    )
    before_oracle = _legacy_oracle_identities(before_arch)
    after_oracle = _legacy_oracle_identities(after_arch)
    unchanged_oracle = sorted(
        path
        for path in set(before_oracle) & set(after_oracle)
        if before_oracle[path].get("present") is True
        and after_oracle[path].get("present") is True
        and before_oracle[path].get("sha256") == after_oracle[path].get("sha256")
    )
    changed_oracle = sorted(
        path
        for path in set(before_oracle) & set(after_oracle)
        if path not in unchanged_oracle
        and after_oracle[path].get("present") is True
    )
    missing_oracle = sorted(
        path
        for path, row in before_oracle.items()
        if row.get("present") is True
        and (
            path not in after_oracle
            or after_oracle[path].get("present") is not True
        )
    )
    report = {
        "formatVersion": 1,
        "unchangedExternalContracts": unchanged_sections,
        "changedExternalContracts": external_changes,
        "reducedMetrics": reduced,
        "increasedMetrics": increased,
        "missingRequiredPublicTargets": missing_required_targets,
        "addedPublicTargets": sorted(after_public_targets - before_public_targets),
        "missingRequiredPublicHeaders": missing_required_headers,
        "addedPublicHeaders": sorted(after_public_headers - before_public_headers),
        "addedTargets": sorted(set(after_targets) - set(before_targets)),
        "removedTargets": sorted(set(before_targets) - set(after_targets)),
        "dependencyEdgeChanges": {
            "added": [list(edge) for edge in sorted(after_edges - before_edges)],
            "removed": [list(edge) for edge in sorted(before_edges - after_edges)],
        },
        "headerCountChanges": header_changes,
        "binarySizeChanges": binary_changes,
        "exportedSymbolFingerprintChanges": exported_symbol_fingerprint_changes,
        "dynamicDependencyChanges": dependency_changes,
        "addedBinaries": sorted(set(after_binaries) - set(before_binaries)),
        "removedBinaries": sorted(set(before_binaries) - set(after_binaries)),
        "addedMetricPaths": sorted(after_numeric_paths - before_numeric_paths),
        "removedMetricPaths": sorted(before_numeric_paths - after_numeric_paths),
        "snodecDependencyChange": {
            "p0Commit": before_dependency.get("exactCommit"),
            "currentCommit": after_dependency.get("exactCommit"),
            "p0Version": before_dependency.get("version"),
            "currentVersion": after_dependency.get("version"),
            "changed": any(
                before_dependency.get(key) != after_dependency.get(key)
                for key in ("exactCommit", "version")
            ),
            "blocking": False,
        },
        "productionReductionSubjectChanges": _deep_differences(
            before_production, after_production
        ),
        "baselineInfrastructureChanges": _deep_differences(
            before_infrastructure, after_infrastructure
        ),
        "legacyImplementationOracleStatus": {
            "p0IdentityStatus": before_arch.get(
                "legacyImplementationOracle", {}
            ).get("identityStatus"),
            "currentIdentityStatus": after_arch.get(
                "legacyImplementationOracle", {}
            ).get("identityStatus"),
            "unchangedSourceIdentities": unchanged_oracle,
            "changedOrReplacedSourceIdentities": changed_oracle,
            "missingSourceIdentities": missing_oracle,
            "addedSourceIdentities": sorted(set(after_oracle) - set(before_oracle)),
            "allP0SourceIdentitiesAbsent": bool(before_oracle)
            and len(missing_oracle) == len(before_oracle),
            "interpretation": "source identity changes are removal/replacement evidence, not proof that the P2 differential and P3 deletion gates passed",
            "currentRemovalGate": after_arch.get(
                "legacyImplementationOracle", {}
            ).get("removalGate"),
        },
        "testCountChanges": {
            "registered": {"p0": before_tests.get("registered"), "current": after_tests.get("registered")},
            "ordinarySuiteTotal": {
                "p0": before_tests.get("ordinarySuite", {}).get("total"),
                "current": after_tests.get("ordinarySuite", {}).get("total"),
            },
            "durationSeconds": {
                "p0": before_tests.get("ordinarySuite", {}).get("durationSeconds"),
                "current": after_tests.get("ordinarySuite", {}).get(
                    "durationSeconds"
                ),
            },
        },
        "failurePolicy": "external-contract drift only; architecture changes are diagnostic",
    }
    if arguments.output:
        _write_json(arguments.output.resolve(), report)
        print(f"comparison written to {arguments.output}")
    else:
        print(json.dumps(report, ensure_ascii=False, sort_keys=True, indent=2))
    return 0 if not external_changes or arguments.diagnostic_only else 1


def self_test(_: argparse.Namespace) -> int:
    if _physical_lines(b"") != 0 or _physical_lines(b"a") != 1 or _physical_lines(b"a\nb\n") != 2:
        raise BaselineError("physical-line self-test failed")
    first = {"b": [2, 1], "a": "x"}
    second = {"a": "x", "b": [2, 1]}
    if _canonical_json_bytes(first) != _canonical_json_bytes(second):
        raise BaselineError("canonical JSON self-test failed")
    if _deep_differences({"a": 1}, {"a": 2}) != ["a: 1 -> 2"]:
        raise BaselineError("comparison self-test failed")
    required = {
        "blocking": True,
        "publicCpp": {
            "requiredTargets": [
                {
                    "importedTarget": "AISuite::Required",
                    "outputLibrary": "librequired.so",
                    "requiredInstalledPublicHeaderPaths": ["include/required.h"],
                }
            ],
            "requiredImportedTargetNames": ["AISuite::Required"],
            "requiredInstalledPublicHeaderPaths": ["include/required.h"],
            "additiveTargetsPermitted": True,
            "additivePublicHeadersPermitted": True,
        },
    }
    additive = json.loads(json.dumps(required))
    additive["publicCpp"]["requiredTargets"].append(
        {
            "importedTarget": "AISuite::Added",
            "outputLibrary": "libadded.so",
            "requiredInstalledPublicHeaderPaths": ["include/added.h"],
        }
    )
    additive["publicCpp"]["requiredImportedTargetNames"].append("AISuite::Added")
    additive["publicCpp"]["requiredInstalledPublicHeaderPaths"].append(
        "include/added.h"
    )
    if _external_contract_differences(required, additive):
        raise BaselineError("additive target/header compatibility self-test failed")
    removed = json.loads(json.dumps(required))
    removed["publicCpp"]["requiredTargets"] = []
    removed["publicCpp"]["requiredInstalledPublicHeaderPaths"] = []
    if not _external_contract_differences(required, removed):
        raise BaselineError("required target/header removal self-test failed")
    with tempfile.TemporaryDirectory(prefix="aisuite-p0-self-test-") as temporary:
        output = pathlib.Path(temporary) / "normalized.json"
        _write_json(output, {"relative": "docs/example.md", "sorted": sorted({"b", "a"})})
        if _load_json(output, "self-test output")["sorted"] != ["a", "b"]:
            raise BaselineError("deterministic output self-test failed")
    print("baseline tool self-tests passed")
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    capture_parser = subparsers.add_parser("capture", help="capture normalized protocol, build, install, source, and test evidence")
    capture_parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    capture_parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    capture_parser.add_argument("--install-dir", type=pathlib.Path, required=True)
    capture_parser.add_argument("--snodec-source-dir", type=pathlib.Path, required=True)
    capture_parser.add_argument("--snodec-build-dir", type=pathlib.Path, required=True)
    capture_parser.add_argument("--output", type=pathlib.Path, required=True)
    capture_parser.add_argument("--ctest-inventory", type=pathlib.Path)
    capture_parser.add_argument("--ctest-results", type=pathlib.Path, required=True)
    capture_parser.add_argument("--ctest-parallelism", type=int, default=2)
    capture_parser.add_argument("--baseline-parent")
    capture_parser.set_defaults(handler=capture)

    validate_parser = subparsers.add_parser(
        "validate-baseline",
        help="validate source-only shape and generated-authority currentness without claiming build/install verification",
    )
    validate_parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    validate_parser.add_argument(
        "--baseline", type=pathlib.Path, default=pathlib.Path(BASELINE_RELATIVE)
    )
    validate_parser.set_defaults(handler=validate_baseline)

    verify_parser = subparsers.add_parser(
        "verify-contract",
        help="verify blocking external contract using configured build, install, executable, and dependency evidence",
    )
    verify_parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    verify_parser.add_argument("--baseline", type=pathlib.Path, default=pathlib.Path(BASELINE_RELATIVE))
    verify_parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    verify_parser.add_argument("--install-dir", type=pathlib.Path, required=True)
    verify_parser.add_argument("--snodec-source-dir", type=pathlib.Path, required=True)
    verify_parser.add_argument("--snodec-build-dir", type=pathlib.Path, required=True)
    verify_parser.set_defaults(handler=verify_contract)

    compare_parser = subparsers.add_parser("compare", help="compare a current capture with immutable P0")
    compare_parser.add_argument("--baseline", type=pathlib.Path, required=True)
    compare_parser.add_argument("--current", type=pathlib.Path, required=True)
    compare_parser.add_argument("--output", type=pathlib.Path)
    compare_parser.add_argument("--diagnostic-only", action="store_true")
    compare_parser.set_defaults(handler=compare)

    self_test_parser = subparsers.add_parser("self-test", help="run deterministic standard-library-only self-tests")
    self_test_parser.set_defaults(handler=self_test)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if getattr(arguments, "ctest_parallelism", 1) < 1:
        parser.error("--ctest-parallelism must be positive")
    try:
        return int(arguments.handler(arguments))
    except BaselineError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
