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
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Types.h"

#include <compare>
#include <functional>
#include <string>
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

    struct PluginInstallParams {
        OptionalNullable<AbsolutePathBuf> marketplacePath;
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
        AbsolutePathBuf marketplacePath;
        std::string pluginId;
        std::string pluginName;
        AbsolutePathBuf pluginPath;
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
        AbsolutePathBuf pluginPath;
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

    class Plugins {
    public:
        using Submission = AppServerClient::RawProtocol::Submission;
        using InstallResult = OperationResult<PluginInstallResponse>;
        using InstallResultHandler = std::function<void(const InstallResult&)>;
        using ShareCheckoutResult = OperationResult<PluginShareCheckoutResponse>;
        using ShareCheckoutResultHandler = std::function<void(const ShareCheckoutResult&)>;
        using ShareDeleteResult = OperationResult<Unit>;
        using ShareDeleteResultHandler = std::function<void(const ShareDeleteResult&)>;
        using ShareSaveResult = OperationResult<PluginShareSaveResponse>;
        using ShareSaveResultHandler = std::function<void(const ShareSaveResult&)>;
        using ShareUpdateTargetsResult = OperationResult<PluginShareUpdateTargetsResponse>;
        using ShareUpdateTargetsResultHandler = std::function<void(const ShareUpdateTargetsResult&)>;
        using ReadSkillResult = OperationResult<PluginSkillReadResponse>;
        using ReadSkillResultHandler = std::function<void(const ReadSkillResult&)>;
        using UninstallResult = OperationResult<Unit>;
        using UninstallResultHandler = std::function<void(const UninstallResult&)>;

        Submission install(PluginInstallParams params, InstallResultHandler handler);
        Submission shareCheckout(PluginShareCheckoutParams params, ShareCheckoutResultHandler handler);
        Submission shareDelete(PluginShareDeleteParams params, ShareDeleteResultHandler handler);
        Submission shareSave(PluginShareSaveParams params, ShareSaveResultHandler handler);
        Submission shareUpdateTargets(PluginShareUpdateTargetsParams params, ShareUpdateTargetsResultHandler handler);
        Submission readSkill(PluginSkillReadParams params, ReadSkillResultHandler handler);
        Submission uninstall(PluginUninstallParams params, UninstallResultHandler handler);

    private:
        friend class ::ai::openai::codex::AppServerClient;

        explicit Plugins(AppServerClient::RawProtocol& protocol) noexcept;

        AppServerClient::RawProtocol* protocol;
    };

} // namespace ai::openai::codex::typed

#endif // AI_OPENAI_CODEX_TYPED_PLUGINS_H
