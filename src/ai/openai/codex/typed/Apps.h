/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_APPS_H
#define AI_OPENAI_CODEX_TYPED_APPS_H

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

    struct AppTemplateUnavailableReason {
        std::string value;

        static AppTemplateUnavailableReason notConfiguredForWorkspace() {
            return {"NOT_CONFIGURED_FOR_WORKSPACE"};
        }

        static AppTemplateUnavailableReason noActiveWorkspace() {
            return {"NO_ACTIVE_WORKSPACE"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "NOT_CONFIGURED_FOR_WORKSPACE" || value == "NO_ACTIVE_WORKSPACE";
        }

        auto operator<=>(const AppTemplateUnavailableReason&) const = default;
    };

    struct AppBranding {
        OptionalNullable<std::string> category;
        OptionalNullable<std::string> developer;
        bool isDiscoverableApp = false;
        OptionalNullable<std::string> privacyPolicy;
        OptionalNullable<std::string> termsOfService;
        OptionalNullable<std::string> website;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppBranding&) const = default;
    };

    struct AppReview {
        std::string status;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppReview&) const = default;
    };

    struct AppScreenshot {
        OptionalNullable<std::string> fileId;
        OptionalNullable<std::string> url;
        std::string userPrompt;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppScreenshot&) const = default;
    };

    struct AppMetadata {
        OptionalNullable<std::vector<std::string>> categories;
        OptionalNullable<std::string> developer;
        OptionalNullable<bool> firstPartyRequiresInstall;
        OptionalNullable<std::string> firstPartyType;
        OptionalNullable<AppReview> review;
        OptionalNullable<std::vector<AppScreenshot>> screenshots;
        OptionalNullable<std::string> seoDescription;
        OptionalNullable<bool> showInComposerWhenUnlinked;
        OptionalNullable<std::vector<std::string>> subCategories;
        OptionalNullable<std::string> version;
        OptionalNullable<std::string> versionId;
        OptionalNullable<std::string> versionNotes;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppMetadata&) const = default;
    };

    struct AppInfo {
        OptionalNullable<AppMetadata> appMetadata;
        OptionalNullable<AppBranding> branding;
        OptionalNullable<std::string> description;
        OptionalNullable<std::string> distributionChannel;
        OptionalNullable<std::map<std::string, std::string>> iconAssets;
        OptionalNullable<std::map<std::string, std::string>> iconDarkAssets;
        std::string id;
        OptionalNullable<std::string> installUrl;
        // These schema-default-bearing fields remain optional so an omitted
        // value is not silently materialized during decoding.
        std::optional<bool> isAccessible;
        std::optional<bool> isEnabled;
        OptionalNullable<std::map<std::string, std::string>> labels;
        OptionalNullable<std::string> logoUrl;
        OptionalNullable<std::string> logoUrlDark;
        std::string name;
        std::optional<std::vector<std::string>> pluginDisplayNames;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppInfo&) const = default;
    };

    struct AppSummary {
        OptionalNullable<std::string> category;
        OptionalNullable<std::string> description;
        std::string id;
        OptionalNullable<std::string> installUrl;
        std::string name;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppSummary&) const = default;
    };

    struct AppTemplateSummary {
        OptionalNullable<std::string> canonicalConnectorId;
        OptionalNullable<std::string> category;
        OptionalNullable<std::string> description;
        OptionalNullable<std::string> logoUrl;
        OptionalNullable<std::string> logoUrlDark;
        std::vector<std::string> materializedAppIds;
        std::string name;
        OptionalNullable<AppTemplateUnavailableReason> reason;
        std::string templateId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppTemplateSummary&) const = default;
    };

    struct AppsListParams {
        OptionalNullable<std::string> cursor;
        std::optional<bool> forceRefetch;
        OptionalNullable<std::uint32_t> limit;
        OptionalNullable<ThreadId> threadId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppsListParams&) const = default;
    };

    struct AppsListResponse {
        std::vector<AppInfo> data;
        OptionalNullable<std::string> nextCursor;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppsListResponse&) const = default;
    };

    struct AppListUpdatedNotification {
        std::vector<AppInfo> data;
        // Notification aggregates retain the complete JSON-RPC envelope.
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const AppListUpdatedNotification&) const = default;
    };

    class Apps {
    public:
        Apps(const Apps&) = delete;
        Apps(Apps&&) = delete;
        Apps& operator=(const Apps&) = delete;
        Apps& operator=(Apps&&) = delete;

        Submission list(AppsListParams params, CompletionHandler<AppsListResponse> handler);
        Submission list(CompletionHandler<AppsListResponse> handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Apps(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_APPS_H
