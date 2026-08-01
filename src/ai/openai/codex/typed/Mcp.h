/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_MCP_H
#define AI_OPENAI_CODEX_TYPED_MCP_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Types.h"

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::typed {

    // Protocol string enums deliberately retain unknown future values. Their
    // decoders add a ForwardCompatibility diagnostic when isKnown() is false.
    struct McpServerStatusDetail {
        std::string value;

        [[nodiscard]] static McpServerStatusDetail full() {
            return {"full"};
        }

        [[nodiscard]] static McpServerStatusDetail toolsAndAuthOnly() {
            return {"toolsAndAuthOnly"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "full" || value == "toolsAndAuthOnly";
        }

        auto operator<=>(const McpServerStatusDetail&) const = default;
    };

    struct McpAuthStatus {
        std::string value;

        [[nodiscard]] static McpAuthStatus unsupported() {
            return {"unsupported"};
        }

        [[nodiscard]] static McpAuthStatus notLoggedIn() {
            return {"notLoggedIn"};
        }

        [[nodiscard]] static McpAuthStatus bearerToken() {
            return {"bearerToken"};
        }

        [[nodiscard]] static McpAuthStatus oauth() {
            return {"oAuth"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "unsupported" || value == "notLoggedIn" || value == "bearerToken" || value == "oAuth";
        }

        auto operator<=>(const McpAuthStatus&) const = default;
    };

    struct McpServerStartupFailureReason {
        std::string value;

        [[nodiscard]] static McpServerStartupFailureReason reauthenticationRequired() {
            return {"reauthenticationRequired"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "reauthenticationRequired";
        }

        auto operator<=>(const McpServerStartupFailureReason&) const = default;
    };

    struct McpServerStartupState {
        std::string value;

        [[nodiscard]] static McpServerStartupState starting() {
            return {"starting"};
        }

        [[nodiscard]] static McpServerStartupState ready() {
            return {"ready"};
        }

        [[nodiscard]] static McpServerStartupState failed() {
            return {"failed"};
        }

        [[nodiscard]] static McpServerStartupState cancelled() {
            return {"cancelled"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "starting" || value == "ready" || value == "failed" || value == "cancelled";
        }

        auto operator<=>(const McpServerStartupState&) const = default;
    };

    struct McpServerOauthLoginParams {
        std::string name;
        OptionalNullable<std::vector<std::string>> scopes;
        OptionalNullable<ThreadId> threadId;
        OptionalNullable<std::int64_t> timeoutSecs;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpServerOauthLoginParams&) const = default;
    };

    struct McpServerOauthLoginResponse {
        std::string authorizationUrl;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpServerOauthLoginResponse&) const = default;
    };

    // ResourceContent is an open anyOf. A valid value has a URI and at least
    // one of text/blob; both are retained when a future server supplies both.
    struct McpResourceContent {
        OptionalNullable<Json> meta;
        std::optional<std::string> blob;
        OptionalNullable<std::string> mimeType;
        std::optional<std::string> text;
        std::string uri;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpResourceContent&) const = default;
    };

    struct McpResourceReadParams {
        std::string server;
        OptionalNullable<ThreadId> threadId;
        std::string uri;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpResourceReadParams&) const = default;
    };

    struct McpResourceReadResponse {
        std::vector<McpResourceContent> contents;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpResourceReadResponse&) const = default;
    };

    struct McpServerToolCallParams {
        OptionalNullable<Json> meta;
        OptionalNullable<Json> arguments;
        std::string server;
        ThreadId threadId;
        std::string tool;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpServerToolCallParams&) const = default;
    };

    struct McpServerToolCallResponse {
        OptionalNullable<Json> meta;
        std::vector<Json> content;
        OptionalNullable<bool> isError;
        OptionalNullable<Json> structuredContent;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpServerToolCallResponse&) const = default;
    };

    struct McpServerInfo {
        OptionalNullable<std::string> description;
        OptionalNullable<std::vector<Json>> icons;
        std::string name;
        OptionalNullable<std::string> title;
        std::string version;
        OptionalNullable<std::string> websiteUrl;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpServerInfo&) const = default;
    };

    struct McpResource {
        OptionalNullable<Json> meta;
        OptionalNullable<Json> annotations;
        OptionalNullable<std::string> description;
        OptionalNullable<std::vector<Json>> icons;
        OptionalNullable<std::string> mimeType;
        std::string name;
        OptionalNullable<std::int64_t> size;
        OptionalNullable<std::string> title;
        std::string uri;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpResource&) const = default;
    };

    struct McpResourceTemplate {
        OptionalNullable<Json> annotations;
        OptionalNullable<std::string> description;
        OptionalNullable<std::string> mimeType;
        std::string name;
        OptionalNullable<std::string> title;
        std::string uriTemplate;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpResourceTemplate&) const = default;
    };

    struct McpTool {
        OptionalNullable<Json> meta;
        OptionalNullable<Json> annotations;
        OptionalNullable<std::string> description;
        OptionalNullable<std::vector<Json>> icons;
        Json inputSchema = nullptr;
        std::string name;
        OptionalNullable<Json> outputSchema;
        OptionalNullable<std::string> title;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpTool&) const = default;
    };

    struct McpServerStatus {
        McpAuthStatus authStatus;
        std::string name;
        std::vector<McpResourceTemplate> resourceTemplates;
        std::vector<McpResource> resources;
        OptionalNullable<McpServerInfo> serverInfo;
        std::map<std::string, McpTool> tools;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpServerStatus&) const = default;
    };

    struct ListMcpServerStatusParams {
        OptionalNullable<std::string> cursor;
        OptionalNullable<McpServerStatusDetail> detail;
        OptionalNullable<std::uint32_t> limit;
        OptionalNullable<ThreadId> threadId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ListMcpServerStatusParams&) const = default;
    };

    struct ListMcpServerStatusResponse {
        std::vector<McpServerStatus> data;
        OptionalNullable<std::string> nextCursor;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const ListMcpServerStatusResponse&) const = default;
    };

    struct McpServerOauthLoginCompletedNotification {
        OptionalNullable<std::string> error;
        std::string name;
        bool success = false;
        OptionalNullable<ThreadId> threadId;
        // Notification aggregates retain the complete JSON-RPC envelope.
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpServerOauthLoginCompletedNotification&) const = default;
    };

    struct McpServerStatusUpdatedNotification {
        OptionalNullable<std::string> error;
        OptionalNullable<McpServerStartupFailureReason> failureReason;
        std::string name;
        McpServerStartupState status;
        OptionalNullable<ThreadId> threadId;
        // Notification aggregates retain the complete JSON-RPC envelope.
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const McpServerStatusUpdatedNotification&) const = default;
    };

    class Mcp {
    public:
        Mcp(const Mcp&) = delete;
        Mcp(Mcp&&) = delete;
        Mcp& operator=(const Mcp&) = delete;
        Mcp& operator=(Mcp&&) = delete;

        Submission startOauthLogin(McpServerOauthLoginParams params, CompletionHandler<McpServerOauthLoginResponse> handler);
        Submission readResource(McpResourceReadParams params, CompletionHandler<McpResourceReadResponse> handler);
        Submission callTool(McpServerToolCallParams params, CompletionHandler<McpServerToolCallResponse> handler);
        Submission listServers(ListMcpServerStatusParams params, CompletionHandler<ListMcpServerStatusResponse> handler);
        Submission listServers(CompletionHandler<ListMcpServerStatusResponse> handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Mcp(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_MCP_H
