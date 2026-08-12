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
    parser.add_argument("--controller", required=True, type=pathlib.Path)
    parser.add_argument("--integration", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    controller_guard(arguments.controller.read_text(encoding="utf-8"))
    integration_guard(arguments.integration.read_text(encoding="utf-8"))
    print("frontend client lifetime and integration source guard: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
