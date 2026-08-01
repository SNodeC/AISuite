/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_TYPED_PLUGINS_H
#define AI_OPENAI_CODEX_TYPED_PLUGINS_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Apps.h"
#include "ai/openai/codex/typed/Hooks.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Skills.h"
#include "ai/openai/codex/typed/Types.h"

#include <compare>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ai::openai::codex::typed {

    // Protocol string enums deliberately retain unknown future values.
    struct PluginAuthPolicy {
        std::string value;

        static PluginAuthPolicy onInstall() {
            return {"ON_INSTALL"};
        }

        static PluginAuthPolicy onUse() {
            return {"ON_USE"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "ON_INSTALL" || value == "ON_USE";
        }

        auto operator<=>(const PluginAuthPolicy&) const = default;
    };

    struct PluginShareDiscoverability {
        std::string value;

        static PluginShareDiscoverability listed() {
            return {"LISTED"};
        }

        static PluginShareDiscoverability unlisted() {
            return {"UNLISTED"};
        }

        static PluginShareDiscoverability privateVisibility() {
            return {"PRIVATE"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "LISTED" || value == "UNLISTED" || value == "PRIVATE";
        }

        auto operator<=>(const PluginShareDiscoverability&) const = default;
    };

    struct PluginSharePrincipalType {
        std::string value;

        static PluginSharePrincipalType user() {
            return {"user"};
        }

        static PluginSharePrincipalType group() {
            return {"group"};
        }

        static PluginSharePrincipalType workspace() {
            return {"workspace"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "user" || value == "group" || value == "workspace";
        }

        auto operator<=>(const PluginSharePrincipalType&) const = default;
    };

    struct PluginShareTargetRole {
        std::string value;

        static PluginShareTargetRole reader() {
            return {"reader"};
        }

        static PluginShareTargetRole editor() {
            return {"editor"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "reader" || value == "editor";
        }

        auto operator<=>(const PluginShareTargetRole&) const = default;
    };

    struct PluginShareUpdateDiscoverability {
        std::string value;

        static PluginShareUpdateDiscoverability unlisted() {
            return {"UNLISTED"};
        }

        static PluginShareUpdateDiscoverability privateVisibility() {
            return {"PRIVATE"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "UNLISTED" || value == "PRIVATE";
        }

        auto operator<=>(const PluginShareUpdateDiscoverability&) const = default;
    };

    struct PluginSharePrincipalRole {
        std::string value;

        static PluginSharePrincipalRole reader() {
            return {"reader"};
        }

        static PluginSharePrincipalRole editor() {
            return {"editor"};
        }

        static PluginSharePrincipalRole owner() {
            return {"owner"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "reader" || value == "editor" || value == "owner";
        }

        auto operator<=>(const PluginSharePrincipalRole&) const = default;
    };

    struct PluginListMarketplaceKind {
        std::string value;

        static PluginListMarketplaceKind local() {
            return {"local"};
        }

        static PluginListMarketplaceKind vertical() {
            return {"vertical"};
        }

        static PluginListMarketplaceKind workspaceDirectory() {
            return {"workspace-directory"};
        }

        static PluginListMarketplaceKind sharedWithMe() {
            return {"shared-with-me"};
        }

        static PluginListMarketplaceKind createdByMeRemote() {
            return {"created-by-me-remote"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "local" || value == "vertical" || value == "workspace-directory" || value == "shared-with-me" ||
                   value == "created-by-me-remote";
        }

        auto operator<=>(const PluginListMarketplaceKind&) const = default;
    };

    struct PluginAvailability {
        std::string value;

        static PluginAvailability disabledByAdmin() {
            return {"DISABLED_BY_ADMIN"};
        }

        static PluginAvailability available() {
            return {"AVAILABLE"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "DISABLED_BY_ADMIN" || value == "AVAILABLE";
        }

        auto operator<=>(const PluginAvailability&) const = default;
    };

    struct PluginInstallPolicy {
        std::string value;

        static PluginInstallPolicy notAvailable() {
            return {"NOT_AVAILABLE"};
        }

        static PluginInstallPolicy available() {
            return {"AVAILABLE"};
        }

        static PluginInstallPolicy installedByDefault() {
            return {"INSTALLED_BY_DEFAULT"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "NOT_AVAILABLE" || value == "AVAILABLE" || value == "INSTALLED_BY_DEFAULT";
        }

        auto operator<=>(const PluginInstallPolicy&) const = default;
    };

    struct PluginInstallPolicySource {
        std::string value;

        static PluginInstallPolicySource workspaceSetting() {
            return {"WORKSPACE_SETTING"};
        }

        static PluginInstallPolicySource implicitCanonicalApp() {
            return {"IMPLICIT_CANONICAL_APP"};
        }

        [[nodiscard]] bool isKnown() const noexcept {
            return value == "WORKSPACE_SETTING" || value == "IMPLICIT_CANONICAL_APP";
        }

        auto operator<=>(const PluginInstallPolicySource&) const = default;
    };

    struct GitPluginSource {
        OptionalNullable<std::string> path;
        OptionalNullable<std::string> refName;
        OptionalNullable<std::string> sha;
        std::string url;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const GitPluginSource&) const = default;
    };

    struct LocalPluginSource {
        AbsolutePath path;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const LocalPluginSource&) const = default;
    };

    struct NpmPluginSource {
        std::string package;
        OptionalNullable<std::string> registry;
        OptionalNullable<std::string> version;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const NpmPluginSource&) const = default;
    };

    struct RemotePluginSource {
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const RemotePluginSource&) const = default;
    };

    // This raw-preserving alternative represents both future discriminators
    // and malformed payloads for a known discriminator. The diagnostic kind
    // distinguishes ForwardCompatibility from MalformedKnownPayload.
    struct UnknownPluginSource {
        std::optional<std::string> type;
        Json raw = Json::object();
        std::optional<DecodeDiagnostic> diagnostic;

        bool operator==(const UnknownPluginSource&) const = default;
    };

    // ABI-relevant order is derived from the production registry:
    // git, local, npm, remote, then the future/raw fallback.
    using PluginSource = std::variant<GitPluginSource, LocalPluginSource, NpmPluginSource, RemotePluginSource, UnknownPluginSource>;

    static_assert(std::variant_size_v<PluginSource> == 5);
    static_assert(std::is_same_v<std::variant_alternative_t<0, PluginSource>, GitPluginSource>);
    static_assert(std::is_same_v<std::variant_alternative_t<1, PluginSource>, LocalPluginSource>);
    static_assert(std::is_same_v<std::variant_alternative_t<2, PluginSource>, NpmPluginSource>);
    static_assert(std::is_same_v<std::variant_alternative_t<3, PluginSource>, RemotePluginSource>);
    static_assert(std::is_same_v<std::variant_alternative_t<4, PluginSource>, UnknownPluginSource>);

    [[nodiscard]] inline std::string pluginSourceDiscriminator(const PluginSource& source) {
        return std::visit(
            [](const auto& alternative) -> std::string {
                using Alternative = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Alternative, GitPluginSource>) {
                    return "git";
                } else if constexpr (std::is_same_v<Alternative, LocalPluginSource>) {
                    return "local";
                } else if constexpr (std::is_same_v<Alternative, NpmPluginSource>) {
                    return "npm";
                } else if constexpr (std::is_same_v<Alternative, RemotePluginSource>) {
                    return "remote";
                } else {
                    return alternative.type.value_or(std::string{});
                }
            },
            source);
    }

    [[nodiscard]] inline const Json& pluginSourceRaw(const PluginSource& source) noexcept {
        return std::visit(
            [](const auto& alternative) -> const Json& {
                return alternative.raw;
            },
            source);
    }

    struct PluginInstallParams {
        OptionalNullable<AbsolutePath> marketplacePath;
        std::string pluginName;
        OptionalNullable<std::string> remoteMarketplaceName;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginInstallParams&) const = default;
    };

    struct PluginInstallResponse {
        std::vector<AppSummary> appsNeedingAuth;
        PluginAuthPolicy authPolicy;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginInstallResponse&) const = default;
    };

    struct PluginShareCheckoutParams {
        std::string remotePluginId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareCheckoutParams&) const = default;
    };

    struct PluginShareCheckoutResponse {
        std::string marketplaceName;
        AbsolutePath marketplacePath;
        std::string pluginId;
        std::string pluginName;
        AbsolutePath pluginPath;
        std::string remotePluginId;
        OptionalNullable<std::string> remoteVersion;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareCheckoutResponse&) const = default;
    };

    struct PluginShareDeleteParams {
        std::string remotePluginId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareDeleteParams&) const = default;
    };

    struct PluginShareTarget {
        std::string principalId;
        PluginSharePrincipalType principalType;
        PluginShareTargetRole role;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareTarget&) const = default;
    };

    struct PluginShareSaveParams {
        OptionalNullable<PluginShareDiscoverability> discoverability;
        AbsolutePath pluginPath;
        OptionalNullable<std::string> remotePluginId;
        OptionalNullable<std::vector<PluginShareTarget>> shareTargets;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareSaveParams&) const = default;
    };

    struct PluginShareSaveResponse {
        std::string remotePluginId;
        std::string shareUrl;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareSaveResponse&) const = default;
    };

    struct PluginShareUpdateTargetsParams {
        PluginShareUpdateDiscoverability discoverability;
        std::string remotePluginId;
        std::vector<PluginShareTarget> shareTargets;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareUpdateTargetsParams&) const = default;
    };

    struct PluginSharePrincipal {
        std::string name;
        std::string principalId;
        PluginSharePrincipalType principalType;
        PluginSharePrincipalRole role;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginSharePrincipal&) const = default;
    };

    struct PluginShareUpdateTargetsResponse {
        PluginShareDiscoverability discoverability;
        std::vector<PluginSharePrincipal> principals;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareUpdateTargetsResponse&) const = default;
    };

    struct PluginSkillReadParams {
        std::string remoteMarketplaceName;
        std::string remotePluginId;
        std::string skillName;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginSkillReadParams&) const = default;
    };

    struct PluginSkillReadResponse {
        OptionalNullable<std::string> contents;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginSkillReadResponse&) const = default;
    };

    struct PluginUninstallParams {
        std::string pluginId;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginUninstallParams&) const = default;
    };

    struct MarketplaceInterface {
        OptionalNullable<std::string> displayName;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MarketplaceInterface&) const = default;
    };

    struct MarketplaceLoadErrorInfo {
        AbsolutePath marketplacePath;
        std::string message;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const MarketplaceLoadErrorInfo&) const = default;
    };

    struct PluginInterface {
        OptionalNullable<std::string> brandColor;
        std::vector<std::string> capabilities;
        OptionalNullable<std::string> category;
        OptionalNullable<AbsolutePath> composerIcon;
        OptionalNullable<std::string> composerIconUrl;
        OptionalNullable<std::vector<std::string>> defaultPrompt;
        OptionalNullable<std::string> developerName;
        OptionalNullable<std::string> displayName;
        OptionalNullable<AbsolutePath> logo;
        OptionalNullable<AbsolutePath> logoDark;
        OptionalNullable<std::string> logoUrl;
        OptionalNullable<std::string> logoUrlDark;
        OptionalNullable<std::string> longDescription;
        OptionalNullable<std::string> privacyPolicyUrl;
        std::vector<std::string> screenshotUrls;
        std::vector<AbsolutePath> screenshots;
        OptionalNullable<std::string> shortDescription;
        OptionalNullable<std::string> termsOfServiceUrl;
        OptionalNullable<std::string> websiteUrl;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginInterface&) const = default;
    };

    struct PluginShareContext {
        OptionalNullable<std::string> creatorAccountUserId;
        OptionalNullable<std::string> creatorName;
        OptionalNullable<PluginShareDiscoverability> discoverability;
        std::string remotePluginId;
        OptionalNullable<std::string> remoteVersion;
        OptionalNullable<std::vector<PluginSharePrincipal>> sharePrincipals;
        OptionalNullable<std::string> shareUrl;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareContext&) const = default;
    };

    struct PluginSummary {
        PluginAuthPolicy authPolicy;
        std::optional<PluginAvailability> availability;
        bool enabled = false;
        std::string id;
        PluginInstallPolicy installPolicy;
        OptionalNullable<PluginInstallPolicySource> installPolicySource;
        bool installed = false;
        OptionalNullable<PluginInterface> interface;
        std::optional<std::vector<std::string>> keywords;
        OptionalNullable<std::string> localVersion;
        std::string name;
        OptionalNullable<std::string> remotePluginId;
        OptionalNullable<PluginShareContext> shareContext;
        PluginSource source;
        OptionalNullable<std::string> version;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginSummary&) const = default;
    };

    struct PluginMarketplaceEntry {
        OptionalNullable<MarketplaceInterface> interface;
        std::string name;
        OptionalNullable<AbsolutePath> path;
        std::vector<PluginSummary> plugins;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginMarketplaceEntry&) const = default;
    };

    struct PluginHookSummary {
        HookEventName eventName;
        std::string key;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginHookSummary&) const = default;
    };

    struct SkillSummary {
        std::string description;
        bool enabled = false;
        OptionalNullable<SkillInterface> interface;
        std::string name;
        OptionalNullable<AbsolutePath> path;
        OptionalNullable<std::string> shortDescription;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const SkillSummary&) const = default;
    };

    struct PluginDetail {
        std::vector<AppTemplateSummary> appTemplates;
        std::vector<AppSummary> apps;
        OptionalNullable<std::string> description;
        std::vector<PluginHookSummary> hooks;
        std::string marketplaceName;
        OptionalNullable<AbsolutePath> marketplacePath;
        std::vector<std::string> mcpServers;
        OptionalNullable<std::string> shareUrl;
        std::vector<SkillSummary> skills;
        PluginSummary summary;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginDetail&) const = default;
    };

    struct PluginShareListItem {
        OptionalNullable<AbsolutePath> localPluginPath;
        PluginSummary plugin;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareListItem&) const = default;
    };

    struct PluginInstalledParams {
        OptionalNullable<std::vector<AbsolutePath>> cwds;
        OptionalNullable<std::vector<std::string>> installSuggestionPluginNames;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginInstalledParams&) const = default;
    };

    struct PluginInstalledResponse {
        std::optional<std::vector<MarketplaceLoadErrorInfo>> marketplaceLoadErrors;
        std::vector<PluginMarketplaceEntry> marketplaces;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginInstalledResponse&) const = default;
    };

    struct PluginListParams {
        OptionalNullable<std::vector<AbsolutePath>> cwds;
        OptionalNullable<std::vector<PluginListMarketplaceKind>> marketplaceKinds;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginListParams&) const = default;
    };

    struct PluginListResponse {
        std::optional<std::vector<std::string>> featuredPluginIds;
        std::optional<std::vector<MarketplaceLoadErrorInfo>> marketplaceLoadErrors;
        std::vector<PluginMarketplaceEntry> marketplaces;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginListResponse&) const = default;
    };

    struct PluginReadParams {
        OptionalNullable<AbsolutePath> marketplacePath;
        std::string pluginName;
        OptionalNullable<std::string> remoteMarketplaceName;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginReadParams&) const = default;
    };

    struct PluginReadResponse {
        PluginDetail plugin;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginReadResponse&) const = default;
    };

    struct PluginShareListParams {
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareListParams&) const = default;
    };

    struct PluginShareListResponse {
        std::vector<PluginShareListItem> data;
        Json raw = Json::object();
        std::vector<DecodeDiagnostic> diagnostics;

        bool operator==(const PluginShareListResponse&) const = default;
    };

    class Plugins {
    public:
        Plugins(const Plugins&) = delete;
        Plugins(Plugins&&) = delete;
        Plugins& operator=(const Plugins&) = delete;
        Plugins& operator=(Plugins&&) = delete;

        Submission install(PluginInstallParams params, CompletionHandler<PluginInstallResponse> handler);
        Submission installed(PluginInstalledParams params, CompletionHandler<PluginInstalledResponse> handler);
        Submission installed(CompletionHandler<PluginInstalledResponse> handler);
        Submission list(PluginListParams params, CompletionHandler<PluginListResponse> handler);
        Submission list(CompletionHandler<PluginListResponse> handler);
        Submission read(PluginReadParams params, CompletionHandler<PluginReadResponse> handler);
        Submission shareCheckout(PluginShareCheckoutParams params, CompletionHandler<PluginShareCheckoutResponse> handler);
        Submission shareDelete(PluginShareDeleteParams params, DoneHandler handler);
        Submission shareList(CompletionHandler<PluginShareListResponse> handler);
        Submission shareSave(PluginShareSaveParams params, CompletionHandler<PluginShareSaveResponse> handler);
        Submission shareUpdateTargets(PluginShareUpdateTargetsParams params, CompletionHandler<PluginShareUpdateTargetsResponse> handler);
        Submission readSkill(PluginSkillReadParams params, CompletionHandler<PluginSkillReadResponse> handler);
        Submission uninstall(PluginUninstallParams params, DoneHandler handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Plugins(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_PLUGINS_H
