/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

// This private table is intentionally included more than once.  The public
// command types remain ordinary C++ structs; this table binds every stable
// application operation to its direct typed façade, exact result, access
// policy, canonical-state disposition, and wire identity.

CODEX_BACKEND_PROVIDER_OPERATION(
    AccountLoginCancel, CancelLoginAccountResponse, accounts, cancelLogin, Controller, true, "account/login/cancel")
CODEX_BACKEND_PROVIDER_OPERATION(AccountLoginStart, LoginAccountResponse, accounts, startLogin, Controller, true, "account/login/start")
CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(AccountLogout, Unit, accounts, logout, Controller, true, "account/logout")
CODEX_BACKEND_PROVIDER_OPERATION(AccountRateLimitResetCreditConsume,
                                 ConsumeAccountRateLimitResetCreditResponse,
                                 accounts,
                                 consumeRateLimitResetCredit,
                                 Controller,
                                 true,
                                 "account/rateLimitResetCredit/consume")
CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(
    AccountRateLimitsRead, GetAccountRateLimitsResponse, accounts, readRateLimits, Observer, true, "account/rateLimits/read")
CODEX_BACKEND_PROVIDER_OPERATION(AccountRead, GetAccountResponse, accounts, read, Observer, true, "account/read")
CODEX_BACKEND_PROVIDER_OPERATION(AccountSendAddCreditsNudgeEmail,
                                 SendAddCreditsNudgeEmailResponse,
                                 accounts,
                                 sendAddCreditsNudgeEmail,
                                 Controller,
                                 false,
                                 "account/sendAddCreditsNudgeEmail")
CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(
    AccountUsageRead, GetAccountTokenUsageResponse, accounts, readUsage, Observer, true, "account/usage/read")
CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(AccountWorkspaceMessagesRead,
                                       GetWorkspaceMessagesResponse,
                                       accounts,
                                       readWorkspaceMessages,
                                       Observer,
                                       true,
                                       "account/workspaceMessages/read")

CODEX_BACKEND_PROVIDER_OPERATION(ConfigBatchWrite, ConfigWriteResponse, configuration, batchWrite, Controller, true, "config/batchWrite")
CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(
    ConfigMcpServerReload, Unit, configuration, reloadMcpServers, Controller, true, "config/mcpServer/reload")
CODEX_BACKEND_PROVIDER_OPERATION(ConfigRead, ConfigReadResponse, configuration, read, Observer, true, "config/read")
CODEX_BACKEND_PROVIDER_OPERATION(ConfigValueWrite, ConfigWriteResponse, configuration, writeValue, Controller, true, "config/value/write")
CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(
    ConfigRequirementsRead, ConfigRequirementsReadResponse, configuration, readRequirements, Observer, true, "configRequirements/read")
CODEX_BACKEND_PROVIDER_OPERATION(ExperimentalFeatureEnablementSet,
                                 ExperimentalFeatureEnablementSetResponse,
                                 configuration,
                                 setExperimentalFeatureEnablement,
                                 Controller,
                                 true,
                                 "experimentalFeature/enablement/set")
CODEX_BACKEND_PROVIDER_OPERATION(ExperimentalFeatureList,
                                 ExperimentalFeatureListResponse,
                                 configuration,
                                 listExperimentalFeatures,
                                 Observer,
                                 true,
                                 "experimentalFeature/list")

CODEX_BACKEND_PROVIDER_OPERATION(ModelList, ModelListResponse, models, list, Observer, true, "model/list")
CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(ModelProviderCapabilitiesRead,
                                       ModelProviderCapabilitiesReadResponse,
                                       models,
                                       readProviderCapabilities,
                                       Observer,
                                       true,
                                       "modelProvider/capabilities/read")

CODEX_BACKEND_PROVIDER_OPERATION(ThreadArchive, Unit, threads, archive, Controller, true, "thread/archive")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadCompactStart, Unit, threads, startCompaction, Controller, true, "thread/compact/start")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadDelete, Unit, threads, remove, Controller, true, "thread/delete")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadFork, ThreadForkResponse, threads, fork, Controller, true, "thread/fork")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadGoalClear, ThreadGoalClearResponse, threads, clearGoal, Controller, true, "thread/goal/clear")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadGoalGet, ThreadGoalGetResponse, threads, getGoal, Observer, true, "thread/goal/get")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadGoalSet, ThreadGoalSetResponse, threads, setGoal, Controller, true, "thread/goal/set")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadInjectItems, Unit, threads, injectItems, Controller, true, "thread/inject_items")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadList, ThreadListResponse, threads, list, Observer, true, "thread/list")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadLoadedList, ThreadLoadedListResponse, threads, listLoaded, Observer, true, "thread/loaded/list")
CODEX_BACKEND_PROVIDER_OPERATION(
    ThreadMetadataUpdate, ThreadMetadataUpdateResponse, threads, updateMetadata, Controller, true, "thread/metadata/update")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadSetName, Unit, threads, setName, Controller, true, "thread/name/set")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadRead, ThreadReadResponse, threads, read, Observer, true, "thread/read")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadResume, ThreadResumeResponse, threads, resume, Controller, true, "thread/resume")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadRollback, ThreadRollbackResponse, threads, rollback, Controller, true, "thread/rollback")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadShellCommand, Unit, threads, shellCommand, Controller, false, "thread/shellCommand")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadStart, ThreadStartResponse, threads, start, Controller, true, "thread/start")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadUnarchive, ThreadUnarchiveResponse, threads, unarchive, Controller, true, "thread/unarchive")
