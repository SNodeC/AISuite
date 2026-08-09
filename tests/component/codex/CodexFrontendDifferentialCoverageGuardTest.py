#!/usr/bin/env python3
"""Verify that the P2 differential corpus covers every generated authority member.

The coverage fixture is intentionally rendered from the reviewed/generated JSON
authorities plus a small declared set of runtime execution invariants.  This
test owns no second list of methods, events, items, scopes, or capabilities: an
authority change changes the expected document and makes the committed coverage
declaration stale.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any, Iterable


ROOT = Path(__file__).resolve().parents[3]
PROTOCOL_MANIFEST = ROOT / "docs/ai/openai/codex/frontend-protocol-v1.manifest.json"
PROTOCOL_SCHEMA = ROOT / "docs/ai/openai/codex/frontend-protocol-v1.schema.json"
GENERATED_FIXTURE = ROOT / "tests/component/codex/fixtures/frontend-protocol-v1.generated.json"
REDUCER_FIXTURE = ROOT / "tests/component/codex/fixtures/frontend-client-reducer/conformance.json"
REGISTRY_SOURCE = ROOT / "tools/frontend/frontend-registry-source.json"
CLIENT_BINDINGS = ROOT / "tools/frontend/cpp-client-bindings.json"
P0_BASELINE = ROOT / "docs/ai/openai/codex/architecture-reduction/p0-baseline.json"
DEFAULT_COVERAGE = ROOT / "tests/component/codex/fixtures/p2-frontend-differential-coverage.json"


class CoverageError(RuntimeError):
    pass


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CoverageError(f"cannot load {path}: {error}") from error


def canonical_hash(value: Any) -> str:
    encoded = json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise CoverageError(message)


def unique(values: Iterable[str], description: str) -> list[str]:
    result = list(values)
    require(len(result) == len(set(result)), f"{description} contains duplicate identities")
    return result


def resolve_schema_reference(schema: dict[str, Any], reference: str, description: str) -> Any:
    require(reference.startswith("#/"), f"{description} is not a local schema reference: {reference!r}")
    node: Any = schema
    for encoded in reference[2:].split("/"):
        token = encoded.replace("~1", "/").replace("~0", "~")
        require(isinstance(node, dict) and token in node, f"{description} is unresolved: {reference!r}")
        node = node[token]
    return node


def ownership(method: dict[str, Any]) -> str:
    if method.get("frontendNative") is True:
        return "native"
    category = method.get("category")
    if category == "provider_operation":
        return "provider"
    if category == "reverse_response":
        return "reverse"
    raise CoverageError(f"method {method.get('id')!r} has an unclassified ownership/category")


def cases(prefix: str, identity: str, *dimensions: str) -> list[str]:
    return [f"{prefix}:{identity}:{dimension}" for dimension in dimensions]


def execution_suite(case_id: str) -> str:
    """Assign a declared case to the executable that must prove it.

    The mapping is structural: identities still come only from generated
    authorities, and the ledger carries those exact case IDs.  Keeping this
    routing here avoids a second hand-maintained list of 105 methods or any
    other authority family.
    """

    parts = case_id.split(":")
    require(len(parts) >= 3, f"invalid differential coverage identity {case_id!r}")
    family = parts[0]
    dimension = parts[-1]
    server_dimensions = {
        "server",
        "server-dispatch",
        "server-legacy",
        "server-expanded",
        "parameter-schema",
        "security",
        "readiness",
        "controller",
        "projection-legacy",
        "projection-expanded",
        "projection",
        "advertisement",
        "negotiation",
        "encode",
        "mapping",
        "method",
        "snapshot",
        "replay",
        "live",
        "sequence",
    }
    client_dimensions = {
        "client",
        "client-dispatch",
        "client-legacy",
        "client-expanded",
        "result-schema",
        "reducer",
        "reduction-legacy",
        "reduction-expanded",
        "reduction",
        "response",
        "client-validation",
        "decode",
    }
    if dimension in server_dimensions:
        return "server"
    if dimension in client_dimensions:
        return "client"
    raise CoverageError(f"unrouted {family} differential coverage dimension {dimension!r} in {case_id!r}")


def derive_document() -> dict[str, Any]:
    manifest = load_json(PROTOCOL_MANIFEST)
    schema = load_json(PROTOCOL_SCHEMA)
    generated_fixture = load_json(GENERATED_FIXTURE)
    reducer_fixture = load_json(REDUCER_FIXTURE)
    registry = load_json(REGISTRY_SOURCE)
    bindings = load_json(CLIENT_BINDINGS)
    baseline = load_json(P0_BASELINE)
    p0_protocol = baseline.get("externalContract", {}).get("protocol", {})

    methods = manifest.get("methods")
    require(isinstance(methods, list), "protocol manifest has no method authority")
    method_ids = unique((row.get("id") for row in methods), "protocol methods")
    method_names = unique((row.get("method") for row in methods), "protocol method wire names")
    require(all(isinstance(value, str) and value for value in method_ids + method_names), "protocol method identity is empty")

    binding_rows = bindings.get("bindings")
    require(isinstance(binding_rows, list), "C++ client binding authority is missing")
    binding_by_id = {row.get("methodId"): row for row in binding_rows}
    require(len(binding_by_id) == len(binding_rows), "C++ client binding authority contains duplicate MethodIds")
    require(set(binding_by_id) == set(method_ids), "C++ client bindings do not exactly cover generated MethodIds")

    generated_methods = generated_fixture.get("methods")
    require(isinstance(generated_methods, list), "generated protocol fixture has no method corpus")
    require(
        {row.get("method") for row in generated_methods} == set(method_names),
        "generated protocol fixture does not exactly cover method wire names",
    )

    method_coverage: list[dict[str, Any]] = []
    for row in methods:
        method_id = row["id"]
        method_name = row["method"]
        method_ownership = ownership(row)
        binding = binding_by_id[method_id]
        require(binding.get("category") == method_ownership, f"{method_id}: manifest/binding ownership mismatch")

        parameter_schema = row.get("parameterSchema")
        result_schema = row.get("resultSchema")
        require(isinstance(parameter_schema, str), f"{method_id}: missing parameter-schema reference")
        require(isinstance(result_schema, str), f"{method_id}: missing result-schema reference")
        resolve_schema_reference(schema, parameter_schema, f"{method_id} parameter schema")
        resolve_schema_reference(schema, result_schema, f"{method_id} result schema")

        required_scopes = row.get("requiredScopes")
        require(isinstance(required_scopes, list), f"{method_id}: requiredScopes is not an array")
        require(isinstance(row.get("securityDecision"), str), f"{method_id}: missing security classification")
        require(isinstance(row.get("controllerRequired"), bool), f"{method_id}: missing controller classification")
        require(isinstance(row.get("providerReadyRequired"), bool), f"{method_id}: missing readiness classification")
        method_coverage.append(
            {
                "id": method_id,
                "method": method_name,
                "ownership": method_ownership,
                "parameterSchema": parameter_schema,
                "resultSchema": result_schema,
                "resultType": row.get("resultType"),
                "securityDecision": row["securityDecision"],
                "requiredScopes": required_scopes,
                "controllerRequired": row["controllerRequired"],
                "providerReadyRequired": row["providerReadyRequired"],
                "cases": cases(
                    "method",
                    method_id,
                    "server-dispatch",
                    "client-dispatch",
                    "parameter-schema",
                    "result-schema",
                    "security",
                    "readiness",
                    "controller",
                ),
            }
        )

    parameter_references = unique((row["parameterSchema"] for row in method_coverage), "method parameter schemas")
    result_references = unique((row["resultSchema"] for row in method_coverage), "method result schemas")

    event_families = unique(manifest.get("eventFamilies", []), "expanded event families")
    fixture_event_families = unique(
        (row.get("type") for row in generated_fixture.get("expandedEvents", [])), "generated expanded-event fixtures"
    )
    require(set(fixture_event_families) == set(event_families), "generated fixtures do not exactly cover expanded event families")
    schema_events = schema.get("$defs", {}).get("ExpandedEventType", {}).get("enum")
    require(schema_events == event_families, "schema expanded-event enum differs from manifest order/content")

    notification_mappings = manifest.get("notificationMappings")
    require(isinstance(notification_mappings, list), "manifest notification projections are missing")
    notification_fields = {
        "registryKey",
        "finalExposure",
        "securityDecision",
        "legacyContract",
        "expandedMappings",
        "requiredScopes",
        "redactionClass",
        "capability",
        "legacyCapabilityBehavior",
        "expandedCapabilityBehavior",
        "duplicateSuppression",
    }
    require(all(isinstance(row, dict) for row in notification_mappings), "notification mapping is not an object")
    notification_registry_keys = [row.get("registryKey") for row in notification_mappings]
    require(
        all(isinstance(registry_key, str) and registry_key for registry_key in notification_registry_keys),
        "notification mapping contains an empty registry key",
    )
    notification_registry_keys = unique(notification_registry_keys, "notification registry keys")
    notification_prefix = "server_notification:ServerNotification:method:"
    for row in notification_mappings:
        registry_key = row.get("registryKey")
        require(set(row) == notification_fields, f"{registry_key!r}: notification mapping fields differ from authority contract")
        require(
            isinstance(registry_key, str)
            and registry_key.startswith(notification_prefix)
            and len(registry_key) > len(notification_prefix),
            f"notification registry key is malformed: {registry_key!r}",
        )
        for field in notification_fields - {"registryKey", "expandedMappings", "requiredScopes"}:
            require(isinstance(row.get(field), str) and row[field], f"{registry_key}: notification {field} is empty")
        expanded_mappings = row.get("expandedMappings")
        require(isinstance(expanded_mappings, list) and expanded_mappings, f"{registry_key}: expandedMappings is empty")
        require(
            all(isinstance(value, str) and value for value in expanded_mappings),
            f"{registry_key}: expandedMappings contains an invalid event identity",
        )
        unique(expanded_mappings, f"{registry_key} expanded mappings")
        unknown_events = set(expanded_mappings) - set(event_families)
        require(not unknown_events, f"{registry_key}: unknown expanded event mappings {sorted(unknown_events)}")
        required_scopes = row.get("requiredScopes")
        require(isinstance(required_scopes, list) and required_scopes, f"{registry_key}: requiredScopes is empty")
        require(
            all(isinstance(value, str) and value for value in required_scopes),
            f"{registry_key}: requiredScopes contains an invalid scope identity",
        )
        unique(required_scopes, f"{registry_key} required scopes")

    item_mappings = manifest.get("threadItemMappings")
    require(isinstance(item_mappings, list), "manifest ThreadItem projections are missing")
    item_discriminators = unique(
        (row.get("registryKey", "").rsplit(":", 1)[-1] for row in item_mappings), "ThreadItem discriminators"
    )
    fixture_items = generated_fixture.get("expandedSnapshot", {}).get("state", {}).get("items", [])
    require(
        {row.get("type") for row in fixture_items} == set(item_discriminators),
        "generated expanded snapshot does not exactly cover ThreadItem discriminators",
    )

    pending_mappings = manifest.get("pendingRequestMappings")
    require(isinstance(pending_mappings, list), "manifest pending-request projections are missing")
    pending_kinds = unique((row.get("kind") for row in pending_mappings), "pending-request kinds")
    fixture_pending = generated_fixture.get("expandedSnapshot", {}).get("state", {}).get("pendingRequests", [])
    require(
        {row.get("kind") for row in fixture_pending} == set(pending_kinds),
        "generated expanded snapshot does not exactly cover pending-request kinds",
    )
    require(
        schema.get("$defs", {}).get("PendingRequestKind", {}).get("enum") == pending_kinds,
        "schema pending-request enum differs from manifest order/content",
    )

    scopes = unique(registry.get("scopeStrings", []), "frontend scopes")
    require(manifest.get("scopeProfiles", {}).get("local_trusted") == scopes, "manifest local scope profile differs from registry")
    require(schema.get("$defs", {}).get("FrontendScope", {}).get("enum") == scopes, "schema scope enum differs from registry")
    scope_set = set(scopes)
    for row in methods:
        unknown = set(row["requiredScopes"]) - scope_set
        require(not unknown, f"{row['id']}: unknown required scopes {sorted(unknown)}")
    for row in notification_mappings:
        unknown = set(row["requiredScopes"]) - scope_set
        require(not unknown, f"{row['registryKey']}: unknown required scopes {sorted(unknown)}")

    capability_rows = manifest.get("capabilities")
    require(isinstance(capability_rows, list), "manifest capability catalog is missing")
    capability_keys = unique((row.get("key") for row in capability_rows), "frontend capabilities")
    require(
        schema.get("$defs", {}).get("FrontendCapability", {}).get("enum") == capability_keys,
        "schema capability enum differs from manifest order/content",
    )
    for row in notification_mappings:
        require(
            row["capability"] in capability_keys,
            f"{row['registryKey']}: unknown notification capability {row['capability']!r}",
        )
    capabilities_by_category = {
        category: [row["key"] for row in capability_rows if row.get("category") == category]
        for category in ("static_mechanism", "conditional_topology", "product")
    }
    p0_capability_categories = p0_protocol.get("capabilityCategories", {})
    require(
        set(capabilities_by_category) == set(p0_capability_categories)
        and all(
            set(values) == set(p0_capability_categories[category])
            for category, values in capabilities_by_category.items()
        ),
        "manifest capability categories differ from the frozen P0 authority",
    )

    message_kinds = unique(manifest.get("messageKinds", []), "message kinds")
    sync_modes = schema.get("$defs", {}).get("Welcome", {}).get("allOf", [{}, {}])[1].get("properties", {}).get("syncMode", {}).get("enum")
    require(isinstance(sync_modes, list), "Welcome synchronization-mode authority is missing")
    sync_modes = unique(sync_modes, "synchronization modes")

    reducer_event_types = {
        event.get("type")
        for case in reducer_fixture.get("cases", [])
        for protocol_input in case.get("orderedProtocolInputs", [])
        if protocol_input.get("kind") == "events"
        for event in protocol_input.get("events", [])
    }
    missing_reducer_events = set(event_families) - reducer_event_types
    require(not missing_reducer_events, f"client reducer fixture misses events {sorted(missing_reducer_events)}")

    derived_counts = {
        "messageKinds": len(message_kinds),
        "methods": len(methods),
        "nativeMethods": sum(row["ownership"] == "native" for row in method_coverage),
        "providerMethods": sum(row["ownership"] == "provider" for row in method_coverage),
        "reverseMethods": sum(row["ownership"] == "reverse" for row in method_coverage),
        "parameterSchemaReferences": len(parameter_references),
        "resultSchemaReferences": len(result_references),
        "expandedEventFamilies": len(event_families),
        "notifications": len(notification_registry_keys),
        "threadItems": len(item_discriminators),
        "pendingRequests": len(pending_kinds),
        "scopes": len(scopes),
        "capabilities": len(capability_keys),
        "staticMechanismCapabilities": len(capabilities_by_category["static_mechanism"]),
        "conditionalTopologyCapabilities": len(capabilities_by_category["conditional_topology"]),
        "productCapabilities": len(capabilities_by_category["product"]),
    }
    manifest_counts = manifest.get("counts", {})
    expected_count_sources = {
        "messageKinds": manifest_counts.get("messageKinds"),
        "methods": manifest_counts.get("methods"),
        "nativeMethods": manifest_counts.get("frontendNativeMethods"),
        "providerMethods": manifest_counts.get("providerOperationMethods"),
        "reverseMethods": manifest_counts.get("reverseMethods"),
        "expandedEventFamilies": manifest_counts.get("expandedEventFamilies"),
        "notifications": manifest_counts.get("notifications"),
        "threadItems": manifest_counts.get("threadItems"),
        "pendingRequests": manifest_counts.get("pendingRequests"),
    }
    for key, expected in expected_count_sources.items():
        require(derived_counts[key] == expected, f"derived {key}={derived_counts[key]} differs from manifest count {expected}")
    p0_expected = {
        "messageKinds": p0_protocol.get("messageKindCount"),
        "methods": p0_protocol.get("methodCount"),
        "nativeMethods": p0_protocol.get("methodOwnershipCounts", {}).get("native"),
        "providerMethods": p0_protocol.get("methodOwnershipCounts", {}).get("provider"),
        "reverseMethods": p0_protocol.get("methodOwnershipCounts", {}).get("reverse"),
        "expandedEventFamilies": p0_protocol.get("expandedEventFamilyCount"),
        "threadItems": p0_protocol.get("threadItemDiscriminatorCount"),
        "scopes": p0_protocol.get("scopeCount"),
        "capabilities": p0_protocol.get("capabilityCount"),
    }
    for key, expected in p0_expected.items():
        require(derived_counts[key] == expected, f"derived {key}={derived_counts[key]} differs from frozen P0 count {expected}")

    authority_values = {
        str(PROTOCOL_MANIFEST.relative_to(ROOT)): manifest,
        str(PROTOCOL_SCHEMA.relative_to(ROOT)): schema,
        str(GENERATED_FIXTURE.relative_to(ROOT)): generated_fixture,
        str(REDUCER_FIXTURE.relative_to(ROOT)): reducer_fixture,
        str(REGISTRY_SOURCE.relative_to(ROOT)): registry,
        str(CLIENT_BINDINGS.relative_to(ROOT)): bindings,
    }

    return {
        "formatVersion": 2,
        "description": "Authority-derived coverage identities and runtime invariants joined to passed P2 old/new differential execution ledgers.",
        "authorities": {path: canonical_hash(value) for path, value in sorted(authority_values.items())},
        "counts": derived_counts,
        "representations": [
            {
                "id": "legacy-v1",
                "cases": cases("representation", "legacy-v1", "server", "client", "snapshot", "replay", "live"),
            },
            {
                "id": "expanded-v1",
                "cases": cases("representation", "expanded-v1", "server", "client", "snapshot", "replay", "live"),
            },
        ],
        "synchronization": [
            {
                "mode": mode,
                "cases": cases("synchronization", mode, "server", "client", "sequence", "projection"),
            }
            for mode in sync_modes
        ],
        "lifecycle": [
            {
                "invariant": invariant,
                "cases": cases("lifecycle", invariant, "client"),
            }
            for invariant in (
                "physical-generation",
                "command-retry",
                "controller-restore",
                "reverse-response-retry",
            )
        ],
        "callbacks": [
            {
                "invariant": "stale-generation",
                "cases": cases("callback", "stale-generation", "client"),
            }
        ],
        "queue": [
            {
                "invariant": "outbound-bound",
                "cases": cases("queue", "outbound-bound", "server"),
            }
        ],
        "messageKinds": [
            {"kind": kind, "cases": cases("message", kind, "server", "client", "encode", "decode")} for kind in message_kinds
        ],
        "methods": method_coverage,
        "eventFamilies": [
            {
                "type": event_type,
                "cases": cases(
                    "event", event_type, "server-legacy", "server-expanded", "client-legacy", "client-expanded", "reducer"
                ),
            }
            for event_type in event_families
        ],
        "notificationMappings": [
            {
                "registryKey": row["registryKey"],
                "finalExposure": row["finalExposure"],
                "securityDecision": row["securityDecision"],
                "legacyContract": row["legacyContract"],
                "expandedMappings": row["expandedMappings"],
                "requiredScopes": row["requiredScopes"],
                "redactionClass": row["redactionClass"],
                "capability": row["capability"],
                "legacyCapabilityBehavior": row["legacyCapabilityBehavior"],
                "expandedCapabilityBehavior": row["expandedCapabilityBehavior"],
                "duplicateSuppression": row["duplicateSuppression"],
                "cases": cases("notification", row["registryKey"], "mapping"),
            }
            for row in notification_mappings
        ],
        "threadItems": [
            {
                "type": discriminator,
                "cases": cases(
                    "item", discriminator, "projection-legacy", "projection-expanded", "reduction-legacy", "reduction-expanded"
                ),
            }
            for discriminator in item_discriminators
        ],
        "pendingRequests": [
            {
                "kind": row["kind"],
                "providerMethod": row["providerMethod"],
                "responseMethods": row["responseMethods"],
                "cases": cases("pending", row["kind"], "projection", "reduction", "response", "security"),
            }
            for row in pending_mappings
        ],
        "scopes": [
            {"scope": scope, "cases": cases("scope", scope, "method", "snapshot", "live", "replay")} for scope in scopes
        ],
        "capabilities": [
            {
                "key": row["key"],
                "category": row["category"],
                "cases": cases("capability", row["key"], "advertisement", "negotiation", "client-validation"),
            }
            for row in capability_rows
        ],
    }


def first_difference(expected: Any, actual: Any, path: str = "$") -> str | None:
    if type(expected) is not type(actual):
        return f"{path}: expected {type(expected).__name__}, got {type(actual).__name__}"
    if isinstance(expected, dict):
        expected_keys = list(expected)
        actual_keys = list(actual)
        if set(expected_keys) != set(actual_keys):
            missing = sorted(set(expected_keys) - set(actual_keys))
            extra = sorted(set(actual_keys) - set(expected_keys))
            return f"{path}: object members differ; missing={missing!r}, extra={extra!r}"
        for key in expected_keys:
            difference = first_difference(expected[key], actual[key], f"{path}/{key}")
            if difference:
                return difference
        return None
    if isinstance(expected, list):
        if len(expected) != len(actual):
            return f"{path}: expected {len(expected)} entries, got {len(actual)}"
        for index, (expected_value, actual_value) in enumerate(zip(expected, actual, strict=True)):
            difference = first_difference(expected_value, actual_value, f"{path}/{index}")
            if difference:
                return difference
        return None
    if expected != actual:
        return f"{path}: expected {expected!r}, got {actual!r}"
    return None


def validate_case_id_uniqueness(document: dict[str, Any]) -> None:
    seen: dict[str, str] = {}

    def visit(value: Any, path: str) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                if key == "cases":
                    require(isinstance(child, list) and child, f"{path}/cases is empty or not an array")
                    for case_id in child:
                        require(isinstance(case_id, str) and case_id, f"{path}/cases contains an empty identity")
                        previous = seen.get(case_id)
                        require(previous is None, f"duplicate coverage identity {case_id!r} at {previous} and {path}/cases")
                        seen[case_id] = path
                else:
                    visit(child, f"{path}/{key}")
        elif isinstance(value, list):
            for index, child in enumerate(value):
                visit(child, f"{path}/{index}")

    visit(document, "$")
    require(seen, "coverage manifest contains no case identities")


def declared_cases(document: dict[str, Any]) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {"server": set(), "client": set()}

    def visit(value: Any) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                if key == "cases":
                    for case_id in child:
                        result[execution_suite(case_id)].add(case_id)
                else:
                    visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    visit(document)
    require(result["server"], "coverage manifest has no server execution cases")
    require(result["client"], "coverage manifest has no client execution cases")
    return result


def load_execution_ledger(path: Path, expected_suite: str) -> set[str]:
    ledger = load_json(path)
    require(isinstance(ledger, dict), f"{path}: execution ledger is not an object")
    require(ledger.get("formatVersion") == 1, f"{path}: unsupported execution-ledger format")
    require(ledger.get("suite") == expected_suite, f"{path}: expected {expected_suite!r} execution suite")
    require(ledger.get("status") == "passed", f"{path}: differential execution did not report passed status")
    cases_value = ledger.get("cases")
    require(isinstance(cases_value, list), f"{path}: execution-ledger cases are not an array")
    require(
        all(isinstance(case_id, str) and case_id for case_id in cases_value),
        f"{path}: execution ledger contains an invalid case identity",
    )
    require(cases_value == sorted(cases_value), f"{path}: execution-ledger cases are not in canonical order")
    require(len(cases_value) == len(set(cases_value)), f"{path}: execution ledger contains duplicate cases")
    return set(cases_value)


def validate_execution_ledgers(document: dict[str, Any], server_path: Path, client_path: Path) -> None:
    expected = declared_cases(document)
    for suite, path in (("server", server_path), ("client", client_path)):
        actual = load_execution_ledger(path, suite)
        missing = sorted(expected[suite] - actual)
        extra = sorted(actual - expected[suite])
        require(
            not missing and not extra,
            f"{path}: {suite} execution coverage differs; missing={missing!r}, extra={extra!r}",
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--coverage", type=Path, default=DEFAULT_COVERAGE)
    parser.add_argument("--server-ledger", type=Path, help="passed server differential execution ledger")
    parser.add_argument("--client-ledger", type=Path, help="passed client differential execution ledger")
    parser.add_argument(
        "--render",
        action="store_true",
        help="render the authority-derived expected fixture to stdout without modifying the source tree",
    )
    arguments = parser.parse_args()

    try:
        expected = derive_document()
        validate_case_id_uniqueness(expected)
        declared = declared_cases(expected)
        if arguments.render:
            print(json.dumps(expected, indent=2, ensure_ascii=False))
            return 0
        require(arguments.server_ledger is not None, "--server-ledger is required for an executable coverage proof")
        require(arguments.client_ledger is not None, "--client-ledger is required for an executable coverage proof")
        actual = load_json(arguments.coverage)
        difference = first_difference(expected, actual)
        if difference:
            raise CoverageError(
                f"stale/incomplete P2 differential coverage manifest {arguments.coverage}: {difference}; "
                "render the authority-derived replacement with --render"
            )
        validate_case_id_uniqueness(actual)
        validate_execution_ledgers(actual, arguments.server_ledger, arguments.client_ledger)
        print(
            "P2 differential coverage complete: "
            f"{expected['counts']['methods']} methods "
            f"({expected['counts']['nativeMethods']}/"
            f"{expected['counts']['providerMethods']}/"
            f"{expected['counts']['reverseMethods']}), "
            f"{expected['counts']['expandedEventFamilies']} events, "
            f"{expected['counts']['notifications']} notifications, "
            f"{expected['counts']['threadItems']} items, "
            f"{expected['counts']['pendingRequests']} pending kinds, "
            f"{expected['counts']['scopes']} scopes, "
            f"{expected['counts']['capabilities']} capabilities, "
            f"{expected['counts']['messageKinds']} message kinds; "
            f"{len(declared['server'])} server and {len(declared['client'])} client executed comparisons"
        )
        return 0
    except CoverageError as error:
        print(f"CodexFrontendDifferentialCoverageGuardTest: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
