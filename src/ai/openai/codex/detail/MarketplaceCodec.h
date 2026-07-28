/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_MARKETPLACECODEC_H
#define AI_OPENAI_CODEX_DETAIL_MARKETPLACECODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Marketplace.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<Json> encodeMarketplaceAddParams(const typed::MarketplaceAddParams& value, std::string& error) noexcept;
    std::optional<Json> encodeMarketplaceRemoveParams(const typed::MarketplaceRemoveParams& value, std::string& error) noexcept;
    std::optional<Json> encodeMarketplaceUpgradeParams(const typed::MarketplaceUpgradeParams& value, std::string& error) noexcept;

    std::optional<typed::MarketplaceAddResponse> decodeMarketplaceAddResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::MarketplaceRemoveResponse> decodeMarketplaceRemoveResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::MarketplaceUpgradeResponse> decodeMarketplaceUpgradeResponse(const Json& value, std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_MARKETPLACECODEC_H
