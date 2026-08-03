#!/usr/bin/env python3
"""Validate the current A1.4 runtime/platform schema and production state."""

from __future__ import annotations

import argparse
import json
import re
import unittest
from collections import Counter
from pathlib import Path


ROOTS = (
    "WindowsSandboxReadinessResponse",
    "WindowsSandboxSetupStartParams",
    "WindowsSandboxSetupStartResponse",
    "DeprecationNoticeNotification",
    "ProcessExitedNotification",
    "ProcessOutputDeltaNotification",
    "RemoteControlStatusChangedNotification",
    "ServerRequestResolvedNotification",
    "WarningNotification",
    "WindowsWorldWritableWarningNotification",
    "WindowsSandboxSetupCompletedNotification",
)

IDENTITIES = {
    ("client_request", "windowsSandbox/readiness"),
    ("client_request", "windowsSandbox/setupStart"),
    ("server_notification", "deprecationNotice"),
    ("server_notification", "process/exited"),
    ("server_notification", "process/outputDelta"),
    ("server_notification", "remoteControl/status/changed"),
    ("server_notification", "serverRequest/resolved"),
    ("server_notification", "warning"),
    ("server_notification", "windows/worldWritableWarning"),
    ("server_notification", "windowsSandbox/setupCompleted"),
}

SENSITIVE_PATHS = {
    "ProcessExitedNotification.stderr",
    "ProcessExitedNotification.stdout",
    "RemoteControlStatusChangedNotification.serverName",
    "ServerRequestResolvedNotification.threadId",
    "WarningNotification.message",
    "WarningNotification.threadId",
    "WindowsSandboxSetupCompletedNotification.error",
    "WindowsSandboxSetupStartParams.cwd",
}

EXPECTED_FIELDS = {
    "WindowsSandboxReadinessResponse": {"status"},
    "WindowsSandboxSetupStartParams": {"cwd", "mode"},
    "WindowsSandboxSetupStartResponse": {"started"},
    "DeprecationNoticeNotification": {"details", "summary"},
    "ProcessExitedNotification": {
        "exitCode",
        "processHandle",
        "stderr",
        "stderrCapReached",
        "stdout",
        "stdoutCapReached",
    },
    "ProcessOutputDeltaNotification": {
        "capReached",
        "deltaBase64",
        "processHandle",
        "stream",
    },
    "RemoteControlStatusChangedNotification": {
        "environmentId",
        "installationId",
        "serverName",
        "status",
    },
    "ServerRequestResolvedNotification": {"requestId", "threadId"},
    "WarningNotification": {"message", "threadId"},
    "WindowsWorldWritableWarningNotification": {
        "extraCount",
        "failedScan",
        "samplePaths",
    },
    "WindowsSandboxSetupCompletedNotification": {"error", "mode", "success"},
}


def _split_cpp_arguments(value: str) -> list[str]:
    arguments: list[str] = []
    current: list[str] = []
    depth = 0
    quoted = False
    escaped = False
    for character in value:
        if quoted:
            current.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                quoted = False
            continue
        if character == '"':
            quoted = True
            current.append(character)
        elif character == "(":
            depth += 1
            current.append(character)
        elif character == ")":
            depth -= 1
            current.append(character)
        elif character == "," and depth == 0:
            arguments.append("".join(current).strip())
            current.clear()
        else:
            current.append(character)
    arguments.append("".join(current).strip())
    return arguments


def _nullable(schema: object) -> bool:
    if not isinstance(schema, dict):
        return False
    value_type = schema.get("type")
    if isinstance(value_type, list) and "null" in value_type:
        return True
    return any(
        isinstance(branch, dict) and branch.get("type") == "null"
        for keyword in ("anyOf", "oneOf")
        for branch in schema.get(keyword, [])
    )


