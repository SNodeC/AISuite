#!/usr/bin/env python3

# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

from __future__ import annotations

import argparse
import pathlib
import re


def mask_comments_and_literals(source: str) -> str:
    masked = list(source)
    index = 0
    state = "code"
    while index < len(source):
        character = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if character == "/" and following == "/":
                masked[index] = masked[index + 1] = " "
                index += 2
                state = "line"
                continue
            if character == "/" and following == "*":
                masked[index] = masked[index + 1] = " "
                index += 2
                state = "block"
                continue
            if character == '"':
                masked[index] = " "
                state = "string"
            elif character == "'":
                masked[index] = " "
                state = "character"
        elif state == "line":
            if character == "\n":
                state = "code"
            else:
                masked[index] = " "
        elif state == "block":
            if character == "*" and following == "/":
                masked[index] = masked[index + 1] = " "
                index += 2
                state = "code"
            elif character != "\n":
                masked[index] = " "
        else:
            masked[index] = " " if character != "\n" else "\n"
            if character == "\\" and following:
                masked[index + 1] = " " if following != "\n" else "\n"
                index += 2
                continue
            if (state == "string" and character == '"') or (
                state == "character" and character == "'"
            ):
                state = "code"
        index += 1
    return "".join(masked)


def section(source: str, start: str, end: str) -> str:
    start_at = source.find(start)
    end_at = source.find(end, start_at + len(start)) if start_at >= 0 else -1
    if start_at < 0 or end_at < 0:
        raise SystemExit(f"contract guard could not locate section {start!r}..{end!r}")
    return source[start_at:end_at]


def require_pattern(source: str, pattern: str, description: str) -> None:
    if re.search(pattern, source, re.MULTILINE | re.DOTALL) is None:
        raise SystemExit(f"contract guard missing {description}")


