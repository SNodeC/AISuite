#!/usr/bin/env python3
"""Focused mutations for the shared C++ frontend differential border."""

from __future__ import annotations

import argparse
import copy
import json
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[3]
GENERATED_CORPUS = ROOT / "tests/component/codex/fixtures/frontend-protocol-v1.generated.json"
REDUCER_CORPUS = ROOT / "tests/component/codex/fixtures/frontend-client-reducer/conformance.json"
PROTOCOL_MANIFEST = ROOT / "docs/ai/openai/codex/frontend-protocol-v1.manifest.json"
SECRET_SENTINEL = "codex-differential-secret-must-not-escape"


def load_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"{path} is not a JSON object")
    return value


def projection_fixture(reducer: dict[str, Any]) -> dict[str, Any]:
    for case in reducer.get("cases", []):
        metadata = case.get("expectedState", {}).get("projectionMetadata", {})
        if metadata.get("omittedFields") and metadata.get("redactedFields"):
            return case
    raise RuntimeError("reviewed reducer corpus has no omission/redaction comparison")


def observation() -> dict[str, Any]:
    generated = load_json(GENERATED_CORPUS)
    manifest = load_json(PROTOCOL_MANIFEST)
    reducer_case = projection_fixture(load_json(REDUCER_CORPUS))

    events: list[dict[str, Any]] = []
    sequence = 40
    for fixture_event in generated["expandedEvents"]:
        event = copy.deepcopy(fixture_event)
        event["sequence"] = sequence
        event["occurrenceId"] = f"occurrence-{event['type']}"
        events.append(event)
        sequence += 1

    item_index = next(index for index, event in enumerate(events) if event["type"] == "item.upserted")
    content_index = next(index for index, event in enumerate(events) if event["type"] == "item.content.updated")
    events[content_index]["sequence"] = events[item_index]["sequence"]
    events[content_index]["occurrenceId"] = events[item_index]["occurrenceId"]
    for index in range(content_index + 1, len(events)):
        events[index]["sequence"] -= 1

    methods = [
        {
            "id": row["id"],
            "controllerRequired": row["controllerRequired"],
            "requiredScopes": row["requiredScopes"],
            "resultType": row["resultType"],
        }
        for row in manifest["methods"]
    ]
    return {
        "events": events,
        "threadItems": copy.deepcopy(generated["expandedSnapshot"]["state"]["items"]),
        "projection": copy.deepcopy(reducer_case["expectedState"]["projectionMetadata"]),
        "methods": methods,
        "notifications": copy.deepcopy(manifest["notificationMappings"]),
        "replay": {
            "requestedAfter": 12,
            "oldestReplayableAfter": 20,
            "outcome": "replay_gap",
            "connectionClosed": False,
        },
        "state": {
            "visibleSequence": events[-1]["sequence"],
            "canonicalSnapshot": copy.deepcopy(generated["expandedSnapshot"]["state"]),
            "reducedState": copy.deepcopy(reducer_case["expectedState"]),
        },
        "lifecycle": {
            "physicalGeneration": 2,
            "commandAttempts": [
                {
                    "requestId": "request-7",
                    "physicalGeneration": 1,
                    "method": "model.list",
                    "kind": "command",
                },
                {
                    "requestId": "reverse-8",
                    "physicalGeneration": 1,
                    "method": "request.approval.respond",
                    "kind": "reverse-response",
                },
            ],
            "controllerOwnedByGeneration": {"1": True, "2": False},
        },
        "queue": {"maximumMessages": 2, "retainedMessages": 2},
        "callbacks": {
            "physicalGeneration": 2,
            "delivered": [{"name": "state.changed", "physicalGeneration": 2}],
        },
    }


def compare_with_probe(probe: Path, oracle: dict[str, Any], candidate: dict[str, Any]) -> dict[str, str] | None:
    completed = subprocess.run(
        [str(probe)],
        input=json.dumps({"oracle": oracle, "candidate": candidate}, ensure_ascii=False, separators=(",", ":")),
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"C++ differential probe failed: {completed.stderr.strip()}")
    value = json.loads(completed.stdout)
    if value is None:
        return None
    if not (
        isinstance(value, dict)
        and isinstance(value.get("path"), str)
        and isinstance(value.get("oldValue"), str)
        and isinstance(value.get("newValue"), str)
    ):
        raise RuntimeError("C++ differential probe returned an invalid mismatch document")
    return value


def wrong_event_discriminator(value: dict[str, Any]) -> str:
    value["events"][0]["type"] = value["events"][1]["type"]
    return "$/events/0/type"