class RuntimePlatformCurrentStateTest(unittest.TestCase):
    repo_root: Path

    @classmethod
    def setUpClass(cls) -> None:
        schema_root = cls.repo_root / "tools/codex/app-server-schema/0.144.6/stable/v2"
        cls.roots: dict[str, dict] = {}
        cls.definitions: dict[str, dict] = {}
        for name in ROOTS:
            document = json.loads(
                (schema_root / f"{name}.json").read_text(encoding="utf-8")
            )
            cls.roots[name] = document
            for definition_name, definition in document.get("definitions", {}).items():
                previous = cls.definitions.setdefault(definition_name, definition)
                if previous != definition:
                    raise AssertionError(f"conflicting stable definition {definition_name}")

    def test_exact_schema_closure_and_fields(self) -> None:
        self.assertEqual(11, len(self.roots))
        self.assertEqual(17, len(self.roots) + len(self.definitions))
        self.assertEqual(
            EXPECTED_FIELDS,
            {name: set(schema.get("properties", {})) for name, schema in self.roots.items()},
        )

        nodes = {**self.definitions, **self.roots}
        properties: list[tuple[str, str, dict, bool]] = []
        array_elements = maps = open_objects = closed_objects = opaque = 0
        for name, schema in nodes.items():
            if schema.get("type") == "object":
                if schema.get("additionalProperties") is False:
                    closed_objects += 1
                else:
                    open_objects += 1
            required = set(schema.get("required", []))
            for field, child in schema.get("properties", {}).items():
                properties.append((name, field, child, field in required))
                array_elements += child.get("type") == "array"
                maps += isinstance(child.get("additionalProperties"), dict)
                opaque += child == {}

        self.assertEqual(30, len(properties))
        self.assertEqual(31, len(properties) + array_elements)
        self.assertEqual(25, sum(required for *_, required in properties))
        self.assertEqual(5, sum(not required for *_, required in properties))
        self.assertEqual(5, sum(_nullable(child) for _, _, child, _ in properties))
        self.assertEqual(0, sum("default" in child for _, _, child, _ in properties))
        self.assertEqual((1, 0), (array_elements, maps))
        self.assertEqual((11, 0, 0), (open_objects, closed_objects, opaque))
        self.assertEqual(
            Counter({"int32": 1, "uint": 1}),
            Counter(child["format"] for _, _, child, _ in properties if "format" in child),
        )
        self.assertEqual(1, sum("minimum" in child for _, _, child, _ in properties))
        self.assertEqual(0, sum("maximum" in child for _, _, child, _ in properties))
        self.assertEqual(
            SENSITIVE_PATHS,
            {
                f"{name}.{field}"
                for name, field, _, _ in properties
                if f"{name}.{field}" in SENSITIVE_PATHS
            },
        )

    def test_registry_is_current_and_complete(self) -> None:
        registry = self.repo_root / "src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc"
        rows: list[dict[str, str]] = []
        prefix = "CODEX_PROTOCOL_SURFACE_ENTRY("
        for line in registry.read_text(encoding="utf-8").splitlines():
            if not line.startswith(prefix):
                continue
            values = _split_cpp_arguments(line[len(prefix) : -1])
            rows.append(
                {
                    "category": values[0].removeprefix("SurfaceCategory::"),
                    "name": json.loads(values[3]),
                    "disposition": values[6].removeprefix("RuntimeDisposition::"),
                    "schema": values[20].removeprefix("TypedSchemaStatus::"),
                    "slice": values[19].removeprefix("A1Slice::"),
                }
            )

        self.assertEqual(387, len(rows))
        self.assertEqual(
            Counter({"Complete": 339, "NotApplicable": 48}),
            Counter(row["schema"] for row in rows),
        )
        self.assertEqual(
            set(),
            {row["name"] for row in rows if row["schema"] == "Partial"},
        )
        self.assertEqual(
            {"initialize", "initialized", "error"},
            {
                row["name"]
                for row in rows
                if row["name"] in {"initialize", "initialized", "error"}
                and row["schema"] == "Complete"
            },
        )
        native = [row for row in rows if row["slice"] == "A1_4"]
        self.assertEqual(Counter({"Complete": 56}), Counter(row["schema"] for row in native))
        inventory_only = [row for row in rows if row["slice"] == "InventoryOnly"]
        self.assertEqual(48, len(inventory_only))
        self.assertEqual({"NotApplicable"}, {row["schema"] for row in inventory_only})
        by_identity = {
            (
                "client_request" if row["category"] == "ClientRequest" else "server_notification",
                row["name"],
            ): row
            for row in rows
            if row["category"] in {"ClientRequest", "ServerNotification"}
        }
        self.assertTrue(IDENTITIES <= set(by_identity))
        for identity in IDENTITIES:
            self.assertEqual("Typed", by_identity[identity]["disposition"])
            self.assertEqual("Complete", by_identity[identity]["schema"])

        client_descriptors = (
            self.repo_root / "src/ai/openai/codex/detail/ClientOperationCodecDescriptors.inc"
        ).read_text(encoding="utf-8")
        notification_descriptors = (
            self.repo_root / "src/ai/openai/codex/detail/ServerNotificationCodecDescriptors.inc"
        ).read_text(encoding="utf-8")
        for category, method in IDENTITIES:
            descriptor = client_descriptors if category == "client_request" else notification_descriptors
            self.assertEqual(1, descriptor.count(f'"{method}"'))

    def test_public_header_inventory_and_soversion(self) -> None:
        main_cmake = (self.repo_root / "src/ai/openai/codex/CMakeLists.txt").read_text(encoding="utf-8")
        main_block = re.search(r"set\(AI_OPENAI_CODEX_PUBLIC_H\s+(.*?)\n\)", main_cmake, re.S)
        self.assertIsNotNone(main_block)
        self.assertEqual(29, len(main_block.group(1).split()))
        self.assertIn("Api.h", main_block.group(1))
        self.assertIn("typed/WindowsSandbox.h", main_block.group(1))
        self.assertNotIn("typed/Client.h", main_block.group(1))
        for relative, variable, expected in (
            ("src/ai/openai/codex/backend/CMakeLists.txt", "AI_OPENAI_CODEX_BACKEND_PUBLIC_H", 7),
            ("src/ai/openai/codex/frontend/CMakeLists.txt", "AI_OPENAI_CODEX_FRONTEND_PUBLIC_H", 9),
        ):
            source = (self.repo_root / relative).read_text(encoding="utf-8")
            block = re.search(rf"set\({variable}\s+(.*?)\n\)", source, re.S)
            self.assertIsNotNone(block)
            self.assertEqual(expected, len(block.group(1).split()))
        root_cmake = (self.repo_root / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(root_cmake, r"set\(AISUITE_CODEX_SOVERSION\s+2\)")

    def test_current_public_api_boundary(self) -> None:
        def source(relative: str) -> str:
            return (self.repo_root / relative).read_text(encoding="utf-8")

        forbidden_by_file = {
            "src/ai/openai/codex/Protocol.h": ("struct InitializeResult",),
            "src/ai/openai/codex/AppServerClient.h": ("getInitializeResult(",),
            "src/ai/openai/codex/detail/ProtocolCodec.h": ("decodeInitializeResult(",),
            "src/ai/openai/codex/typed/Threads.h": (
                "ThreadStartOptions",
                "ThreadResumeOptions",
                "ThreadListOptions",
                "ThreadReadOptions",
                "toThreadStartParams",
                "toThreadResumeParams",
                "toThreadListParams",
                "toThreadReadParams",
                "ThreadResultHandler",
            ),
            "src/ai/openai/codex/typed/Turns.h": (
                "TurnInterruptResult",
                "TurnStartOptions",
                "toTurnStartParams",
                "toTurnInterruptParams",
                "TurnResultHandler",
                "InterruptResultHandler",
            ),
            "src/ai/openai/codex/typed/Conversation.h": ("fromLegacy(",),
            "src/ai/openai/codex/typed/Items.h": ("decodingError",),
            "src/ai/openai/codex/typed/Events.h": ("decodingError",),
            "src/ai/openai/codex/typed/ServerRequests.h": ("decodingError",),
        }
        for relative, forbidden_names in forbidden_by_file.items():
            contents = source(relative)
            for forbidden_name in forbidden_names:
                self.assertNotIn(forbidden_name, contents, f"{forbidden_name} remains in {relative}")

        optional_nullable = source("src/ai/openai/codex/typed/Types.h")
        self.assertNotRegex(
            optional_nullable,
            r"OptionalNullable\s*\(\s*bool\s+isPresent\s*,\s*std::optional<T>",
        )
        for retained in (
            "OptionalNullable()",
            "OptionalNullable(std::nullopt_t)",
            "OptionalNullable(const T&",
            "OptionalNullable(T&&",
            "OptionalNullable(const std::optional<T>&",
            "OptionalNullable(std::optional<T>&&",
            "OptionalNullable omitted()",
            "OptionalNullable explicitNull()",
            "OptionalNullable withValue(",
        ):
            self.assertIn(retained, optional_nullable)

        public_client = source("src/ai/openai/codex/AppServerClient.h")
        for forbidden in (
            "typed() noexcept",
            "raw() const noexcept",
            '[[deprecated("use typed()',
        ):
            self.assertNotIn(forbidden, public_client)
        self.assertFalse((self.repo_root / "src/ai/openai/codex/typed/Client.h").exists())

        for facade, accessor in (
            ("Accounts", "accounts"),
            ("Apps", "apps"),
            ("Commands", "commands"),
            ("Configuration", "configuration"),
            ("Events", "events"),
            ("ExternalAgents", "externalAgents"),
            ("Feedback", "feedback"),
            ("Filesystem", "filesystem"),
            ("Hooks", "hooks"),
            ("Marketplace", "marketplace"),
            ("Mcp", "mcp"),
            ("Models", "models"),
            ("PermissionProfiles", "permissionProfiles"),
            ("Plugins", "plugins"),
            ("Requests", "requests"),
            ("Reviews", "reviews"),
            ("Skills", "skills"),
            ("Threads", "threads"),
            ("Turns", "turns"),
            ("WindowsSandbox", "windowsSandbox"),
        ):
            self.assertEqual(
                1,
                public_client.count(f"typed::{facade}& {accessor}() noexcept"),
                f"{accessor} must have exactly one non-const direct accessor",
            )

        retained_by_file = {
            "src/ai/openai/codex/AppServerClient.h": (
                "struct ClientInfo",
                'std::string name = "aisuite"',
                'std::string title = "AISuite"',
                "RawProtocol& raw() noexcept",
                "typed::Threads& threads() noexcept",
                "typed::Turns& turns() noexcept",
                "typed::Events& events() noexcept",
                "typed::Requests& requests() noexcept",
                "getInitializeResponse() const",
            ),
            "src/ai/openai/codex/typed/Types.h": (
                "struct InitializeParams",
                "InitializeParams(const ai::openai::codex::ClientInfo&",
                "struct AbsolutePath",
            ),
            "src/ai/openai/codex/typed/Threads.h": (
                "Submission start(ThreadStartParams",
                "Submission resume(ThreadResumeParams",
                "Submission list(ThreadListParams",
                "Submission read(ThreadReadParams",
            ),
            "src/ai/openai/codex/typed/Turns.h": (
                "Submission start(TurnStartParams",
                "Submission interrupt(TurnInterruptParams",
            ),
            "src/ai/openai/codex/typed/Conversation.h": (
                "struct ExternalSandboxPolicy",
                "struct TextInput",
                "struct ImageUrlInput",
                "struct LocalImageInput",
                "struct SkillInput",
                "struct MentionInput",
                "struct UnknownTurnInput",
                "using TurnInput = std::variant<",
            ),
            "src/ai/openai/codex/typed/Items.h": (
                "using ThreadItem = std::variant<",
                "using ResponseItem = std::variant<",
                "struct PathString",
                "struct UnknownItem",
                "struct UnknownResponseItem",
                "std::optional<DecodeDiagnostic> diagnostic",
            ),
            "src/ai/openai/codex/typed/Events.h": (
                "struct TurnErrorEvent",
                "struct UnknownEvent",
                "std::optional<DecodeDiagnostic> diagnostic",
            ),
            "src/ai/openai/codex/typed/ServerRequests.h": (
                "struct AuthenticationRequest",
                "struct PermissionsApprovalRequest",
                "struct UnknownServerRequest",
                "std::optional<DecodeDiagnostic> diagnostic",
            ),
        }
        for relative, retained_names in retained_by_file.items():
            contents = source(relative)
            for retained_name in retained_names:
                self.assertIn(retained_name, contents, f"{retained_name} is missing from {relative}")

        forbidden_names = {
            "src/ai/openai/codex/typed/Conversation.h": (
                "ExternalSandboxSandboxPolicy",
                "TextUserInput",
                "ImageUserInput",
                "LocalImageUserInput",
                "SkillUserInput",
                "MentionUserInput",
                "UnknownUserInput",
                "using UserInput =",
            ),
            "src/ai/openai/codex/typed/Items.h": (
                "using AgentMessageItem =",
                "using CommandExecutionItem =",
                "using FileChangeItem =",
                "using ToolCallItem =",
                "using ReasoningItem =",
                "using UserMessageItem =",
                "using WebSearchItem =",
                "using Item =",
                "LegacyAppPathString",
            ),
            "src/ai/openai/codex/typed/Threads.h": ("using ThreadPage =",),
            "src/ai/openai/codex/typed/ServerRequests.h": (
                "PermissionsRequestApprovalRequest",
                "ChatgptAuthTokensRefreshRequest",
                "respondRefresh(",
            ),
            "src/ai/openai/codex/typed/Types.h": ("AbsolutePathBuf",),
        }
        for relative, names in forbidden_names.items():
            contents = source(relative)
            for name in names:
                self.assertNotIn(name, contents, f"{name} remains in {relative}")

        api = source("src/ai/openai/codex/Api.h")
        self.assertIn("ai/openai/codex/AppServerClient.h", api)
        self.assertIn("ai/openai/codex/stdio/Client.h", api)
        for facade in (
            "Accounts",
            "Apps",
            "Commands",
            "Configuration",
            "Events",
            "ExternalAgents",
            "Feedback",
            "Filesystem",
            "Hooks",
            "Marketplace",
            "Mcp",
            "Models",
            "PermissionProfiles",
            "Plugins",
            "ServerRequests",
            "Reviews",
            "Skills",
            "Threads",
            "Turns",
            "WindowsSandbox",
        ):
            self.assertIn(f"ai/openai/codex/typed/{facade}.h", api)

        for relative in (
            "src/ai/openai/codex/CMakeLists.txt",
            "src/ai/openai/codex/backend/CMakeLists.txt",
            "src/ai/openai/codex/frontend/CMakeLists.txt",
        ):
            cmake = source(relative)
            self.assertNotIn("snodec::ai-openai-codex", cmake)
        self.assertIn("AISuite::OpenAICodex", source("src/ai/openai/codex/CMakeLists.txt"))
        self.assertIn("AISuite::OpenAICodexBackend", source("src/ai/openai/codex/backend/CMakeLists.txt"))
        self.assertIn("AISuite::OpenAICodexFrontend", source("src/ai/openai/codex/frontend/CMakeLists.txt"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    arguments, remaining = parser.parse_known_args()
    RuntimePlatformCurrentStateTest.repo_root = arguments.repo_root.resolve()
    unittest.main(argv=[__file__, *remaining])
    return 0


if __name__ == "__main__":
    main()
