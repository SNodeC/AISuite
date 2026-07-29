/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_HOOKCODEC_H
#define AI_OPENAI_CODEX_DETAIL_HOOKCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Hooks.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<Json> encodeHooksListParams(const typed::HooksListParams& value, std::string& error) noexcept;

    std::optional<typed::HooksListResponse> decodeHooksListResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::HookCompletedNotification> decodeHookCompletedNotification(const Notification& notification,
                                                                                    std::string& error) noexcept;
    std::optional<typed::HookStartedNotification> decodeHookStartedNotification(const Notification& notification,
                                                                                std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_HOOKCODEC_H