def snake_case_item_discriminator(value: dict[str, Any]) -> str:
    index = next(index for index, item in enumerate(value["threadItems"]) if item["type"] == "commandExecution")
    value["threadItems"][index]["type"] = "command_execution"
    return f"$/threadItems/{index}/type"


def missing_thread_item(value: dict[str, Any]) -> str:
    value["threadItems"].pop()
    return "$/threadItems/size"


def wrong_sequence(value: dict[str, Any]) -> str:
    index = len(value["events"]) - 1
    value["events"][index]["sequence"] += 1
    return f"$/events/{index}/sequence"


def invalid_equal_sequence_group(value: dict[str, Any]) -> str:
    index = next(index for index, event in enumerate(value["events"]) if event["type"] == "item.content.updated")
    value["events"][index]["occurrenceId"] = "unrelated-occurrence"
    return f"$/events/{index}/occurrenceId"


def missing_redaction(value: dict[str, Any]) -> str:
    value["projection"]["redactedFields"].clear()
    return "$/projection/redactedFields/size"


def extra_secret_field(value: dict[str, Any]) -> str:
    value["state"]["canonicalSnapshot"]["accounts"]["accessToken"] = SECRET_SENTINEL
    return "$/state/canonicalSnapshot/accounts/accessToken"


def nested_secret_container(value: dict[str, Any]) -> str:
    value["state"]["canonicalSnapshot"]["accounts"] = [{"apiKey": SECRET_SENTINEL}]
    return "$/state/canonicalSnapshot/accounts"


def wrong_omitted_field_path(value: dict[str, Any]) -> str:
    value["projection"]["omittedFields"][0] += "/wrong"
    return "$/projection/omittedFields/0"


def wrong_method_identity(value: dict[str, Any]) -> str:
    value["methods"][0]["id"] += "Wrong"
    return "$/methods/0/id"


def missing_method(value: dict[str, Any]) -> str:
    value["methods"].pop()
    return "$/methods/size"


def wrong_controller_requirement(value: dict[str, Any]) -> str:
    index = next(index for index, method in enumerate(value["methods"]) if method["controllerRequired"])
    value["methods"][index]["controllerRequired"] = False
    return f"$/methods/{index}/controllerRequired"


def wrong_scope_requirement(value: dict[str, Any]) -> str:
    index = next(index for index, method in enumerate(value["methods"]) if len(method["requiredScopes"]) > 1)
    value["methods"][index]["requiredScopes"] = value["methods"][index]["requiredScopes"][:-1]
    return f"$/methods/{index}/requiredScopes/size"


def wrong_method_result_type(value: dict[str, Any]) -> str:
    index = next(index for index, method in enumerate(value["methods"]) if method["resultType"] != "Unit")
    value["methods"][index]["resultType"] = "Unit"
    return f"$/methods/{index}/resultType"


def missing_notification(value: dict[str, Any]) -> str:
    value["notifications"].pop()
    return "$/notifications/size"


def wrong_notification_identity(value: dict[str, Any]) -> str:
    value["notifications"][0]["registryKey"] += "/wrong"
    return "$/notifications/0/registryKey"


def wrong_notification_mapping(value: dict[str, Any]) -> str:
    value["notifications"][0]["expandedMappings"].append("diagnostics.updated")
    return "$/notifications/0/expandedMappings/size"


def wrong_replay_gap_behavior(value: dict[str, Any]) -> str:
    value["replay"]["outcome"] = "accepted"
    return "$/replay/outcome"


def state_reducer_omission(value: dict[str, Any]) -> str:
    value["state"]["reducedState"]["items"] = []
    return "$/state/reducedState/items/size"


def command_retry_after_disconnect(value: dict[str, Any]) -> str:
    attempt = next(row for row in value["lifecycle"]["commandAttempts"] if row["kind"] == "command")
    retried = copy.deepcopy(attempt)
    retried["physicalGeneration"] = 2
    value["lifecycle"]["commandAttempts"].append(retried)
    return "$/lifecycle/commandAttempts/size"


def reverse_response_retry_after_disconnect(value: dict[str, Any]) -> str:
    attempt = next(row for row in value["lifecycle"]["commandAttempts"] if row["kind"] == "reverse-response")
    retried = copy.deepcopy(attempt)
    retried["physicalGeneration"] = 2
    value["lifecycle"]["commandAttempts"].append(retried)
    return "$/lifecycle/commandAttempts/size"


def wrong_physical_generation(value: dict[str, Any]) -> str:
    value["lifecycle"]["physicalGeneration"] = 3
    return "$/lifecycle/physicalGeneration"


