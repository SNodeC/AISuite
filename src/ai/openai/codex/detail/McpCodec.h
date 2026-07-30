/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_MCPCODEC_H
#define AI_OPENAI_CODEX_DETAIL_MCPCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Mcp.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<Json> encodeMcpServerOauthLoginParams(const typed::McpServerOauthLoginParams& value, std::string& error) noexcept;
    std::optional<Json> encodeMcpResourceReadParams(const typed::McpResourceReadParams& value, std::string& error) noexcept;
    std::optional<Json> encodeMcpServerToolCallParams(const typed::McpServerToolCallParams& value, std::string& error) noexcept;
    std::optional<Json> encodeListMcpServerStatusParams(const typed::ListMcpServerStatusParams& value, std::string& error) noexcept;

    std::optional<typed::McpServerOauthLoginResponse> decodeMcpServerOauthLoginResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::McpResourceReadResponse> decodeMcpResourceReadResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::McpServerToolCallResponse> decodeMcpServerToolCallResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::ListMcpServerStatusResponse> decodeListMcpServerStatusResponse(const Json& value, std::string& error) noexcept;

    std::optional<typed::McpServerOauthLoginCompletedNotification>
    decodeMcpServerOauthLoginCompletedNotification(const Notification& notification, std::string& error) noexcept;
    std::optional<typed::McpServerStatusUpdatedNotification> decodeMcpServerStatusUpdatedNotification(const Notification& notification,
                                                                                                      std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_MCPCODEC_H
