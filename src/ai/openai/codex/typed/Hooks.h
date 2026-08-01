/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_HOOKS_H
#define AI_OPENAI_CODEX_TYPED_HOOKS_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Types.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::typed {

    struct HookEventName {
        std::string value;

        static HookEventName preToolUse() {
            return {"preToolUse"};
        }

        static HookEventName permissionRequest() {
            return {"permissionRequest"};
        }

        static HookEventName postToolUse() {
            return {"postToolUse"};
        }

        static HookEventName preCompact() {
            return {"preCompact"};
        }

        static HookEventName postCompact() {
            return {"postCompact"};
        }

        static HookEventName sessionStart() {
            return {"sessionStart"};
        }

        static HookEventName userPromptSubmit() {
            return {"userPromptSubmit"};
        }

        static HookEventName subagentStart() {
            return {"subagentStart"};
        }

        static HookEventName subagentStop() {
            return {"subagentStop"};
        }

        static HookEventName stop() {
            return {"stop"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "preToolUse" || value == "permissionRequest" || value == "postToolUse" || value == "preCompact" ||
                   value == "postCompact" || value == "sessionStart" || value == "userPromptSubmit" || value == "subagentStart" ||
                   value == "subagentStop" || value == "stop";
        }

        auto operator<=>(const HookEventName&) const = default;
    };

    struct HookHandlerType {
        std::string value;

        static HookHandlerType command() {
            return {"command"};
        }

        static HookHandlerType prompt() {
            return {"prompt"};
        }

        static HookHandlerType agent() {
            return {"agent"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "command" || value == "prompt" || value == "agent";
        }

        auto operator<=>(const HookHandlerType&) const = default;
    };

    struct HookSource {
        std::string value;

        static HookSource system() {
            return {"system"};
        }

        static HookSource user() {
            return {"user"};
        }

        static HookSource project() {
            return {"project"};
        }

        static HookSource mdm() {
            return {"mdm"};
        }

        static HookSource sessionFlags() {
            return {"sessionFlags"};
        }

        static HookSource plugin() {
            return {"plugin"};
        }

        static HookSource cloudRequirements() {
            return {"cloudRequirements"};
        }

        static HookSource cloudManagedConfig() {
            return {"cloudManagedConfig"};
        }

        static HookSource legacyManagedConfigFile() {
            return {"legacyManagedConfigFile"};
        }

        static HookSource legacyManagedConfigMdm() {
            return {"legacyManagedConfigMdm"};
        }

        static HookSource unknown() {
            return {"unknown"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "system" || value == "user" || value == "project" || value == "mdm" || value == "sessionFlags" ||
                   value == "plugin" || value == "cloudRequirements" || value == "cloudManagedConfig" ||
                   value == "legacyManagedConfigFile" || value == "legacyManagedConfigMdm" || value == "unknown";
        }

        auto operator<=>(const HookSource&) const = default;
    };

    struct HookTrustStatus {
        std::string value;

        static HookTrustStatus managed() {
            return {"managed"};
        }

        static HookTrustStatus untrusted() {
            return {"untrusted"};
        }

        static HookTrustStatus trusted() {
            return {"trusted"};
        }

        static HookTrustStatus modified() {
            return {"modified"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "managed" || value == "untrusted" || value == "trusted" || value == "modified";
        }

        auto operator<=>(const HookTrustStatus&) const = default;
    };

    struct HookExecutionMode {
        std::string value;

        static HookExecutionMode sync() {
            return {"sync"};
        }

        static HookExecutionMode async() {
            return {"async"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "sync" || value == "async";
        }

        auto operator<=>(const HookExecutionMode&) const = default;
    };

    struct HookOutputEntryKind {
        std::string value;

        static HookOutputEntryKind warning() {
            return {"warning"};
        }

        static HookOutputEntryKind stop() {
            return {"stop"};
        }

        static HookOutputEntryKind feedback() {
            return {"feedback"};
        }

        static HookOutputEntryKind context() {
            return {"context"};
        }

        static HookOutputEntryKind error() {
            return {"error"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "warning" || value == "stop" || value == "feedback" || value == "context" || value == "error";
        }

        auto operator<=>(const HookOutputEntryKind&) const = default;
    };

    struct HookRunStatus {
        std::string value;

        static HookRunStatus running() {
            return {"running"};
        }

        static HookRunStatus completed() {
            return {"completed"};
        }

        static HookRunStatus failed() {
            return {"failed"};
        }

        static HookRunStatus blocked() {
            return {"blocked"};
        }

        static HookRunStatus stopped() {
            return {"stopped"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "running" || value == "completed" || value == "failed" || value == "blocked" || value == "stopped";
        }

        auto operator<=>(const HookRunStatus&) const = default;
    };

    struct HookScope {
        std::string value;

        static HookScope thread() {
            return {"thread"};
        }

        static HookScope turn() {
            return {"turn"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "thread" || value == "turn";
        }

        auto operator<=>(const HookScope&) const = default;
    };

    struct HookErrorInfo {
        std::string message;
        std::string path;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HookErrorInfo&) const = default;
    };

    struct HookMetadata {
        OptionalNullable<std::string> command;
        std::string currentHash;
        std::int64_t displayOrder = 0;
        bool enabled = false;
        HookEventName eventName;
        HookHandlerType handlerType;
        bool isManaged = false;
        std::string key;
        OptionalNullable<std::string> matcher;
        OptionalNullable<std::string> pluginId;
        HookSource source;
        AbsolutePath sourcePath;
        OptionalNullable<std::string> statusMessage;
        std::uint64_t timeoutSec = 0;
        HookTrustStatus trustStatus;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HookMetadata&) const = default;
    };

    struct HooksListEntry {
        std::string cwd;
        std::vector<HookErrorInfo> errors;
        std::vector<HookMetadata> hooks;
        std::vector<std::string> warnings;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HooksListEntry&) const = default;
    };

    struct HookOutputEntry {
        HookOutputEntryKind kind;
        std::string text;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HookOutputEntry&) const = default;
    };

    struct HookRunSummary {
        OptionalNullable<std::int64_t> completedAt;
        std::int64_t displayOrder = 0;
        OptionalNullable<std::int64_t> durationMs;
        std::vector<HookOutputEntry> entries;
        HookEventName eventName;
        HookExecutionMode executionMode;
        HookHandlerType handlerType;
        std::string id;
        HookScope scope;
        // This field has a schema default, but remains optional so an omitted
        // value is not materialized during decoding.
        std::optional<HookSource> source;
        AbsolutePath sourcePath;
        std::int64_t startedAt = 0;
        HookRunStatus status;
        OptionalNullable<std::string> statusMessage;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HookRunSummary&) const = default;
    };

    struct HooksListParams {
        std::optional<std::vector<std::string>> cwds;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HooksListParams&) const = default;
    };

    struct HooksListResponse {
        std::vector<HooksListEntry> data;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HooksListResponse&) const = default;
    };

    struct HookStartedNotification {
        HookRunSummary run;
        ThreadId threadId;
        OptionalNullable<TurnId> turnId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HookStartedNotification&) const = default;
    };

    struct HookCompletedNotification {
        HookRunSummary run;
        ThreadId threadId;
        OptionalNullable<TurnId> turnId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HookCompletedNotification&) const = default;
    };

    class Hooks {
    public:
        Hooks(const Hooks&) = delete;
        Hooks(Hooks&&) = delete;
        Hooks& operator=(const Hooks&) = delete;
        Hooks& operator=(Hooks&&) = delete;

        Submission list(HooksListParams params, CompletionHandler<HooksListResponse> handler);
        Submission list(CompletionHandler<HooksListResponse> handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Hooks(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_HOOKS_H