def automatic_controller_restoration(value: dict[str, Any]) -> str:
    value["lifecycle"]["controllerOwnedByGeneration"]["2"] = True
    return "$/lifecycle/controllerOwnedByGeneration/2"


def queue_bound_violation(value: dict[str, Any]) -> str:
    value["queue"]["retainedMessages"] = 3
    return "$/queue/retainedMessages"


def stale_callback_from_old_generation(value: dict[str, Any]) -> str:
    value["callbacks"]["delivered"].append({"name": "operation.completed", "physicalGeneration": 1})
    return "$/callbacks/delivered/size"


Mutation = tuple[str, str, Callable[[dict[str, Any]], str]]

MUTATIONS: tuple[Mutation, ...] = (
    ("wrong event discriminator", "event.discriminator", wrong_event_discriminator),
    ("snake_case versus canonical item discriminator", "item.discriminator", snake_case_item_discriminator),
    ("missing ThreadItem authority member", "item.discriminator", missing_thread_item),
    ("wrong sequence", "sequence.value", wrong_sequence),
    ("invalid equal-sequence grouping", "sequence.group", invalid_equal_sequence_group),
    ("missing redaction", "projection.redaction", missing_redaction),
    ("extra secret field", "projection.secret", extra_secret_field),
    ("secret nested below a mismatched container", "projection.secret", nested_secret_container),
    ("wrong omitted-field path", "projection.omitted_path", wrong_omitted_field_path),
    ("wrong method identity", "method.identity", wrong_method_identity),
    ("missing method authority member", "method.identity", missing_method),
    ("wrong controller requirement", "method.controller_requirement", wrong_controller_requirement),
    ("wrong scope requirement", "method.scope_requirement", wrong_scope_requirement),
    ("wrong method result type", "method.result_type", wrong_method_result_type),
    ("missing notification authority member", "notification.identity", missing_notification),
    ("wrong notification identity", "notification.identity", wrong_notification_identity),
    ("wrong notification projection mapping", "notification.mapping", wrong_notification_mapping),
    ("wrong replay-gap behavior", "replay.gap", wrong_replay_gap_behavior),
    ("state reducer omission", "state.reducer", state_reducer_omission),
    ("wrong active physical generation", "lifecycle:physical-generation:client", wrong_physical_generation),
    ("command retry after disconnect", "lifecycle:command-retry:client", command_retry_after_disconnect),
    ("reverse response retry after disconnect", "lifecycle:reverse-response-retry:client", reverse_response_retry_after_disconnect),
    ("automatic controller restoration", "lifecycle:controller-restore:client", automatic_controller_restoration),
    ("queue-bound violation", "queue:outbound-bound:server", queue_bound_violation),
    ("stale callback from an old connection generation", "callback:stale-generation:client", stale_callback_from_old_generation),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True, help="compiled shared C++ differential comparison probe")
    arguments = parser.parse_args()

    try:
        oracle = observation()
        baseline_mismatch = compare_with_probe(arguments.probe, oracle, copy.deepcopy(oracle))
        if baseline_mismatch is not None:
            raise RuntimeError(f"unmutated C++ comparison unexpectedly failed: {baseline_mismatch!r}")

        failures: list[str] = []
        for name, mutation_identity, mutate in MUTATIONS:
            candidate = copy.deepcopy(oracle)
            expected_path = mutate(candidate)
            mismatch = compare_with_probe(arguments.probe, oracle, candidate)
            if mismatch is None or mismatch["path"] != expected_path:
                diagnostic = "no mismatch" if mismatch is None else mismatch["path"]
                failures.append(f"{name} ({mutation_identity}): expected {expected_path}, observed {diagnostic}")
            elif mutation_identity == "projection.secret":
                encoded_diagnostic = json.dumps(mismatch, ensure_ascii=False, separators=(",", ":"))
                if SECRET_SENTINEL in encoded_diagnostic:
                    failures.append(f"{name} ({mutation_identity}): secret sentinel escaped in the C++ diagnostic")
                elif mutate is extra_secret_field and mismatch["newValue"] != "<redacted>":
                    failures.append(f"{name} ({mutation_identity}): sensitive leaf was not explicitly redacted")
                elif mutate is nested_secret_container and mismatch["newValue"] != "array(1)":
                    failures.append(f"{name} ({mutation_identity}): mismatched container diagnostic was not shape-only")

        if failures:
            for failure in failures:
                print(f"CodexFrontendDifferentialMutationTest: {failure}", file=sys.stderr)
            return 1
        print(f"shared C++ differential border caught {len(MUTATIONS)} focused mutations at their exact paths")
        return 0
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"CodexFrontendDifferentialMutationTest: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
