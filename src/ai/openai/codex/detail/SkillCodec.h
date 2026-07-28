/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_SKILLCODEC_H
#define AI_OPENAI_CODEX_DETAIL_SKILLCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Skills.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<Json> encodeSkillsConfigWriteParams(const typed::SkillsConfigWriteParams& value, std::string& error) noexcept;
    std::optional<Json> encodeSkillsExtraRootsSetParams(const typed::SkillsExtraRootsSetParams& value, std::string& error) noexcept;
    std::optional<Json> encodeSkillsListParams(const typed::SkillsListParams& value, std::string& error) noexcept;

    std::optional<typed::SkillsConfigWriteResponse> decodeSkillsConfigWriteResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::SkillsListResponse> decodeSkillsListResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::SkillsChangedNotification> decodeSkillsChangedNotification(const Notification& notification,
                                                                                    std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_SKILLCODEC_H
