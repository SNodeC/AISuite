/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_EXTERNALAGENTCODEC_H
#define AI_OPENAI_CODEX_DETAIL_EXTERNALAGENTCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/ExternalAgents.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<Json> encodeExternalAgentConfigDetectParams(const typed::ExternalAgentConfigDetectParams& value,
                                                              std::string& error) noexcept;
    std::optional<Json> encodeExternalAgentConfigImportParams(const typed::ExternalAgentConfigImportParams& value,
                                                              std::string& error) noexcept;

    std::optional<typed::ExternalAgentConfigDetectResponse> decodeExternalAgentConfigDetectResponse(const Json& value,
                                                                                                    std::string& error) noexcept;
    std::optional<typed::ExternalAgentConfigImportResponse> decodeExternalAgentConfigImportResponse(const Json& value,
                                                                                                    std::string& error) noexcept;
    std::optional<typed::ExternalAgentConfigImportHistoriesReadResponse>
    decodeExternalAgentConfigImportHistoriesReadResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::ExternalAgentConfigImportCompletedNotification>
    decodeExternalAgentConfigImportCompletedNotification(const Notification& notification, std::string& error) noexcept;
    std::optional<typed::ExternalAgentConfigImportProgressNotification>
    decodeExternalAgentConfigImportProgressNotification(const Notification& notification, std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_EXTERNALAGENTCODEC_H
