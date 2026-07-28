/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_FEEDBACKCODEC_H
#define AI_OPENAI_CODEX_DETAIL_FEEDBACKCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Feedback.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<Json> encodeFeedbackUploadParams(const typed::FeedbackUploadParams& value, std::string& error) noexcept;
    std::optional<typed::FeedbackUploadResponse> decodeFeedbackUploadResponse(const Json& value, std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_FEEDBACKCODEC_H
