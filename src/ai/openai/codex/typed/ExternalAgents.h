/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_EXTERNALAGENTS_H
#define AI_OPENAI_CODEX_TYPED_EXTERNALAGENTS_H

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

    struct ExternalAgentConfigMigrationItemType {
        std::string value;

        static ExternalAgentConfigMigrationItemType agentsMd() {
            return {"AGENTS_MD"};
        }

        static ExternalAgentConfigMigrationItemType config() {
            return {"CONFIG"};
        }

        static ExternalAgentConfigMigrationItemType skills() {
            return {"SKILLS"};
        }

        static ExternalAgentConfigMigrationItemType plugins() {
            return {"PLUGINS"};
        }

        static ExternalAgentConfigMigrationItemType mcpServerConfig() {
            return {"MCP_SERVER_CONFIG"};
        }

        static ExternalAgentConfigMigrationItemType subagents() {
            return {"SUBAGENTS"};
        }

        static ExternalAgentConfigMigrationItemType hooks() {
            return {"HOOKS"};
        }

        static ExternalAgentConfigMigrationItemType commands() {
            return {"COMMANDS"};
        }

        static ExternalAgentConfigMigrationItemType sessions() {
            return {"SESSIONS"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "AGENTS_MD" || value == "CONFIG" || value == "SKILLS" || value == "PLUGINS" ||
                   value == "MCP_SERVER_CONFIG" || value == "SUBAGENTS" || value == "HOOKS" || value == "COMMANDS" ||
                   value == "SESSIONS";
        }

        auto operator<=>(const ExternalAgentConfigMigrationItemType&) const = default;
    };

    struct CommandMigration {
        std::string name;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const CommandMigration&) const = default;
    };

    struct HookMigration {
        std::string name;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const HookMigration&) const = default;
    };

    struct McpServerMigration {
        std::string name;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpServerMigration&) const = default;
    };

    struct PluginsMigration {
        std::string marketplaceName;
        std::vector<std::string> pluginNames;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginsMigration&) const = default;
    };

    struct SessionMigration {
        std::string cwd;
        std::string path;
        OptionalNullable<std::string> title;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SessionMigration&) const = default;
    };

    struct SkillMigration {
        std::string name;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillMigration&) const = default;
    };

    struct SubagentMigration {
        std::string name;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SubagentMigration&) const = default;
    };

    struct MigrationDetails {
        // All seven fields carry schema defaults, but remain optional so a
        // missing field is distinguishable from an explicit empty array.
        std::optional<std::vector<CommandMigration>> commands;
        std::optional<std::vector<HookMigration>> hooks;
        std::optional<std::vector<McpServerMigration>> mcpServers;
        std::optional<std::vector<PluginsMigration>> plugins;
        std::optional<std::vector<SessionMigration>> sessions;
        std::optional<std::vector<SkillMigration>> skills;
        std::optional<std::vector<SubagentMigration>> subagents;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MigrationDetails&) const = default;
    };

    struct ExternalAgentConfigMigrationItem {
        OptionalNullable<std::string> cwd;
        std::string description;
        OptionalNullable<MigrationDetails> details;
        ExternalAgentConfigMigrationItemType itemType;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigMigrationItem&) const = default;
    };

    struct ExternalAgentConfigImportItemTypeFailure {
        OptionalNullable<std::string> cwd;
        OptionalNullable<std::string> errorType;
        std::string failureStage;
        ExternalAgentConfigMigrationItemType itemType;
        std::string message;
        OptionalNullable<std::string> source;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigImportItemTypeFailure&) const = default;
    };

    struct ExternalAgentConfigImportItemTypeSuccess {
        OptionalNullable<std::string> cwd;
        ExternalAgentConfigMigrationItemType itemType;
        OptionalNullable<std::string> source;
        OptionalNullable<std::string> target;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigImportItemTypeSuccess&) const = default;
    };

    struct ExternalAgentConfigImportTypeResult {
        std::vector<ExternalAgentConfigImportItemTypeFailure> failures;
        ExternalAgentConfigMigrationItemType itemType;
        std::vector<ExternalAgentConfigImportItemTypeSuccess> successes;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigImportTypeResult&) const = default;
    };

    struct ExternalAgentConfigImportHistory {
        std::int64_t completedAtMs = 0;
        std::vector<ExternalAgentConfigImportItemTypeFailure> failures;
        std::string importId;
        std::vector<ExternalAgentConfigImportItemTypeSuccess> successes;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigImportHistory&) const = default;
    };

    struct ExternalAgentConfigDetectParams {
        OptionalNullable<std::vector<std::string>> cwds;
        std::optional<bool> includeHome;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigDetectParams&) const = default;
    };

    struct ExternalAgentConfigDetectResponse {
        std::vector<ExternalAgentConfigMigrationItem> items;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigDetectResponse&) const = default;
    };

    struct ExternalAgentConfigImportParams {
        std::vector<ExternalAgentConfigMigrationItem> migrationItems;
        OptionalNullable<std::string> source;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigImportParams&) const = default;
    };

    struct ExternalAgentConfigImportResponse {
        std::string importId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigImportResponse&) const = default;
    };

    struct ExternalAgentConfigImportHistoriesReadResponse {
        std::vector<ExternalAgentConfigImportHistory> data;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigImportHistoriesReadResponse&) const = default;
    };

    struct ExternalAgentConfigImportCompletedNotification {
        std::string importId;
        std::vector<ExternalAgentConfigImportTypeResult> itemTypeResults;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigImportCompletedNotification&) const = default;
    };

    struct ExternalAgentConfigImportProgressNotification {
        std::string importId;
        std::vector<ExternalAgentConfigImportTypeResult> itemTypeResults;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ExternalAgentConfigImportProgressNotification&) const = default;
    };

    class ExternalAgents {
    public:
        using Submission = AppServerClient::RawProtocol::Submission;
        using DetectResult = OperationResult<ExternalAgentConfigDetectResponse>;
        using DetectResultHandler = std::function<void(const DetectResult&)>;
        using ImportConfigurationResult = OperationResult<ExternalAgentConfigImportResponse>;
        using ImportConfigurationResultHandler = std::function<void(const ImportConfigurationResult&)>;
        using ReadImportHistoriesResult = OperationResult<ExternalAgentConfigImportHistoriesReadResponse>;
        using ReadImportHistoriesResultHandler = std::function<void(const ReadImportHistoriesResult&)>;

        Submission detect(ExternalAgentConfigDetectParams params, DetectResultHandler handler);
        Submission importConfiguration(ExternalAgentConfigImportParams params, ImportConfigurationResultHandler handler);
        Submission readImportHistories(Unit params, ReadImportHistoriesResultHandler handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit ExternalAgents(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_EXTERNALAGENTS_H
