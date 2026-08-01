/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_MARKETPLACE_H
#define AI_OPENAI_CODEX_TYPED_MARKETPLACE_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Types.h"

#include <functional>
#include <string>
#include <vector>

namespace ai::openai::codex::typed {

    struct MarketplaceAddParams {
        OptionalNullable<std::string> refName;
        std::string source;
        OptionalNullable<std::vector<std::string>> sparsePaths;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MarketplaceAddParams&) const = default;
    };

    struct MarketplaceAddResponse {
        bool alreadyAdded = false;
        AbsolutePath installedRoot;
        std::string marketplaceName;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MarketplaceAddResponse&) const = default;
    };

    struct MarketplaceRemoveParams {
        std::string marketplaceName;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MarketplaceRemoveParams&) const = default;
    };

    struct MarketplaceRemoveResponse {
        OptionalNullable<AbsolutePath> installedRoot;
        std::string marketplaceName;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MarketplaceRemoveResponse&) const = default;
    };

    struct MarketplaceUpgradeErrorInfo {
        std::string marketplaceName;
        std::string message;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MarketplaceUpgradeErrorInfo&) const = default;
    };

    struct MarketplaceUpgradeParams {
        OptionalNullable<std::string> marketplaceName;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MarketplaceUpgradeParams&) const = default;
    };

    struct MarketplaceUpgradeResponse {
        std::vector<MarketplaceUpgradeErrorInfo> errors;
        std::vector<std::string> selectedMarketplaces;
        std::vector<AbsolutePath> upgradedRoots;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MarketplaceUpgradeResponse&) const = default;
    };

    class Marketplace {
    public:
        Marketplace(const Marketplace&) = delete;
        Marketplace(Marketplace&&) = delete;
        Marketplace& operator=(const Marketplace&) = delete;
        Marketplace& operator=(Marketplace&&) = delete;

        Submission add(MarketplaceAddParams params, CompletionHandler<MarketplaceAddResponse> handler);
        Submission remove(MarketplaceRemoveParams params, CompletionHandler<MarketplaceRemoveResponse> handler);
        Submission upgrade(MarketplaceUpgradeParams params, CompletionHandler<MarketplaceUpgradeResponse> handler);
        Submission upgrade(CompletionHandler<MarketplaceUpgradeResponse> handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Marketplace(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_MARKETPLACE_H
