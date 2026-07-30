/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_MCPREVERSEREQUESTCODEC_H
#define AI_OPENAI_CODEX_DETAIL_MCPREVERSEREQUESTCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/ServerRequests.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<typed::AttestationGenerateParams> decodeAttestationGenerateParams(const Json& value, std::string& error) noexcept;
    std::optional<typed::DynamicToolCallParams> decodeDynamicToolCallParams(const Json& value, std::string& error) noexcept;

    std::optional<Json> encodeAttestationGenerateResponse(const typed::AttestationGenerateResponse& value, std::string& error) noexcept;
    std::optional<Json> encodeDynamicToolCallResponse(const typed::DynamicToolCallResponse& value, std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_MCPREVERSEREQUESTCODEC_H