def projection_guard(source: str) -> None:
    masked = mask_comments_and_literals(source)
    if "findNamedString" in masked:
        raise SystemExit("expanded projection must not recursively guess entity identity")
    for shared_helper in ("eventThreadId", "eventTurnId", "eventItemId"):
        if shared_helper in masked:
            raise SystemExit(f"expanded projection must not share cross-family identity helper {shared_helper}")
    if "occurrenceItem" in masked:
        raise SystemExit("item occurrence identity must not retain backend/legacy representation")

    exact_string = section(source, "exactStringAt(", "boundedStringAt(")
    if "boundedText" in exact_string:
        raise SystemExit("exact entity identity extraction must never truncate")
    require_pattern(exact_string, r"exact\.size\(\)\s*>\s*maximumBytes", "oversized exact-identity rejection")
    require_pattern(exact_string, r"!validUtf8\s*\(\s*exact\s*\)", "invalid-UTF-8 exact-identity rejection")

    provider_identity = section(source, "providerParamsIdentityUnavailable", "std::optional<std::string> providerMethod")
    require_pattern(provider_identity, r'\{\s*"truncation"\s*,\s*"params"\s*\}', "truncated provider-params rejection")
    require_pattern(provider_identity, r'\{\s*"sensitiveFieldsRedacted"\s*\}', "provider redaction classification")
    require_pattern(provider_identity, r'\*identity\s*==\s*"\[redacted\]"', "redacted identity-sentinel rejection")

    event_data = section(source, "Json eventData(", "std::optional<FrontendCapability>")
    if re.search(r"\.\s*back\s*\(", mask_comments_and_literals(event_data)):
        raise SystemExit("identity-bearing expanded projection must not substitute a collection tail")
    item_identity = section(source, "struct ItemEventIdentity", "std::optional<ItemEventIdentity> itemIdentityAt")
    if "Json" in mask_comments_and_literals(item_identity) or "*" in mask_comments_and_literals(item_identity):
        raise SystemExit("ItemEventIdentity must carry exact string identity only")
    require_pattern(
        source,
        r"findItem\s*\(\s*const backend::Snapshot& snapshot\s*,\s*std::string_view threadId\s*,\s*std::string_view turnId\s*,\s*std::string_view itemId\s*\)",
        "hierarchical thread/turn/item lookup",
    )
    require_pattern(
        source,
        r"turn\s*==\s*thread->turns\.end\(\)\s*\|\|\s*turn->threadId\s*!=\s*threadId",
        "redundant turn-parent mismatch rejection",
    )
    require_pattern(
        event_data,
        r"findItem\s*\(\s*snapshot\s*,\s*identity->threadId\s*,\s*identity->turnId\s*,\s*identity->itemId\s*\)",
        "exact item occurrence triple lookup",
    )
    require_pattern(event_data, r"expandedItemJson\s*\(", "shared expanded item projector for live events")
    require_pattern(source, r"expandedItemJson\s*\([\s\S]*?state\[\"items\"\]", "shared expanded item projector for snapshots")

    family_helpers = {
        "thread.upserted": section(source, "std::optional<std::string> threadUpsertedId", "std::optional<std::string> threadRemovedId"),
        "thread.removed": section(source, "std::optional<std::string> threadRemovedId", "struct TurnEventIdentity"),
        "turn.upserted": section(source, "std::optional<TurnEventIdentity> turnUpsertedIdentity", "struct ItemEventIdentity"),
        "item.upserted": section(source, "std::optional<ItemEventIdentity> itemUpsertedIdentity", "std::optional<ItemEventIdentity> itemContentUpdatedIdentity"),
        "item.content.updated": section(source, "std::optional<ItemEventIdentity> itemContentUpdatedIdentity", "std::optional<std::string> processUpdatedId"),
    }
    require_pattern(family_helpers["thread.upserted"], r'\{\s*"thread"\s*,\s*"id"\s*\}', "thread.upserted thread.id path")
    if '"turn"' in family_helpers["thread.upserted"] or '"item"' in family_helpers["thread.upserted"]:
        raise SystemExit("thread.upserted identity must not consume another family path")
    require_pattern(family_helpers["thread.removed"], r'\{\s*"threadId"\s*\}', "thread.removed threadId path")
    if re.search(r'\{\s*"thread"\s*,\s*"id"\s*\}', family_helpers["thread.removed"]):
        raise SystemExit("thread.removed must not consume the thread.upserted identity path")
    require_pattern(
        family_helpers["turn.upserted"],
        r'\{\s*"turn"\s*,\s*"id"\s*\}[\s\S]*?\{\s*"turn"\s*,\s*"threadId"\s*\}',
        "turn.upserted exact identity and parent paths",
    )
    require_pattern(
        family_helpers["turn.upserted"],
        r'\{\s*"params"\s*,\s*"turn"\s*,\s*"id"\s*\}',
        "turn.upserted provider turn.id path",
    )
    require_pattern(
        family_helpers["turn.upserted"],
        r'\{\s*"params"\s*,\s*"threadId"\s*\}',
        "turn.upserted provider parent threadId path",
    )
    require_pattern(
        family_helpers["item.upserted"],
        r'\{\s*"item"\s*,\s*"id"\s*\}[\s\S]*?\{\s*"threadId"\s*\}[\s\S]*?\{\s*"turnId"\s*\}',
        "item.upserted exact identity and parent paths",
    )
    require_pattern(
        family_helpers["item.content.updated"],
        r'\{\s*"itemId"\s*\}[\s\S]*?\{\s*"threadId"\s*\}[\s\S]*?\{\s*"turnId"\s*\}',
        "item.content.updated exact identity and parent paths",
    )
    for forbidden_path in (
        r'\{\s*"params"\s*,\s*"item"\s*,\s*"threadId"\s*\}',
        r'\{\s*"params"\s*,\s*"item"\s*,\s*"turnId"\s*\}',
    ):
        if re.search(forbidden_path, family_helpers["item.upserted"]):
            raise SystemExit("item.upserted provider identity must use reviewed root parent paths")
    require_pattern(
        family_helpers["turn.upserted"],
        r'thread/compacted[\s\S]*?thread/tokenUsage/updated[\s\S]*?model/rerouted[\s\S]*?model/safetyBuffering/updated[\s\S]*?model/verification',
        "turn-mutating thread/model provider family classification",
    )
    for helper in family_helpers.values():
        require_pattern(helper, r"providerParamsIdentityUnavailable", "provider identity redaction/truncation validation")
    for family, helper in family_helpers.items():
        require_pattern(event_data, rf"case\s+ExpandedEventType::{re.escape(''.join(part.title() for part in family.split('.')))}[\s\S]*?{re.escape(helper.split('(')[0].split()[-1])}", f"{family} branch-specific helper")

    exact_paths = {
        r'\{\s*"process"\s*,\s*"processHandle"\s*\}': "process.processHandle path",
        r'\{\s*"filesystemWatch"\s*,\s*"watchId"\s*\}': "filesystemWatch.watchId path",
        r'\{\s*"fuzzySearch"\s*,\s*"sessionId"\s*\}': "fuzzySearch.sessionId path",
        r'\{\s*"activity"\s*\}': "exact activity wrapper",
        r'\{\s*"notice"\s*\}': "exact notice wrapper",
    }
    for pattern, description in exact_paths.items():
        require_pattern(source, pattern, description)

    require_pattern(source, r"record\.snapshotRequired", "safe missing-identity snapshot fallback")
    require_pattern(
        source,
        r"event\.requiredScopes\.assign\s*\(\s*methodScopes\.begin\(\)\s*,\s*methodScopes\.end\(\)\s*\)",
        "whole-event suppression for required item-content members",
    )
    require_pattern(
        event_data,
        r'item/fileChange/outputDelta[\s\S]*?channel\s*=\s*"commandOutput"[\s\S]*?location\.item->commandOutput',
        "exact accumulated file-change output projection",
    )