CODEX_BACKEND_PROVIDER_OPERATION(ThreadUnsubscribe, ThreadUnsubscribeResponse, threads, unsubscribe, Controller, true, "thread/unsubscribe")
CODEX_BACKEND_PROVIDER_OPERATION(
    ThreadApproveGuardianDeniedAction, Unit, threads, approveGuardianDeniedAction, Controller, true, "thread/approveGuardianDeniedAction")

CODEX_BACKEND_PROVIDER_OPERATION(TurnStart, TurnStartResponse, turns, start, Controller, true, "turn/start")
CODEX_BACKEND_PROVIDER_OPERATION(TurnInterrupt, Unit, turns, interrupt, Controller, false, "turn/interrupt")
CODEX_BACKEND_PROVIDER_OPERATION(TurnSteer, TurnSteerResponse, turns, steer, Controller, true, "turn/steer")

CODEX_BACKEND_PROVIDER_OPERATION(CommandExec, CommandExecResponse, commands, exec, Controller, true, "command/exec")
CODEX_BACKEND_PROVIDER_OPERATION(CommandExecResize, Unit, commands, resize, Controller, true, "command/exec/resize")
CODEX_BACKEND_PROVIDER_OPERATION(CommandExecTerminate, Unit, commands, terminate, Controller, true, "command/exec/terminate")
CODEX_BACKEND_PROVIDER_OPERATION(CommandExecWrite, Unit, commands, write, Controller, true, "command/exec/write")

CODEX_BACKEND_PROVIDER_OPERATION(FsCopy, Unit, filesystem, copy, Controller, false, "fs/copy")
CODEX_BACKEND_PROVIDER_OPERATION(FsCreateDirectory, Unit, filesystem, createDirectory, Controller, false, "fs/createDirectory")
CODEX_BACKEND_PROVIDER_OPERATION(FsGetMetadata, FsGetMetadataResponse, filesystem, getMetadata, Observer, false, "fs/getMetadata")
CODEX_BACKEND_PROVIDER_OPERATION(FsReadDirectory, FsReadDirectoryResponse, filesystem, readDirectory, Observer, false, "fs/readDirectory")
CODEX_BACKEND_PROVIDER_OPERATION(FsReadFile, FsReadFileResponse, filesystem, readFile, Observer, false, "fs/readFile")
CODEX_BACKEND_PROVIDER_OPERATION(FsRemove, Unit, filesystem, remove, Controller, false, "fs/remove")
CODEX_BACKEND_PROVIDER_OPERATION(FsUnwatch, Unit, filesystem, unwatch, Controller, true, "fs/unwatch")
CODEX_BACKEND_PROVIDER_OPERATION(FsWatch, FsWatchResponse, filesystem, watch, Controller, true, "fs/watch")
CODEX_BACKEND_PROVIDER_OPERATION(FsWriteFile, Unit, filesystem, writeFile, Controller, false, "fs/writeFile")
CODEX_BACKEND_PROVIDER_OPERATION(FuzzyFileSearch, FuzzyFileSearchResponse, filesystem, fuzzyFileSearch, Observer, true, "fuzzyFileSearch")

CODEX_BACKEND_PROVIDER_OPERATION(
    PermissionProfileList, PermissionProfileListResponse, permissionProfiles, list, Observer, true, "permissionProfile/list")
CODEX_BACKEND_PROVIDER_OPERATION(ReviewStart, ReviewStartResponse, reviews, start, Controller, true, "review/start")

CODEX_BACKEND_PROVIDER_OPERATION(AppsList, AppsListResponse, apps, list, Observer, true, "app/list")
CODEX_BACKEND_PROVIDER_OPERATION(
    ExternalAgentConfigDetect, ExternalAgentConfigDetectResponse, externalAgents, detect, Observer, true, "externalAgentConfig/detect")
