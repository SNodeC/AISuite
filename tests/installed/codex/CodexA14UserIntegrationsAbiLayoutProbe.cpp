/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Apps.h"
#include "ai/openai/codex/typed/Client.h"
#include "ai/openai/codex/typed/Events.h"
#include "ai/openai/codex/typed/ExternalAgents.h"
#include "ai/openai/codex/typed/Feedback.h"
#include "ai/openai/codex/typed/Hooks.h"
#include "ai/openai/codex/typed/Marketplace.h"
#include "ai/openai/codex/typed/Plugins.h"
#include "ai/openai/codex/typed/Skills.h"

#include <cstddef>
#include <iostream>
#include <variant>

namespace {

    template <typename Type>
    void printLayout(const char* name) {
        std::cout << name << "|size=" << sizeof(Type) << "|align=" << alignof(Type) << '\n';
    }

} // namespace

#define AISUITE_PRINT_LAYOUT(TYPE) printLayout<typed::TYPE>(#TYPE)

int main() {
    namespace typed = ai::openai::codex::typed;

    printLayout<ai::openai::codex::AppServerClient>("AppServerClient");
    AISUITE_PRINT_LAYOUT(Client);
    AISUITE_PRINT_LAYOUT(CanonicalServerNotification);
    AISUITE_PRINT_LAYOUT(Event);
    AISUITE_PRINT_LAYOUT(PluginSource);

    AISUITE_PRINT_LAYOUT(Apps);
    AISUITE_PRINT_LAYOUT(ExternalAgents);
    AISUITE_PRINT_LAYOUT(Feedback);
    AISUITE_PRINT_LAYOUT(Hooks);
    AISUITE_PRINT_LAYOUT(Marketplace);
    AISUITE_PRINT_LAYOUT(Plugins);
    AISUITE_PRINT_LAYOUT(Skills);

    AISUITE_PRINT_LAYOUT(AppBranding);
    AISUITE_PRINT_LAYOUT(AppReview);
    AISUITE_PRINT_LAYOUT(AppScreenshot);
    AISUITE_PRINT_LAYOUT(AppMetadata);
    AISUITE_PRINT_LAYOUT(AppInfo);
    AISUITE_PRINT_LAYOUT(AppSummary);
    AISUITE_PRINT_LAYOUT(AppTemplateSummary);
    AISUITE_PRINT_LAYOUT(AppsListParams);
    AISUITE_PRINT_LAYOUT(AppsListResponse);
    AISUITE_PRINT_LAYOUT(AppListUpdatedNotification);

    AISUITE_PRINT_LAYOUT(CommandMigration);
    AISUITE_PRINT_LAYOUT(HookMigration);
    AISUITE_PRINT_LAYOUT(McpServerMigration);
    AISUITE_PRINT_LAYOUT(PluginsMigration);
    AISUITE_PRINT_LAYOUT(SessionMigration);
    AISUITE_PRINT_LAYOUT(SkillMigration);
    AISUITE_PRINT_LAYOUT(SubagentMigration);
    AISUITE_PRINT_LAYOUT(MigrationDetails);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigMigrationItem);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigImportItemTypeFailure);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigImportItemTypeSuccess);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigImportTypeResult);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigImportHistory);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigDetectParams);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigDetectResponse);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigImportParams);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigImportResponse);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigImportHistoriesReadResponse);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigImportCompletedNotification);
    AISUITE_PRINT_LAYOUT(ExternalAgentConfigImportProgressNotification);

    AISUITE_PRINT_LAYOUT(FeedbackUploadParams);
    AISUITE_PRINT_LAYOUT(FeedbackUploadResponse);

    AISUITE_PRINT_LAYOUT(HookErrorInfo);
    AISUITE_PRINT_LAYOUT(HookMetadata);
    AISUITE_PRINT_LAYOUT(HooksListEntry);
    AISUITE_PRINT_LAYOUT(HookOutputEntry);
    AISUITE_PRINT_LAYOUT(HookRunSummary);
    AISUITE_PRINT_LAYOUT(HooksListParams);
    AISUITE_PRINT_LAYOUT(HooksListResponse);
    AISUITE_PRINT_LAYOUT(HookStartedNotification);
    AISUITE_PRINT_LAYOUT(HookCompletedNotification);

    AISUITE_PRINT_LAYOUT(MarketplaceAddParams);
    AISUITE_PRINT_LAYOUT(MarketplaceAddResponse);
    AISUITE_PRINT_LAYOUT(MarketplaceRemoveParams);
    AISUITE_PRINT_LAYOUT(MarketplaceRemoveResponse);
    AISUITE_PRINT_LAYOUT(MarketplaceUpgradeErrorInfo);
    AISUITE_PRINT_LAYOUT(MarketplaceUpgradeParams);
    AISUITE_PRINT_LAYOUT(MarketplaceUpgradeResponse);

    AISUITE_PRINT_LAYOUT(GitPluginSource);
    AISUITE_PRINT_LAYOUT(LocalPluginSource);
    AISUITE_PRINT_LAYOUT(NpmPluginSource);
    AISUITE_PRINT_LAYOUT(RemotePluginSource);
    AISUITE_PRINT_LAYOUT(UnknownPluginSource);
    AISUITE_PRINT_LAYOUT(PluginInstallParams);
    AISUITE_PRINT_LAYOUT(PluginInstallResponse);
    AISUITE_PRINT_LAYOUT(PluginShareCheckoutParams);
    AISUITE_PRINT_LAYOUT(PluginShareCheckoutResponse);
    AISUITE_PRINT_LAYOUT(PluginShareDeleteParams);
    AISUITE_PRINT_LAYOUT(PluginShareTarget);
    AISUITE_PRINT_LAYOUT(PluginShareSaveParams);
    AISUITE_PRINT_LAYOUT(PluginShareSaveResponse);
    AISUITE_PRINT_LAYOUT(PluginShareUpdateTargetsParams);
    AISUITE_PRINT_LAYOUT(PluginSharePrincipal);
    AISUITE_PRINT_LAYOUT(PluginShareUpdateTargetsResponse);
    AISUITE_PRINT_LAYOUT(PluginSkillReadParams);
    AISUITE_PRINT_LAYOUT(PluginSkillReadResponse);
    AISUITE_PRINT_LAYOUT(PluginUninstallParams);
    AISUITE_PRINT_LAYOUT(MarketplaceInterface);
    AISUITE_PRINT_LAYOUT(MarketplaceLoadErrorInfo);
    AISUITE_PRINT_LAYOUT(PluginInterface);
    AISUITE_PRINT_LAYOUT(PluginShareContext);
    AISUITE_PRINT_LAYOUT(PluginSummary);
    AISUITE_PRINT_LAYOUT(PluginMarketplaceEntry);
    AISUITE_PRINT_LAYOUT(PluginHookSummary);
    AISUITE_PRINT_LAYOUT(SkillSummary);
    AISUITE_PRINT_LAYOUT(PluginDetail);
    AISUITE_PRINT_LAYOUT(PluginShareListItem);
    AISUITE_PRINT_LAYOUT(PluginInstalledParams);
    AISUITE_PRINT_LAYOUT(PluginInstalledResponse);
    AISUITE_PRINT_LAYOUT(PluginListParams);
    AISUITE_PRINT_LAYOUT(PluginListResponse);
    AISUITE_PRINT_LAYOUT(PluginReadParams);
    AISUITE_PRINT_LAYOUT(PluginReadResponse);
    AISUITE_PRINT_LAYOUT(PluginShareListParams);
    AISUITE_PRINT_LAYOUT(PluginShareListResponse);

    AISUITE_PRINT_LAYOUT(SkillToolDependency);
    AISUITE_PRINT_LAYOUT(SkillDependencies);
    AISUITE_PRINT_LAYOUT(SkillErrorInfo);
    AISUITE_PRINT_LAYOUT(SkillInterface);
    AISUITE_PRINT_LAYOUT(SkillMetadata);
    AISUITE_PRINT_LAYOUT(SkillsListEntry);
    AISUITE_PRINT_LAYOUT(SkillsConfigWriteParams);
    AISUITE_PRINT_LAYOUT(SkillsConfigWriteResponse);
    AISUITE_PRINT_LAYOUT(SkillsExtraRootsSetParams);
    AISUITE_PRINT_LAYOUT(SkillsListParams);
    AISUITE_PRINT_LAYOUT(SkillsListResponse);
    AISUITE_PRINT_LAYOUT(SkillsChangedNotification);

    std::cout << "CanonicalServerNotification|alternatives=" << std::variant_size_v<typed::CanonicalServerNotification> << '\n';
    std::cout << "Event|alternatives=" << std::variant_size_v<typed::Event> << '\n';
    std::cout << "PluginSource|alternatives=" << std::variant_size_v<typed::PluginSource> << '\n';
}

#undef AISUITE_PRINT_LAYOUT
