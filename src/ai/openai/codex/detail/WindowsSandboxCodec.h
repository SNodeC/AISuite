/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_WINDOWSSANDBOXCODEC_H
#define AI_OPENAI_CODEX_DETAIL_WINDOWSSANDBOXCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/WindowsSandbox.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<Json> encodeWindowsSandboxSetupStartParams(const typed::WindowsSandboxSetupStartParams& value,
                                                             std::string& error) noexcept;

    std::optional<typed::WindowsSandboxReadinessResponse> decodeWindowsSandboxReadinessResponse(const Json& value,
                                                                                                std::string& error) noexcept;
    std::optional<typed::WindowsSandboxSetupStartResponse> decodeWindowsSandboxSetupStartResponse(const Json& value,
                                                                                                  std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_WINDOWSSANDBOXCODEC_H