CODEX_BACKEND_PROVIDER_OPERATION(ExternalAgentConfigImport,
                                 ExternalAgentConfigImportResponse,
                                 externalAgents,
                                 importConfiguration,
                                 Controller,
                                 true,
                                 "externalAgentConfig/import")
CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(ExternalAgentConfigImportHistoriesRead,
                                       ExternalAgentConfigImportHistoriesReadResponse,
                                       externalAgents,
                                       readImportHistories,
                                       Observer,
                                       true,
                                       "externalAgentConfig/import/readHistories")

CODEX_BACKEND_PROVIDER_OPERATION(FeedbackUpload, FeedbackUploadResponse, feedback, upload, Controller, false, "feedback/upload")
CODEX_BACKEND_PROVIDER_OPERATION(HooksList, HooksListResponse, hooks, list, Observer, true, "hooks/list")

CODEX_BACKEND_PROVIDER_OPERATION(MarketplaceAdd, MarketplaceAddResponse, marketplace, add, Controller, true, "marketplace/add")
CODEX_BACKEND_PROVIDER_OPERATION(MarketplaceRemove, MarketplaceRemoveResponse, marketplace, remove, Controller, true, "marketplace/remove")
CODEX_BACKEND_PROVIDER_OPERATION(
    MarketplaceUpgrade, MarketplaceUpgradeResponse, marketplace, upgrade, Controller, true, "marketplace/upgrade")

CODEX_BACKEND_PROVIDER_OPERATION(PluginInstall, PluginInstallResponse, plugins, install, Controller, true, "plugin/install")
CODEX_BACKEND_PROVIDER_OPERATION(PluginInstalled, PluginInstalledResponse, plugins, installed, Observer, true, "plugin/installed")
CODEX_BACKEND_PROVIDER_OPERATION(PluginList, PluginListResponse, plugins, list, Observer, true, "plugin/list")
CODEX_BACKEND_PROVIDER_OPERATION(PluginRead, PluginReadResponse, plugins, read, Observer, true, "plugin/read")
CODEX_BACKEND_PROVIDER_OPERATION(
    PluginShareCheckout, PluginShareCheckoutResponse, plugins, shareCheckout, Controller, true, "plugin/share/checkout")
CODEX_BACKEND_PROVIDER_OPERATION(PluginShareDelete, Unit, plugins, shareDelete, Controller, true, "plugin/share/delete")
CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(PluginShareList, PluginShareListResponse, plugins, shareList, Observer, true, "plugin/share/list")
CODEX_BACKEND_PROVIDER_OPERATION(PluginShareSave, PluginShareSaveResponse, plugins, shareSave, Controller, true, "plugin/share/save")
CODEX_BACKEND_PROVIDER_OPERATION(
    PluginShareUpdateTargets, PluginShareUpdateTargetsResponse, plugins, shareUpdateTargets, Controller, true, "plugin/share/updateTargets")
CODEX_BACKEND_PROVIDER_OPERATION(PluginSkillRead, PluginSkillReadResponse, plugins, readSkill, Observer, true, "plugin/skill/read")
CODEX_BACKEND_PROVIDER_OPERATION(PluginUninstall, Unit, plugins, uninstall, Controller, true, "plugin/uninstall")

CODEX_BACKEND_PROVIDER_OPERATION(SkillsConfigWrite, SkillsConfigWriteResponse, skills, writeConfig, Controller, true, "skills/config/write")
CODEX_BACKEND_PROVIDER_OPERATION(SkillsExtraRootsSet, Unit, skills, setExtraRoots, Controller, true, "skills/extraRoots/set")
CODEX_BACKEND_PROVIDER_OPERATION(SkillsList, SkillsListResponse, skills, list, Observer, true, "skills/list")

CODEX_BACKEND_PROVIDER_OPERATION(
    McpServerOauthLogin, McpServerOauthLoginResponse, mcp, startOauthLogin, Controller, true, "mcpServer/oauth/login")
CODEX_BACKEND_PROVIDER_OPERATION(McpResourceRead, McpResourceReadResponse, mcp, readResource, Observer, false, "mcpServer/resource/read")
CODEX_BACKEND_PROVIDER_OPERATION(McpServerToolCall, McpServerToolCallResponse, mcp, callTool, Controller, false, "mcpServer/tool/call")
CODEX_BACKEND_PROVIDER_OPERATION(McpServerStatusList, ListMcpServerStatusResponse, mcp, listServers, Observer, true, "mcpServerStatus/list")

CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(
    WindowsSandboxReadiness, WindowsSandboxReadinessResponse, windowsSandbox, checkReadiness, Observer, true, "windowsSandbox/readiness")
CODEX_BACKEND_PROVIDER_OPERATION(
    WindowsSandboxSetupStart, WindowsSandboxSetupStartResponse, windowsSandbox, startSetup, Controller, true, "windowsSandbox/setupStart")