def controller_guard(source: str) -> None:
    callback_sections = (
        ("void CommandDrainController::operationCompleted", "void CommandDrainController::synchronizationCompleted"),
        ("void CommandDrainController::synchronizationCompleted", "void CommandDrainController::threadStartCompleted"),
        ("void CommandDrainController::threadStartCompleted", "void CommandDrainController::turnStartCompleted"),
        ("void CommandDrainController::turnStartCompleted", "void CommandDrainController::completeActiveNewFailure"),
        ("void CommandDrainController::completeActiveNewFailure", "void CommandDrainController::recordCommandFailure"),
    )
    for start, end in callback_sections:
        callback = mask_comments_and_literals(section(source, start, end))
        if "terminateApplicationFailure" in callback or re.search(r"\bfinish\s*\(", callback):
            raise SystemExit(f"ordinary operation callback {start} must not terminate the application")

    disconnected = mask_comments_and_literals(
        section(source, "void CommandDrainController::markDisconnected", "void CommandDrainController::inputEof")
    )
    if "terminateApplicationFailure" in disconnected or re.search(r"\bfinish\s*\(", disconnected):
        raise SystemExit("application Disconnected state must remain nonterminal")

    submit_new = mask_comments_and_literals(
        section(source, "CommandDrainController::submitNew", "CommandDrainController::submitActiveNewTurn")
    )
    if "terminateApplicationFailure" in submit_new or re.search(r"\bfinish\s*\(", submit_new):
        raise SystemExit("new workflow initial-submission rejection must not terminate the application")
    if "activeNew.reset()" not in submit_new:
        raise SystemExit("rejected first-stage new submission must reset only the active workflow")


def integration_guard(source: str) -> None:
    required = (
        "actualPageIds == expectedPageIds",
        "projectedOccurrences == PageThreadCount",
        "exactProjectedContent",
        "exactResponseContent",
        "exactStateContent",
    )
    missing = [value for value in required if value not in source]
    if missing:
        raise SystemExit("thread-list integration must assert exact identities/content, missing: " + ", ".join(missing))
    lifecycle_required = (
        "LifecycleUserItemId",
        "LifecycleAgentItemId",
        'wireUser->type == "userMessage"',
        'wireAgent->type == "agentMessage"',
        'wireAgentContent->channel == "agentText"',
        "turn->terminal",
        "expandedEventsEmitted == harness->expandedEventsSchemaValid",
        "pendingOperationCount() == 0",
        "beginReplay(lifecycleReplayAfter)",
        "replayUser->data.at(\"item\") == liveUser->item",
        "a second typed command is accepted after the completed realistic turn",
    )
    missing_lifecycle = [value for value in lifecycle_required if value not in source]
    if missing_lifecycle:
        raise SystemExit(
            "turn-lifecycle integration must retain the real projection/serialization/SDK gate, missing: "
            + ", ".join(missing_lifecycle)
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--projection", required=True, type=pathlib.Path)
    parser.add_argument("--controller", required=True, type=pathlib.Path)
    parser.add_argument("--integration", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    projection_guard(arguments.projection.read_text(encoding="utf-8"))
    controller_guard(arguments.controller.read_text(encoding="utf-8"))
    integration_guard(arguments.integration.read_text(encoding="utf-8"))
    print("frontend client lifetime and projection identity source guard: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
