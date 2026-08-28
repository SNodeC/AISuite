/*
 * Generated from Codex app-server protocol exports. DO NOT EDIT.
 * Schema SHA-256: 48f025b407c0a96bffed6e06f0fe1c9e23e770039d9262688d6f7b708d1890fd
 * Protocol source SHA-256: a88f193d64f6c24364ee9d8c6895698c7f4d4b22c83468ac0488966b7f49f907
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

export const protocolGeneration = {
    schemaSha256: "48f025b407c0a96bffed6e06f0fe1c9e23e770039d9262688d6f7b708d1890fd",
    protocolSourceSha256: "a88f193d64f6c24364ee9d8c6895698c7f4d4b22c83468ac0488966b7f49f907",
    generatedTypes: 1920,
    canonicalRootTypes: 81,
    canonicalV2Types: 585,
} as const;

export type RootAbsolutePathBuf = string;

export type RootAdditionalPermissionProfile = {
    readonly "fileSystem"?: RootAdditionalPermissionProfileFileSystem;
    readonly "network"?: RootAdditionalPermissionProfileNetwork;
    readonly [key: string]: unknown;
};

export type RootAdditionalPermissionProfileFileSystem = V2AdditionalFileSystemPermissions | null;

export type RootAdditionalPermissionProfileNetwork = V2AdditionalNetworkPermissions | null;

export type RootApplyPatchApprovalParams = {
    readonly "callId": string;
    readonly "conversationId": V2ThreadId;
    readonly "fileChanges": RootApplyPatchApprovalParamsFileChanges;
    readonly "grantRoot"?: string | null;
    readonly "reason"?: string | null;
    readonly [key: string]: unknown;
};

export type RootApplyPatchApprovalParamsFileChanges = {
    readonly [key: string]: unknown;
};

export type RootApplyPatchApprovalResponse = {
    readonly "decision": RootReviewDecision;
    readonly [key: string]: unknown;
};

export type RootAttestationGenerateParams = {
    readonly [key: string]: unknown;
};

export type RootAttestationGenerateResponse = {
    readonly "token": string;
    readonly [key: string]: unknown;
};

export type RootChatgptAuthTokensRefreshParams = {
    readonly "previousAccountId"?: string | null;
    readonly "reason": RootChatgptAuthTokensRefreshReason;
    readonly [key: string]: unknown;
};

export type RootChatgptAuthTokensRefreshReason = RootChatgptAuthTokensRefreshReasonOneOf1;

export type RootChatgptAuthTokensRefreshReasonOneOf1 = "unauthorized";

export type RootChatgptAuthTokensRefreshResponse = {
    readonly "accessToken": string;
    readonly "chatgptAccountId": string;
    readonly "chatgptPlanType"?: string | null;
    readonly [key: string]: unknown;
};

export type RootClientInfo = {
    readonly "name": string;
    readonly "title"?: string | null;
    readonly "version": string;
    readonly [key: string]: unknown;
};

export type RootClientNotification = RootInitializedNotification;

export type RootInitializedNotification = {
    readonly "method": RootInitializedNotificationMethod;
    readonly [key: string]: unknown;
};

export type RootInitializedNotificationMethod = "initialized";

export type RootClientRequest = RootInitializeRequest | RootThreadStartRequest | RootThreadResumeRequest | RootThreadForkRequest | RootThreadArchiveRequest | RootThreadDeleteRequest | RootThreadUnsubscribeRequest | RootThreadNameSetRequest | RootThreadGoalSetRequest | RootThreadGoalGetRequest | RootThreadGoalClearRequest | RootThreadMetadataUpdateRequest | RootThreadSectionMoveRequest | RootThreadUnarchiveRequest | RootThreadCompactStartRequest | RootThreadShellCommandRequest | RootThreadApproveGuardianDeniedActionRequest | RootThreadRollbackRequest | RootThreadListRequest | RootThreadSectionListRequest | RootThreadSectionCreateRequest | RootThreadSectionUpdateRequest | RootThreadSectionDeleteRequest | RootThreadLoadedListRequest | RootThreadReadRequest | RootThreadInjectItemsRequest | RootSkillsListRequest | RootSkillsExtraRootsSetRequest | RootHooksListRequest | RootMarketplaceAddRequest | RootMarketplaceRemoveRequest | RootMarketplaceUpgradeRequest | RootPluginListRequest | RootPluginInstalledRequest | RootPluginReadRequest | RootPluginSkillReadRequest | RootPluginShareSaveRequest | RootPluginShareUpdateTargetsRequest | RootPluginShareListRequest | RootPluginShareCheckoutRequest | RootPluginShareDeleteRequest | RootAppReadRequest | RootAppListRequest | RootAppInstalledRequest | RootFsReadFileRequest | RootFsWriteFileRequest | RootFsCreateDirectoryRequest | RootFsGetMetadataRequest | RootFsReadDirectoryRequest | RootFsRemoveRequest | RootFsCopyRequest | RootFsWatchRequest | RootFsUnwatchRequest | RootSkillsConfigWriteRequest | RootPluginInstallRequest | RootPluginUninstallRequest | RootTurnStartRequest | RootTurnSteerRequest | RootTurnInterruptRequest | RootReviewStartRequest | RootModelListRequest | RootModelProviderCapabilitiesReadRequest | RootExperimentalFeatureListRequest | RootPermissionProfileListRequest | RootExperimentalFeatureEnablementSetRequest | RootMcpServerOauthLoginRequest | RootConfigMcpServerReloadRequest | RootMcpServerStatusListRequest | RootMcpServerResourceReadRequest | RootMcpServerToolCallRequest | RootWindowsSandboxSetupStartRequest | RootWindowsSandboxReadinessRequest | RootAccountLoginStartRequest | RootAccountLoginCancelRequest | RootAccountLogoutRequest | RootAccountRateLimitsReadRequest | RootAccountRateLimitResetCreditConsumeRequest | RootAccountUsageReadRequest | RootAccountWorkspaceMessagesReadRequest | RootAccountSendAddCreditsNudgeEmailRequest | RootFeedbackUploadRequest | RootCommandExecRequest | RootCommandExecWriteRequest | RootCommandExecTerminateRequest | RootCommandExecResizeRequest | RootConfigReadRequest | RootExternalAgentConfigDetectRequest | RootExternalAgentConfigImportRequest | RootExternalAgentConfigImportRecordHistoryRequest | RootExternalAgentConfigImportReadHistoriesRequest | RootConfigValueWriteRequest | RootConfigBatchWriteRequest | RootConfigRequirementsReadRequest | RootAccountReadRequest | RootFuzzyFileSearchRequest;

export type RootInitializeRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootInitializeRequestMethod;
    readonly "params": RootInitializeParams;
    readonly [key: string]: unknown;
};

export type RootInitializeRequestMethod = "initialize";

export type RootThreadStartRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadStartRequestMethod;
    readonly "params": V2ThreadStartParams;
    readonly [key: string]: unknown;
};

export type RootThreadStartRequestMethod = "thread/start";

export type RootThreadResumeRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadResumeRequestMethod;
    readonly "params": V2ThreadResumeParams;
    readonly [key: string]: unknown;
};

export type RootThreadResumeRequestMethod = "thread/resume";

export type RootThreadForkRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadForkRequestMethod;
    readonly "params": V2ThreadForkParams;
    readonly [key: string]: unknown;
};

export type RootThreadForkRequestMethod = "thread/fork";

export type RootThreadArchiveRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadArchiveRequestMethod;
    readonly "params": V2ThreadArchiveParams;
    readonly [key: string]: unknown;
};

export type RootThreadArchiveRequestMethod = "thread/archive";

export type RootThreadDeleteRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadDeleteRequestMethod;
    readonly "params": V2ThreadDeleteParams;
    readonly [key: string]: unknown;
};

export type RootThreadDeleteRequestMethod = "thread/delete";

export type RootThreadUnsubscribeRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadUnsubscribeRequestMethod;
    readonly "params": V2ThreadUnsubscribeParams;
    readonly [key: string]: unknown;
};

export type RootThreadUnsubscribeRequestMethod = "thread/unsubscribe";

export type RootThreadNameSetRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadNameSetRequestMethod;
    readonly "params": V2ThreadSetNameParams;
    readonly [key: string]: unknown;
};

export type RootThreadNameSetRequestMethod = "thread/name/set";

export type RootThreadGoalSetRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadGoalSetRequestMethod;
    readonly "params": V2ThreadGoalSetParams;
    readonly [key: string]: unknown;
};

export type RootThreadGoalSetRequestMethod = "thread/goal/set";

export type RootThreadGoalGetRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadGoalGetRequestMethod;
    readonly "params": V2ThreadGoalGetParams;
    readonly [key: string]: unknown;
};

export type RootThreadGoalGetRequestMethod = "thread/goal/get";

export type RootThreadGoalClearRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadGoalClearRequestMethod;
    readonly "params": V2ThreadGoalClearParams;
    readonly [key: string]: unknown;
};

export type RootThreadGoalClearRequestMethod = "thread/goal/clear";

export type RootThreadMetadataUpdateRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadMetadataUpdateRequestMethod;
    readonly "params": V2ThreadMetadataUpdateParams;
    readonly [key: string]: unknown;
};

export type RootThreadMetadataUpdateRequestMethod = "thread/metadata/update";

export type RootThreadSectionMoveRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadSectionMoveRequestMethod;
    readonly "params": V2ThreadSectionMoveParams;
    readonly [key: string]: unknown;
};

export type RootThreadSectionMoveRequestMethod = "thread/section/move";

export type RootThreadUnarchiveRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadUnarchiveRequestMethod;
    readonly "params": V2ThreadUnarchiveParams;
    readonly [key: string]: unknown;
};

export type RootThreadUnarchiveRequestMethod = "thread/unarchive";

export type RootThreadCompactStartRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadCompactStartRequestMethod;
    readonly "params": V2ThreadCompactStartParams;
    readonly [key: string]: unknown;
};

export type RootThreadCompactStartRequestMethod = "thread/compact/start";

export type RootThreadShellCommandRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadShellCommandRequestMethod;
    readonly "params": V2ThreadShellCommandParams;
    readonly [key: string]: unknown;
};

export type RootThreadShellCommandRequestMethod = "thread/shellCommand";

export type RootThreadApproveGuardianDeniedActionRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadApproveGuardianDeniedActionRequestMethod;
    readonly "params": V2ThreadApproveGuardianDeniedActionParams;
    readonly [key: string]: unknown;
};

export type RootThreadApproveGuardianDeniedActionRequestMethod = "thread/approveGuardianDeniedAction";

export type RootThreadRollbackRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadRollbackRequestMethod;
    readonly "params": V2ThreadRollbackParams;
    readonly [key: string]: unknown;
};

export type RootThreadRollbackRequestMethod = "thread/rollback";

export type RootThreadListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadListRequestMethod;
    readonly "params": V2ThreadListParams;
    readonly [key: string]: unknown;
};

export type RootThreadListRequestMethod = "thread/list";

export type RootThreadSectionListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadSectionListRequestMethod;
    readonly "params": V2ThreadSectionListParams;
    readonly [key: string]: unknown;
};

export type RootThreadSectionListRequestMethod = "threadSection/list";

export type RootThreadSectionCreateRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadSectionCreateRequestMethod;
    readonly "params": V2ThreadSectionCreateParams;
    readonly [key: string]: unknown;
};

export type RootThreadSectionCreateRequestMethod = "threadSection/create";

export type RootThreadSectionUpdateRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadSectionUpdateRequestMethod;
    readonly "params": V2ThreadSectionUpdateParams;
    readonly [key: string]: unknown;
};

export type RootThreadSectionUpdateRequestMethod = "threadSection/update";

export type RootThreadSectionDeleteRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadSectionDeleteRequestMethod;
    readonly "params": V2ThreadSectionDeleteParams;
    readonly [key: string]: unknown;
};

export type RootThreadSectionDeleteRequestMethod = "threadSection/delete";

export type RootThreadLoadedListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadLoadedListRequestMethod;
    readonly "params": V2ThreadLoadedListParams;
    readonly [key: string]: unknown;
};

export type RootThreadLoadedListRequestMethod = "thread/loaded/list";

export type RootThreadReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadReadRequestMethod;
    readonly "params": V2ThreadReadParams;
    readonly [key: string]: unknown;
};

export type RootThreadReadRequestMethod = "thread/read";

export type RootThreadInjectItemsRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootThreadInjectItemsRequestMethod;
    readonly "params": V2ThreadInjectItemsParams;
    readonly [key: string]: unknown;
};

export type RootThreadInjectItemsRequestMethod = "thread/inject_items";

export type RootSkillsListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootSkillsListRequestMethod;
    readonly "params": V2SkillsListParams;
    readonly [key: string]: unknown;
};

export type RootSkillsListRequestMethod = "skills/list";

export type RootSkillsExtraRootsSetRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootSkillsExtraRootsSetRequestMethod;
    readonly "params": V2SkillsExtraRootsSetParams;
    readonly [key: string]: unknown;
};

export type RootSkillsExtraRootsSetRequestMethod = "skills/extraRoots/set";

export type RootHooksListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootHooksListRequestMethod;
    readonly "params": V2HooksListParams;
    readonly [key: string]: unknown;
};

export type RootHooksListRequestMethod = "hooks/list";

export type RootMarketplaceAddRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootMarketplaceAddRequestMethod;
    readonly "params": V2MarketplaceAddParams;
    readonly [key: string]: unknown;
};

export type RootMarketplaceAddRequestMethod = "marketplace/add";

export type RootMarketplaceRemoveRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootMarketplaceRemoveRequestMethod;
    readonly "params": V2MarketplaceRemoveParams;
    readonly [key: string]: unknown;
};

export type RootMarketplaceRemoveRequestMethod = "marketplace/remove";

export type RootMarketplaceUpgradeRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootMarketplaceUpgradeRequestMethod;
    readonly "params": V2MarketplaceUpgradeParams;
    readonly [key: string]: unknown;
};

export type RootMarketplaceUpgradeRequestMethod = "marketplace/upgrade";

export type RootPluginListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginListRequestMethod;
    readonly "params": V2PluginListParams;
    readonly [key: string]: unknown;
};

export type RootPluginListRequestMethod = "plugin/list";

export type RootPluginInstalledRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginInstalledRequestMethod;
    readonly "params": V2PluginInstalledParams;
    readonly [key: string]: unknown;
};

export type RootPluginInstalledRequestMethod = "plugin/installed";

export type RootPluginReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginReadRequestMethod;
    readonly "params": V2PluginReadParams;
    readonly [key: string]: unknown;
};

export type RootPluginReadRequestMethod = "plugin/read";

export type RootPluginSkillReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginSkillReadRequestMethod;
    readonly "params": V2PluginSkillReadParams;
    readonly [key: string]: unknown;
};

export type RootPluginSkillReadRequestMethod = "plugin/skill/read";

export type RootPluginShareSaveRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginShareSaveRequestMethod;
    readonly "params": V2PluginShareSaveParams;
    readonly [key: string]: unknown;
};

export type RootPluginShareSaveRequestMethod = "plugin/share/save";

export type RootPluginShareUpdateTargetsRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginShareUpdateTargetsRequestMethod;
    readonly "params": V2PluginShareUpdateTargetsParams;
    readonly [key: string]: unknown;
};

export type RootPluginShareUpdateTargetsRequestMethod = "plugin/share/updateTargets";

export type RootPluginShareListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginShareListRequestMethod;
    readonly "params": V2PluginShareListParams;
    readonly [key: string]: unknown;
};

export type RootPluginShareListRequestMethod = "plugin/share/list";

export type RootPluginShareCheckoutRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginShareCheckoutRequestMethod;
    readonly "params": V2PluginShareCheckoutParams;
    readonly [key: string]: unknown;
};

export type RootPluginShareCheckoutRequestMethod = "plugin/share/checkout";

export type RootPluginShareDeleteRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginShareDeleteRequestMethod;
    readonly "params": V2PluginShareDeleteParams;
    readonly [key: string]: unknown;
};

export type RootPluginShareDeleteRequestMethod = "plugin/share/delete";

export type RootAppReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAppReadRequestMethod;
    readonly "params": V2AppsReadParams;
    readonly [key: string]: unknown;
};

export type RootAppReadRequestMethod = "app/read";

export type RootAppListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAppListRequestMethod;
    readonly "params": V2AppsListParams;
    readonly [key: string]: unknown;
};

export type RootAppListRequestMethod = "app/list";

export type RootAppInstalledRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAppInstalledRequestMethod;
    readonly "params": V2AppsInstalledParams;
    readonly [key: string]: unknown;
};

export type RootAppInstalledRequestMethod = "app/installed";

export type RootFsReadFileRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFsReadFileRequestMethod;
    readonly "params": V2FsReadFileParams;
    readonly [key: string]: unknown;
};

export type RootFsReadFileRequestMethod = "fs/readFile";

export type RootFsWriteFileRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFsWriteFileRequestMethod;
    readonly "params": V2FsWriteFileParams;
    readonly [key: string]: unknown;
};

export type RootFsWriteFileRequestMethod = "fs/writeFile";

export type RootFsCreateDirectoryRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFsCreateDirectoryRequestMethod;
    readonly "params": V2FsCreateDirectoryParams;
    readonly [key: string]: unknown;
};

export type RootFsCreateDirectoryRequestMethod = "fs/createDirectory";

export type RootFsGetMetadataRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFsGetMetadataRequestMethod;
    readonly "params": V2FsGetMetadataParams;
    readonly [key: string]: unknown;
};

export type RootFsGetMetadataRequestMethod = "fs/getMetadata";

export type RootFsReadDirectoryRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFsReadDirectoryRequestMethod;
    readonly "params": V2FsReadDirectoryParams;
    readonly [key: string]: unknown;
};

export type RootFsReadDirectoryRequestMethod = "fs/readDirectory";

export type RootFsRemoveRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFsRemoveRequestMethod;
    readonly "params": V2FsRemoveParams;
    readonly [key: string]: unknown;
};

export type RootFsRemoveRequestMethod = "fs/remove";

export type RootFsCopyRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFsCopyRequestMethod;
    readonly "params": V2FsCopyParams;
    readonly [key: string]: unknown;
};

export type RootFsCopyRequestMethod = "fs/copy";

export type RootFsWatchRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFsWatchRequestMethod;
    readonly "params": V2FsWatchParams;
    readonly [key: string]: unknown;
};

export type RootFsWatchRequestMethod = "fs/watch";

export type RootFsUnwatchRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFsUnwatchRequestMethod;
    readonly "params": V2FsUnwatchParams;
    readonly [key: string]: unknown;
};

export type RootFsUnwatchRequestMethod = "fs/unwatch";

export type RootSkillsConfigWriteRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootSkillsConfigWriteRequestMethod;
    readonly "params": V2SkillsConfigWriteParams;
    readonly [key: string]: unknown;
};

export type RootSkillsConfigWriteRequestMethod = "skills/config/write";

export type RootPluginInstallRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginInstallRequestMethod;
    readonly "params": V2PluginInstallParams;
    readonly [key: string]: unknown;
};

export type RootPluginInstallRequestMethod = "plugin/install";

export type RootPluginUninstallRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPluginUninstallRequestMethod;
    readonly "params": V2PluginUninstallParams;
    readonly [key: string]: unknown;
};

export type RootPluginUninstallRequestMethod = "plugin/uninstall";

export type RootTurnStartRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootTurnStartRequestMethod;
    readonly "params": V2TurnStartParams;
    readonly [key: string]: unknown;
};

export type RootTurnStartRequestMethod = "turn/start";

export type RootTurnSteerRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootTurnSteerRequestMethod;
    readonly "params": V2TurnSteerParams;
    readonly [key: string]: unknown;
};

export type RootTurnSteerRequestMethod = "turn/steer";

export type RootTurnInterruptRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootTurnInterruptRequestMethod;
    readonly "params": V2TurnInterruptParams;
    readonly [key: string]: unknown;
};

export type RootTurnInterruptRequestMethod = "turn/interrupt";

export type RootReviewStartRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootReviewStartRequestMethod;
    readonly "params": V2ReviewStartParams;
    readonly [key: string]: unknown;
};

export type RootReviewStartRequestMethod = "review/start";

export type RootModelListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootModelListRequestMethod;
    readonly "params": V2ModelListParams;
    readonly [key: string]: unknown;
};

export type RootModelListRequestMethod = "model/list";

export type RootModelProviderCapabilitiesReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootModelProviderCapabilitiesReadRequestMethod;
    readonly "params": V2ModelProviderCapabilitiesReadParams;
    readonly [key: string]: unknown;
};

export type RootModelProviderCapabilitiesReadRequestMethod = "modelProvider/capabilities/read";

export type RootExperimentalFeatureListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootExperimentalFeatureListRequestMethod;
    readonly "params": V2ExperimentalFeatureListParams;
    readonly [key: string]: unknown;
};

export type RootExperimentalFeatureListRequestMethod = "experimentalFeature/list";

export type RootPermissionProfileListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootPermissionProfileListRequestMethod;
    readonly "params": V2PermissionProfileListParams;
    readonly [key: string]: unknown;
};

export type RootPermissionProfileListRequestMethod = "permissionProfile/list";

export type RootExperimentalFeatureEnablementSetRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootExperimentalFeatureEnablementSetRequestMethod;
    readonly "params": V2ExperimentalFeatureEnablementSetParams;
    readonly [key: string]: unknown;
};

export type RootExperimentalFeatureEnablementSetRequestMethod = "experimentalFeature/enablement/set";

export type RootMcpServerOauthLoginRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootMcpServerOauthLoginRequestMethod;
    readonly "params": V2McpServerOauthLoginParams;
    readonly [key: string]: unknown;
};

export type RootMcpServerOauthLoginRequestMethod = "mcpServer/oauth/login";

export type RootConfigMcpServerReloadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootConfigMcpServerReloadRequestMethod;
    readonly "params"?: null;
    readonly [key: string]: unknown;
};

export type RootConfigMcpServerReloadRequestMethod = "config/mcpServer/reload";

export type RootMcpServerStatusListRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootMcpServerStatusListRequestMethod;
    readonly "params": V2ListMcpServerStatusParams;
    readonly [key: string]: unknown;
};

export type RootMcpServerStatusListRequestMethod = "mcpServerStatus/list";

export type RootMcpServerResourceReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootMcpServerResourceReadRequestMethod;
    readonly "params": V2McpResourceReadParams;
    readonly [key: string]: unknown;
};

export type RootMcpServerResourceReadRequestMethod = "mcpServer/resource/read";

export type RootMcpServerToolCallRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootMcpServerToolCallRequestMethod;
    readonly "params": V2McpServerToolCallParams;
    readonly [key: string]: unknown;
};

export type RootMcpServerToolCallRequestMethod = "mcpServer/tool/call";

export type RootWindowsSandboxSetupStartRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootWindowsSandboxSetupStartRequestMethod;
    readonly "params": V2WindowsSandboxSetupStartParams;
    readonly [key: string]: unknown;
};

export type RootWindowsSandboxSetupStartRequestMethod = "windowsSandbox/setupStart";

export type RootWindowsSandboxReadinessRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootWindowsSandboxReadinessRequestMethod;
    readonly "params"?: null;
    readonly [key: string]: unknown;
};

export type RootWindowsSandboxReadinessRequestMethod = "windowsSandbox/readiness";

export type RootAccountLoginStartRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountLoginStartRequestMethod;
    readonly "params": V2LoginAccountParams;
    readonly [key: string]: unknown;
};

export type RootAccountLoginStartRequestMethod = "account/login/start";

export type RootAccountLoginCancelRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountLoginCancelRequestMethod;
    readonly "params": V2CancelLoginAccountParams;
    readonly [key: string]: unknown;
};

export type RootAccountLoginCancelRequestMethod = "account/login/cancel";

export type RootAccountLogoutRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountLogoutRequestMethod;
    readonly "params"?: null;
    readonly [key: string]: unknown;
};

export type RootAccountLogoutRequestMethod = "account/logout";

export type RootAccountRateLimitsReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountRateLimitsReadRequestMethod;
    readonly "params"?: null;
    readonly [key: string]: unknown;
};

export type RootAccountRateLimitsReadRequestMethod = "account/rateLimits/read";

export type RootAccountRateLimitResetCreditConsumeRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountRateLimitResetCreditConsumeRequestMethod;
    readonly "params": V2ConsumeAccountRateLimitResetCreditParams;
    readonly [key: string]: unknown;
};

export type RootAccountRateLimitResetCreditConsumeRequestMethod = "account/rateLimitResetCredit/consume";

export type RootAccountUsageReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountUsageReadRequestMethod;
    readonly "params"?: RootAccountUsageReadRequestParams;
    readonly [key: string]: unknown;
};

export type RootAccountUsageReadRequestMethod = "account/usage/read";

export type RootAccountUsageReadRequestParams = V2GetAccountTokenUsageParams | null;

export type RootAccountWorkspaceMessagesReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountWorkspaceMessagesReadRequestMethod;
    readonly "params"?: null;
    readonly [key: string]: unknown;
};

export type RootAccountWorkspaceMessagesReadRequestMethod = "account/workspaceMessages/read";

export type RootAccountSendAddCreditsNudgeEmailRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountSendAddCreditsNudgeEmailRequestMethod;
    readonly "params": V2SendAddCreditsNudgeEmailParams;
    readonly [key: string]: unknown;
};

export type RootAccountSendAddCreditsNudgeEmailRequestMethod = "account/sendAddCreditsNudgeEmail";

export type RootFeedbackUploadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFeedbackUploadRequestMethod;
    readonly "params": V2FeedbackUploadParams;
    readonly [key: string]: unknown;
};

export type RootFeedbackUploadRequestMethod = "feedback/upload";

export type RootCommandExecRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootCommandExecRequestMethod;
    readonly "params": V2CommandExecParams;
    readonly [key: string]: unknown;
};

export type RootCommandExecRequestMethod = "command/exec";

export type RootCommandExecWriteRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootCommandExecWriteRequestMethod;
    readonly "params": V2CommandExecWriteParams;
    readonly [key: string]: unknown;
};

export type RootCommandExecWriteRequestMethod = "command/exec/write";

export type RootCommandExecTerminateRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootCommandExecTerminateRequestMethod;
    readonly "params": V2CommandExecTerminateParams;
    readonly [key: string]: unknown;
};

export type RootCommandExecTerminateRequestMethod = "command/exec/terminate";

export type RootCommandExecResizeRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootCommandExecResizeRequestMethod;
    readonly "params": V2CommandExecResizeParams;
    readonly [key: string]: unknown;
};

export type RootCommandExecResizeRequestMethod = "command/exec/resize";

export type RootConfigReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootConfigReadRequestMethod;
    readonly "params": V2ConfigReadParams;
    readonly [key: string]: unknown;
};

export type RootConfigReadRequestMethod = "config/read";

export type RootExternalAgentConfigDetectRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootExternalAgentConfigDetectRequestMethod;
    readonly "params": V2ExternalAgentConfigDetectParams;
    readonly [key: string]: unknown;
};

export type RootExternalAgentConfigDetectRequestMethod = "externalAgentConfig/detect";

export type RootExternalAgentConfigImportRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootExternalAgentConfigImportRequestMethod;
    readonly "params": V2ExternalAgentConfigImportParams;
    readonly [key: string]: unknown;
};

export type RootExternalAgentConfigImportRequestMethod = "externalAgentConfig/import";

export type RootExternalAgentConfigImportRecordHistoryRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootExternalAgentConfigImportRecordHistoryRequestMethod;
    readonly "params": V2ExternalAgentConfigImportHistoryRecordParams;
    readonly [key: string]: unknown;
};

export type RootExternalAgentConfigImportRecordHistoryRequestMethod = "externalAgentConfig/import/recordHistory";

export type RootExternalAgentConfigImportReadHistoriesRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootExternalAgentConfigImportReadHistoriesRequestMethod;
    readonly "params"?: null;
    readonly [key: string]: unknown;
};

export type RootExternalAgentConfigImportReadHistoriesRequestMethod = "externalAgentConfig/import/readHistories";

export type RootConfigValueWriteRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootConfigValueWriteRequestMethod;
    readonly "params": V2ConfigValueWriteParams;
    readonly [key: string]: unknown;
};

export type RootConfigValueWriteRequestMethod = "config/value/write";

export type RootConfigBatchWriteRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootConfigBatchWriteRequestMethod;
    readonly "params": V2ConfigBatchWriteParams;
    readonly [key: string]: unknown;
};

export type RootConfigBatchWriteRequestMethod = "config/batchWrite";

export type RootConfigRequirementsReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootConfigRequirementsReadRequestMethod;
    readonly "params"?: null;
    readonly [key: string]: unknown;
};

export type RootConfigRequirementsReadRequestMethod = "configRequirements/read";

export type RootAccountReadRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountReadRequestMethod;
    readonly "params": V2GetAccountParams;
    readonly [key: string]: unknown;
};

export type RootAccountReadRequestMethod = "account/read";

export type RootFuzzyFileSearchRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootFuzzyFileSearchRequestMethod;
    readonly "params": RootFuzzyFileSearchParams;
    readonly [key: string]: unknown;
};

export type RootFuzzyFileSearchRequestMethod = "fuzzyFileSearch";

export type RootCommandExecutionApprovalDecision = RootCommandExecutionApprovalDecisionOneOf1 | RootCommandExecutionApprovalDecisionOneOf2 | RootAcceptWithExecpolicyAmendmentCommandExecutionApprovalDecision | RootApplyNetworkPolicyAmendmentCommandExecutionApprovalDecision | RootCommandExecutionApprovalDecisionOneOf5 | RootCommandExecutionApprovalDecisionOneOf6;

export type RootCommandExecutionApprovalDecisionOneOf1 = "accept";

export type RootCommandExecutionApprovalDecisionOneOf2 = "acceptForSession";

export type RootAcceptWithExecpolicyAmendmentCommandExecutionApprovalDecision = {
    readonly "acceptWithExecpolicyAmendment": RootAcceptWithExecpolicyAmendmentCommandExecutionApprovalDecisionAcceptWithExecpolicyAmendment;
};

export type RootAcceptWithExecpolicyAmendmentCommandExecutionApprovalDecisionAcceptWithExecpolicyAmendment = {
    readonly "execpolicy_amendment": RootAcceptWithExecpolicyAmendmentCommandExecutionApprovalDecisionAcceptWithExecpolicyAmendmentExecpolicyAmendment;
    readonly [key: string]: unknown;
};

export type RootAcceptWithExecpolicyAmendmentCommandExecutionApprovalDecisionAcceptWithExecpolicyAmendmentExecpolicyAmendment = ReadonlyArray<string>;

export type RootApplyNetworkPolicyAmendmentCommandExecutionApprovalDecision = {
    readonly "applyNetworkPolicyAmendment": RootApplyNetworkPolicyAmendmentCommandExecutionApprovalDecisionApplyNetworkPolicyAmendment;
};

export type RootApplyNetworkPolicyAmendmentCommandExecutionApprovalDecisionApplyNetworkPolicyAmendment = {
    readonly "network_policy_amendment": RootNetworkPolicyAmendment;
    readonly [key: string]: unknown;
};

export type RootCommandExecutionApprovalDecisionOneOf5 = "decline";

export type RootCommandExecutionApprovalDecisionOneOf6 = "cancel";

export type RootCommandExecutionRequestApprovalParams = {
    readonly "approvalId"?: string | null;
    readonly "command"?: string | null;
    readonly "commandActions"?: RootCommandExecutionRequestApprovalParamsCommandActions;
    readonly "cwd"?: RootCommandExecutionRequestApprovalParamsCwd;
    readonly "environmentId"?: string | null;
    readonly "itemId": string;
    readonly "networkApprovalContext"?: RootCommandExecutionRequestApprovalParamsNetworkApprovalContext;
    readonly "proposedExecpolicyAmendment"?: RootCommandExecutionRequestApprovalParamsProposedExecpolicyAmendment;
    readonly "proposedNetworkPolicyAmendments"?: RootCommandExecutionRequestApprovalParamsProposedNetworkPolicyAmendments;
    readonly "reason"?: string | null;
    readonly "startedAtMs": number;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type RootCommandExecutionRequestApprovalParamsCommandActions = ReadonlyArray<V2CommandAction> | null;

export type RootCommandExecutionRequestApprovalParamsCwd = V2LegacyAppPathString | null;

export type RootCommandExecutionRequestApprovalParamsNetworkApprovalContext = RootNetworkApprovalContext | null;

export type RootCommandExecutionRequestApprovalParamsProposedExecpolicyAmendment = ReadonlyArray<string> | null;

export type RootCommandExecutionRequestApprovalParamsProposedNetworkPolicyAmendments = ReadonlyArray<RootNetworkPolicyAmendment> | null;

export type RootCommandExecutionRequestApprovalResponse = {
    readonly "decision": RootCommandExecutionApprovalDecision;
    readonly [key: string]: unknown;
};

export type RootDynamicToolCallParams = {
    readonly "arguments": unknown;
    readonly "callId": string;
    readonly "namespace"?: string | null;
    readonly "threadId": string;
    readonly "tool": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type RootDynamicToolCallResponse = {
    readonly "contentItems": RootDynamicToolCallResponseContentItems;
    readonly "success": boolean;
    readonly [key: string]: unknown;
};

export type RootDynamicToolCallResponseContentItems = ReadonlyArray<V2DynamicToolCallOutputContentItem>;

export type RootExecCommandApprovalParams = {
    readonly "approvalId"?: string | null;
    readonly "callId": string;
    readonly "command": RootExecCommandApprovalParamsCommand;
    readonly "conversationId": V2ThreadId;
    readonly "cwd": string;
    readonly "parsedCmd": RootExecCommandApprovalParamsParsedCmd;
    readonly "reason"?: string | null;
    readonly [key: string]: unknown;
};

export type RootExecCommandApprovalParamsCommand = ReadonlyArray<string>;

export type RootExecCommandApprovalParamsParsedCmd = ReadonlyArray<RootParsedCommand>;

export type RootExecCommandApprovalResponse = {
    readonly "decision": RootReviewDecision;
    readonly [key: string]: unknown;
};

export type RootFileChange = RootAddFileChange | RootDeleteFileChange | RootUpdateFileChange;

export type RootAddFileChange = {
    readonly "content": string;
    readonly "type": RootAddFileChangeType;
    readonly [key: string]: unknown;
};

export type RootAddFileChangeType = "add";

export type RootDeleteFileChange = {
    readonly "content": string;
    readonly "type": RootDeleteFileChangeType;
    readonly [key: string]: unknown;
};

export type RootDeleteFileChangeType = "delete";

export type RootUpdateFileChange = {
    readonly "move_path"?: string | null;
    readonly "type": RootUpdateFileChangeType;
    readonly "unified_diff": string;
    readonly [key: string]: unknown;
};

export type RootUpdateFileChangeType = "update";

export type RootFileChangeApprovalDecision = RootFileChangeApprovalDecisionOneOf1 | RootFileChangeApprovalDecisionOneOf2 | RootFileChangeApprovalDecisionOneOf3 | RootFileChangeApprovalDecisionOneOf4;

export type RootFileChangeApprovalDecisionOneOf1 = "accept";

export type RootFileChangeApprovalDecisionOneOf2 = "acceptForSession";

export type RootFileChangeApprovalDecisionOneOf3 = "decline";

export type RootFileChangeApprovalDecisionOneOf4 = "cancel";

export type RootFileChangeRequestApprovalParams = {
    readonly "grantRoot"?: string | null;
    readonly "itemId": string;
    readonly "reason"?: string | null;
    readonly "startedAtMs": number;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type RootFileChangeRequestApprovalResponse = {
    readonly "decision": RootFileChangeApprovalDecision;
    readonly [key: string]: unknown;
};

export type RootFuzzyFileSearchMatchType = "file" | "directory";

export type RootFuzzyFileSearchParams = {
    readonly "cancellationToken"?: string | null;
    readonly "query": string;
    readonly "roots": RootFuzzyFileSearchParamsRoots;
    readonly [key: string]: unknown;
};

export type RootFuzzyFileSearchParamsRoots = ReadonlyArray<string>;

export type RootFuzzyFileSearchResponse = {
    readonly "files": RootFuzzyFileSearchResponseFiles;
    readonly [key: string]: unknown;
};

export type RootFuzzyFileSearchResponseFiles = ReadonlyArray<RootFuzzyFileSearchResult>;

export type RootFuzzyFileSearchResult = {
    readonly "file_name": string;
    readonly "indices"?: RootFuzzyFileSearchResultIndices;
    readonly "match_type": RootFuzzyFileSearchMatchType;
    readonly "path": string;
    readonly "root": string;
    readonly "score": number;
    readonly [key: string]: unknown;
};

export type RootFuzzyFileSearchResultIndices = ReadonlyArray<number> | null;

export type RootFuzzyFileSearchSessionCompletedNotification = {
    readonly "sessionId": string;
    readonly [key: string]: unknown;
};

export type RootFuzzyFileSearchSessionUpdatedNotification = {
    readonly "files": RootFuzzyFileSearchSessionUpdatedNotificationFiles;
    readonly "query": string;
    readonly "sessionId": string;
    readonly [key: string]: unknown;
};

export type RootFuzzyFileSearchSessionUpdatedNotificationFiles = ReadonlyArray<RootFuzzyFileSearchResult>;

export type RootGrantedPermissionProfile = {
    readonly "fileSystem"?: RootGrantedPermissionProfileFileSystem;
    readonly "network"?: RootGrantedPermissionProfileNetwork;
    readonly [key: string]: unknown;
};

export type RootGrantedPermissionProfileFileSystem = V2AdditionalFileSystemPermissions | null;

export type RootGrantedPermissionProfileNetwork = V2AdditionalNetworkPermissions | null;

export type RootInitializeCapabilities = {
    readonly "experimentalApi"?: boolean;
    readonly "extensions"?: RootInitializeCapabilitiesExtensions;
    readonly "mcpServerOpenaiFormElicitation"?: boolean;
    readonly "optOutNotificationMethods"?: RootInitializeCapabilitiesOptOutNotificationMethods;
    readonly "requestAttestation"?: boolean;
    readonly [key: string]: unknown;
};

export type RootInitializeCapabilitiesExtensions = {
    readonly [key: string]: unknown;
} | null;

export type RootInitializeCapabilitiesOptOutNotificationMethods = ReadonlyArray<string> | null;

export type RootInitializeParams = {
    readonly "capabilities"?: RootInitializeParamsCapabilities;
    readonly "clientInfo": RootClientInfo;
    readonly [key: string]: unknown;
};

export type RootInitializeParamsCapabilities = RootInitializeCapabilities | null;

export type RootInitializeResponse = {
    readonly "codexHome": RootInitializeResponseCodexHome;
    readonly "platformFamily": string;
    readonly "platformOs": string;
    readonly "userAgent": string;
    readonly [key: string]: unknown;
};

export type RootInitializeResponseCodexHome = V2AbsolutePathBuf;

export type RootJSONRPCError = {
    readonly "error": RootJSONRPCErrorError;
    readonly "id": V2RequestId;
    readonly [key: string]: unknown;
};

export type RootJSONRPCErrorError = {
    readonly "code": number;
    readonly "data"?: unknown;
    readonly "message": string;
    readonly [key: string]: unknown;
};

export type RootJSONRPCMessage = RootJSONRPCRequest | RootJSONRPCNotification | RootJSONRPCResponse | RootJSONRPCError;

export type RootJSONRPCNotification = {
    readonly "method": string;
    readonly "params"?: unknown;
    readonly [key: string]: unknown;
};

export type RootJSONRPCRequest = {
    readonly "id": V2RequestId;
    readonly "method": string;
    readonly "params"?: unknown;
    readonly "trace"?: RootJSONRPCRequestTrace;
    readonly [key: string]: unknown;
};

export type RootJSONRPCRequestTrace = RootW3cTraceContext | null;

export type RootJSONRPCResponse = {
    readonly "id": V2RequestId;
    readonly "result": unknown;
    readonly [key: string]: unknown;
};

export type RootMcpElicitationArrayType = "array";

export type RootMcpElicitationBooleanSchema = {
    readonly "default"?: boolean | null;
    readonly "description"?: string | null;
    readonly "title"?: string | null;
    readonly "type": RootMcpElicitationBooleanType;
};

export type RootMcpElicitationBooleanType = "boolean";

export type RootMcpElicitationConstOption = {
    readonly "const": string;
    readonly "title": string;
};

export type RootMcpElicitationEnumSchema = RootMcpElicitationSingleSelectEnumSchema | RootMcpElicitationMultiSelectEnumSchema | RootMcpElicitationLegacyTitledEnumSchema;

export type RootMcpElicitationLegacyTitledEnumSchema = {
    readonly "default"?: string | null;
    readonly "description"?: string | null;
    readonly "enum": RootMcpElicitationLegacyTitledEnumSchemaEnum;
    readonly "enumNames"?: RootMcpElicitationLegacyTitledEnumSchemaEnumNames;
    readonly "title"?: string | null;
    readonly "type": RootMcpElicitationStringType;
};

export type RootMcpElicitationLegacyTitledEnumSchemaEnum = ReadonlyArray<string>;

export type RootMcpElicitationLegacyTitledEnumSchemaEnumNames = ReadonlyArray<string> | null;

export type RootMcpElicitationMultiSelectEnumSchema = RootMcpElicitationUntitledMultiSelectEnumSchema | RootMcpElicitationTitledMultiSelectEnumSchema;

export type RootMcpElicitationNumberSchema = {
    readonly "default"?: number | null;
    readonly "description"?: string | null;
    readonly "maximum"?: number | null;
    readonly "minimum"?: number | null;
    readonly "title"?: string | null;
    readonly "type": RootMcpElicitationNumberType;
};

export type RootMcpElicitationNumberType = "number" | "integer";

export type RootMcpElicitationObjectType = "object";

export type RootMcpElicitationPrimitiveSchema = RootMcpElicitationEnumSchema | RootMcpElicitationStringSchema | RootMcpElicitationNumberSchema | RootMcpElicitationBooleanSchema;

export type RootMcpElicitationSchema = {
    readonly "$schema"?: string | null;
    readonly "properties": RootMcpElicitationSchemaProperties;
    readonly "required"?: RootMcpElicitationSchemaRequired;
    readonly "type": RootMcpElicitationObjectType;
};

export type RootMcpElicitationSchemaProperties = {
    readonly [key: string]: unknown;
};

export type RootMcpElicitationSchemaRequired = ReadonlyArray<string> | null;

export type RootMcpElicitationSingleSelectEnumSchema = RootMcpElicitationUntitledSingleSelectEnumSchema | RootMcpElicitationTitledSingleSelectEnumSchema;

export type RootMcpElicitationStringFormat = "email" | "uri" | "date" | "date-time";

export type RootMcpElicitationStringSchema = {
    readonly "default"?: string | null;
    readonly "description"?: string | null;
    readonly "format"?: RootMcpElicitationStringSchemaFormat;
    readonly "maxLength"?: number | null;
    readonly "minLength"?: number | null;
    readonly "title"?: string | null;
    readonly "type": RootMcpElicitationStringType;
};

export type RootMcpElicitationStringSchemaFormat = RootMcpElicitationStringFormat | null;

export type RootMcpElicitationStringType = "string";

export type RootMcpElicitationTitledEnumItems = {
    readonly "anyOf": RootMcpElicitationTitledEnumItemsAnyOf;
};

export type RootMcpElicitationTitledEnumItemsAnyOf = ReadonlyArray<RootMcpElicitationConstOption>;

export type RootMcpElicitationTitledMultiSelectEnumSchema = {
    readonly "default"?: RootMcpElicitationTitledMultiSelectEnumSchemaDefault;
    readonly "description"?: string | null;
    readonly "items": RootMcpElicitationTitledEnumItems;
    readonly "maxItems"?: number | null;
    readonly "minItems"?: number | null;
    readonly "title"?: string | null;
    readonly "type": RootMcpElicitationArrayType;
};

export type RootMcpElicitationTitledMultiSelectEnumSchemaDefault = ReadonlyArray<string> | null;

export type RootMcpElicitationTitledSingleSelectEnumSchema = {
    readonly "default"?: string | null;
    readonly "description"?: string | null;
    readonly "oneOf": RootMcpElicitationTitledSingleSelectEnumSchemaOneOf;
    readonly "title"?: string | null;
    readonly "type": RootMcpElicitationStringType;
};

export type RootMcpElicitationTitledSingleSelectEnumSchemaOneOf = ReadonlyArray<RootMcpElicitationConstOption>;

export type RootMcpElicitationUntitledEnumItems = {
    readonly "enum": RootMcpElicitationUntitledEnumItemsEnum;
    readonly "type": RootMcpElicitationStringType;
};

export type RootMcpElicitationUntitledEnumItemsEnum = ReadonlyArray<string>;

export type RootMcpElicitationUntitledMultiSelectEnumSchema = {
    readonly "default"?: RootMcpElicitationUntitledMultiSelectEnumSchemaDefault;
    readonly "description"?: string | null;
    readonly "items": RootMcpElicitationUntitledEnumItems;
    readonly "maxItems"?: number | null;
    readonly "minItems"?: number | null;
    readonly "title"?: string | null;
    readonly "type": RootMcpElicitationArrayType;
};

export type RootMcpElicitationUntitledMultiSelectEnumSchemaDefault = ReadonlyArray<string> | null;

export type RootMcpElicitationUntitledSingleSelectEnumSchema = {
    readonly "default"?: string | null;
    readonly "description"?: string | null;
    readonly "enum": RootMcpElicitationUntitledSingleSelectEnumSchemaEnum;
    readonly "title"?: string | null;
    readonly "type": RootMcpElicitationStringType;
};

export type RootMcpElicitationUntitledSingleSelectEnumSchemaEnum = ReadonlyArray<string>;

export type RootMcpServerElicitationAction = "accept" | "decline" | "cancel";

export type RootMcpServerElicitationRequestParams = RootMcpServerElicitationRequestParamsOneOf1 | RootMcpServerElicitationRequestParamsOneOf2 | RootMcpServerElicitationRequestParamsOneOf3;

export type RootMcpServerElicitationRequestParamsOneOf1 = {
    readonly "_meta"?: unknown;
    readonly "message": string;
    readonly "mode": RootMcpServerElicitationRequestParamsOneOf1Mode;
    readonly "requestedSchema": RootMcpElicitationSchema;
    readonly [key: string]: unknown;
};

export type RootMcpServerElicitationRequestParamsOneOf1Mode = "form";

export type RootMcpServerElicitationRequestParamsOneOf2 = {
    readonly "_meta"?: unknown;
    readonly "message": string;
    readonly "mode": RootMcpServerElicitationRequestParamsOneOf2Mode;
    readonly "requestedSchema": unknown;
    readonly [key: string]: unknown;
};

export type RootMcpServerElicitationRequestParamsOneOf2Mode = "openai/form";

export type RootMcpServerElicitationRequestParamsOneOf3 = {
    readonly "_meta"?: unknown;
    readonly "elicitationId": string;
    readonly "message": string;
    readonly "mode": RootMcpServerElicitationRequestParamsOneOf3Mode;
    readonly "url": string;
    readonly [key: string]: unknown;
};

export type RootMcpServerElicitationRequestParamsOneOf3Mode = "url";

export type RootMcpServerElicitationRequestResponse = {
    readonly "_meta"?: unknown;
    readonly "action": RootMcpServerElicitationAction;
    readonly "content"?: unknown;
    readonly [key: string]: unknown;
};

export type RootNetworkApprovalContext = {
    readonly "host": string;
    readonly "protocol": V2NetworkApprovalProtocol;
    readonly [key: string]: unknown;
};

export type RootNetworkPolicyAmendment = {
    readonly "action": RootNetworkPolicyRuleAction;
    readonly "host": string;
    readonly [key: string]: unknown;
};

export type RootNetworkPolicyRuleAction = "allow" | "deny";

export type RootParsedCommand = RootReadParsedCommand | RootListFilesParsedCommand | RootSearchParsedCommand | RootUnknownParsedCommand;

export type RootReadParsedCommand = {
    readonly "cmd": string;
    readonly "name": string;
    readonly "path": string;
    readonly "type": RootReadParsedCommandType;
    readonly [key: string]: unknown;
};

export type RootReadParsedCommandType = "read";

export type RootListFilesParsedCommand = {
    readonly "cmd": string;
    readonly "path"?: string | null;
    readonly "type": RootListFilesParsedCommandType;
    readonly [key: string]: unknown;
};

export type RootListFilesParsedCommandType = "list_files";

export type RootSearchParsedCommand = {
    readonly "cmd": string;
    readonly "path"?: string | null;
    readonly "query"?: string | null;
    readonly "type": RootSearchParsedCommandType;
    readonly [key: string]: unknown;
};

export type RootSearchParsedCommandType = "search";

export type RootUnknownParsedCommand = {
    readonly "cmd": string;
    readonly "type": RootUnknownParsedCommandType;
    readonly [key: string]: unknown;
};

export type RootUnknownParsedCommandType = "unknown";

export type RootPermissionGrantScope = "turn" | "session";

export type RootPermissionsRequestApprovalParams = {
    readonly "cwd": V2AbsolutePathBuf;
    readonly "environmentId"?: string | null;
    readonly "itemId": string;
    readonly "permissions": V2RequestPermissionProfile;
    readonly "reason"?: string | null;
    readonly "startedAtMs": number;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type RootPermissionsRequestApprovalResponse = {
    readonly "permissions": RootGrantedPermissionProfile;
    readonly "scope"?: RootPermissionsRequestApprovalResponseScope;
    readonly "strictAutoReview"?: boolean | null;
    readonly [key: string]: unknown;
};

export type RootPermissionsRequestApprovalResponseScope = RootPermissionGrantScope;

export type RootRequestId = string | number;

export type RootReviewDecision = RootReviewDecisionOneOf1 | RootApprovedExecpolicyAmendmentReviewDecision | RootReviewDecisionOneOf3 | RootReviewDecisionOneOf4 | RootNetworkPolicyAmendmentReviewDecision | RootDeniedReviewDecision | RootReviewDecisionOneOf7 | RootReviewDecisionOneOf8;

export type RootReviewDecisionOneOf1 = "approved";

export type RootApprovedExecpolicyAmendmentReviewDecision = {
    readonly "approved_execpolicy_amendment": RootApprovedExecpolicyAmendmentReviewDecisionApprovedExecpolicyAmendment;
};

export type RootApprovedExecpolicyAmendmentReviewDecisionApprovedExecpolicyAmendment = {
    readonly "proposed_execpolicy_amendment": RootApprovedExecpolicyAmendmentReviewDecisionApprovedExecpolicyAmendmentProposedExecpolicyAmendment;
    readonly [key: string]: unknown;
};

export type RootApprovedExecpolicyAmendmentReviewDecisionApprovedExecpolicyAmendmentProposedExecpolicyAmendment = ReadonlyArray<string>;

export type RootReviewDecisionOneOf3 = "approved_for_session";

export type RootReviewDecisionOneOf4 = "approved_mcp_policy_amendment";

export type RootNetworkPolicyAmendmentReviewDecision = {
    readonly "network_policy_amendment": RootNetworkPolicyAmendmentReviewDecisionNetworkPolicyAmendment;
};

export type RootNetworkPolicyAmendmentReviewDecisionNetworkPolicyAmendment = {
    readonly "network_policy_amendment": RootNetworkPolicyAmendment;
    readonly [key: string]: unknown;
};

export type RootDeniedReviewDecision = {
    readonly "denied": RootDeniedReviewDecisionDenied;
};

export type RootDeniedReviewDecisionDenied = {
    readonly "rejection": string;
    readonly [key: string]: unknown;
};

export type RootReviewDecisionOneOf7 = "timed_out";

export type RootReviewDecisionOneOf8 = "abort";

export type RootServerNotification = RootErrorNotification | RootThreadStartedNotification | RootThreadStatusChangedNotification | RootThreadArchivedNotification | RootThreadDeletedNotification | RootThreadUnarchivedNotification | RootThreadClosedNotification | RootThreadRevertedNotification | RootSkillsChangedNotification | RootThreadNameUpdatedNotification | RootThreadGoalUpdatedNotification | RootThreadGoalClearedNotification | RootThreadQueueChangedNotification | RootProjectChangedNotification | RootThreadProjectUpdatedNotification | RootThreadEnvironmentConnectedNotification | RootThreadEnvironmentDisconnectedNotification | RootThreadSettingsUpdatedNotification | RootThreadTokenUsageUpdatedNotification | RootTurnStartedNotification | RootHookStartedNotification | RootTurnCompletedNotification | RootHookCompletedNotification | RootTurnDiffUpdatedNotification | RootTurnPlanUpdatedNotification | RootItemStartedNotification | RootItemAutoApprovalReviewStartedNotification | RootItemAutoApprovalReviewCompletedNotification | RootAutoApprovalReviewStrictReviewRequiredNotification | RootItemCompletedNotification | RootItemAgentMessageDeltaNotification | RootItemPlanDeltaNotification | RootCommandExecOutputDeltaNotification | RootProcessOutputDeltaNotification | RootProcessExitedNotification | RootItemCommandExecutionOutputDeltaNotification | RootItemCommandExecutionTerminalInteractionNotification | RootItemFileChangeOutputDeltaNotification | RootItemFileChangePatchUpdatedNotification | RootServerRequestResolvedNotification | RootItemMcpToolCallProgressNotification | RootMcpServerOauthLoginCompletedNotification | RootMcpServerStartupStatusUpdatedNotification | RootMcpServerEventStreamNotificationNotification | RootAccountUpdatedNotification | RootAccountRateLimitsUpdatedNotification | RootAppListUpdatedNotification | RootRemoteControlStatusChangedNotification | RootExternalAgentConfigImportProgressNotification | RootExternalAgentConfigImportCompletedNotification | RootFsChangedNotification | RootItemReasoningSummaryTextDeltaNotification | RootItemReasoningSummaryPartAddedNotification | RootItemReasoningTextDeltaNotification | RootThreadCompactedNotification | RootModelReroutedNotification | RootModelVerificationNotification | RootTurnModerationMetadataNotification | RootModelSafetyBufferingUpdatedNotification | RootWarningNotification | RootGuardianWarningNotification | RootDeprecationNoticeNotification | RootConfigWarningNotification | RootFuzzyFileSearchSessionUpdatedNotification2 | RootFuzzyFileSearchSessionCompletedNotification2 | RootThreadRealtimeStartedNotification | RootThreadRealtimeItemAddedNotification | RootThreadRealtimeTranscriptDeltaNotification | RootThreadRealtimeTranscriptDoneNotification | RootThreadRealtimeOutputAudioDeltaNotification | RootThreadRealtimeSdpNotification | RootThreadRealtimeErrorNotification | RootThreadRealtimeClosedNotification | RootWindowsWorldWritableWarningNotification | RootWindowsSandboxSetupCompletedNotification | RootAccountLoginCompletedNotification;

export type RootErrorNotification = {
    readonly "method": RootErrorNotificationMethod;
    readonly "params": V2ErrorNotification;
    readonly [key: string]: unknown;
};

export type RootErrorNotificationMethod = "error";

export type RootThreadStartedNotification = {
    readonly "method": RootThreadStartedNotificationMethod;
    readonly "params": V2ThreadStartedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadStartedNotificationMethod = "thread/started";

export type RootThreadStatusChangedNotification = {
    readonly "method": RootThreadStatusChangedNotificationMethod;
    readonly "params": V2ThreadStatusChangedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadStatusChangedNotificationMethod = "thread/status/changed";

export type RootThreadArchivedNotification = {
    readonly "method": RootThreadArchivedNotificationMethod;
    readonly "params": V2ThreadArchivedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadArchivedNotificationMethod = "thread/archived";

export type RootThreadDeletedNotification = {
    readonly "method": RootThreadDeletedNotificationMethod;
    readonly "params": V2ThreadDeletedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadDeletedNotificationMethod = "thread/deleted";

export type RootThreadUnarchivedNotification = {
    readonly "method": RootThreadUnarchivedNotificationMethod;
    readonly "params": V2ThreadUnarchivedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadUnarchivedNotificationMethod = "thread/unarchived";

export type RootThreadClosedNotification = {
    readonly "method": RootThreadClosedNotificationMethod;
    readonly "params": V2ThreadClosedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadClosedNotificationMethod = "thread/closed";

export type RootThreadRevertedNotification = {
    readonly "method": RootThreadRevertedNotificationMethod;
    readonly "params": V2ThreadRevertedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadRevertedNotificationMethod = "thread/reverted";

export type RootSkillsChangedNotification = {
    readonly "method": RootSkillsChangedNotificationMethod;
    readonly "params": V2SkillsChangedNotification;
    readonly [key: string]: unknown;
};

export type RootSkillsChangedNotificationMethod = "skills/changed";

export type RootThreadNameUpdatedNotification = {
    readonly "method": RootThreadNameUpdatedNotificationMethod;
    readonly "params": V2ThreadNameUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadNameUpdatedNotificationMethod = "thread/name/updated";

export type RootThreadGoalUpdatedNotification = {
    readonly "method": RootThreadGoalUpdatedNotificationMethod;
    readonly "params": V2ThreadGoalUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadGoalUpdatedNotificationMethod = "thread/goal/updated";

export type RootThreadGoalClearedNotification = {
    readonly "method": RootThreadGoalClearedNotificationMethod;
    readonly "params": V2ThreadGoalClearedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadGoalClearedNotificationMethod = "thread/goal/cleared";

export type RootThreadQueueChangedNotification = {
    readonly "method": RootThreadQueueChangedNotificationMethod;
    readonly "params": V2ThreadQueueChangedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadQueueChangedNotificationMethod = "thread/queue/changed";

export type RootProjectChangedNotification = {
    readonly "method": RootProjectChangedNotificationMethod;
    readonly "params": V2ProjectChangedNotification;
    readonly [key: string]: unknown;
};

export type RootProjectChangedNotificationMethod = "project/changed";

export type RootThreadProjectUpdatedNotification = {
    readonly "method": RootThreadProjectUpdatedNotificationMethod;
    readonly "params": V2ThreadProjectUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadProjectUpdatedNotificationMethod = "thread/project/updated";

export type RootThreadEnvironmentConnectedNotification = {
    readonly "method": RootThreadEnvironmentConnectedNotificationMethod;
    readonly "params": V2EnvironmentConnectionNotification;
    readonly [key: string]: unknown;
};

export type RootThreadEnvironmentConnectedNotificationMethod = "thread/environment/connected";

export type RootThreadEnvironmentDisconnectedNotification = {
    readonly "method": RootThreadEnvironmentDisconnectedNotificationMethod;
    readonly "params": V2EnvironmentConnectionNotification;
    readonly [key: string]: unknown;
};

export type RootThreadEnvironmentDisconnectedNotificationMethod = "thread/environment/disconnected";

export type RootThreadSettingsUpdatedNotification = {
    readonly "method": RootThreadSettingsUpdatedNotificationMethod;
    readonly "params": V2ThreadSettingsUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadSettingsUpdatedNotificationMethod = "thread/settings/updated";

export type RootThreadTokenUsageUpdatedNotification = {
    readonly "method": RootThreadTokenUsageUpdatedNotificationMethod;
    readonly "params": V2ThreadTokenUsageUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadTokenUsageUpdatedNotificationMethod = "thread/tokenUsage/updated";

export type RootTurnStartedNotification = {
    readonly "method": RootTurnStartedNotificationMethod;
    readonly "params": V2TurnStartedNotification;
    readonly [key: string]: unknown;
};

export type RootTurnStartedNotificationMethod = "turn/started";

export type RootHookStartedNotification = {
    readonly "method": RootHookStartedNotificationMethod;
    readonly "params": V2HookStartedNotification;
    readonly [key: string]: unknown;
};

export type RootHookStartedNotificationMethod = "hook/started";

export type RootTurnCompletedNotification = {
    readonly "method": RootTurnCompletedNotificationMethod;
    readonly "params": V2TurnCompletedNotification;
    readonly [key: string]: unknown;
};

export type RootTurnCompletedNotificationMethod = "turn/completed";

export type RootHookCompletedNotification = {
    readonly "method": RootHookCompletedNotificationMethod;
    readonly "params": V2HookCompletedNotification;
    readonly [key: string]: unknown;
};

export type RootHookCompletedNotificationMethod = "hook/completed";

export type RootTurnDiffUpdatedNotification = {
    readonly "method": RootTurnDiffUpdatedNotificationMethod;
    readonly "params": V2TurnDiffUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootTurnDiffUpdatedNotificationMethod = "turn/diff/updated";

export type RootTurnPlanUpdatedNotification = {
    readonly "method": RootTurnPlanUpdatedNotificationMethod;
    readonly "params": V2TurnPlanUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootTurnPlanUpdatedNotificationMethod = "turn/plan/updated";

export type RootItemStartedNotification = {
    readonly "method": RootItemStartedNotificationMethod;
    readonly "params": V2ItemStartedNotification;
    readonly [key: string]: unknown;
};

export type RootItemStartedNotificationMethod = "item/started";

export type RootItemAutoApprovalReviewStartedNotification = {
    readonly "method": RootItemAutoApprovalReviewStartedNotificationMethod;
    readonly "params": V2ItemGuardianApprovalReviewStartedNotification;
    readonly [key: string]: unknown;
};

export type RootItemAutoApprovalReviewStartedNotificationMethod = "item/autoApprovalReview/started";

export type RootItemAutoApprovalReviewCompletedNotification = {
    readonly "method": RootItemAutoApprovalReviewCompletedNotificationMethod;
    readonly "params": V2ItemGuardianApprovalReviewCompletedNotification;
    readonly [key: string]: unknown;
};

export type RootItemAutoApprovalReviewCompletedNotificationMethod = "item/autoApprovalReview/completed";

export type RootAutoApprovalReviewStrictReviewRequiredNotification = {
    readonly "method": RootAutoApprovalReviewStrictReviewRequiredNotificationMethod;
    readonly "params": V2StrictReviewRequiredNotification;
    readonly [key: string]: unknown;
};

export type RootAutoApprovalReviewStrictReviewRequiredNotificationMethod = "autoApprovalReview/strictReviewRequired";

export type RootItemCompletedNotification = {
    readonly "method": RootItemCompletedNotificationMethod;
    readonly "params": V2ItemCompletedNotification;
    readonly [key: string]: unknown;
};

export type RootItemCompletedNotificationMethod = "item/completed";

export type RootItemAgentMessageDeltaNotification = {
    readonly "method": RootItemAgentMessageDeltaNotificationMethod;
    readonly "params": V2AgentMessageDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootItemAgentMessageDeltaNotificationMethod = "item/agentMessage/delta";

export type RootItemPlanDeltaNotification = {
    readonly "method": RootItemPlanDeltaNotificationMethod;
    readonly "params": V2PlanDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootItemPlanDeltaNotificationMethod = "item/plan/delta";

export type RootCommandExecOutputDeltaNotification = {
    readonly "method": RootCommandExecOutputDeltaNotificationMethod;
    readonly "params": V2CommandExecOutputDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootCommandExecOutputDeltaNotificationMethod = "command/exec/outputDelta";

export type RootProcessOutputDeltaNotification = {
    readonly "method": RootProcessOutputDeltaNotificationMethod;
    readonly "params": V2ProcessOutputDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootProcessOutputDeltaNotificationMethod = "process/outputDelta";

export type RootProcessExitedNotification = {
    readonly "method": RootProcessExitedNotificationMethod;
    readonly "params": V2ProcessExitedNotification;
    readonly [key: string]: unknown;
};

export type RootProcessExitedNotificationMethod = "process/exited";

export type RootItemCommandExecutionOutputDeltaNotification = {
    readonly "method": RootItemCommandExecutionOutputDeltaNotificationMethod;
    readonly "params": V2CommandExecutionOutputDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootItemCommandExecutionOutputDeltaNotificationMethod = "item/commandExecution/outputDelta";

export type RootItemCommandExecutionTerminalInteractionNotification = {
    readonly "method": RootItemCommandExecutionTerminalInteractionNotificationMethod;
    readonly "params": V2TerminalInteractionNotification;
    readonly [key: string]: unknown;
};

export type RootItemCommandExecutionTerminalInteractionNotificationMethod = "item/commandExecution/terminalInteraction";

export type RootItemFileChangeOutputDeltaNotification = {
    readonly "method": RootItemFileChangeOutputDeltaNotificationMethod;
    readonly "params": V2FileChangeOutputDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootItemFileChangeOutputDeltaNotificationMethod = "item/fileChange/outputDelta";

export type RootItemFileChangePatchUpdatedNotification = {
    readonly "method": RootItemFileChangePatchUpdatedNotificationMethod;
    readonly "params": V2FileChangePatchUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootItemFileChangePatchUpdatedNotificationMethod = "item/fileChange/patchUpdated";

export type RootServerRequestResolvedNotification = {
    readonly "method": RootServerRequestResolvedNotificationMethod;
    readonly "params": V2ServerRequestResolvedNotification;
    readonly [key: string]: unknown;
};

export type RootServerRequestResolvedNotificationMethod = "serverRequest/resolved";

export type RootItemMcpToolCallProgressNotification = {
    readonly "method": RootItemMcpToolCallProgressNotificationMethod;
    readonly "params": V2McpToolCallProgressNotification;
    readonly [key: string]: unknown;
};

export type RootItemMcpToolCallProgressNotificationMethod = "item/mcpToolCall/progress";

export type RootMcpServerOauthLoginCompletedNotification = {
    readonly "method": RootMcpServerOauthLoginCompletedNotificationMethod;
    readonly "params": V2McpServerOauthLoginCompletedNotification;
    readonly [key: string]: unknown;
};

export type RootMcpServerOauthLoginCompletedNotificationMethod = "mcpServer/oauthLogin/completed";

export type RootMcpServerStartupStatusUpdatedNotification = {
    readonly "method": RootMcpServerStartupStatusUpdatedNotificationMethod;
    readonly "params": V2McpServerStatusUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootMcpServerStartupStatusUpdatedNotificationMethod = "mcpServer/startupStatus/updated";

export type RootMcpServerEventStreamNotificationNotification = {
    readonly "method": RootMcpServerEventStreamNotificationNotificationMethod;
    readonly "params": V2McpServerEventStreamNotification;
    readonly [key: string]: unknown;
};

export type RootMcpServerEventStreamNotificationNotificationMethod = "mcpServer/event/stream/notification";

export type RootAccountUpdatedNotification = {
    readonly "method": RootAccountUpdatedNotificationMethod;
    readonly "params": V2AccountUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootAccountUpdatedNotificationMethod = "account/updated";

export type RootAccountRateLimitsUpdatedNotification = {
    readonly "method": RootAccountRateLimitsUpdatedNotificationMethod;
    readonly "params": V2AccountRateLimitsUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootAccountRateLimitsUpdatedNotificationMethod = "account/rateLimits/updated";

export type RootAppListUpdatedNotification = {
    readonly "method": RootAppListUpdatedNotificationMethod;
    readonly "params": V2AppListUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootAppListUpdatedNotificationMethod = "app/list/updated";

export type RootRemoteControlStatusChangedNotification = {
    readonly "method": RootRemoteControlStatusChangedNotificationMethod;
    readonly "params": V2RemoteControlStatusChangedNotification;
    readonly [key: string]: unknown;
};

export type RootRemoteControlStatusChangedNotificationMethod = "remoteControl/status/changed";

export type RootExternalAgentConfigImportProgressNotification = {
    readonly "method": RootExternalAgentConfigImportProgressNotificationMethod;
    readonly "params": V2ExternalAgentConfigImportProgressNotification;
    readonly [key: string]: unknown;
};

export type RootExternalAgentConfigImportProgressNotificationMethod = "externalAgentConfig/import/progress";

export type RootExternalAgentConfigImportCompletedNotification = {
    readonly "method": RootExternalAgentConfigImportCompletedNotificationMethod;
    readonly "params": V2ExternalAgentConfigImportCompletedNotification;
    readonly [key: string]: unknown;
};

export type RootExternalAgentConfigImportCompletedNotificationMethod = "externalAgentConfig/import/completed";

export type RootFsChangedNotification = {
    readonly "method": RootFsChangedNotificationMethod;
    readonly "params": V2FsChangedNotification;
    readonly [key: string]: unknown;
};

export type RootFsChangedNotificationMethod = "fs/changed";

export type RootItemReasoningSummaryTextDeltaNotification = {
    readonly "method": RootItemReasoningSummaryTextDeltaNotificationMethod;
    readonly "params": V2ReasoningSummaryTextDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootItemReasoningSummaryTextDeltaNotificationMethod = "item/reasoning/summaryTextDelta";

export type RootItemReasoningSummaryPartAddedNotification = {
    readonly "method": RootItemReasoningSummaryPartAddedNotificationMethod;
    readonly "params": V2ReasoningSummaryPartAddedNotification;
    readonly [key: string]: unknown;
};

export type RootItemReasoningSummaryPartAddedNotificationMethod = "item/reasoning/summaryPartAdded";

export type RootItemReasoningTextDeltaNotification = {
    readonly "method": RootItemReasoningTextDeltaNotificationMethod;
    readonly "params": V2ReasoningTextDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootItemReasoningTextDeltaNotificationMethod = "item/reasoning/textDelta";

export type RootThreadCompactedNotification = {
    readonly "method": RootThreadCompactedNotificationMethod;
    readonly "params": V2ContextCompactedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadCompactedNotificationMethod = "thread/compacted";

export type RootModelReroutedNotification = {
    readonly "method": RootModelReroutedNotificationMethod;
    readonly "params": V2ModelReroutedNotification;
    readonly [key: string]: unknown;
};

export type RootModelReroutedNotificationMethod = "model/rerouted";

export type RootModelVerificationNotification = {
    readonly "method": RootModelVerificationNotificationMethod;
    readonly "params": V2ModelVerificationNotification;
    readonly [key: string]: unknown;
};

export type RootModelVerificationNotificationMethod = "model/verification";

export type RootTurnModerationMetadataNotification = {
    readonly "method": RootTurnModerationMetadataNotificationMethod;
    readonly "params": V2TurnModerationMetadataNotification;
    readonly [key: string]: unknown;
};

export type RootTurnModerationMetadataNotificationMethod = "turn/moderationMetadata";

export type RootModelSafetyBufferingUpdatedNotification = {
    readonly "method": RootModelSafetyBufferingUpdatedNotificationMethod;
    readonly "params": V2ModelSafetyBufferingUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootModelSafetyBufferingUpdatedNotificationMethod = "model/safetyBuffering/updated";

export type RootWarningNotification = {
    readonly "method": RootWarningNotificationMethod;
    readonly "params": V2WarningNotification;
    readonly [key: string]: unknown;
};

export type RootWarningNotificationMethod = "warning";

export type RootGuardianWarningNotification = {
    readonly "method": RootGuardianWarningNotificationMethod;
    readonly "params": V2GuardianWarningNotification;
    readonly [key: string]: unknown;
};

export type RootGuardianWarningNotificationMethod = "guardianWarning";

export type RootDeprecationNoticeNotification = {
    readonly "method": RootDeprecationNoticeNotificationMethod;
    readonly "params": V2DeprecationNoticeNotification;
    readonly [key: string]: unknown;
};

export type RootDeprecationNoticeNotificationMethod = "deprecationNotice";

export type RootConfigWarningNotification = {
    readonly "method": RootConfigWarningNotificationMethod;
    readonly "params": V2ConfigWarningNotification;
    readonly [key: string]: unknown;
};

export type RootConfigWarningNotificationMethod = "configWarning";

export type RootFuzzyFileSearchSessionUpdatedNotification2 = {
    readonly "method": RootFuzzyFileSearchSessionUpdatedNotification2Method;
    readonly "params": RootFuzzyFileSearchSessionUpdatedNotification;
    readonly [key: string]: unknown;
};

export type RootFuzzyFileSearchSessionUpdatedNotification2Method = "fuzzyFileSearch/sessionUpdated";

export type RootFuzzyFileSearchSessionCompletedNotification2 = {
    readonly "method": RootFuzzyFileSearchSessionCompletedNotification2Method;
    readonly "params": RootFuzzyFileSearchSessionCompletedNotification;
    readonly [key: string]: unknown;
};

export type RootFuzzyFileSearchSessionCompletedNotification2Method = "fuzzyFileSearch/sessionCompleted";

export type RootThreadRealtimeStartedNotification = {
    readonly "method": RootThreadRealtimeStartedNotificationMethod;
    readonly "params": V2ThreadRealtimeStartedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadRealtimeStartedNotificationMethod = "thread/realtime/started";

export type RootThreadRealtimeItemAddedNotification = {
    readonly "method": RootThreadRealtimeItemAddedNotificationMethod;
    readonly "params": V2ThreadRealtimeItemAddedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadRealtimeItemAddedNotificationMethod = "thread/realtime/itemAdded";

export type RootThreadRealtimeTranscriptDeltaNotification = {
    readonly "method": RootThreadRealtimeTranscriptDeltaNotificationMethod;
    readonly "params": V2ThreadRealtimeTranscriptDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootThreadRealtimeTranscriptDeltaNotificationMethod = "thread/realtime/transcript/delta";

export type RootThreadRealtimeTranscriptDoneNotification = {
    readonly "method": RootThreadRealtimeTranscriptDoneNotificationMethod;
    readonly "params": V2ThreadRealtimeTranscriptDoneNotification;
    readonly [key: string]: unknown;
};

export type RootThreadRealtimeTranscriptDoneNotificationMethod = "thread/realtime/transcript/done";

export type RootThreadRealtimeOutputAudioDeltaNotification = {
    readonly "method": RootThreadRealtimeOutputAudioDeltaNotificationMethod;
    readonly "params": V2ThreadRealtimeOutputAudioDeltaNotification;
    readonly [key: string]: unknown;
};

export type RootThreadRealtimeOutputAudioDeltaNotificationMethod = "thread/realtime/outputAudio/delta";

export type RootThreadRealtimeSdpNotification = {
    readonly "method": RootThreadRealtimeSdpNotificationMethod;
    readonly "params": V2ThreadRealtimeSdpNotification;
    readonly [key: string]: unknown;
};

export type RootThreadRealtimeSdpNotificationMethod = "thread/realtime/sdp";

export type RootThreadRealtimeErrorNotification = {
    readonly "method": RootThreadRealtimeErrorNotificationMethod;
    readonly "params": V2ThreadRealtimeErrorNotification;
    readonly [key: string]: unknown;
};

export type RootThreadRealtimeErrorNotificationMethod = "thread/realtime/error";

export type RootThreadRealtimeClosedNotification = {
    readonly "method": RootThreadRealtimeClosedNotificationMethod;
    readonly "params": V2ThreadRealtimeClosedNotification;
    readonly [key: string]: unknown;
};

export type RootThreadRealtimeClosedNotificationMethod = "thread/realtime/closed";

export type RootWindowsWorldWritableWarningNotification = {
    readonly "method": RootWindowsWorldWritableWarningNotificationMethod;
    readonly "params": V2WindowsWorldWritableWarningNotification;
    readonly [key: string]: unknown;
};

export type RootWindowsWorldWritableWarningNotificationMethod = "windows/worldWritableWarning";

export type RootWindowsSandboxSetupCompletedNotification = {
    readonly "method": RootWindowsSandboxSetupCompletedNotificationMethod;
    readonly "params": V2WindowsSandboxSetupCompletedNotification;
    readonly [key: string]: unknown;
};

export type RootWindowsSandboxSetupCompletedNotificationMethod = "windowsSandbox/setupCompleted";

export type RootAccountLoginCompletedNotification = {
    readonly "method": RootAccountLoginCompletedNotificationMethod;
    readonly "params": V2AccountLoginCompletedNotification;
    readonly [key: string]: unknown;
};

export type RootAccountLoginCompletedNotificationMethod = "account/login/completed";

export type RootServerRequest = RootItemCommandExecutionRequestApprovalRequest | RootItemFileChangeRequestApprovalRequest | RootItemToolRequestUserInputRequest | RootMcpServerElicitationRequestRequest | RootItemPermissionsRequestApprovalRequest | RootItemToolCallRequest | RootAccountChatgptAuthTokensRefreshRequest | RootAttestationGenerateRequest | RootApplyPatchApprovalRequest | RootExecCommandApprovalRequest;

export type RootItemCommandExecutionRequestApprovalRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootItemCommandExecutionRequestApprovalRequestMethod;
    readonly "params": RootCommandExecutionRequestApprovalParams;
    readonly [key: string]: unknown;
};

export type RootItemCommandExecutionRequestApprovalRequestMethod = "item/commandExecution/requestApproval";

export type RootItemFileChangeRequestApprovalRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootItemFileChangeRequestApprovalRequestMethod;
    readonly "params": RootFileChangeRequestApprovalParams;
    readonly [key: string]: unknown;
};

export type RootItemFileChangeRequestApprovalRequestMethod = "item/fileChange/requestApproval";

export type RootItemToolRequestUserInputRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootItemToolRequestUserInputRequestMethod;
    readonly "params": RootToolRequestUserInputParams;
    readonly [key: string]: unknown;
};

export type RootItemToolRequestUserInputRequestMethod = "item/tool/requestUserInput";

export type RootMcpServerElicitationRequestRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootMcpServerElicitationRequestRequestMethod;
    readonly "params": RootMcpServerElicitationRequestParams;
    readonly [key: string]: unknown;
};

export type RootMcpServerElicitationRequestRequestMethod = "mcpServer/elicitation/request";

export type RootItemPermissionsRequestApprovalRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootItemPermissionsRequestApprovalRequestMethod;
    readonly "params": RootPermissionsRequestApprovalParams;
    readonly [key: string]: unknown;
};

export type RootItemPermissionsRequestApprovalRequestMethod = "item/permissions/requestApproval";

export type RootItemToolCallRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootItemToolCallRequestMethod;
    readonly "params": RootDynamicToolCallParams;
    readonly [key: string]: unknown;
};

export type RootItemToolCallRequestMethod = "item/tool/call";

export type RootAccountChatgptAuthTokensRefreshRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAccountChatgptAuthTokensRefreshRequestMethod;
    readonly "params": RootChatgptAuthTokensRefreshParams;
    readonly [key: string]: unknown;
};

export type RootAccountChatgptAuthTokensRefreshRequestMethod = "account/chatgptAuthTokens/refresh";

export type RootAttestationGenerateRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootAttestationGenerateRequestMethod;
    readonly "params": RootAttestationGenerateParams;
    readonly [key: string]: unknown;
};

export type RootAttestationGenerateRequestMethod = "attestation/generate";

export type RootApplyPatchApprovalRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootApplyPatchApprovalRequestMethod;
    readonly "params": RootApplyPatchApprovalParams;
    readonly [key: string]: unknown;
};

export type RootApplyPatchApprovalRequestMethod = "applyPatchApproval";

export type RootExecCommandApprovalRequest = {
    readonly "id": V2RequestId;
    readonly "method": RootExecCommandApprovalRequestMethod;
    readonly "params": RootExecCommandApprovalParams;
    readonly [key: string]: unknown;
};

export type RootExecCommandApprovalRequestMethod = "execCommandApproval";

export type RootToolRequestUserInputAnswer = {
    readonly "answers": RootToolRequestUserInputAnswerAnswers;
    readonly [key: string]: unknown;
};

export type RootToolRequestUserInputAnswerAnswers = ReadonlyArray<string>;

export type RootToolRequestUserInputOption = {
    readonly "description": string;
    readonly "label": string;
    readonly [key: string]: unknown;
};

export type RootToolRequestUserInputParams = {
    readonly "autoResolutionMs"?: number | null;
    readonly "isBlocking": boolean;
    readonly "itemId": string;
    readonly "questions": RootToolRequestUserInputParamsQuestions;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type RootToolRequestUserInputParamsQuestions = ReadonlyArray<RootToolRequestUserInputQuestion>;

export type RootToolRequestUserInputQuestion = {
    readonly "header": string;
    readonly "id": string;
    readonly "isOther"?: boolean;
    readonly "isSecret"?: boolean;
    readonly "options"?: RootToolRequestUserInputQuestionOptions;
    readonly "question": string;
    readonly [key: string]: unknown;
};

export type RootToolRequestUserInputQuestionOptions = ReadonlyArray<RootToolRequestUserInputOption> | null;

export type RootToolRequestUserInputResponse = {
    readonly "answers": RootToolRequestUserInputResponseAnswers;
    readonly [key: string]: unknown;
};

export type RootToolRequestUserInputResponseAnswers = {
    readonly [key: string]: unknown;
};

export type RootW3cTraceContext = {
    readonly "traceparent"?: string | null;
    readonly "tracestate"?: string | null;
    readonly [key: string]: unknown;
};

export type V2AbsolutePathBuf = string;

export type V2Account = V2ApiKeyAccount | V2ChatgptAccount | V2AmazonBedrockAccount;

export type V2ApiKeyAccount = {
    readonly "type": V2ApiKeyAccountType;
    readonly [key: string]: unknown;
};

export type V2ApiKeyAccountType = "apiKey";

export type V2ChatgptAccount = {
    readonly "email": string | null;
    readonly "planType": V2PlanType;
    readonly "type": V2ChatgptAccountType;
    readonly [key: string]: unknown;
};

export type V2ChatgptAccountType = "chatgpt";

export type V2AmazonBedrockAccount = {
    readonly "type": V2AmazonBedrockAccountType;
    readonly "usesCodexManagedCredentials"?: boolean;
    readonly [key: string]: unknown;
};

export type V2AmazonBedrockAccountType = "amazonBedrock";

export type V2AccountLoginCompletedNotification = {
    readonly "error"?: string | null;
    readonly "loginId"?: string | null;
    readonly "onboardingEntrypoint"?: V2AccountLoginCompletedNotificationOnboardingEntrypoint;
    readonly "success": boolean;
    readonly [key: string]: unknown;
};

export type V2AccountLoginCompletedNotificationOnboardingEntrypoint = V2DesktopOnboardingEntrypoint | null;

export type V2AccountRateLimitsUpdatedNotification = {
    readonly "rateLimits": V2RateLimitSnapshot;
    readonly [key: string]: unknown;
};

export type V2AccountTokenUsageDailyBucket = {
    readonly "startDate": string;
    readonly "tokens": number;
    readonly [key: string]: unknown;
};

export type V2AccountTokenUsageSummary = {
    readonly "currentStreakDays"?: number | null;
    readonly "lifetimeTokens"?: number | null;
    readonly "longestRunningTurnSec"?: number | null;
    readonly "longestStreakDays"?: number | null;
    readonly "peakDailyTokens"?: number | null;
    readonly [key: string]: unknown;
};

export type V2AccountUpdatedNotification = {
    readonly "authMode"?: V2AccountUpdatedNotificationAuthMode;
    readonly "planType"?: V2AccountUpdatedNotificationPlanType;
    readonly [key: string]: unknown;
};

export type V2AccountUpdatedNotificationAuthMode = V2AuthMode | null;

export type V2AccountUpdatedNotificationPlanType = V2PlanType | null;

export type V2ActivePermissionProfile = {
    readonly "extends"?: string | null;
    readonly "id": string;
    readonly [key: string]: unknown;
};

export type V2AddCreditsNudgeCreditType = "credits" | "usage_limit";

export type V2AddCreditsNudgeEmailStatus = "sent" | "cooldown_active";

export type V2AdditionalContextEntry = {
    readonly "kind": V2AdditionalContextKind;
    readonly "value": string;
    readonly [key: string]: unknown;
};

export type V2AdditionalContextKind = "untrusted" | "application";

export type V2AdditionalFileSystemPermissions = {
    readonly "entries"?: V2AdditionalFileSystemPermissionsEntries;
    readonly "globScanMaxDepth"?: number | null;
    readonly "read"?: V2AdditionalFileSystemPermissionsRead;
    readonly "write"?: V2AdditionalFileSystemPermissionsWrite;
    readonly [key: string]: unknown;
};

export type V2AdditionalFileSystemPermissionsEntries = ReadonlyArray<V2FileSystemSandboxEntry> | null;

export type V2AdditionalFileSystemPermissionsRead = ReadonlyArray<V2LegacyAppPathString> | null;

export type V2AdditionalFileSystemPermissionsWrite = ReadonlyArray<V2LegacyAppPathString> | null;

export type V2AdditionalNetworkPermissions = {
    readonly "enabled"?: boolean | null;
    readonly [key: string]: unknown;
};

export type V2AgentMessageDelivery = "async";

export type V2AgentMessageDeltaNotification = {
    readonly "delta": string;
    readonly "itemId": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2AgentMessageInputContent = V2InputTextAgentMessageInputContent | V2EncryptedContentAgentMessageInputContent;

export type V2InputTextAgentMessageInputContent = {
    readonly "text": string;
    readonly "type": V2InputTextAgentMessageInputContentType;
    readonly [key: string]: unknown;
};

export type V2InputTextAgentMessageInputContentType = "input_text";

export type V2EncryptedContentAgentMessageInputContent = {
    readonly "encrypted_content": string;
    readonly "type": V2EncryptedContentAgentMessageInputContentType;
    readonly [key: string]: unknown;
};

export type V2EncryptedContentAgentMessageInputContentType = "encrypted_content";

export type V2AgentPath = string;

export type V2AllowDenyRequirement = "allow" | "deny";

export type V2AnalyticsConfig = {
    readonly "enabled"?: boolean | null;
    readonly [key: string]: unknown;
};

export type V2AppBranding = {
    readonly "category"?: string | null;
    readonly "developer"?: string | null;
    readonly "isDiscoverableApp": boolean;
    readonly "privacyPolicy"?: string | null;
    readonly "termsOfService"?: string | null;
    readonly "website"?: string | null;
    readonly [key: string]: unknown;
};

export type V2AppConfig = {
    readonly "approvals_reviewer"?: V2AppConfigApprovalsReviewer;
    readonly "default_tools_approval_mode"?: V2AppConfigDefaultToolsApprovalMode;
    readonly "default_tools_enabled"?: boolean | null;
    readonly "destructive_enabled"?: boolean | null;
    readonly "enabled"?: boolean;
    readonly "open_world_enabled"?: boolean | null;
    readonly "tools"?: V2AppConfigTools;
    readonly [key: string]: unknown;
};

export type V2AppConfigApprovalsReviewer = V2ApprovalsReviewer | null;

export type V2AppConfigDefaultToolsApprovalMode = V2AppToolApproval | null;

export type V2AppConfigTools = V2AppToolsConfig | null;

export type V2AppInfo = {
    readonly "appMetadata"?: V2AppInfoAppMetadata;
    readonly "branding"?: V2AppInfoBranding;
    readonly "description"?: string | null;
    readonly "distributionChannel"?: string | null;
    readonly "iconAssets"?: V2AppInfoIconAssets;
    readonly "iconDarkAssets"?: V2AppInfoIconDarkAssets;
    readonly "id": string;
    readonly "installUrl"?: string | null;
    readonly "isAccessible"?: boolean;
    readonly "isEnabled"?: boolean;
    readonly "labels"?: V2AppInfoLabels;
    readonly "logoUrl"?: string | null;
    readonly "logoUrlDark"?: string | null;
    readonly "name": string;
    readonly "pluginDisplayNames"?: V2AppInfoPluginDisplayNames;
    readonly [key: string]: unknown;
};

export type V2AppInfoAppMetadata = V2AppMetadata | null;

export type V2AppInfoBranding = V2AppBranding | null;

export type V2AppInfoIconAssets = {
    readonly [key: string]: unknown;
} | null;

export type V2AppInfoIconDarkAssets = {
    readonly [key: string]: unknown;
} | null;

export type V2AppInfoLabels = {
    readonly [key: string]: unknown;
} | null;

export type V2AppInfoPluginDisplayNames = ReadonlyArray<string>;

export type V2AppListUpdatedNotification = {
    readonly "data": V2AppListUpdatedNotificationData;
    readonly [key: string]: unknown;
};

export type V2AppListUpdatedNotificationData = ReadonlyArray<V2AppInfo>;

export type V2AppMetadata = {
    readonly "categories"?: V2AppMetadataCategories;
    readonly "developer"?: string | null;
    readonly "firstPartyRequiresInstall"?: boolean | null;
    readonly "review"?: V2AppMetadataReview;
    readonly "screenshots"?: V2AppMetadataScreenshots;
    readonly "seoDescription"?: string | null;
    readonly "showInComposerWhenUnlinked"?: boolean | null;
    readonly "subCategories"?: V2AppMetadataSubCategories;
    readonly "version"?: string | null;
    readonly "versionId"?: string | null;
    readonly "versionNotes"?: string | null;
    readonly [key: string]: unknown;
};

export type V2AppMetadataCategories = ReadonlyArray<string> | null;

export type V2AppMetadataReview = V2AppReview | null;

export type V2AppMetadataScreenshots = ReadonlyArray<V2AppScreenshot> | null;

export type V2AppMetadataSubCategories = ReadonlyArray<string> | null;

export type V2AppReview = {
    readonly "status": string;
    readonly [key: string]: unknown;
};

export type V2AppScreenshot = {
    readonly "fileId"?: string | null;
    readonly "url"?: string | null;
    readonly "userPrompt": string;
    readonly [key: string]: unknown;
};

export type V2AppSummary = {
    readonly "category"?: string | null;
    readonly "description"?: string | null;
    readonly "id": string;
    readonly "installUrl"?: string | null;
    readonly "name": string;
    readonly [key: string]: unknown;
};

export type V2AppTemplateSummary = {
    readonly "canonicalConnectorId"?: string | null;
    readonly "category"?: string | null;
    readonly "description"?: string | null;
    readonly "logoUrl"?: string | null;
    readonly "logoUrlDark"?: string | null;
    readonly "materializedAppIds": V2AppTemplateSummaryMaterializedAppIds;
    readonly "name": string;
    readonly "reason"?: V2AppTemplateSummaryReason;
    readonly "templateId": string;
    readonly [key: string]: unknown;
};

export type V2AppTemplateSummaryMaterializedAppIds = ReadonlyArray<string>;

export type V2AppTemplateSummaryReason = V2AppTemplateUnavailableReason | null;

export type V2AppTemplateUnavailableReason = "NOT_CONFIGURED_FOR_WORKSPACE" | "NO_ACTIVE_WORKSPACE";

export type V2AppToolApproval = "auto" | "prompt" | "writes" | "approve";

export type V2AppToolConfig = {
    readonly "approval_mode"?: V2AppToolConfigApprovalMode;
    readonly "enabled"?: boolean | null;
    readonly [key: string]: unknown;
};

export type V2AppToolConfigApprovalMode = V2AppToolApproval | null;

export type V2AppToolSummary = {
    readonly "description": string;
    readonly "disabledReason"?: string | null;
    readonly "isEnabled"?: boolean;
    readonly "isReadOnly"?: boolean;
    readonly "name": string;
    readonly "title"?: string | null;
    readonly [key: string]: unknown;
};

export type V2AppToolsConfig = {
    readonly [key: string]: unknown;
};

export type V2ApprovalsReviewer = "user" | "auto_review" | "guardian_subagent";

export type V2AppsConfig = {
    readonly "_default"?: V2AppsConfigDefault;
    readonly [key: string]: unknown;
};

export type V2AppsConfigDefault = V2AppsDefaultConfig | null;

export type V2AppsDefaultConfig = {
    readonly "approvals_reviewer"?: V2AppsDefaultConfigApprovalsReviewer;
    readonly "default_tools_approval_mode"?: V2AppsDefaultConfigDefaultToolsApprovalMode;
    readonly "destructive_enabled"?: boolean;
    readonly "enabled"?: boolean;
    readonly "open_world_enabled"?: boolean;
    readonly [key: string]: unknown;
};

export type V2AppsDefaultConfigApprovalsReviewer = V2ApprovalsReviewer | null;

export type V2AppsDefaultConfigDefaultToolsApprovalMode = V2AppToolApproval | null;

export type V2AppsInstalledParams = {
    readonly "forceRefresh"?: boolean;
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2AppsInstalledResponse = {
    readonly "apps": V2AppsInstalledResponseApps;
    readonly [key: string]: unknown;
};

export type V2AppsInstalledResponseApps = ReadonlyArray<V2InstalledApp>;

export type V2AppsListParams = {
    readonly "cursor"?: string | null;
    readonly "forceRefetch"?: boolean;
    readonly "limit"?: number | null;
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2AppsListResponse = {
    readonly "data": V2AppsListResponseData;
    readonly "nextCursor"?: string | null;
    readonly [key: string]: unknown;
};

export type V2AppsListResponseData = ReadonlyArray<V2AppInfo>;

export type V2AppsReadParams = {
    readonly "appIds": V2AppsReadParamsAppIds;
    readonly "includeTools"?: boolean;
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2AppsReadParamsAppIds = ReadonlyArray<string>;

export type V2AppsReadResponse = {
    readonly "apps": V2AppsReadResponseApps;
    readonly "missingAppIds": V2AppsReadResponseMissingAppIds;
    readonly [key: string]: unknown;
};

export type V2AppsReadResponseApps = ReadonlyArray<V2ConnectorMetadata>;

export type V2AppsReadResponseMissingAppIds = ReadonlyArray<string>;

export type V2AskForApproval = V2AskForApprovalOneOf1 | V2GranularAskForApproval;

export type V2AskForApprovalOneOf1 = "untrusted" | "on-request" | "never";

export type V2GranularAskForApproval = {
    readonly "granular": V2GranularAskForApprovalGranular;
};

export type V2GranularAskForApprovalGranular = {
    readonly "mcp_elicitations": boolean;
    readonly "request_permissions"?: boolean;
    readonly "rules": boolean;
    readonly "sandbox_approval": boolean;
    readonly "skill_approval"?: boolean;
    readonly [key: string]: unknown;
};

export type V2AuthMode = V2AuthModeOneOf1 | V2AuthModeOneOf2 | V2AuthModeOneOf3 | V2AuthModeOneOf4 | V2AuthModeOneOf5 | V2AuthModeOneOf6 | V2AuthModeOneOf7;

export type V2AuthModeOneOf1 = "apikey";

export type V2AuthModeOneOf2 = "chatgpt";

export type V2AuthModeOneOf3 = "chatgptAuthTokens";

export type V2AuthModeOneOf4 = "headers";

export type V2AuthModeOneOf5 = "agentIdentity";

export type V2AuthModeOneOf6 = "personalAccessToken";

export type V2AuthModeOneOf7 = "bedrockApiKey";

export type V2AutoCompactTokenLimitScope = V2AutoCompactTokenLimitScopeOneOf1 | V2AutoCompactTokenLimitScopeOneOf2;

export type V2AutoCompactTokenLimitScopeOneOf1 = "total";

export type V2AutoCompactTokenLimitScopeOneOf2 = "body_after_prefix";

export type V2AutoReviewDecisionSource = "agent";

export type V2AutoReviewRequirements = {
    readonly "ignoreRules"?: V2AutoReviewRequirementsIgnoreRules;
    readonly "requiredOnModels"?: V2AutoReviewRequirementsRequiredOnModels;
    readonly [key: string]: unknown;
};

export type V2AutoReviewRequirementsIgnoreRules = ReadonlyArray<string> | null;

export type V2AutoReviewRequirementsRequiredOnModels = ReadonlyArray<string> | null;

export type V2BrowserUseAccessApprovalLifetime = "turn" | "thread";

export type V2BrowserUseConfig = {
    readonly "allow_history_access"?: boolean | null;
    readonly "default_origin_policy"?: V2BrowserUseConfigDefaultOriginPolicy;
    readonly "origins"?: V2BrowserUseConfigOrigins;
    readonly [key: string]: unknown;
};

export type V2BrowserUseConfigDefaultOriginPolicy = V2BrowserUseOriginPolicyConfig | null;

export type V2BrowserUseConfigOrigins = {
    readonly [key: string]: unknown;
} | null;

export type V2BrowserUseOriginPolicy = {
    readonly "access"?: V2BrowserUseOriginPolicyAccess;
    readonly "accessApprovalLifetime"?: V2BrowserUseOriginPolicyAccessApprovalLifetime;
    readonly "autoReview"?: V2BrowserUseOriginPolicyAutoReview;
    readonly "downloads"?: V2BrowserUseOriginPolicyDownloads;
    readonly "fullCdpAccess"?: V2BrowserUseOriginPolicyFullCdpAccess;
    readonly "persistentApproval"?: boolean | null;
    readonly "uploads"?: V2BrowserUseOriginPolicyUploads;
    readonly [key: string]: unknown;
};

export type V2BrowserUseOriginPolicyAccess = V2AllowDenyRequirement | null;

export type V2BrowserUseOriginPolicyAccessApprovalLifetime = V2BrowserUseAccessApprovalLifetime | null;

export type V2BrowserUseOriginPolicyAutoReview = V2AllowDenyRequirement | null;

export type V2BrowserUseOriginPolicyDownloads = V2AllowDenyRequirement | null;

export type V2BrowserUseOriginPolicyFullCdpAccess = V2AllowDenyRequirement | null;

export type V2BrowserUseOriginPolicyUploads = V2AllowDenyRequirement | null;

export type V2BrowserUseOriginPolicyConfig = {
    readonly "access"?: V2BrowserUseOriginPolicyConfigAccess;
    readonly "downloads"?: V2BrowserUseOriginPolicyConfigDownloads;
    readonly "full_cdp_access"?: V2BrowserUseOriginPolicyConfigFullCdpAccess;
    readonly "uploads"?: V2BrowserUseOriginPolicyConfigUploads;
    readonly [key: string]: unknown;
};

export type V2BrowserUseOriginPolicyConfigAccess = V2AllowDenyRequirement | null;

export type V2BrowserUseOriginPolicyConfigDownloads = V2AllowDenyRequirement | null;

export type V2BrowserUseOriginPolicyConfigFullCdpAccess = V2AllowDenyRequirement | null;

export type V2BrowserUseOriginPolicyConfigUploads = V2AllowDenyRequirement | null;

export type V2BrowserUseRequirements = {
    readonly "allowGlobalPersistentApproval"?: boolean | null;
    readonly "allowHistoryAccess"?: boolean | null;
    readonly "defaultOriginPolicy"?: V2BrowserUseRequirementsDefaultOriginPolicy;
    readonly "disableAutoReview"?: boolean | null;
    readonly "origins"?: V2BrowserUseRequirementsOrigins;
    readonly [key: string]: unknown;
};

export type V2BrowserUseRequirementsDefaultOriginPolicy = V2BrowserUseOriginPolicy | null;

export type V2BrowserUseRequirementsOrigins = {
    readonly [key: string]: unknown;
} | null;

export type V2ByteRange = {
    readonly "end": number;
    readonly "start": number;
    readonly [key: string]: unknown;
};

export type V2CancelLoginAccountParams = {
    readonly "loginId": string;
    readonly [key: string]: unknown;
};

export type V2CancelLoginAccountResponse = {
    readonly "status": V2CancelLoginAccountStatus;
    readonly [key: string]: unknown;
};

export type V2CancelLoginAccountStatus = "canceled" | "notFound";

export type V2CapabilityRootLocation = V2EnvironmentCapabilityRootLocation;

export type V2EnvironmentCapabilityRootLocation = {
    readonly "environmentId": string;
    readonly "path": string;
    readonly "type": V2EnvironmentCapabilityRootLocationType;
    readonly [key: string]: unknown;
};

export type V2EnvironmentCapabilityRootLocationType = "environment";

export type V2CliAuthCredentialsStoreMode = "file" | "keyring" | "auto" | "ephemeral";

export type V2CodexErrorInfo = V2CodexErrorInfoOneOf1 | V2HttpConnectionFailedCodexErrorInfo | V2ResponseStreamConnectionFailedCodexErrorInfo | V2ResponseStreamDisconnectedCodexErrorInfo | V2ResponseTooManyFailedAttemptsCodexErrorInfo | V2ActiveTurnNotSteerableCodexErrorInfo;

export type V2CodexErrorInfoOneOf1 = "contextWindowExceeded" | "sessionBudgetExceeded" | "usageLimitExceeded" | "serverOverloaded" | "cyberPolicy" | "misalignmentPolicyViolation" | "internalServerError" | "unauthorized" | "badRequest" | "threadRollbackFailed" | "sandboxError" | "other";

export type V2HttpConnectionFailedCodexErrorInfo = {
    readonly "httpConnectionFailed": V2HttpConnectionFailedCodexErrorInfoHttpConnectionFailed;
};

export type V2HttpConnectionFailedCodexErrorInfoHttpConnectionFailed = {
    readonly "httpStatusCode"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ResponseStreamConnectionFailedCodexErrorInfo = {
    readonly "responseStreamConnectionFailed": V2ResponseStreamConnectionFailedCodexErrorInfoResponseStreamConnectionFailed;
};

export type V2ResponseStreamConnectionFailedCodexErrorInfoResponseStreamConnectionFailed = {
    readonly "httpStatusCode"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ResponseStreamDisconnectedCodexErrorInfo = {
    readonly "responseStreamDisconnected": V2ResponseStreamDisconnectedCodexErrorInfoResponseStreamDisconnected;
};

export type V2ResponseStreamDisconnectedCodexErrorInfoResponseStreamDisconnected = {
    readonly "httpStatusCode"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ResponseTooManyFailedAttemptsCodexErrorInfo = {
    readonly "responseTooManyFailedAttempts": V2ResponseTooManyFailedAttemptsCodexErrorInfoResponseTooManyFailedAttempts;
};

export type V2ResponseTooManyFailedAttemptsCodexErrorInfoResponseTooManyFailedAttempts = {
    readonly "httpStatusCode"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ActiveTurnNotSteerableCodexErrorInfo = {
    readonly "activeTurnNotSteerable": V2ActiveTurnNotSteerableCodexErrorInfoActiveTurnNotSteerable;
};

export type V2ActiveTurnNotSteerableCodexErrorInfoActiveTurnNotSteerable = {
    readonly "turnKind": V2NonSteerableTurnKind;
    readonly [key: string]: unknown;
};

export type V2CodexResponseHandoffMode = "thinking" | "commentary" | "bemTags";

export type V2CollabAgentState = {
    readonly "message"?: string | null;
    readonly "status": V2CollabAgentStatus;
    readonly [key: string]: unknown;
};

export type V2CollabAgentStatus = "pendingInit" | "running" | "interrupted" | "completed" | "errored" | "shutdown" | "notFound";

export type V2CollabAgentTool = "spawnAgent" | "sendInput" | "resumeAgent" | "wait" | "closeAgent";

export type V2CollabAgentToolCallStatus = "inProgress" | "completed" | "failed";

export type V2CollaborationMode = {
    readonly "mode": V2ModeKind;
    readonly "settings": V2Settings;
    readonly [key: string]: unknown;
};

export type V2CollaborationModeMask = {
    readonly "mode"?: V2CollaborationModeMaskMode;
    readonly "model"?: string | null;
    readonly "name": string;
    readonly "reasoning_effort"?: V2CollaborationModeMaskReasoningEffort;
    readonly [key: string]: unknown;
};

export type V2CollaborationModeMaskMode = V2ModeKind | null;

export type V2CollaborationModeMaskReasoningEffort = V2CollaborationModeMaskReasoningEffortAnyOf1 | null;

export type V2CollaborationModeMaskReasoningEffortAnyOf1 = V2ReasoningEffort | null;

export type V2CommandAction = V2ReadCommandAction | V2ListFilesCommandAction | V2SearchCommandAction | V2UnknownCommandAction;

export type V2ReadCommandAction = {
    readonly "command": string;
    readonly "name": string;
    readonly "path": V2LegacyAppPathString;
    readonly "type": V2ReadCommandActionType;
    readonly [key: string]: unknown;
};

export type V2ReadCommandActionType = "read";

export type V2ListFilesCommandAction = {
    readonly "command": string;
    readonly "path"?: string | null;
    readonly "type": V2ListFilesCommandActionType;
    readonly [key: string]: unknown;
};

export type V2ListFilesCommandActionType = "listFiles";

export type V2SearchCommandAction = {
    readonly "command": string;
    readonly "path"?: string | null;
    readonly "query"?: string | null;
    readonly "type": V2SearchCommandActionType;
    readonly [key: string]: unknown;
};

export type V2SearchCommandActionType = "search";

export type V2UnknownCommandAction = {
    readonly "command": string;
    readonly "type": V2UnknownCommandActionType;
    readonly [key: string]: unknown;
};

export type V2UnknownCommandActionType = "unknown";

export type V2CommandExecOutputDeltaNotification = {
    readonly "capReached": boolean;
    readonly "deltaBase64": string;
    readonly "processId": string;
    readonly "stream": V2CommandExecOutputDeltaNotificationStream;
    readonly [key: string]: unknown;
};

export type V2CommandExecOutputDeltaNotificationStream = V2CommandExecOutputStream;

export type V2CommandExecOutputStream = V2CommandExecOutputStreamOneOf1 | V2CommandExecOutputStreamOneOf2;

export type V2CommandExecOutputStreamOneOf1 = "stdout";

export type V2CommandExecOutputStreamOneOf2 = "stderr";

export type V2CommandExecParams = {
    readonly "command": V2CommandExecParamsCommand;
    readonly "cwd"?: string | null;
    readonly "disableOutputCap"?: boolean;
    readonly "disableTimeout"?: boolean;
    readonly "env"?: V2CommandExecParamsEnv;
    readonly "outputBytesCap"?: number | null;
    readonly "processId"?: string | null;
    readonly "sandboxPolicy"?: V2CommandExecParamsSandboxPolicy;
    readonly "size"?: V2CommandExecParamsSize;
    readonly "streamStdin"?: boolean;
    readonly "streamStdoutStderr"?: boolean;
    readonly "timeoutMs"?: number | null;
    readonly "tty"?: boolean;
    readonly [key: string]: unknown;
};

export type V2CommandExecParamsCommand = ReadonlyArray<string>;

export type V2CommandExecParamsEnv = {
    readonly [key: string]: unknown;
} | null;

export type V2CommandExecParamsSandboxPolicy = V2SandboxPolicy | null;

export type V2CommandExecParamsSize = V2CommandExecTerminalSize | null;

export type V2CommandExecResizeParams = {
    readonly "processId": string;
    readonly "size": V2CommandExecResizeParamsSize;
    readonly [key: string]: unknown;
};

export type V2CommandExecResizeParamsSize = V2CommandExecTerminalSize;

export type V2CommandExecResizeResponse = {
    readonly [key: string]: unknown;
};

export type V2CommandExecResponse = {
    readonly "exitCode": number;
    readonly "stderr": string;
    readonly "stdout": string;
    readonly [key: string]: unknown;
};

export type V2CommandExecTerminalSize = {
    readonly "cols": number;
    readonly "rows": number;
    readonly [key: string]: unknown;
};

export type V2CommandExecTerminateParams = {
    readonly "processId": string;
    readonly [key: string]: unknown;
};

export type V2CommandExecTerminateResponse = {
    readonly [key: string]: unknown;
};

export type V2CommandExecWriteParams = {
    readonly "closeStdin"?: boolean;
    readonly "deltaBase64"?: string | null;
    readonly "processId": string;
    readonly [key: string]: unknown;
};

export type V2CommandExecWriteResponse = {
    readonly [key: string]: unknown;
};

export type V2CommandExecutionOutputDeltaNotification = {
    readonly "delta": string;
    readonly "itemId": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2CommandExecutionSource = "agent" | "userShell" | "unifiedExecStartup" | "unifiedExecInteraction";

export type V2CommandExecutionStatus = "inProgress" | "completed" | "failed" | "declined";

export type V2CommandMigration = {
    readonly "name": string;
    readonly [key: string]: unknown;
};

export type V2ComputerUseConfig = {
    readonly "default_app_access"?: V2ComputerUseConfigDefaultAppAccess;
    readonly "macos"?: V2ComputerUseConfigMacos;
    readonly "windows"?: V2ComputerUseConfigWindows;
    readonly [key: string]: unknown;
};

export type V2ComputerUseConfigDefaultAppAccess = V2AllowDenyRequirement | null;

export type V2ComputerUseConfigMacos = V2ComputerUseMacosConfig | null;

export type V2ComputerUseConfigWindows = V2ComputerUseWindowsConfig | null;

export type V2ComputerUseMacosConfig = {
    readonly "bundle_ids"?: V2ComputerUseMacosConfigBundleIds;
    readonly [key: string]: unknown;
};

export type V2ComputerUseMacosConfigBundleIds = {
    readonly [key: string]: unknown;
} | null;

export type V2ComputerUseMacosRequirements = {
    readonly "bundleIds"?: V2ComputerUseMacosRequirementsBundleIds;
    readonly [key: string]: unknown;
};

export type V2ComputerUseMacosRequirementsBundleIds = {
    readonly [key: string]: unknown;
} | null;

export type V2ComputerUseRequirements = {
    readonly "allowLockedComputerUse"?: boolean | null;
    readonly "allowPersistentApproval"?: boolean | null;
    readonly "defaultAppAccess"?: V2ComputerUseRequirementsDefaultAppAccess;
    readonly "macos"?: V2ComputerUseRequirementsMacos;
    readonly "windows"?: V2ComputerUseRequirementsWindows;
    readonly [key: string]: unknown;
};

export type V2ComputerUseRequirementsDefaultAppAccess = V2AllowDenyRequirement | null;

export type V2ComputerUseRequirementsMacos = V2ComputerUseMacosRequirements | null;

export type V2ComputerUseRequirementsWindows = V2ComputerUseWindowsRequirements | null;

export type V2ComputerUseWindowsConfig = {
    readonly "aumids"?: V2ComputerUseWindowsConfigAumids;
    readonly "exes"?: V2ComputerUseWindowsConfigExes;
    readonly [key: string]: unknown;
};

export type V2ComputerUseWindowsConfigAumids = {
    readonly [key: string]: unknown;
} | null;

export type V2ComputerUseWindowsConfigExes = ReadonlyArray<V2ComputerUseWindowsExeConfig> | null;

export type V2ComputerUseWindowsExeConfig = {
    readonly "access": V2AllowDenyRequirement;
    readonly "binary_name"?: string | null;
    readonly "product_name": string;
    readonly "publisher_name": string;
    readonly [key: string]: unknown;
};

export type V2ComputerUseWindowsExeRequirement = {
    readonly "access": V2AllowDenyRequirement;
    readonly "binaryName"?: string | null;
    readonly "productName": string;
    readonly "publisherName": string;
    readonly [key: string]: unknown;
};

export type V2ComputerUseWindowsRequirements = {
    readonly "aumids"?: V2ComputerUseWindowsRequirementsAumids;
    readonly "exes"?: V2ComputerUseWindowsRequirementsExes;
    readonly [key: string]: unknown;
};

export type V2ComputerUseWindowsRequirementsAumids = {
    readonly [key: string]: unknown;
} | null;

export type V2ComputerUseWindowsRequirementsExes = ReadonlyArray<V2ComputerUseWindowsExeRequirement> | null;

export type V2Config = {
    readonly "analytics"?: V2ConfigAnalytics;
    readonly "approval_policy"?: V2ConfigApprovalPolicy;
    readonly "approvals_reviewer"?: V2ConfigApprovalsReviewer;
    readonly "browser_use"?: V2ConfigBrowserUse;
    readonly "compact_prompt"?: string | null;
    readonly "computer_use"?: V2ConfigComputerUse;
    readonly "desktop"?: V2ConfigDesktop;
    readonly "developer_instructions"?: string | null;
    readonly "forced_chatgpt_workspace_id"?: V2ConfigForcedChatgptWorkspaceId;
    readonly "forced_login_method"?: V2ConfigForcedLoginMethod;
    readonly "instructions"?: string | null;
    readonly "model"?: string | null;
    readonly "model_auto_compact_token_limit"?: number | null;
    readonly "model_auto_compact_token_limit_scope"?: V2ConfigModelAutoCompactTokenLimitScope;
    readonly "model_context_window"?: number | null;
    readonly "model_provider"?: string | null;
    readonly "model_reasoning_effort"?: V2ConfigModelReasoningEffort;
    readonly "model_reasoning_summary"?: V2ConfigModelReasoningSummary;
    readonly "model_verbosity"?: V2ConfigModelVerbosity;
    readonly "review_model"?: string | null;
    readonly "sandbox_mode"?: V2ConfigSandboxMode;
    readonly "sandbox_workspace_write"?: V2ConfigSandboxWorkspaceWrite;
    readonly "service_tier"?: string | null;
    readonly "tools"?: V2ConfigTools;
    readonly "web_search"?: V2ConfigWebSearch;
    readonly [key: string]: unknown;
};

export type V2ConfigAnalytics = V2AnalyticsConfig | null;

export type V2ConfigApprovalPolicy = V2AskForApproval | null;

export type V2ConfigApprovalsReviewer = V2ApprovalsReviewer | null;

export type V2ConfigBrowserUse = V2BrowserUseConfig | null;

export type V2ConfigComputerUse = V2ComputerUseConfig | null;

export type V2ConfigDesktop = {
    readonly [key: string]: unknown;
} | null;

export type V2ConfigForcedChatgptWorkspaceId = V2ForcedChatgptWorkspaceIds | null;

export type V2ConfigForcedLoginMethod = V2ForcedLoginMethod | null;

export type V2ConfigModelAutoCompactTokenLimitScope = V2AutoCompactTokenLimitScope | null;

export type V2ConfigModelReasoningEffort = V2ReasoningEffort | null;

export type V2ConfigModelReasoningSummary = V2ReasoningSummary | null;

export type V2ConfigModelVerbosity = V2Verbosity | null;

export type V2ConfigSandboxMode = V2SandboxMode | null;

export type V2ConfigSandboxWorkspaceWrite = V2SandboxWorkspaceWrite | null;

export type V2ConfigTools = V2ToolsV2 | null;

export type V2ConfigWebSearch = V2WebSearchMode | null;

export type V2ConfigBatchWriteParams = {
    readonly "edits": V2ConfigBatchWriteParamsEdits;
    readonly "expectedVersion"?: string | null;
    readonly "filePath"?: string | null;
    readonly "reloadUserConfig"?: boolean;
    readonly [key: string]: unknown;
};

export type V2ConfigBatchWriteParamsEdits = ReadonlyArray<V2ConfigEdit>;

export type V2ConfigEdit = {
    readonly "keyPath": string;
    readonly "mergeStrategy": V2MergeStrategy;
    readonly "value": unknown;
    readonly [key: string]: unknown;
};

export type V2ConfigLayer = {
    readonly "config": unknown;
    readonly "disabledReason"?: string | null;
    readonly "name": V2ConfigLayerSource;
    readonly "version": string;
    readonly [key: string]: unknown;
};

export type V2ConfigLayerMetadata = {
    readonly "name": V2ConfigLayerSource;
    readonly "version": string;
    readonly [key: string]: unknown;
};

export type V2ConfigLayerSource = V2PackagedDefaultsConfigLayerSource | V2MdmConfigLayerSource | V2SystemConfigLayerSource | V2EnterpriseManagedConfigLayerSource | V2UserConfigLayerSource | V2ProjectConfigLayerSource | V2SessionFlagsConfigLayerSource | V2LegacyManagedConfigTomlFromFileConfigLayerSource | V2LegacyManagedConfigTomlFromMdmConfigLayerSource;

export type V2PackagedDefaultsConfigLayerSource = {
    readonly "file": V2PackagedDefaultsConfigLayerSourceFile;
    readonly "type": V2PackagedDefaultsConfigLayerSourceType;
    readonly [key: string]: unknown;
};

export type V2PackagedDefaultsConfigLayerSourceFile = V2AbsolutePathBuf;

export type V2PackagedDefaultsConfigLayerSourceType = "packagedDefaults";

export type V2MdmConfigLayerSource = {
    readonly "domain": string;
    readonly "key": string;
    readonly "type": V2MdmConfigLayerSourceType;
    readonly [key: string]: unknown;
};

export type V2MdmConfigLayerSourceType = "mdm";

export type V2SystemConfigLayerSource = {
    readonly "file": V2SystemConfigLayerSourceFile;
    readonly "type": V2SystemConfigLayerSourceType;
    readonly [key: string]: unknown;
};

export type V2SystemConfigLayerSourceFile = V2AbsolutePathBuf;

export type V2SystemConfigLayerSourceType = "system";

export type V2EnterpriseManagedConfigLayerSource = {
    readonly "id": string;
    readonly "name": string;
    readonly "type": V2EnterpriseManagedConfigLayerSourceType;
    readonly [key: string]: unknown;
};

export type V2EnterpriseManagedConfigLayerSourceType = "enterpriseManaged";

export type V2UserConfigLayerSource = {
    readonly "file": V2UserConfigLayerSourceFile;
    readonly "profile"?: string | null;
    readonly "type": V2UserConfigLayerSourceType;
    readonly [key: string]: unknown;
};

export type V2UserConfigLayerSourceFile = V2AbsolutePathBuf;

export type V2UserConfigLayerSourceType = "user";

export type V2ProjectConfigLayerSource = {
    readonly "dotCodexFolder": V2AbsolutePathBuf;
    readonly "type": V2ProjectConfigLayerSourceType;
    readonly [key: string]: unknown;
};

export type V2ProjectConfigLayerSourceType = "project";

export type V2SessionFlagsConfigLayerSource = {
    readonly "type": V2SessionFlagsConfigLayerSourceType;
    readonly [key: string]: unknown;
};

export type V2SessionFlagsConfigLayerSourceType = "sessionFlags";

export type V2LegacyManagedConfigTomlFromFileConfigLayerSource = {
    readonly "file": V2AbsolutePathBuf;
    readonly "type": V2LegacyManagedConfigTomlFromFileConfigLayerSourceType;
    readonly [key: string]: unknown;
};

export type V2LegacyManagedConfigTomlFromFileConfigLayerSourceType = "legacyManagedConfigTomlFromFile";

export type V2LegacyManagedConfigTomlFromMdmConfigLayerSource = {
    readonly "type": V2LegacyManagedConfigTomlFromMdmConfigLayerSourceType;
    readonly [key: string]: unknown;
};

export type V2LegacyManagedConfigTomlFromMdmConfigLayerSourceType = "legacyManagedConfigTomlFromMdm";

export type V2ConfigReadParams = {
    readonly "cwd"?: string | null;
    readonly "includeLayers"?: boolean;
    readonly [key: string]: unknown;
};

export type V2ConfigReadResponse = {
    readonly "config": V2Config;
    readonly "layers"?: V2ConfigReadResponseLayers;
    readonly "origins": V2ConfigReadResponseOrigins;
    readonly [key: string]: unknown;
};

export type V2ConfigReadResponseLayers = ReadonlyArray<V2ConfigLayer> | null;

export type V2ConfigReadResponseOrigins = {
    readonly [key: string]: unknown;
};

export type V2ConfigRequirements = {
    readonly "additionalDeveloperInstructions"?: string | null;
    readonly "allowAppshots"?: boolean | null;
    readonly "allowBrowserAndComputerUse"?: boolean | null;
    readonly "allowLoginShell"?: boolean | null;
    readonly "allowManagedHooksOnly"?: boolean | null;
    readonly "allowRemoteControl"?: boolean | null;
    readonly "allowedApprovalPolicies"?: V2ConfigRequirementsAllowedApprovalPolicies;
    readonly "allowedPermissionProfiles"?: V2ConfigRequirementsAllowedPermissionProfiles;
    readonly "allowedSandboxModes"?: V2ConfigRequirementsAllowedSandboxModes;
    readonly "allowedWebSearchModes"?: V2ConfigRequirementsAllowedWebSearchModes;
    readonly "allowedWindowsSandboxImplementations"?: V2ConfigRequirementsAllowedWindowsSandboxImplementations;
    readonly "autoReview"?: V2ConfigRequirementsAutoReview;
    readonly "browserUse"?: V2ConfigRequirementsBrowserUse;
    readonly "chatgptBaseUrl"?: string | null;
    readonly "checkForUpdateOnStartup"?: boolean | null;
    readonly "cliAuthCredentialsStore"?: V2ConfigRequirementsCliAuthCredentialsStore;
    readonly "computerUse"?: V2ConfigRequirementsComputerUse;
    readonly "defaultPermissions"?: string | null;
    readonly "enforceResidency"?: V2ConfigRequirementsEnforceResidency;
    readonly "featureRequirements"?: V2ConfigRequirementsFeatureRequirements;
    readonly "feedback"?: V2ConfigRequirementsFeedback;
    readonly "inAppBrowser"?: V2ConfigRequirementsInAppBrowser;
    readonly "logDir"?: string | null;
    readonly "modelCatalogJson"?: string | null;
    readonly "models"?: V2ConfigRequirementsModels;
    readonly "sqliteHome"?: string | null;
    readonly "windowsSandboxPrivateDesktop"?: boolean | null;
    readonly [key: string]: unknown;
};

export type V2ConfigRequirementsAllowedApprovalPolicies = ReadonlyArray<V2AskForApproval> | null;

export type V2ConfigRequirementsAllowedPermissionProfiles = {
    readonly [key: string]: unknown;
} | null;

export type V2ConfigRequirementsAllowedSandboxModes = ReadonlyArray<V2SandboxMode> | null;

export type V2ConfigRequirementsAllowedWebSearchModes = ReadonlyArray<V2WebSearchMode> | null;

export type V2ConfigRequirementsAllowedWindowsSandboxImplementations = ReadonlyArray<V2WindowsSandboxSetupMode> | null;

export type V2ConfigRequirementsAutoReview = V2AutoReviewRequirements | null;

export type V2ConfigRequirementsBrowserUse = V2BrowserUseRequirements | null;

export type V2ConfigRequirementsCliAuthCredentialsStore = V2CliAuthCredentialsStoreMode | null;

export type V2ConfigRequirementsComputerUse = V2ComputerUseRequirements | null;

export type V2ConfigRequirementsEnforceResidency = V2ResidencyRequirement | null;

export type V2ConfigRequirementsFeatureRequirements = {
    readonly [key: string]: unknown;
} | null;

export type V2ConfigRequirementsFeedback = V2FeedbackRequirements | null;

export type V2ConfigRequirementsInAppBrowser = V2InAppBrowserRequirements | null;

export type V2ConfigRequirementsModels = V2ModelsRequirements | null;

export type V2ConfigRequirementsReadResponse = {
    readonly "requirements"?: V2ConfigRequirementsReadResponseRequirements;
    readonly [key: string]: unknown;
};

export type V2ConfigRequirementsReadResponseRequirements = V2ConfigRequirements | null;

export type V2ConfigValueWriteParams = {
    readonly "expectedVersion"?: string | null;
    readonly "filePath"?: string | null;
    readonly "keyPath": string;
    readonly "mergeStrategy": V2MergeStrategy;
    readonly "value": unknown;
    readonly [key: string]: unknown;
};

export type V2ConfigWarningNotification = {
    readonly "details"?: string | null;
    readonly "path"?: string | null;
    readonly "range"?: V2ConfigWarningNotificationRange;
    readonly "summary": string;
    readonly [key: string]: unknown;
};

export type V2ConfigWarningNotificationRange = V2TextRange | null;

export type V2ConfigWriteResponse = {
    readonly "filePath": V2ConfigWriteResponseFilePath;
    readonly "overriddenMetadata"?: V2ConfigWriteResponseOverriddenMetadata;
    readonly "status": V2WriteStatus;
    readonly "version": string;
    readonly [key: string]: unknown;
};

export type V2ConfigWriteResponseFilePath = V2AbsolutePathBuf;

export type V2ConfigWriteResponseOverriddenMetadata = V2OverriddenMetadata | null;

export type V2ConfiguredHookHandler = V2CommandConfiguredHookHandler | V2McpToolConfiguredHookHandler | V2PromptConfiguredHookHandler | V2AgentConfiguredHookHandler;

export type V2CommandConfiguredHookHandler = {
    readonly "additionalContextLimit"?: number | null;
    readonly "async": boolean;
    readonly "command": string;
    readonly "commandWindows"?: string | null;
    readonly "statusMessage"?: string | null;
    readonly "timeoutSec"?: number | null;
    readonly "type": V2CommandConfiguredHookHandlerType;
    readonly [key: string]: unknown;
};

export type V2CommandConfiguredHookHandlerType = "command";

export type V2McpToolConfiguredHookHandler = {
    readonly "input": V2McpToolConfiguredHookHandlerInput;
    readonly "server": string;
    readonly "statusMessage"?: string | null;
    readonly "timeoutSec"?: number | null;
    readonly "tool": string;
    readonly "type": V2McpToolConfiguredHookHandlerType;
    readonly [key: string]: unknown;
};

export type V2McpToolConfiguredHookHandlerInput = {
    readonly [key: string]: unknown;
};

export type V2McpToolConfiguredHookHandlerType = "mcp_tool";

export type V2PromptConfiguredHookHandler = {
    readonly "type": V2PromptConfiguredHookHandlerType;
    readonly [key: string]: unknown;
};

export type V2PromptConfiguredHookHandlerType = "prompt";

export type V2AgentConfiguredHookHandler = {
    readonly "type": V2AgentConfiguredHookHandlerType;
    readonly [key: string]: unknown;
};

export type V2AgentConfiguredHookHandlerType = "agent";

export type V2ConfiguredHookMatcherGroup = {
    readonly "hooks": V2ConfiguredHookMatcherGroupHooks;
    readonly "matcher"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ConfiguredHookMatcherGroupHooks = ReadonlyArray<V2ConfiguredHookHandler>;

export type V2ConnectorMetadata = {
    readonly "description"?: string | null;
    readonly "distributionChannel"?: string | null;
    readonly "iconUrl"?: string | null;
    readonly "iconUrlDark"?: string | null;
    readonly "id": string;
    readonly "installUrl"?: string | null;
    readonly "name": string;
    readonly "pluginDisplayNames"?: V2ConnectorMetadataPluginDisplayNames;
    readonly "toolSummaries"?: V2ConnectorMetadataToolSummaries;
    readonly [key: string]: unknown;
};

export type V2ConnectorMetadataPluginDisplayNames = ReadonlyArray<string>;

export type V2ConnectorMetadataToolSummaries = ReadonlyArray<V2AppToolSummary> | null;

export type V2ConsumeAccountRateLimitResetCreditOutcome = V2ConsumeAccountRateLimitResetCreditOutcomeOneOf1 | V2ConsumeAccountRateLimitResetCreditOutcomeOneOf2 | V2ConsumeAccountRateLimitResetCreditOutcomeOneOf3 | V2ConsumeAccountRateLimitResetCreditOutcomeOneOf4;

export type V2ConsumeAccountRateLimitResetCreditOutcomeOneOf1 = "reset";

export type V2ConsumeAccountRateLimitResetCreditOutcomeOneOf2 = "nothingToReset";

export type V2ConsumeAccountRateLimitResetCreditOutcomeOneOf3 = "noCredit";

export type V2ConsumeAccountRateLimitResetCreditOutcomeOneOf4 = "alreadyRedeemed";

export type V2ConsumeAccountRateLimitResetCreditParams = {
    readonly "creditId"?: string | null;
    readonly "idempotencyKey": string;
    readonly [key: string]: unknown;
};

export type V2ConsumeAccountRateLimitResetCreditResponse = {
    readonly "outcome": V2ConsumeAccountRateLimitResetCreditOutcome;
    readonly [key: string]: unknown;
};

export type V2ContentItem = V2InputTextContentItem | V2InputImageContentItem | V2InputAudioContentItem | V2OutputTextContentItem;

export type V2InputTextContentItem = {
    readonly "text": string;
    readonly "type": V2InputTextContentItemType;
    readonly [key: string]: unknown;
};

export type V2InputTextContentItemType = "input_text";

export type V2InputImageContentItem = {
    readonly "detail"?: V2InputImageContentItemDetail;
    readonly "image_url": string;
    readonly "type": V2InputImageContentItemType;
    readonly [key: string]: unknown;
};

export type V2InputImageContentItemDetail = V2ImageDetail | null;

export type V2InputImageContentItemType = "input_image";

export type V2InputAudioContentItem = {
    readonly "audio_url": string;
    readonly "type": V2InputAudioContentItemType;
    readonly [key: string]: unknown;
};

export type V2InputAudioContentItemType = "input_audio";

export type V2OutputTextContentItem = {
    readonly "text": string;
    readonly "type": V2OutputTextContentItemType;
    readonly [key: string]: unknown;
};

export type V2OutputTextContentItemType = "output_text";

export type V2ContextCompactedNotification = {
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2ConversationTextRole = "user" | "developer" | "assistant";

export type V2CreditsSnapshot = {
    readonly "balance"?: string | null;
    readonly "hasCredits": boolean;
    readonly "unlimited": boolean;
    readonly [key: string]: unknown;
};

export type V2DeprecationNoticeNotification = {
    readonly "details"?: string | null;
    readonly "summary": string;
    readonly [key: string]: unknown;
};

export type V2DesktopOnboardingEntrypoint = "life_sciences";

export type V2DynamicToolCallOutputContentItem = V2InputTextDynamicToolCallOutputContentItem | V2InputImageDynamicToolCallOutputContentItem | V2InputAudioDynamicToolCallOutputContentItem;

export type V2InputTextDynamicToolCallOutputContentItem = {
    readonly "text": string;
    readonly "type": V2InputTextDynamicToolCallOutputContentItemType;
    readonly [key: string]: unknown;
};

export type V2InputTextDynamicToolCallOutputContentItemType = "inputText";

export type V2InputImageDynamicToolCallOutputContentItem = {
    readonly "imageUrl": string;
    readonly "type": V2InputImageDynamicToolCallOutputContentItemType;
    readonly [key: string]: unknown;
};

export type V2InputImageDynamicToolCallOutputContentItemType = "inputImage";

export type V2InputAudioDynamicToolCallOutputContentItem = {
    readonly "audioUrl": string;
    readonly "type": V2InputAudioDynamicToolCallOutputContentItemType;
    readonly [key: string]: unknown;
};

export type V2InputAudioDynamicToolCallOutputContentItemType = "inputAudio";

export type V2DynamicToolCallStatus = "inProgress" | "completed" | "failed";

export type V2DynamicToolNamespaceTool = V2FunctionDynamicToolNamespaceTool;

export type V2FunctionDynamicToolNamespaceTool = {
    readonly "deferLoading"?: boolean;
    readonly "description": string;
    readonly "inputSchema": unknown;
    readonly "name": string;
    readonly "type": V2FunctionDynamicToolNamespaceToolType;
    readonly [key: string]: unknown;
};

export type V2FunctionDynamicToolNamespaceToolType = "function";

export type V2DynamicToolSpec = V2FunctionDynamicToolSpec | V2NamespaceDynamicToolSpec;

export type V2FunctionDynamicToolSpec = {
    readonly "deferLoading"?: boolean;
    readonly "description": string;
    readonly "inputSchema": unknown;
    readonly "name": string;
    readonly "type": V2FunctionDynamicToolSpecType;
    readonly [key: string]: unknown;
};

export type V2FunctionDynamicToolSpecType = "function";

export type V2NamespaceDynamicToolSpec = {
    readonly "description": string;
    readonly "name": string;
    readonly "tools": V2NamespaceDynamicToolSpecTools;
    readonly "type": V2NamespaceDynamicToolSpecType;
    readonly [key: string]: unknown;
};

export type V2NamespaceDynamicToolSpecTools = ReadonlyArray<V2DynamicToolNamespaceTool>;

export type V2NamespaceDynamicToolSpecType = "namespace";

export type V2EnvironmentConnectionNotification = {
    readonly "environmentId": string;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ErrorNotification = {
    readonly "error": V2TurnError;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly "willRetry": boolean;
    readonly [key: string]: unknown;
};

export type V2ExperimentalFeature = {
    readonly "announcement"?: string | null;
    readonly "defaultEnabled": boolean;
    readonly "description"?: string | null;
    readonly "displayName"?: string | null;
    readonly "enabled": boolean;
    readonly "name": string;
    readonly "stage": V2ExperimentalFeatureStage2;
    readonly [key: string]: unknown;
};

export type V2ExperimentalFeatureStage2 = V2ExperimentalFeatureStage;

export type V2ExperimentalFeatureEnablementSetParams = {
    readonly "enablement": V2ExperimentalFeatureEnablementSetParamsEnablement;
    readonly [key: string]: unknown;
};

export type V2ExperimentalFeatureEnablementSetParamsEnablement = {
    readonly [key: string]: unknown;
};

export type V2ExperimentalFeatureEnablementSetResponse = {
    readonly "enablement": V2ExperimentalFeatureEnablementSetResponseEnablement;
    readonly [key: string]: unknown;
};

export type V2ExperimentalFeatureEnablementSetResponseEnablement = {
    readonly [key: string]: unknown;
};

export type V2ExperimentalFeatureListParams = {
    readonly "cursor"?: string | null;
    readonly "limit"?: number | null;
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ExperimentalFeatureListResponse = {
    readonly "data": V2ExperimentalFeatureListResponseData;
    readonly "nextCursor"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ExperimentalFeatureListResponseData = ReadonlyArray<V2ExperimentalFeature>;

export type V2ExperimentalFeatureStage = V2ExperimentalFeatureStageOneOf1 | V2ExperimentalFeatureStageOneOf2 | V2ExperimentalFeatureStageOneOf3 | V2ExperimentalFeatureStageOneOf4 | V2ExperimentalFeatureStageOneOf5;

export type V2ExperimentalFeatureStageOneOf1 = "beta";

export type V2ExperimentalFeatureStageOneOf2 = "underDevelopment";

export type V2ExperimentalFeatureStageOneOf3 = "stable";

export type V2ExperimentalFeatureStageOneOf4 = "deprecated";

export type V2ExperimentalFeatureStageOneOf5 = "removed";

export type V2ExternalAgentConfigDetectParams = {
    readonly "cwds"?: V2ExternalAgentConfigDetectParamsCwds;
    readonly "includeHome"?: boolean;
    readonly "maxSessionAgeDays"?: number | null;
    readonly "maxSessions"?: number | null;
    readonly "migrationSource"?: string | null;
    readonly "source"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigDetectParamsCwds = ReadonlyArray<string> | null;

export type V2ExternalAgentConfigDetectResponse = {
    readonly "connectors"?: V2ExternalAgentConfigDetectResponseConnectors;
    readonly "items": V2ExternalAgentConfigDetectResponseItems;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigDetectResponseConnectors = ReadonlyArray<V2ExternalAgentDetectedConnectorCandidate>;

export type V2ExternalAgentConfigDetectResponseItems = ReadonlyArray<V2ExternalAgentConfigMigrationItem>;

export type V2ExternalAgentConfigImportCompletedNotification = {
    readonly "importId": string;
    readonly "itemTypeResults": V2ExternalAgentConfigImportCompletedNotificationItemTypeResults;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportCompletedNotificationItemTypeResults = ReadonlyArray<V2ExternalAgentConfigImportTypeResult>;

export type V2ExternalAgentConfigImportHistoriesReadResponse = {
    readonly "connectors": V2ExternalAgentConfigImportHistoriesReadResponseConnectors;
    readonly "data": V2ExternalAgentConfigImportHistoriesReadResponseData;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportHistoriesReadResponseConnectors = ReadonlyArray<V2ExternalAgentImportedConnectorCandidate>;

export type V2ExternalAgentConfigImportHistoriesReadResponseData = ReadonlyArray<V2ExternalAgentConfigImportHistory>;

export type V2ExternalAgentConfigImportHistory = {
    readonly "completedAtMs": number;
    readonly "failures": V2ExternalAgentConfigImportHistoryFailures;
    readonly "importId": string;
    readonly "providerId"?: string | null;
    readonly "successes": V2ExternalAgentConfigImportHistorySuccesses;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportHistoryFailures = ReadonlyArray<V2ExternalAgentConfigImportItemTypeFailure>;

export type V2ExternalAgentConfigImportHistorySuccesses = ReadonlyArray<V2ExternalAgentConfigImportItemTypeSuccess>;

export type V2ExternalAgentConfigImportHistoryRecordParams = {
    readonly "itemTypeResults": V2ExternalAgentConfigImportHistoryRecordParamsItemTypeResults;
    readonly "providerId": string;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportHistoryRecordParamsItemTypeResults = ReadonlyArray<V2ExternalAgentConfigImportHistoryRecordTypeResultParams>;

export type V2ExternalAgentConfigImportHistoryRecordResponse = {
    readonly "importId": string;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportHistoryRecordSuccessParams = {
    readonly "cwd"?: string | null;
    readonly "itemType": V2ExternalAgentConfigMigrationItemType;
    readonly "source"?: string | null;
    readonly "target"?: string | null;
    readonly "title"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportHistoryRecordTypeResultParams = {
    readonly "failures": V2ExternalAgentConfigImportHistoryRecordTypeResultParamsFailures;
    readonly "itemType": V2ExternalAgentConfigMigrationItemType;
    readonly "successes": V2ExternalAgentConfigImportHistoryRecordTypeResultParamsSuccesses;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportHistoryRecordTypeResultParamsFailures = ReadonlyArray<V2ExternalAgentConfigImportItemTypeFailure>;

export type V2ExternalAgentConfigImportHistoryRecordTypeResultParamsSuccesses = ReadonlyArray<V2ExternalAgentConfigImportHistoryRecordSuccessParams>;

export type V2ExternalAgentConfigImportItemTypeFailure = {
    readonly "cwd"?: string | null;
    readonly "errorType"?: string | null;
    readonly "failureStage": string;
    readonly "itemType": V2ExternalAgentConfigMigrationItemType;
    readonly "message": string;
    readonly "source"?: string | null;
    readonly "subErrorType"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportItemTypeSuccess = {
    readonly "cwd"?: string | null;
    readonly "itemType": V2ExternalAgentConfigMigrationItemType;
    readonly "source"?: string | null;
    readonly "target"?: string | null;
    readonly "title"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportParams = {
    readonly "migrationItems": V2ExternalAgentConfigImportParamsMigrationItems;
    readonly "migrationSource"?: string | null;
    readonly "providerId"?: string | null;
    readonly "source"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportParamsMigrationItems = ReadonlyArray<V2ExternalAgentConfigMigrationItem>;

export type V2ExternalAgentConfigImportProgressNotification = {
    readonly "importId": string;
    readonly "itemTypeResults": V2ExternalAgentConfigImportProgressNotificationItemTypeResults;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportProgressNotificationItemTypeResults = ReadonlyArray<V2ExternalAgentConfigImportTypeResult>;

export type V2ExternalAgentConfigImportResponse = {
    readonly "importId": string;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportTypeResult = {
    readonly "failures": V2ExternalAgentConfigImportTypeResultFailures;
    readonly "itemType": V2ExternalAgentConfigMigrationItemType;
    readonly "successes": V2ExternalAgentConfigImportTypeResultSuccesses;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigImportTypeResultFailures = ReadonlyArray<V2ExternalAgentConfigImportItemTypeFailure>;

export type V2ExternalAgentConfigImportTypeResultSuccesses = ReadonlyArray<V2ExternalAgentConfigImportItemTypeSuccess>;

export type V2ExternalAgentConfigMigrationItem = {
    readonly "cwd"?: string | null;
    readonly "description": string;
    readonly "details"?: V2ExternalAgentConfigMigrationItemDetails;
    readonly "itemType": V2ExternalAgentConfigMigrationItemType;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentConfigMigrationItemDetails = V2MigrationDetails | null;

export type V2ExternalAgentConfigMigrationItemType = "AGENTS_MD" | "CONFIG" | "SKILLS" | "PLUGINS" | "MCP_SERVER_CONFIG" | "SUBAGENTS" | "HOOKS" | "COMMANDS" | "MEMORY" | "SESSIONS";

export type V2ExternalAgentDetectedConnectorCandidate = {
    readonly "name": string;
    readonly "sessionCount": number;
    readonly "source": V2ExternalAgentDetectedConnectorSource;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentDetectedConnectorSource = "remoteMcpServersConfig" | "sessionToolUse";

export type V2ExternalAgentImportedConnectorCandidate = {
    readonly "name": string;
    readonly "sessionCount": number;
    readonly "source": V2ExternalAgentImportedConnectorSource;
    readonly [key: string]: unknown;
};

export type V2ExternalAgentImportedConnectorSource = "remoteMcpServersConfig";

export type V2FeedbackRequirements = {
    readonly "enabled"?: boolean | null;
    readonly [key: string]: unknown;
};

export type V2FeedbackUploadParams = {
    readonly "classification": string;
    readonly "extraLogFiles"?: V2FeedbackUploadParamsExtraLogFiles;
    readonly "includeLogs"?: boolean;
    readonly "reason"?: string | null;
    readonly "tags"?: V2FeedbackUploadParamsTags;
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2FeedbackUploadParamsExtraLogFiles = ReadonlyArray<string> | null;

export type V2FeedbackUploadParamsTags = {
    readonly [key: string]: unknown;
} | null;

export type V2FeedbackUploadResponse = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2FileChangeOutputDeltaNotification = {
    readonly "delta": string;
    readonly "itemId": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2FileChangePatchUpdatedNotification = {
    readonly "changes": V2FileChangePatchUpdatedNotificationChanges;
    readonly "itemId": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2FileChangePatchUpdatedNotificationChanges = ReadonlyArray<V2FileUpdateChange>;

export type V2FileSystemAccessMode = "read" | "write" | "deny";

export type V2FileSystemPath = V2PathFileSystemPath | V2GlobPatternFileSystemPath | V2SpecialFileSystemPath;

export type V2PathFileSystemPath = {
    readonly "path": V2LegacyAppPathString;
    readonly "type": V2PathFileSystemPathType;
    readonly [key: string]: unknown;
};

export type V2PathFileSystemPathType = "path";

export type V2GlobPatternFileSystemPath = {
    readonly "pattern": string;
    readonly "type": V2GlobPatternFileSystemPathType;
    readonly [key: string]: unknown;
};

export type V2GlobPatternFileSystemPathType = "glob_pattern";

export type V2SpecialFileSystemPath = {
    readonly "type": V2SpecialFileSystemPathType;
    readonly "value": V2FileSystemSpecialPath;
    readonly [key: string]: unknown;
};

export type V2SpecialFileSystemPathType = "special";

export type V2FileSystemSandboxEntry = {
    readonly "access": V2FileSystemAccessMode;
    readonly "path": V2FileSystemPath;
    readonly [key: string]: unknown;
};

export type V2FileSystemSpecialPath = V2RootFileSystemSpecialPath | V2MinimalFileSystemSpecialPath | V2KindFileSystemSpecialPath | V2TmpdirFileSystemSpecialPath | V2SlashTmpFileSystemSpecialPath | V2FileSystemSpecialPathOneOf6;

export type V2RootFileSystemSpecialPath = {
    readonly "kind": V2RootFileSystemSpecialPathKind;
    readonly [key: string]: unknown;
};

export type V2RootFileSystemSpecialPathKind = "root";

export type V2MinimalFileSystemSpecialPath = {
    readonly "kind": V2MinimalFileSystemSpecialPathKind;
    readonly [key: string]: unknown;
};

export type V2MinimalFileSystemSpecialPathKind = "minimal";

export type V2KindFileSystemSpecialPath = {
    readonly "kind": V2KindFileSystemSpecialPathKind;
    readonly "subpath"?: V2KindFileSystemSpecialPathSubpath;
    readonly [key: string]: unknown;
};

export type V2KindFileSystemSpecialPathKind = "project_roots";

export type V2KindFileSystemSpecialPathSubpath = V2LegacyAppPathString | null;

export type V2TmpdirFileSystemSpecialPath = {
    readonly "kind": V2TmpdirFileSystemSpecialPathKind;
    readonly [key: string]: unknown;
};

export type V2TmpdirFileSystemSpecialPathKind = "tmpdir";

export type V2SlashTmpFileSystemSpecialPath = {
    readonly "kind": V2SlashTmpFileSystemSpecialPathKind;
    readonly [key: string]: unknown;
};

export type V2SlashTmpFileSystemSpecialPathKind = "slash_tmp";

export type V2FileSystemSpecialPathOneOf6 = {
    readonly "kind": V2FileSystemSpecialPathOneOf6Kind;
    readonly "path": string;
    readonly "subpath"?: V2FileSystemSpecialPathOneOf6Subpath;
    readonly [key: string]: unknown;
};

export type V2FileSystemSpecialPathOneOf6Kind = "unknown";

export type V2FileSystemSpecialPathOneOf6Subpath = V2LegacyAppPathString | null;

export type V2FileUpdateChange = {
    readonly "diff": string;
    readonly "kind": V2PatchChangeKind;
    readonly "path": string;
    readonly [key: string]: unknown;
};

export type V2ForcedChatgptWorkspaceIds = string | V2ForcedChatgptWorkspaceIdsAnyOf2;

export type V2ForcedChatgptWorkspaceIdsAnyOf2 = ReadonlyArray<string>;

export type V2ForcedLoginMethod = "chatgpt" | "api";

export type V2FsChangedNotification = {
    readonly "changedPaths": V2FsChangedNotificationChangedPaths;
    readonly "watchId": string;
    readonly [key: string]: unknown;
};

export type V2FsChangedNotificationChangedPaths = ReadonlyArray<V2AbsolutePathBuf>;

export type V2FsCopyParams = {
    readonly "destinationPath": V2FsCopyParamsDestinationPath;
    readonly "recursive"?: boolean;
    readonly "sourcePath": V2FsCopyParamsSourcePath;
    readonly [key: string]: unknown;
};

export type V2FsCopyParamsDestinationPath = V2AbsolutePathBuf;

export type V2FsCopyParamsSourcePath = V2AbsolutePathBuf;

export type V2FsCopyResponse = {
    readonly [key: string]: unknown;
};

export type V2FsCreateDirectoryParams = {
    readonly "path": V2FsCreateDirectoryParamsPath;
    readonly "recursive"?: boolean | null;
    readonly [key: string]: unknown;
};

export type V2FsCreateDirectoryParamsPath = V2AbsolutePathBuf;

export type V2FsCreateDirectoryResponse = {
    readonly [key: string]: unknown;
};

export type V2FsGetMetadataParams = {
    readonly "path": V2FsGetMetadataParamsPath;
    readonly [key: string]: unknown;
};

export type V2FsGetMetadataParamsPath = V2AbsolutePathBuf;

export type V2FsGetMetadataResponse = {
    readonly "createdAtMs": number;
    readonly "isDirectory": boolean;
    readonly "isFile": boolean;
    readonly "isSymlink": boolean;
    readonly "modifiedAtMs": number;
    readonly [key: string]: unknown;
};

export type V2FsReadDirectoryEntry = {
    readonly "fileName": string;
    readonly "isDirectory": boolean;
    readonly "isFile": boolean;
    readonly [key: string]: unknown;
};

export type V2FsReadDirectoryParams = {
    readonly "path": V2FsReadDirectoryParamsPath;
    readonly [key: string]: unknown;
};

export type V2FsReadDirectoryParamsPath = V2AbsolutePathBuf;

export type V2FsReadDirectoryResponse = {
    readonly "entries": V2FsReadDirectoryResponseEntries;
    readonly [key: string]: unknown;
};

export type V2FsReadDirectoryResponseEntries = ReadonlyArray<V2FsReadDirectoryEntry>;

export type V2FsReadFileParams = {
    readonly "path": V2FsReadFileParamsPath;
    readonly [key: string]: unknown;
};

export type V2FsReadFileParamsPath = V2AbsolutePathBuf;

export type V2FsReadFileResponse = {
    readonly "dataBase64": string;
    readonly [key: string]: unknown;
};

export type V2FsRemoveParams = {
    readonly "force"?: boolean | null;
    readonly "path": V2FsRemoveParamsPath;
    readonly "recursive"?: boolean | null;
    readonly [key: string]: unknown;
};

export type V2FsRemoveParamsPath = V2AbsolutePathBuf;

export type V2FsRemoveResponse = {
    readonly [key: string]: unknown;
};

export type V2FsUnwatchParams = {
    readonly "watchId": string;
    readonly [key: string]: unknown;
};

export type V2FsUnwatchResponse = {
    readonly [key: string]: unknown;
};

export type V2FsWatchParams = {
    readonly "path": V2FsWatchParamsPath;
    readonly "watchId": string;
    readonly [key: string]: unknown;
};

export type V2FsWatchParamsPath = V2AbsolutePathBuf;

export type V2FsWatchResponse = {
    readonly "path": V2FsWatchResponsePath;
    readonly [key: string]: unknown;
};

export type V2FsWatchResponsePath = V2AbsolutePathBuf;

export type V2FsWriteFileParams = {
    readonly "dataBase64": string;
    readonly "path": V2FsWriteFileParamsPath;
    readonly [key: string]: unknown;
};

export type V2FsWriteFileParamsPath = V2AbsolutePathBuf;

export type V2FsWriteFileResponse = {
    readonly [key: string]: unknown;
};

export type V2FunctionCallOutputBody = string | V2FunctionCallOutputBodyAnyOf2;

export type V2FunctionCallOutputBodyAnyOf2 = ReadonlyArray<V2FunctionCallOutputContentItem>;

export type V2FunctionCallOutputContentItem = V2InputTextFunctionCallOutputContentItem | V2InputImageFunctionCallOutputContentItem | V2InputAudioFunctionCallOutputContentItem | V2EncryptedContentFunctionCallOutputContentItem;

export type V2InputTextFunctionCallOutputContentItem = {
    readonly "text": string;
    readonly "type": V2InputTextFunctionCallOutputContentItemType;
    readonly [key: string]: unknown;
};

export type V2InputTextFunctionCallOutputContentItemType = "input_text";

export type V2InputImageFunctionCallOutputContentItem = {
    readonly "detail"?: V2InputImageFunctionCallOutputContentItemDetail;
    readonly "image_url": string;
    readonly "type": V2InputImageFunctionCallOutputContentItemType;
    readonly [key: string]: unknown;
};

export type V2InputImageFunctionCallOutputContentItemDetail = V2ImageDetail | null;

export type V2InputImageFunctionCallOutputContentItemType = "input_image";

export type V2InputAudioFunctionCallOutputContentItem = {
    readonly "audio_url": string;
    readonly "type": V2InputAudioFunctionCallOutputContentItemType;
    readonly [key: string]: unknown;
};

export type V2InputAudioFunctionCallOutputContentItemType = "input_audio";

export type V2EncryptedContentFunctionCallOutputContentItem = {
    readonly "encrypted_content": string;
    readonly "type": V2EncryptedContentFunctionCallOutputContentItemType;
    readonly [key: string]: unknown;
};

export type V2EncryptedContentFunctionCallOutputContentItemType = "encrypted_content";

export type V2GetAccountParams = {
    readonly "refreshToken"?: boolean;
    readonly [key: string]: unknown;
};

export type V2GetAccountRateLimitsResponse = {
    readonly "rateLimitResetCredits"?: V2GetAccountRateLimitsResponseRateLimitResetCredits;
    readonly "rateLimits": V2GetAccountRateLimitsResponseRateLimits;
    readonly "rateLimitsByLimitId"?: V2GetAccountRateLimitsResponseRateLimitsByLimitId;
    readonly [key: string]: unknown;
};

export type V2GetAccountRateLimitsResponseRateLimitResetCredits = V2RateLimitResetCreditsSummary | null;

export type V2GetAccountRateLimitsResponseRateLimits = V2RateLimitSnapshot;

export type V2GetAccountRateLimitsResponseRateLimitsByLimitId = {
    readonly [key: string]: unknown;
} | null;

export type V2GetAccountResponse = {
    readonly "account"?: V2GetAccountResponseAccount;
    readonly "requiresOpenaiAuth": boolean;
    readonly [key: string]: unknown;
};

export type V2GetAccountResponseAccount = V2Account | null;

export type V2GetAccountTokenUsageParams = {
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2GetAccountTokenUsageResponse = {
    readonly "dailyUsageBuckets"?: V2GetAccountTokenUsageResponseDailyUsageBuckets;
    readonly "summary": V2AccountTokenUsageSummary;
    readonly "threadUsage"?: V2GetAccountTokenUsageResponseThreadUsage;
    readonly [key: string]: unknown;
};

export type V2GetAccountTokenUsageResponseDailyUsageBuckets = ReadonlyArray<V2AccountTokenUsageDailyBucket> | null;

export type V2GetAccountTokenUsageResponseThreadUsage = V2ThreadUsage | null;

export type V2GetWorkspaceMessagesResponse = {
    readonly "featureEnabled": boolean;
    readonly "messages": V2GetWorkspaceMessagesResponseMessages;
    readonly [key: string]: unknown;
};

export type V2GetWorkspaceMessagesResponseMessages = ReadonlyArray<V2WorkspaceMessage>;

export type V2GitInfo = {
    readonly "branch"?: string | null;
    readonly "originUrl"?: string | null;
    readonly "sha"?: string | null;
    readonly [key: string]: unknown;
};

export type V2GuardianApprovalReview = {
    readonly "rationale"?: string | null;
    readonly "riskLevel"?: V2GuardianApprovalReviewRiskLevel;
    readonly "status": V2GuardianApprovalReviewStatus;
    readonly "userAuthorization"?: V2GuardianApprovalReviewUserAuthorization;
    readonly [key: string]: unknown;
};

export type V2GuardianApprovalReviewRiskLevel = V2GuardianRiskLevel | null;

export type V2GuardianApprovalReviewUserAuthorization = V2GuardianUserAuthorization | null;

export type V2GuardianApprovalReviewAction = V2CommandGuardianApprovalReviewAction | V2ExecveGuardianApprovalReviewAction | V2ApplyPatchGuardianApprovalReviewAction | V2NetworkAccessGuardianApprovalReviewAction | V2McpToolCallGuardianApprovalReviewAction | V2RequestPermissionsGuardianApprovalReviewAction;

export type V2CommandGuardianApprovalReviewAction = {
    readonly "command": string;
    readonly "cwd": V2AbsolutePathBuf;
    readonly "source": V2GuardianCommandSource;
    readonly "type": V2CommandGuardianApprovalReviewActionType;
    readonly [key: string]: unknown;
};

export type V2CommandGuardianApprovalReviewActionType = "command";

export type V2ExecveGuardianApprovalReviewAction = {
    readonly "argv": V2ExecveGuardianApprovalReviewActionArgv;
    readonly "cwd": V2AbsolutePathBuf;
    readonly "program": string;
    readonly "source": V2GuardianCommandSource;
    readonly "type": V2ExecveGuardianApprovalReviewActionType;
    readonly [key: string]: unknown;
};

export type V2ExecveGuardianApprovalReviewActionArgv = ReadonlyArray<string>;

export type V2ExecveGuardianApprovalReviewActionType = "execve";

export type V2ApplyPatchGuardianApprovalReviewAction = {
    readonly "cwd": V2AbsolutePathBuf;
    readonly "files": V2ApplyPatchGuardianApprovalReviewActionFiles;
    readonly "type": V2ApplyPatchGuardianApprovalReviewActionType;
    readonly [key: string]: unknown;
};

export type V2ApplyPatchGuardianApprovalReviewActionFiles = ReadonlyArray<V2AbsolutePathBuf>;

export type V2ApplyPatchGuardianApprovalReviewActionType = "applyPatch";

export type V2NetworkAccessGuardianApprovalReviewAction = {
    readonly "host": string;
    readonly "port": number;
    readonly "protocol": V2NetworkApprovalProtocol;
    readonly "target": string;
    readonly "type": V2NetworkAccessGuardianApprovalReviewActionType;
    readonly [key: string]: unknown;
};

export type V2NetworkAccessGuardianApprovalReviewActionType = "networkAccess";

export type V2McpToolCallGuardianApprovalReviewAction = {
    readonly "connectorId"?: string | null;
    readonly "connectorName"?: string | null;
    readonly "server": string;
    readonly "toolName": string;
    readonly "toolTitle"?: string | null;
    readonly "type": V2McpToolCallGuardianApprovalReviewActionType;
    readonly [key: string]: unknown;
};

export type V2McpToolCallGuardianApprovalReviewActionType = "mcpToolCall";

export type V2RequestPermissionsGuardianApprovalReviewAction = {
    readonly "permissions": V2RequestPermissionProfile;
    readonly "reason"?: string | null;
    readonly "type": V2RequestPermissionsGuardianApprovalReviewActionType;
    readonly [key: string]: unknown;
};

export type V2RequestPermissionsGuardianApprovalReviewActionType = "requestPermissions";

export type V2GuardianApprovalReviewStatus = "inProgress" | "approved" | "denied" | "timedOut" | "aborted";

export type V2GuardianCommandSource = "shell" | "unifiedExec";

export type V2GuardianRiskLevel = "low" | "medium" | "high" | "critical";

export type V2GuardianUserAuthorization = "unknown" | "low" | "medium" | "high";

export type V2GuardianWarningNotification = {
    readonly "message": string;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2HookCompletedNotification = {
    readonly "run": V2HookRunSummary;
    readonly "threadId": string;
    readonly "turnId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2HookErrorInfo = {
    readonly "message": string;
    readonly "path": string;
    readonly [key: string]: unknown;
};

export type V2HookEventName = "preToolUse" | "permissionRequest" | "postToolUse" | "preCompact" | "postCompact" | "sessionStart" | "sessionEnd" | "userPromptSubmit" | "subagentStart" | "subagentStop" | "stop";

export type V2HookExecutionMode = "sync" | "async";

export type V2HookHandlerType = "command" | "mcpTool" | "prompt" | "agent";

export type V2HookMetadata = V2HookMetadataOneOf1 | V2HookMetadataOneOf2 | V2PromptHookMetadata | V2AgentHookMetadata;

export type V2HookMetadataOneOf1 = {
    readonly "async"?: boolean;
    readonly "command": string;
    readonly "handlerType": V2HookMetadataOneOf1HandlerType;
    readonly [key: string]: unknown;
};

export type V2HookMetadataOneOf1HandlerType = "command";

export type V2HookMetadataOneOf2 = {
    readonly "handlerType": V2HookMetadataOneOf2HandlerType;
    readonly "server": string;
    readonly "tool": string;
    readonly [key: string]: unknown;
};

export type V2HookMetadataOneOf2HandlerType = "mcpTool";

export type V2PromptHookMetadata = {
    readonly "handlerType": V2PromptHookMetadataHandlerType;
    readonly [key: string]: unknown;
};

export type V2PromptHookMetadataHandlerType = "prompt";

export type V2AgentHookMetadata = {
    readonly "handlerType": V2AgentHookMetadataHandlerType;
    readonly [key: string]: unknown;
};

export type V2AgentHookMetadataHandlerType = "agent";

export type V2HookMigration = {
    readonly "name": string;
    readonly [key: string]: unknown;
};

export type V2HookOutputEntry = {
    readonly "kind": V2HookOutputEntryKind;
    readonly "text": string;
    readonly [key: string]: unknown;
};

export type V2HookOutputEntryKind = "warning" | "stop" | "feedback" | "context" | "error";

export type V2HookPromptFragment = {
    readonly "hookRunId": string;
    readonly "text": string;
    readonly [key: string]: unknown;
};

export type V2HookRunStatus = "running" | "completed" | "failed" | "blocked" | "stopped";

export type V2HookRunSummary = {
    readonly "completedAt"?: number | null;
    readonly "displayOrder": number;
    readonly "durationMs"?: number | null;
    readonly "entries": V2HookRunSummaryEntries;
    readonly "eventName": V2HookEventName;
    readonly "executionMode": V2HookExecutionMode;
    readonly "handlerType": V2HookHandlerType;
    readonly "id": string;
    readonly "scope": V2HookScope;
    readonly "source"?: V2HookRunSummarySource;
    readonly "sourcePath": V2AbsolutePathBuf;
    readonly "startedAt": number;
    readonly "status": V2HookRunStatus;
    readonly "statusMessage"?: string | null;
    readonly [key: string]: unknown;
};

export type V2HookRunSummaryEntries = ReadonlyArray<V2HookOutputEntry>;

export type V2HookRunSummarySource = V2HookSource;

export type V2HookScope = "thread" | "turn";

export type V2HookSource = "system" | "user" | "project" | "mdm" | "sessionFlags" | "plugin" | "cloudRequirements" | "cloudManagedConfig" | "legacyManagedConfigFile" | "legacyManagedConfigMdm" | "unknown";

export type V2HookStartedNotification = {
    readonly "run": V2HookRunSummary;
    readonly "threadId": string;
    readonly "turnId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2HookTrustStatus = "managed" | "untrusted" | "trusted" | "modified";

export type V2HooksListEntry = {
    readonly "cwd": string;
    readonly "errors": V2HooksListEntryErrors;
    readonly "hooks": V2HooksListEntryHooks;
    readonly "warnings": V2HooksListEntryWarnings;
    readonly [key: string]: unknown;
};

export type V2HooksListEntryErrors = ReadonlyArray<V2HookErrorInfo>;

export type V2HooksListEntryHooks = ReadonlyArray<V2HookMetadata>;

export type V2HooksListEntryWarnings = ReadonlyArray<string>;

export type V2HooksListParams = {
    readonly "cwds"?: V2HooksListParamsCwds;
    readonly [key: string]: unknown;
};

export type V2HooksListParamsCwds = ReadonlyArray<string>;

export type V2HooksListResponse = {
    readonly "data": V2HooksListResponseData;
    readonly [key: string]: unknown;
};

export type V2HooksListResponseData = ReadonlyArray<V2HooksListEntry>;

export type V2ImageDetail = "auto" | "low" | "high" | "original";

export type V2ImageGenerationFailure = V2UsageLimitExceededImageGenerationFailure;

export type V2UsageLimitExceededImageGenerationFailure = {
    readonly "limitId": string;
    readonly "resetsAt"?: number | null;
    readonly "type": V2UsageLimitExceededImageGenerationFailureType;
    readonly [key: string]: unknown;
};

export type V2UsageLimitExceededImageGenerationFailureType = "usageLimitExceeded";

export type V2InAppBrowserRequirements = {
    readonly "allowExternalBrowserSettingsImport"?: boolean | null;
    readonly [key: string]: unknown;
};

export type V2InputModality = V2InputModalityOneOf1 | V2InputModalityOneOf2 | V2InputModalityOneOf3;

export type V2InputModalityOneOf1 = "text";

export type V2InputModalityOneOf2 = "image";

export type V2InputModalityOneOf3 = "audio";

export type V2InstalledApp = {
    readonly "callable": boolean;
    readonly "enabled": boolean;
    readonly "id": string;
    readonly "runtimeName"?: string | null;
    readonly [key: string]: unknown;
};

export type V2InternalChatMessageMetadataPassthrough = {
    readonly "turn_id"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ItemCompletedNotification = {
    readonly "completedAtMs": number;
    readonly "item": V2ThreadItem;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2ItemGuardianApprovalReviewCompletedNotification = {
    readonly "action": V2GuardianApprovalReviewAction;
    readonly "completedAtMs": number;
    readonly "decisionSource": V2AutoReviewDecisionSource;
    readonly "review": V2GuardianApprovalReview;
    readonly "reviewId": string;
    readonly "startedAtMs": number;
    readonly "targetItemId"?: string | null;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2ItemGuardianApprovalReviewStartedNotification = {
    readonly "action": V2GuardianApprovalReviewAction;
    readonly "review": V2GuardianApprovalReview;
    readonly "reviewId": string;
    readonly "startedAtMs": number;
    readonly "targetItemId"?: string | null;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2ItemStartedNotification = {
    readonly "item": V2ThreadItem;
    readonly "startedAtMs": number;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2LegacyAppPathString = string;

export type V2ListMcpServerStatusParams = {
    readonly "cursor"?: string | null;
    readonly "detail"?: V2ListMcpServerStatusParamsDetail;
    readonly "limit"?: number | null;
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ListMcpServerStatusParamsDetail = V2McpServerStatusDetail | null;

export type V2ListMcpServerStatusResponse = {
    readonly "data": V2ListMcpServerStatusResponseData;
    readonly "nextCursor"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ListMcpServerStatusResponseData = ReadonlyArray<V2McpServerStatus>;

export type V2LocalShellAction = V2ExecLocalShellAction;

export type V2ExecLocalShellAction = {
    readonly "command": V2ExecLocalShellActionCommand;
    readonly "env"?: V2ExecLocalShellActionEnv;
    readonly "timeout_ms"?: number | null;
    readonly "type": V2ExecLocalShellActionType;
    readonly "user"?: string | null;
    readonly "working_directory"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ExecLocalShellActionCommand = ReadonlyArray<string>;

export type V2ExecLocalShellActionEnv = {
    readonly [key: string]: unknown;
} | null;

export type V2ExecLocalShellActionType = "exec";

export type V2LocalShellStatus = "completed" | "in_progress" | "incomplete";

export type V2LoginAccountParams = V2ApiKeyv2LoginAccountParams | V2Chatgptv2LoginAccountParams | V2ChatgptDeviceCodev2LoginAccountParams | V2ChatgptAuthTokensv2LoginAccountParams | V2AmazonBedrockv2LoginAccountParams;

export type V2ApiKeyv2LoginAccountParams = {
    readonly "apiKey": string;
    readonly "type": V2ApiKeyv2LoginAccountParamsType;
    readonly [key: string]: unknown;
};

export type V2ApiKeyv2LoginAccountParamsType = "apiKey";

export type V2Chatgptv2LoginAccountParams = {
    readonly "appBrand"?: V2Chatgptv2LoginAccountParamsAppBrand;
    readonly "codexStreamlinedLogin"?: boolean;
    readonly "type": V2Chatgptv2LoginAccountParamsType;
    readonly "useHostedLoginSuccessPage"?: boolean;
    readonly [key: string]: unknown;
};

export type V2Chatgptv2LoginAccountParamsAppBrand = V2LoginAppBrand | null;

export type V2Chatgptv2LoginAccountParamsType = "chatgpt";

export type V2ChatgptDeviceCodev2LoginAccountParams = {
    readonly "type": V2ChatgptDeviceCodev2LoginAccountParamsType;
    readonly [key: string]: unknown;
};

export type V2ChatgptDeviceCodev2LoginAccountParamsType = "chatgptDeviceCode";

export type V2ChatgptAuthTokensv2LoginAccountParams = {
    readonly "accessToken": string;
    readonly "chatgptAccountId": string;
    readonly "chatgptPlanType"?: string | null;
    readonly "type": V2ChatgptAuthTokensv2LoginAccountParamsType;
    readonly [key: string]: unknown;
};

export type V2ChatgptAuthTokensv2LoginAccountParamsType = "chatgptAuthTokens";

export type V2AmazonBedrockv2LoginAccountParams = {
    readonly "apiKey": string;
    readonly "region": string;
    readonly "type": V2AmazonBedrockv2LoginAccountParamsType;
    readonly [key: string]: unknown;
};

export type V2AmazonBedrockv2LoginAccountParamsType = "amazonBedrock";

export type V2LoginAccountResponse = V2ApiKeyv2LoginAccountResponse | V2Chatgptv2LoginAccountResponse | V2ChatgptDeviceCodev2LoginAccountResponse | V2ChatgptAuthTokensv2LoginAccountResponse | V2AmazonBedrockv2LoginAccountResponse;

export type V2ApiKeyv2LoginAccountResponse = {
    readonly "type": V2ApiKeyv2LoginAccountResponseType;
    readonly [key: string]: unknown;
};

export type V2ApiKeyv2LoginAccountResponseType = "apiKey";

export type V2Chatgptv2LoginAccountResponse = {
    readonly "authUrl": string;
    readonly "loginId": string;
    readonly "type": V2Chatgptv2LoginAccountResponseType;
    readonly [key: string]: unknown;
};

export type V2Chatgptv2LoginAccountResponseType = "chatgpt";

export type V2ChatgptDeviceCodev2LoginAccountResponse = {
    readonly "loginId": string;
    readonly "type": V2ChatgptDeviceCodev2LoginAccountResponseType;
    readonly "userCode": string;
    readonly "verificationUrl": string;
    readonly [key: string]: unknown;
};

export type V2ChatgptDeviceCodev2LoginAccountResponseType = "chatgptDeviceCode";

export type V2ChatgptAuthTokensv2LoginAccountResponse = {
    readonly "type": V2ChatgptAuthTokensv2LoginAccountResponseType;
    readonly [key: string]: unknown;
};

export type V2ChatgptAuthTokensv2LoginAccountResponseType = "chatgptAuthTokens";

export type V2AmazonBedrockv2LoginAccountResponse = {
    readonly "type": V2AmazonBedrockv2LoginAccountResponseType;
    readonly [key: string]: unknown;
};

export type V2AmazonBedrockv2LoginAccountResponseType = "amazonBedrock";

export type V2LoginAppBrand = "codex" | "chatgpt";

export type V2LogoutAccountResponse = {
    readonly [key: string]: unknown;
};

export type V2ManagedHooksRequirements = {
    readonly "PermissionRequest": V2ManagedHooksRequirementsPermissionRequest;
    readonly "PostCompact": V2ManagedHooksRequirementsPostCompact;
    readonly "PostToolUse": V2ManagedHooksRequirementsPostToolUse;
    readonly "PreCompact": V2ManagedHooksRequirementsPreCompact;
    readonly "PreToolUse": V2ManagedHooksRequirementsPreToolUse;
    readonly "SessionEnd"?: V2ManagedHooksRequirementsSessionEnd;
    readonly "SessionStart": V2ManagedHooksRequirementsSessionStart;
    readonly "Stop": V2ManagedHooksRequirementsStop;
    readonly "SubagentStart": V2ManagedHooksRequirementsSubagentStart;
    readonly "SubagentStop": V2ManagedHooksRequirementsSubagentStop;
    readonly "UserPromptSubmit": V2ManagedHooksRequirementsUserPromptSubmit;
    readonly "managedDir"?: string | null;
    readonly "windowsManagedDir"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ManagedHooksRequirementsPermissionRequest = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsPostCompact = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsPostToolUse = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsPreCompact = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsPreToolUse = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsSessionEnd = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsSessionStart = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsStop = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsSubagentStart = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsSubagentStop = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2ManagedHooksRequirementsUserPromptSubmit = ReadonlyArray<V2ConfiguredHookMatcherGroup>;

export type V2MarketplaceAddParams = {
    readonly "refName"?: string | null;
    readonly "source": string;
    readonly "sparsePaths"?: V2MarketplaceAddParamsSparsePaths;
    readonly [key: string]: unknown;
};

export type V2MarketplaceAddParamsSparsePaths = ReadonlyArray<string> | null;

export type V2MarketplaceAddResponse = {
    readonly "alreadyAdded": boolean;
    readonly "installedRoot": V2AbsolutePathBuf;
    readonly "marketplaceName": string;
    readonly [key: string]: unknown;
};

export type V2MarketplaceInterface = {
    readonly "displayName"?: string | null;
    readonly [key: string]: unknown;
};

export type V2MarketplaceLoadErrorInfo = {
    readonly "marketplacePath": V2AbsolutePathBuf;
    readonly "message": string;
    readonly [key: string]: unknown;
};

export type V2MarketplaceRemoveParams = {
    readonly "marketplaceName": string;
    readonly [key: string]: unknown;
};

export type V2MarketplaceRemoveResponse = {
    readonly "installedRoot"?: V2MarketplaceRemoveResponseInstalledRoot;
    readonly "marketplaceName": string;
    readonly [key: string]: unknown;
};

export type V2MarketplaceRemoveResponseInstalledRoot = V2AbsolutePathBuf | null;

export type V2MarketplaceUpgradeErrorInfo = {
    readonly "marketplaceName": string;
    readonly "message": string;
    readonly [key: string]: unknown;
};

export type V2MarketplaceUpgradeParams = {
    readonly "marketplaceName"?: string | null;
    readonly [key: string]: unknown;
};

export type V2MarketplaceUpgradeResponse = {
    readonly "errors": V2MarketplaceUpgradeResponseErrors;
    readonly "selectedMarketplaces": V2MarketplaceUpgradeResponseSelectedMarketplaces;
    readonly "upgradedRoots": V2MarketplaceUpgradeResponseUpgradedRoots;
    readonly [key: string]: unknown;
};

export type V2MarketplaceUpgradeResponseErrors = ReadonlyArray<V2MarketplaceUpgradeErrorInfo>;

export type V2MarketplaceUpgradeResponseSelectedMarketplaces = ReadonlyArray<string>;

export type V2MarketplaceUpgradeResponseUpgradedRoots = ReadonlyArray<V2AbsolutePathBuf>;

export type V2McpAuthStatus = "unknown" | "unsupported" | "notLoggedIn" | "bearerToken" | "oAuth";

export type V2McpResourceReadParams = {
    readonly "connectorId"?: string | null;
    readonly "originCallId"?: string | null;
    readonly "server": string;
    readonly "threadId"?: string | null;
    readonly "uri": string;
    readonly [key: string]: unknown;
};

export type V2McpResourceReadResponse = {
    readonly "contents": V2McpResourceReadResponseContents;
    readonly "originCallId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2McpResourceReadResponseContents = ReadonlyArray<V2ResourceContent>;

export type V2McpServerConnectionStatus = "notStarted" | "starting" | "connected" | "authenticationRequired" | "failed" | "cancelled" | "disabled";

export type V2McpServerEventNotification = {
    readonly "method": string;
    readonly "params": unknown;
    readonly [key: string]: unknown;
};

export type V2McpServerEventStreamNotification = {
    readonly "notification": V2McpServerEventNotification;
    readonly "subscriptionId": string;
    readonly [key: string]: unknown;
};

export type V2McpServerInfo = {
    readonly "description"?: string | null;
    readonly "icons"?: V2McpServerInfoIcons;
    readonly "name": string;
    readonly "title"?: string | null;
    readonly "version": string;
    readonly "websiteUrl"?: string | null;
    readonly [key: string]: unknown;
};

export type V2McpServerInfoIcons = ReadonlyArray<unknown> | null;

export type V2McpServerMigration = {
    readonly "name": string;
    readonly [key: string]: unknown;
};

export type V2McpServerOauthClientRegistration = "auto" | "cimd" | "dcr";

export type V2McpServerOauthLoginCompletedNotification = {
    readonly "error"?: string | null;
    readonly "name": string;
    readonly "success": boolean;
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2McpServerOauthLoginParams = {
    readonly "clientRegistration"?: V2McpServerOauthLoginParamsClientRegistration;
    readonly "name": string;
    readonly "scopes"?: V2McpServerOauthLoginParamsScopes;
    readonly "threadId"?: string | null;
    readonly "timeoutSecs"?: number | null;
    readonly [key: string]: unknown;
};

export type V2McpServerOauthLoginParamsClientRegistration = V2McpServerOauthClientRegistration | null;

export type V2McpServerOauthLoginParamsScopes = ReadonlyArray<string> | null;

export type V2McpServerOauthLoginResponse = {
    readonly "authorizationUrl": string;
    readonly [key: string]: unknown;
};

export type V2McpServerRefreshResponse = {
    readonly [key: string]: unknown;
};

export type V2McpServerStartupFailureReason = "reauthenticationRequired";

export type V2McpServerStartupState = "starting" | "ready" | "failed" | "cancelled";

export type V2McpServerStatus = {
    readonly "authStatus": V2McpAuthStatus;
    readonly "name": string;
    readonly "pluginId"?: string | null;
    readonly "resourceTemplates": V2McpServerStatusResourceTemplates;
    readonly "resources": V2McpServerStatusResources;
    readonly "runtimeStatus"?: V2McpServerStatusRuntimeStatus;
    readonly "serverInfo"?: V2McpServerStatusServerInfo;
    readonly "tools": V2McpServerStatusTools;
    readonly [key: string]: unknown;
};

export type V2McpServerStatusResourceTemplates = ReadonlyArray<V2ResourceTemplate>;

export type V2McpServerStatusResources = ReadonlyArray<V2Resource>;

export type V2McpServerStatusRuntimeStatus = V2McpServerConnectionStatus | null;

export type V2McpServerStatusServerInfo = V2McpServerInfo | null;

export type V2McpServerStatusTools = {
    readonly [key: string]: unknown;
};

export type V2McpServerStatusDetail = "full" | "toolsAndAuthOnly";

export type V2McpServerStatusUpdatedNotification = {
    readonly "error"?: string | null;
    readonly "failureReason"?: V2McpServerStatusUpdatedNotificationFailureReason;
    readonly "name": string;
    readonly "status": V2McpServerStartupState;
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2McpServerStatusUpdatedNotificationFailureReason = V2McpServerStartupFailureReason | null;

export type V2McpServerToolCallParams = {
    readonly "_meta"?: unknown;
    readonly "arguments"?: unknown;
    readonly "server": string;
    readonly "threadId": string;
    readonly "tool": string;
    readonly [key: string]: unknown;
};

export type V2McpServerToolCallResponse = {
    readonly "_meta"?: unknown;
    readonly "content": V2McpServerToolCallResponseContent;
    readonly "isError"?: boolean | null;
    readonly "structuredContent"?: unknown;
    readonly [key: string]: unknown;
};

export type V2McpServerToolCallResponseContent = ReadonlyArray<unknown>;

export type V2McpToolCallAppContext = {
    readonly "actionName"?: string | null;
    readonly "appName"?: string | null;
    readonly "connectorId": string;
    readonly "linkId"?: string | null;
    readonly "resourceUri"?: string | null;
    readonly [key: string]: unknown;
};

export type V2McpToolCallError = {
    readonly "message": string;
    readonly [key: string]: unknown;
};

export type V2McpToolCallProgressNotification = {
    readonly "itemId": string;
    readonly "message": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2McpToolCallResult = {
    readonly "_meta"?: unknown;
    readonly "content": V2McpToolCallResultContent;
    readonly "structuredContent"?: unknown;
    readonly [key: string]: unknown;
};

export type V2McpToolCallResultContent = ReadonlyArray<unknown>;

export type V2McpToolCallStatus = "inProgress" | "completed" | "failed";

export type V2MemoryCitation = {
    readonly "entries": V2MemoryCitationEntries;
    readonly "threadIds": V2MemoryCitationThreadIds;
    readonly [key: string]: unknown;
};

export type V2MemoryCitationEntries = ReadonlyArray<V2MemoryCitationEntry>;

export type V2MemoryCitationThreadIds = ReadonlyArray<string>;

export type V2MemoryCitationEntry = {
    readonly "lineEnd": number;
    readonly "lineStart": number;
    readonly "note": string;
    readonly "path": string;
    readonly [key: string]: unknown;
};

export type V2MergeStrategy = "replace" | "upsert";

export type V2MessagePhase = V2MessagePhaseOneOf1 | V2MessagePhaseOneOf2;

export type V2MessagePhaseOneOf1 = "commentary";

export type V2MessagePhaseOneOf2 = "final_answer";

export type V2MigrationDetails = {
    readonly "commands"?: V2MigrationDetailsCommands;
    readonly "hooks"?: V2MigrationDetailsHooks;
    readonly "mcpServers"?: V2MigrationDetailsMcpServers;
    readonly "memory"?: V2MigrationDetailsMemory;
    readonly "plugins"?: V2MigrationDetailsPlugins;
    readonly "sessions"?: V2MigrationDetailsSessions;
    readonly "skills"?: V2MigrationDetailsSkills;
    readonly "subagents"?: V2MigrationDetailsSubagents;
    readonly [key: string]: unknown;
};

export type V2MigrationDetailsCommands = ReadonlyArray<V2CommandMigration>;

export type V2MigrationDetailsHooks = ReadonlyArray<V2HookMigration>;

export type V2MigrationDetailsMcpServers = ReadonlyArray<V2McpServerMigration>;

export type V2MigrationDetailsMemory = ReadonlyArray<string>;

export type V2MigrationDetailsPlugins = ReadonlyArray<V2PluginsMigration>;

export type V2MigrationDetailsSessions = ReadonlyArray<V2SessionMigration>;

export type V2MigrationDetailsSkills = ReadonlyArray<V2SkillMigration>;

export type V2MigrationDetailsSubagents = ReadonlyArray<V2SubagentMigration>;

export type V2ModeKind = "plan" | "default";

export type V2Model = {
    readonly "additionalSpeedTiers"?: V2ModelAdditionalSpeedTiers;
    readonly "availabilityNux"?: V2ModelAvailabilityNux2;
    readonly "defaultReasoningEffort": V2ReasoningEffort;
    readonly "defaultServiceTier"?: string | null;
    readonly "description": string;
    readonly "displayName": string;
    readonly "hidden": boolean;
    readonly "id": string;
    readonly "inputModalities"?: V2ModelInputModalities;
    readonly "isDefault": boolean;
    readonly "model": string;
    readonly "modelSpecialty"?: string | null;
    readonly "multiAgentVersion"?: V2ModelMultiAgentVersion;
    readonly "serviceTiers"?: V2ModelServiceTiers;
    readonly "supportedReasoningEfforts": V2ModelSupportedReasoningEfforts;
    readonly "supportsPersonality"?: boolean;
    readonly "upgrade"?: string | null;
    readonly "upgradeInfo"?: V2ModelUpgradeInfo2;
    readonly [key: string]: unknown;
};

export type V2ModelAdditionalSpeedTiers = ReadonlyArray<string>;

export type V2ModelAvailabilityNux2 = V2ModelAvailabilityNux | null;

export type V2ModelInputModalities = ReadonlyArray<V2InputModality>;

export type V2ModelMultiAgentVersion = V2MultiAgentVersion | null;

export type V2ModelServiceTiers = ReadonlyArray<V2ModelServiceTier>;

export type V2ModelSupportedReasoningEfforts = ReadonlyArray<V2ReasoningEffortOption>;

export type V2ModelUpgradeInfo2 = V2ModelUpgradeInfo | null;

export type V2ModelAvailabilityNux = {
    readonly "message": string;
    readonly [key: string]: unknown;
};

export type V2ModelListParams = {
    readonly "cursor"?: string | null;
    readonly "includeHidden"?: boolean | null;
    readonly "limit"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ModelListResponse = {
    readonly "data": V2ModelListResponseData;
    readonly "nextCursor"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ModelListResponseData = ReadonlyArray<V2Model>;

export type V2ModelProviderCapabilitiesReadParams = {
    readonly [key: string]: unknown;
};

export type V2ModelProviderCapabilitiesReadResponse = {
    readonly "imageGeneration": boolean;
    readonly "namespaceTools": boolean;
    readonly "webSearch": boolean;
    readonly [key: string]: unknown;
};

export type V2ModelRerouteReason = "highRiskCyberActivity";

export type V2ModelReroutedNotification = {
    readonly "fromModel": string;
    readonly "reason": V2ModelRerouteReason;
    readonly "threadId": string;
    readonly "toModel": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2ModelSafetyBufferingUpdatedNotification = {
    readonly "fasterModel"?: string | null;
    readonly "model": string;
    readonly "reasons": V2ModelSafetyBufferingUpdatedNotificationReasons;
    readonly "showBufferingUi": boolean;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly "useCases": V2ModelSafetyBufferingUpdatedNotificationUseCases;
    readonly [key: string]: unknown;
};

export type V2ModelSafetyBufferingUpdatedNotificationReasons = ReadonlyArray<string>;

export type V2ModelSafetyBufferingUpdatedNotificationUseCases = ReadonlyArray<string>;

export type V2ModelServiceTier = {
    readonly "description": string;
    readonly "id": string;
    readonly "name": string;
    readonly [key: string]: unknown;
};

export type V2ModelUpgradeInfo = {
    readonly "migrationMarkdown"?: string | null;
    readonly "model": string;
    readonly "modelLink"?: string | null;
    readonly "retirementAt"?: number | null;
    readonly "upgradeCopy"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ModelVerification = "trustedAccessForCyber";

export type V2ModelVerificationNotification = {
    readonly "threadId": string;
    readonly "turnId": string;
    readonly "verifications": V2ModelVerificationNotificationVerifications;
    readonly [key: string]: unknown;
};

export type V2ModelVerificationNotificationVerifications = ReadonlyArray<V2ModelVerification>;

export type V2ModelsRequirements = {
    readonly "newThread"?: V2ModelsRequirementsNewThread;
    readonly [key: string]: unknown;
};

export type V2ModelsRequirementsNewThread = V2NewThreadModelDefaults | null;

export type V2MultiAgentMode = V2MultiAgentModeOneOf1 | V2CustomMultiAgentMode;

export type V2MultiAgentModeOneOf1 = "explicitRequestOnly" | "proactive";

export type V2CustomMultiAgentMode = {
    readonly "custom": string;
};

export type V2MultiAgentVersion = "disabled" | "v1" | "v2";

export type V2NetworkAccess = "restricted" | "enabled";

export type V2NetworkApprovalProtocol = "http" | "https" | "socks5Tcp" | "socks5Udp";

export type V2NetworkDomainPermission = "allow" | "deny";

export type V2NetworkRequirements = {
    readonly "allowLocalBinding"?: boolean | null;
    readonly "allowUnixSockets"?: V2NetworkRequirementsAllowUnixSockets;
    readonly "allowUpstreamProxy"?: boolean | null;
    readonly "allowedDomains"?: V2NetworkRequirementsAllowedDomains;
    readonly "dangerouslyAllowAllUnixSockets"?: boolean | null;
    readonly "dangerouslyAllowNonLoopbackProxy"?: boolean | null;
    readonly "deniedDomains"?: V2NetworkRequirementsDeniedDomains;
    readonly "domains"?: V2NetworkRequirementsDomains;
    readonly "enabled"?: boolean | null;
    readonly "httpPort"?: number | null;
    readonly "managedAllowedDomainsOnly"?: boolean | null;
    readonly "socksPort"?: number | null;
    readonly "unixSockets"?: V2NetworkRequirementsUnixSockets;
    readonly [key: string]: unknown;
};

export type V2NetworkRequirementsAllowUnixSockets = ReadonlyArray<string> | null;

export type V2NetworkRequirementsAllowedDomains = ReadonlyArray<string> | null;

export type V2NetworkRequirementsDeniedDomains = ReadonlyArray<string> | null;

export type V2NetworkRequirementsDomains = {
    readonly [key: string]: unknown;
} | null;

export type V2NetworkRequirementsUnixSockets = {
    readonly [key: string]: unknown;
} | null;

export type V2NetworkUnixSocketPermission = "allow" | "deny";

export type V2NewThreadModelDefaults = {
    readonly "model"?: string | null;
    readonly "modelReasoningEffort"?: V2NewThreadModelDefaultsModelReasoningEffort;
    readonly "serviceTier"?: string | null;
    readonly [key: string]: unknown;
};

export type V2NewThreadModelDefaultsModelReasoningEffort = V2ReasoningEffort | null;

export type V2NonSteerableTurnKind = "review" | "compact";

export type V2NullableGetAccountTokenUsageParams = V2GetAccountTokenUsageParams | null;

export type V2OverriddenMetadata = {
    readonly "effectiveValue": unknown;
    readonly "message": string;
    readonly "overridingLayer": V2ConfigLayerMetadata;
    readonly [key: string]: unknown;
};

export type V2PatchApplyStatus = "inProgress" | "completed" | "failed" | "declined";

export type V2PatchChangeKind = V2AddPatchChangeKind | V2DeletePatchChangeKind | V2UpdatePatchChangeKind;

export type V2AddPatchChangeKind = {
    readonly "type": V2AddPatchChangeKindType;
    readonly [key: string]: unknown;
};

export type V2AddPatchChangeKindType = "add";

export type V2DeletePatchChangeKind = {
    readonly "type": V2DeletePatchChangeKindType;
    readonly [key: string]: unknown;
};

export type V2DeletePatchChangeKindType = "delete";

export type V2UpdatePatchChangeKind = {
    readonly "move_path"?: string | null;
    readonly "type": V2UpdatePatchChangeKindType;
    readonly [key: string]: unknown;
};

export type V2UpdatePatchChangeKindType = "update";

export type V2PathUri = string;

export type V2PermissionProfileListParams = {
    readonly "cursor"?: string | null;
    readonly "cwd"?: string | null;
    readonly "limit"?: number | null;
    readonly [key: string]: unknown;
};

export type V2PermissionProfileListResponse = {
    readonly "data": V2PermissionProfileListResponseData;
    readonly "nextCursor"?: string | null;
    readonly [key: string]: unknown;
};

export type V2PermissionProfileListResponseData = ReadonlyArray<V2PermissionProfileSummary>;

export type V2PermissionProfileSummary = {
    readonly "allowed": boolean;
    readonly "description"?: string | null;
    readonly "id": string;
    readonly [key: string]: unknown;
};

export type V2Personality = "none" | "friendly" | "pragmatic";

export type V2PlanDeltaNotification = {
    readonly "delta": string;
    readonly "itemId": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2PlanType = "free" | "go" | "plus" | "pro" | "prolite" | "team" | "self_serve_business_prolite" | "self_serve_business_usage_based" | "business" | "ent26" | "enterprise_cbp_automation" | "enterprise_cbp_usage_based" | "enterprise" | "edu" | "edu_plus" | "edu_pro" | "unknown";

export type V2PluginAuthPolicy = "ON_INSTALL" | "ON_USE";

export type V2PluginAvailability = V2PluginAvailabilityOneOf1 | V2PluginAvailabilityOneOf2;

export type V2PluginAvailabilityOneOf1 = "DISABLED_BY_ADMIN";

export type V2PluginAvailabilityOneOf2 = "AVAILABLE";

export type V2PluginDetail = {
    readonly "appTemplates": V2PluginDetailAppTemplates;
    readonly "apps": V2PluginDetailApps;
    readonly "description"?: string | null;
    readonly "hooks": V2PluginDetailHooks;
    readonly "marketplaceName": string;
    readonly "marketplacePath"?: V2PluginDetailMarketplacePath;
    readonly "mcpServers": V2PluginDetailMcpServers;
    readonly "scheduledTasks"?: V2PluginDetailScheduledTasks;
    readonly "shareUrl"?: string | null;
    readonly "skills": V2PluginDetailSkills;
    readonly "summary": V2PluginSummary;
    readonly [key: string]: unknown;
};

export type V2PluginDetailAppTemplates = ReadonlyArray<V2AppTemplateSummary>;

export type V2PluginDetailApps = ReadonlyArray<V2AppSummary>;

export type V2PluginDetailHooks = ReadonlyArray<V2PluginHookSummary>;

export type V2PluginDetailMarketplacePath = V2AbsolutePathBuf | null;

export type V2PluginDetailMcpServers = ReadonlyArray<string>;

export type V2PluginDetailScheduledTasks = ReadonlyArray<V2ScheduledTaskSummary> | null;

export type V2PluginDetailSkills = ReadonlyArray<V2SkillSummary>;

export type V2PluginDisabledReason = "disabled_by_admin" | "plan_not_eligible" | "required_app_unavailable" | "unknown";

export type V2PluginHookSummary = {
    readonly "eventName": V2HookEventName;
    readonly "key": string;
    readonly [key: string]: unknown;
};

export type V2PluginInstallParams = {
    readonly "installAttemptId"?: string | null;
    readonly "marketplacePath"?: V2PluginInstallParamsMarketplacePath;
    readonly "pluginName": string;
    readonly "remoteMarketplaceName"?: string | null;
    readonly [key: string]: unknown;
};

export type V2PluginInstallParamsMarketplacePath = V2AbsolutePathBuf | null;

export type V2PluginInstallPolicy = "NOT_AVAILABLE" | "AVAILABLE" | "INSTALLED_BY_DEFAULT";

export type V2PluginInstallPolicySource = "WORKSPACE_SETTING" | "IMPLICIT_CANONICAL_APP";

export type V2PluginInstallResponse = {
    readonly "appsNeedingAuth": V2PluginInstallResponseAppsNeedingAuth;
    readonly "authPolicy": V2PluginAuthPolicy;
    readonly [key: string]: unknown;
};

export type V2PluginInstallResponseAppsNeedingAuth = ReadonlyArray<V2AppSummary>;

export type V2PluginInstalledParams = {
    readonly "cwds"?: V2PluginInstalledParamsCwds;
    readonly "installSuggestionPluginNames"?: V2PluginInstalledParamsInstallSuggestionPluginNames;
    readonly [key: string]: unknown;
};

export type V2PluginInstalledParamsCwds = ReadonlyArray<V2AbsolutePathBuf> | null;

export type V2PluginInstalledParamsInstallSuggestionPluginNames = ReadonlyArray<string> | null;

export type V2PluginInstalledResponse = {
    readonly "marketplaceLoadErrors"?: V2PluginInstalledResponseMarketplaceLoadErrors;
    readonly "marketplaces": V2PluginInstalledResponseMarketplaces;
    readonly [key: string]: unknown;
};

export type V2PluginInstalledResponseMarketplaceLoadErrors = ReadonlyArray<V2MarketplaceLoadErrorInfo>;

export type V2PluginInstalledResponseMarketplaces = ReadonlyArray<V2PluginMarketplaceEntry>;

export type V2PluginInterface = {
    readonly "brandColor"?: string | null;
    readonly "capabilities": V2PluginInterfaceCapabilities;
    readonly "category"?: string | null;
    readonly "composerIcon"?: V2PluginInterfaceComposerIcon;
    readonly "composerIconUrl"?: string | null;
    readonly "defaultPrompt"?: V2PluginInterfaceDefaultPrompt;
    readonly "developerName"?: string | null;
    readonly "displayName"?: string | null;
    readonly "logo"?: V2PluginInterfaceLogo;
    readonly "logoDark"?: V2PluginInterfaceLogoDark;
    readonly "logoUrl"?: string | null;
    readonly "logoUrlDark"?: string | null;
    readonly "longDescription"?: string | null;
    readonly "privacyPolicyUrl"?: string | null;
    readonly "screenshotUrls": V2PluginInterfaceScreenshotUrls;
    readonly "screenshots": V2PluginInterfaceScreenshots;
    readonly "shortDescription"?: string | null;
    readonly "termsOfServiceUrl"?: string | null;
    readonly "websiteUrl"?: string | null;
    readonly [key: string]: unknown;
};

export type V2PluginInterfaceCapabilities = ReadonlyArray<string>;

export type V2PluginInterfaceComposerIcon = V2AbsolutePathBuf | null;

export type V2PluginInterfaceDefaultPrompt = ReadonlyArray<string> | null;

export type V2PluginInterfaceLogo = V2AbsolutePathBuf | null;

export type V2PluginInterfaceLogoDark = V2AbsolutePathBuf | null;

export type V2PluginInterfaceScreenshotUrls = ReadonlyArray<string>;

export type V2PluginInterfaceScreenshots = ReadonlyArray<V2AbsolutePathBuf>;

export type V2PluginListMarketplaceKind = "local" | "vertical" | "workspace-directory" | "shared-with-me" | "created-by-me-remote";

export type V2PluginListParams = {
    readonly "cwds"?: V2PluginListParamsCwds;
    readonly "forceRefetch"?: boolean;
    readonly "marketplaceKinds"?: V2PluginListParamsMarketplaceKinds;
    readonly [key: string]: unknown;
};

export type V2PluginListParamsCwds = ReadonlyArray<V2AbsolutePathBuf> | null;

export type V2PluginListParamsMarketplaceKinds = ReadonlyArray<V2PluginListMarketplaceKind> | null;

export type V2PluginListResponse = {
    readonly "featuredPluginIds"?: V2PluginListResponseFeaturedPluginIds;
    readonly "marketplaceLoadErrors"?: V2PluginListResponseMarketplaceLoadErrors;
    readonly "marketplaces": V2PluginListResponseMarketplaces;
    readonly [key: string]: unknown;
};

export type V2PluginListResponseFeaturedPluginIds = ReadonlyArray<string>;

export type V2PluginListResponseMarketplaceLoadErrors = ReadonlyArray<V2MarketplaceLoadErrorInfo>;

export type V2PluginListResponseMarketplaces = ReadonlyArray<V2PluginMarketplaceEntry>;

export type V2PluginMarketplaceEntry = {
    readonly "interface"?: V2PluginMarketplaceEntryInterface;
    readonly "name": string;
    readonly "path"?: V2PluginMarketplaceEntryPath;
    readonly "plugins": V2PluginMarketplaceEntryPlugins;
    readonly [key: string]: unknown;
};

export type V2PluginMarketplaceEntryInterface = V2MarketplaceInterface | null;

export type V2PluginMarketplaceEntryPath = V2AbsolutePathBuf | null;

export type V2PluginMarketplaceEntryPlugins = ReadonlyArray<V2PluginSummary>;

export type V2PluginReadParams = {
    readonly "marketplacePath"?: V2PluginReadParamsMarketplacePath;
    readonly "pluginName": string;
    readonly "remoteMarketplaceName"?: string | null;
    readonly [key: string]: unknown;
};

export type V2PluginReadParamsMarketplacePath = V2AbsolutePathBuf | null;

export type V2PluginReadResponse = {
    readonly "plugin": V2PluginDetail;
    readonly [key: string]: unknown;
};

export type V2PluginSearchResult = {
    readonly "marketplaceName": string;
    readonly "marketplacePath"?: V2PluginSearchResultMarketplacePath;
    readonly "plugin": V2PluginSummary;
    readonly [key: string]: unknown;
};

export type V2PluginSearchResultMarketplacePath = V2AbsolutePathBuf | null;

export type V2PluginSearchScope = "global" | "workspace" | "personal";

export type V2PluginShareCheckoutParams = {
    readonly "remotePluginId": string;
    readonly [key: string]: unknown;
};

export type V2PluginShareCheckoutResponse = {
    readonly "marketplaceName": string;
    readonly "marketplacePath": V2AbsolutePathBuf;
    readonly "pluginId": string;
    readonly "pluginName": string;
    readonly "pluginPath": V2AbsolutePathBuf;
    readonly "remotePluginId": string;
    readonly "remoteVersion"?: string | null;
    readonly [key: string]: unknown;
};

export type V2PluginShareContext = {
    readonly "canPublishToWorkspace"?: boolean | null;
    readonly "creatorAccountUserId"?: string | null;
    readonly "creatorName"?: string | null;
    readonly "discoverability"?: V2PluginShareContextDiscoverability;
    readonly "remotePluginId": string;
    readonly "remoteVersion"?: string | null;
    readonly "sharePrincipals"?: V2PluginShareContextSharePrincipals;
    readonly "shareUrl"?: string | null;
    readonly [key: string]: unknown;
};

export type V2PluginShareContextDiscoverability = V2PluginShareDiscoverability | null;

export type V2PluginShareContextSharePrincipals = ReadonlyArray<V2PluginSharePrincipal> | null;

export type V2PluginShareDeleteParams = {
    readonly "remotePluginId": string;
    readonly [key: string]: unknown;
};

export type V2PluginShareDeleteResponse = {
    readonly [key: string]: unknown;
};

export type V2PluginShareDiscoverability = "LISTED" | "UNLISTED" | "PRIVATE";

export type V2PluginShareListItem = {
    readonly "localPluginPath"?: V2PluginShareListItemLocalPluginPath;
    readonly "plugin": V2PluginSummary;
    readonly [key: string]: unknown;
};

export type V2PluginShareListItemLocalPluginPath = V2AbsolutePathBuf | null;

export type V2PluginShareListParams = {
    readonly [key: string]: unknown;
};

export type V2PluginShareListResponse = {
    readonly "data": V2PluginShareListResponseData;
    readonly [key: string]: unknown;
};

export type V2PluginShareListResponseData = ReadonlyArray<V2PluginShareListItem>;

export type V2PluginSharePrincipal = {
    readonly "name": string;
    readonly "principalId": string;
    readonly "principalType": V2PluginSharePrincipalType;
    readonly "role": V2PluginSharePrincipalRole;
    readonly [key: string]: unknown;
};

export type V2PluginSharePrincipalRole = "reader" | "editor" | "owner";

export type V2PluginSharePrincipalType = "user" | "group" | "workspace";

export type V2PluginShareSaveParams = {
    readonly "discoverability"?: V2PluginShareSaveParamsDiscoverability;
    readonly "pluginPath": V2AbsolutePathBuf;
    readonly "remotePluginId"?: string | null;
    readonly "shareTargets"?: V2PluginShareSaveParamsShareTargets;
    readonly [key: string]: unknown;
};

export type V2PluginShareSaveParamsDiscoverability = V2PluginShareDiscoverability | null;

export type V2PluginShareSaveParamsShareTargets = ReadonlyArray<V2PluginShareTarget> | null;

export type V2PluginShareSaveResponse = {
    readonly "canPublishToWorkspace"?: boolean | null;
    readonly "remotePluginId": string;
    readonly "shareUrl": string;
    readonly [key: string]: unknown;
};

export type V2PluginShareTarget = {
    readonly "principalId": string;
    readonly "principalType": V2PluginSharePrincipalType;
    readonly "role": V2PluginShareTargetRole;
    readonly [key: string]: unknown;
};

export type V2PluginShareTargetRole = "reader" | "editor";

export type V2PluginShareUpdateDiscoverability = "UNLISTED" | "PRIVATE" | "LISTED";

export type V2PluginShareUpdateTargetsParams = {
    readonly "discoverability": V2PluginShareUpdateDiscoverability;
    readonly "remotePluginId": string;
    readonly "shareTargets": V2PluginShareUpdateTargetsParamsShareTargets;
    readonly [key: string]: unknown;
};

export type V2PluginShareUpdateTargetsParamsShareTargets = ReadonlyArray<V2PluginShareTarget>;

export type V2PluginShareUpdateTargetsResponse = {
    readonly "discoverability": V2PluginShareDiscoverability;
    readonly "principals": V2PluginShareUpdateTargetsResponsePrincipals;
    readonly [key: string]: unknown;
};

export type V2PluginShareUpdateTargetsResponsePrincipals = ReadonlyArray<V2PluginSharePrincipal>;

export type V2PluginSkillReadParams = {
    readonly "remoteMarketplaceName": string;
    readonly "remotePluginId": string;
    readonly "skillName": string;
    readonly [key: string]: unknown;
};

export type V2PluginSkillReadResponse = {
    readonly "contents"?: string | null;
    readonly [key: string]: unknown;
};

export type V2PluginSource = V2LocalPluginSource | V2GitPluginSource | V2NpmPluginSource | V2RemotePluginSource;

export type V2LocalPluginSource = {
    readonly "path": V2AbsolutePathBuf;
    readonly "type": V2LocalPluginSourceType;
    readonly [key: string]: unknown;
};

export type V2LocalPluginSourceType = "local";

export type V2GitPluginSource = {
    readonly "path"?: string | null;
    readonly "refName"?: string | null;
    readonly "sha"?: string | null;
    readonly "type": V2GitPluginSourceType;
    readonly "url": string;
    readonly [key: string]: unknown;
};

export type V2GitPluginSourceType = "git";

export type V2NpmPluginSource = {
    readonly "package": string;
    readonly "registry"?: string | null;
    readonly "type": V2NpmPluginSourceType;
    readonly "version"?: string | null;
    readonly [key: string]: unknown;
};

export type V2NpmPluginSourceType = "npm";

export type V2RemotePluginSource = {
    readonly "type": V2RemotePluginSourceType;
    readonly [key: string]: unknown;
};

export type V2RemotePluginSourceType = "remote";

export type V2PluginSummary = {
    readonly "authPolicy": V2PluginAuthPolicy;
    readonly "availability"?: V2PluginSummaryAvailability;
    readonly "disabledReason"?: V2PluginSummaryDisabledReason;
    readonly "eligiblePlanTypes"?: V2PluginSummaryEligiblePlanTypes;
    readonly "enabled": boolean;
    readonly "id": string;
    readonly "installPolicy": V2PluginInstallPolicy;
    readonly "installPolicySource"?: V2PluginSummaryInstallPolicySource;
    readonly "installed": boolean;
    readonly "installedAt"?: number | null;
    readonly "interface"?: V2PluginSummaryInterface;
    readonly "keywords"?: V2PluginSummaryKeywords;
    readonly "localVersion"?: string | null;
    readonly "mustShowInstallationInterstitial"?: boolean | null;
    readonly "name": string;
    readonly "remotePluginId"?: string | null;
    readonly "shareContext"?: V2PluginSummaryShareContext;
    readonly "source": V2PluginSource;
    readonly "version"?: string | null;
    readonly [key: string]: unknown;
};

export type V2PluginSummaryAvailability = V2PluginAvailability;

export type V2PluginSummaryDisabledReason = V2PluginDisabledReason | null;

export type V2PluginSummaryEligiblePlanTypes = ReadonlyArray<string> | null;

export type V2PluginSummaryInstallPolicySource = V2PluginInstallPolicySource | null;

export type V2PluginSummaryInterface = V2PluginInterface | null;

export type V2PluginSummaryKeywords = ReadonlyArray<string>;

export type V2PluginSummaryShareContext = V2PluginShareContext | null;

export type V2PluginUninstallParams = {
    readonly "pluginId": string;
    readonly [key: string]: unknown;
};

export type V2PluginUninstallResponse = {
    readonly [key: string]: unknown;
};

export type V2PluginsMigration = {
    readonly "marketplaceName": string;
    readonly "pluginNames": V2PluginsMigrationPluginNames;
    readonly [key: string]: unknown;
};

export type V2PluginsMigrationPluginNames = ReadonlyArray<string>;

export type V2ProcessExitedNotification = {
    readonly "exitCode": number;
    readonly "processHandle": string;
    readonly "stderr": string;
    readonly "stderrCapReached": boolean;
    readonly "stdout": string;
    readonly "stdoutCapReached": boolean;
    readonly [key: string]: unknown;
};

export type V2ProcessOutputDeltaNotification = {
    readonly "capReached": boolean;
    readonly "deltaBase64": string;
    readonly "processHandle": string;
    readonly "stream": V2ProcessOutputDeltaNotificationStream;
    readonly [key: string]: unknown;
};

export type V2ProcessOutputDeltaNotificationStream = V2ProcessOutputStream;

export type V2ProcessOutputStream = V2ProcessOutputStreamOneOf1 | V2ProcessOutputStreamOneOf2;

export type V2ProcessOutputStreamOneOf1 = "stdout";

export type V2ProcessOutputStreamOneOf2 = "stderr";

export type V2ProcessTerminalSize = {
    readonly "cols": number;
    readonly "rows": number;
    readonly [key: string]: unknown;
};

export type V2Project = {
    readonly "createdAt": number;
    readonly "id": string;
    readonly "metadata": V2ProjectMetadata;
    readonly "name": string;
    readonly "position": number;
    readonly "roots": V2ProjectRoots;
    readonly "updatedAt": number;
    readonly [key: string]: unknown;
};

export type V2ProjectMetadata = {
    readonly [key: string]: unknown;
};

export type V2ProjectRoots = ReadonlyArray<V2ProjectRoot>;

export type V2ProjectChangeType = "created" | "updated" | "deleted";

export type V2ProjectChangedNotification = {
    readonly "changeType": V2ProjectChangeType;
    readonly "projectId": string;
    readonly [key: string]: unknown;
};

export type V2ProjectRoot = {
    readonly "path": V2AbsolutePathBuf;
    readonly [key: string]: unknown;
};

export type V2QueuedSubmission = {
    readonly "clientUserMessageId": string;
    readonly "id": string;
    readonly "input": V2QueuedSubmissionInput;
    readonly [key: string]: unknown;
};

export type V2QueuedSubmissionInput = ReadonlyArray<V2UserInput>;

export type V2RateLimitReachedType = "rate_limit_reached" | "workspace_owner_credits_depleted" | "workspace_member_credits_depleted" | "workspace_owner_usage_limit_reached" | "workspace_member_usage_limit_reached";

export type V2RateLimitResetCredit = {
    readonly "description"?: string | null;
    readonly "expiresAt"?: number | null;
    readonly "grantedAt": number;
    readonly "id": string;
    readonly "resetType": V2RateLimitResetType;
    readonly "status": V2RateLimitResetCreditStatus;
    readonly "title"?: string | null;
    readonly [key: string]: unknown;
};

export type V2RateLimitResetCreditStatus = "available" | "redeeming" | "redeemed" | "unknown";

export type V2RateLimitResetCreditsSummary = {
    readonly "availableCount": number;
    readonly "credits"?: V2RateLimitResetCreditsSummaryCredits;
    readonly [key: string]: unknown;
};

export type V2RateLimitResetCreditsSummaryCredits = ReadonlyArray<V2RateLimitResetCredit> | null;

export type V2RateLimitResetType = "codexRateLimits" | "unknown";

export type V2RateLimitSnapshot = {
    readonly "credits"?: V2RateLimitSnapshotCredits;
    readonly "individualLimit"?: V2RateLimitSnapshotIndividualLimit;
    readonly "limitId"?: string | null;
    readonly "limitName"?: string | null;
    readonly "planType"?: V2RateLimitSnapshotPlanType;
    readonly "primary"?: V2RateLimitSnapshotPrimary;
    readonly "rateLimitReachedType"?: V2RateLimitSnapshotRateLimitReachedType;
    readonly "secondary"?: V2RateLimitSnapshotSecondary;
    readonly "spendControlReached"?: boolean | null;
    readonly [key: string]: unknown;
};

export type V2RateLimitSnapshotCredits = V2CreditsSnapshot | null;

export type V2RateLimitSnapshotIndividualLimit = V2SpendControlLimitSnapshot | null;

export type V2RateLimitSnapshotPlanType = V2PlanType | null;

export type V2RateLimitSnapshotPrimary = V2RateLimitWindow | null;

export type V2RateLimitSnapshotRateLimitReachedType = V2RateLimitReachedType | null;

export type V2RateLimitSnapshotSecondary = V2RateLimitWindow | null;

export type V2RateLimitWindow = {
    readonly "resetsAt"?: number | null;
    readonly "usedPercent": number;
    readonly "windowDurationMins"?: number | null;
    readonly [key: string]: unknown;
};

export type V2RawResponseCompletedNotification = {
    readonly "responseId": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly "usage"?: V2RawResponseCompletedNotificationUsage;
    readonly [key: string]: unknown;
};

export type V2RawResponseCompletedNotificationUsage = V2TokenUsageBreakdown | null;

export type V2RawResponseItemCompletedNotification = {
    readonly "item": V2ResponseItem;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2RealtimeConversationVersion = "v1" | "v2" | "v3";

export type V2RealtimeOutputModality = "text" | "audio";

export type V2RealtimeVoice = "alloy" | "arbor" | "ash" | "ballad" | "breeze" | "cedar" | "coral" | "cove" | "echo" | "ember" | "juniper" | "maple" | "marin" | "sage" | "shimmer" | "sol" | "spruce" | "vale" | "verse";

export type V2RealtimeVoicesList = {
    readonly "defaultV1": V2RealtimeVoice;
    readonly "defaultV2": V2RealtimeVoice;
    readonly "v1": V2RealtimeVoicesListV1;
    readonly "v2": V2RealtimeVoicesListV2;
    readonly [key: string]: unknown;
};

export type V2RealtimeVoicesListV1 = ReadonlyArray<V2RealtimeVoice>;

export type V2RealtimeVoicesListV2 = ReadonlyArray<V2RealtimeVoice>;

export type V2ReasoningEffort = string;

export type V2ReasoningEffortOption = {
    readonly "description": string;
    readonly "reasoningEffort": V2ReasoningEffort;
    readonly [key: string]: unknown;
};

export type V2ReasoningItemContent = V2ReasoningTextReasoningItemContent | V2TextReasoningItemContent;

export type V2ReasoningTextReasoningItemContent = {
    readonly "text": string;
    readonly "type": V2ReasoningTextReasoningItemContentType;
    readonly [key: string]: unknown;
};

export type V2ReasoningTextReasoningItemContentType = "reasoning_text";

export type V2TextReasoningItemContent = {
    readonly "text": string;
    readonly "type": V2TextReasoningItemContentType;
    readonly [key: string]: unknown;
};

export type V2TextReasoningItemContentType = "text";

export type V2ReasoningItemReasoningSummary = V2SummaryTextReasoningItemReasoningSummary;

export type V2SummaryTextReasoningItemReasoningSummary = {
    readonly "text": string;
    readonly "type": V2SummaryTextReasoningItemReasoningSummaryType;
    readonly [key: string]: unknown;
};

export type V2SummaryTextReasoningItemReasoningSummaryType = "summary_text";

export type V2ReasoningSummary = V2ReasoningSummaryOneOf1 | V2ReasoningSummaryOneOf2;

export type V2ReasoningSummaryOneOf1 = "auto" | "concise" | "detailed";

export type V2ReasoningSummaryOneOf2 = "none";

export type V2ReasoningSummaryPartAddedNotification = {
    readonly "itemId": string;
    readonly "summaryIndex": number;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2ReasoningSummaryTextDeltaNotification = {
    readonly "delta": string;
    readonly "itemId": string;
    readonly "summaryIndex": number;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2ReasoningTextDeltaNotification = {
    readonly "contentIndex": number;
    readonly "delta": string;
    readonly "itemId": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2RemoteControlConnectionStatus = "disabled" | "connecting" | "connected" | "errored";

export type V2RemoteControlDisableParams = {
    readonly "ephemeral"?: boolean;
    readonly [key: string]: unknown;
};

export type V2RemoteControlEnableParams = {
    readonly "ephemeral"?: boolean;
    readonly [key: string]: unknown;
};

export type V2RemoteControlStatusChangedNotification = {
    readonly "environmentId"?: string | null;
    readonly "installationId": string;
    readonly "serverName": string;
    readonly "status": V2RemoteControlConnectionStatus;
    readonly [key: string]: unknown;
};

export type V2RequestId = string | number;

export type V2RequestPermissionProfile = {
    readonly "fileSystem"?: V2RequestPermissionProfileFileSystem;
    readonly "network"?: V2RequestPermissionProfileNetwork;
};

export type V2RequestPermissionProfileFileSystem = V2AdditionalFileSystemPermissions | null;

export type V2RequestPermissionProfileNetwork = V2AdditionalNetworkPermissions | null;

export type V2ResidencyRequirement = "us";

export type V2Resource = {
    readonly "_meta"?: unknown;
    readonly "annotations"?: unknown;
    readonly "description"?: string | null;
    readonly "icons"?: V2ResourceIcons;
    readonly "mimeType"?: string | null;
    readonly "name": string;
    readonly "size"?: number | null;
    readonly "title"?: string | null;
    readonly "uri": string;
    readonly [key: string]: unknown;
};

export type V2ResourceIcons = ReadonlyArray<unknown> | null;

export type V2ResourceContent = V2ResourceContentAnyOf1 | V2ResourceContentAnyOf2;

export type V2ResourceContentAnyOf1 = {
    readonly "_meta"?: unknown;
    readonly "mimeType"?: string | null;
    readonly "text": string;
    readonly "uri": string;
    readonly [key: string]: unknown;
};

export type V2ResourceContentAnyOf2 = {
    readonly "_meta"?: unknown;
    readonly "blob": string;
    readonly "mimeType"?: string | null;
    readonly "uri": string;
    readonly [key: string]: unknown;
};

export type V2ResourceTemplate = {
    readonly "annotations"?: unknown;
    readonly "description"?: string | null;
    readonly "mimeType"?: string | null;
    readonly "name": string;
    readonly "title"?: string | null;
    readonly "uriTemplate": string;
    readonly [key: string]: unknown;
};

export type V2ResponseItem = V2MessageResponseItem | V2AgentMessageResponseItem | V2ReasoningResponseItem | V2LocalShellCallResponseItem | V2FunctionCallResponseItem | V2ToolSearchCallResponseItem | V2FunctionCallOutputResponseItem | V2CustomToolCallResponseItem | V2CustomToolCallOutputResponseItem | V2ToolSearchOutputResponseItem | V2WebSearchCallResponseItem | V2ImageGenerationCallResponseItem | V2CompactionResponseItem | V2CompactionTriggerResponseItem | V2ContextCompactionResponseItem | V2OtherResponseItem;

export type V2MessageResponseItem = {
    readonly "content": V2MessageResponseItemContent;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2MessageResponseItemInternalChatMessageMetadataPassthrough;
    readonly "phase"?: V2MessageResponseItemPhase;
    readonly "role": string;
    readonly "type": V2MessageResponseItemType;
    readonly [key: string]: unknown;
};

export type V2MessageResponseItemContent = ReadonlyArray<V2ContentItem>;

export type V2MessageResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2MessageResponseItemPhase = V2MessagePhase | null;

export type V2MessageResponseItemType = "message";

export type V2AgentMessageResponseItem = {
    readonly "author": string;
    readonly "content": V2AgentMessageResponseItemContent;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2AgentMessageResponseItemInternalChatMessageMetadataPassthrough;
    readonly "recipient": string;
    readonly "type": V2AgentMessageResponseItemType;
    readonly [key: string]: unknown;
};

export type V2AgentMessageResponseItemContent = ReadonlyArray<V2AgentMessageInputContent>;

export type V2AgentMessageResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2AgentMessageResponseItemType = "agent_message";

export type V2ReasoningResponseItem = {
    readonly "content"?: V2ReasoningResponseItemContent;
    readonly "encrypted_content"?: string | null;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2ReasoningResponseItemInternalChatMessageMetadataPassthrough;
    readonly "summary": V2ReasoningResponseItemSummary;
    readonly "type": V2ReasoningResponseItemType;
    readonly [key: string]: unknown;
};

export type V2ReasoningResponseItemContent = ReadonlyArray<V2ReasoningItemContent> | null;

export type V2ReasoningResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2ReasoningResponseItemSummary = ReadonlyArray<V2ReasoningItemReasoningSummary>;

export type V2ReasoningResponseItemType = "reasoning";

export type V2LocalShellCallResponseItem = {
    readonly "action": V2LocalShellAction;
    readonly "call_id"?: string | null;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2LocalShellCallResponseItemInternalChatMessageMetadataPassthrough;
    readonly "status": V2LocalShellStatus;
    readonly "type": V2LocalShellCallResponseItemType;
    readonly [key: string]: unknown;
};

export type V2LocalShellCallResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2LocalShellCallResponseItemType = "local_shell_call";

export type V2FunctionCallResponseItem = {
    readonly "arguments": string;
    readonly "call_id": string;
    readonly "encrypted_function_args"?: V2FunctionCallResponseItemEncryptedFunctionArgs;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2FunctionCallResponseItemInternalChatMessageMetadataPassthrough;
    readonly "name": string;
    readonly "namespace"?: string | null;
    readonly "type": V2FunctionCallResponseItemType;
    readonly [key: string]: unknown;
};

export type V2FunctionCallResponseItemEncryptedFunctionArgs = ReadonlyArray<string> | null;

export type V2FunctionCallResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2FunctionCallResponseItemType = "function_call";

export type V2ToolSearchCallResponseItem = {
    readonly "arguments": unknown;
    readonly "call_id"?: string | null;
    readonly "execution": string;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2ToolSearchCallResponseItemInternalChatMessageMetadataPassthrough;
    readonly "status"?: string | null;
    readonly "type": V2ToolSearchCallResponseItemType;
    readonly [key: string]: unknown;
};

export type V2ToolSearchCallResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2ToolSearchCallResponseItemType = "tool_search_call";

export type V2FunctionCallOutputResponseItem = {
    readonly "call_id"?: string | null;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2FunctionCallOutputResponseItemInternalChatMessageMetadataPassthrough;
    readonly "name"?: string | null;
    readonly "namespace"?: string | null;
    readonly "output": V2FunctionCallOutputBody;
    readonly "type": V2FunctionCallOutputResponseItemType;
    readonly [key: string]: unknown;
};

export type V2FunctionCallOutputResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2FunctionCallOutputResponseItemType = "function_call_output";

export type V2CustomToolCallResponseItem = {
    readonly "call_id": string;
    readonly "id"?: string | null;
    readonly "input": string;
    readonly "internal_chat_message_metadata_passthrough"?: V2CustomToolCallResponseItemInternalChatMessageMetadataPassthrough;
    readonly "name": string;
    readonly "namespace"?: string | null;
    readonly "status"?: string | null;
    readonly "type": V2CustomToolCallResponseItemType;
    readonly [key: string]: unknown;
};

export type V2CustomToolCallResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2CustomToolCallResponseItemType = "custom_tool_call";

export type V2CustomToolCallOutputResponseItem = {
    readonly "call_id": string;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2CustomToolCallOutputResponseItemInternalChatMessageMetadataPassthrough;
    readonly "name"?: string | null;
    readonly "output": V2FunctionCallOutputBody;
    readonly "type": V2CustomToolCallOutputResponseItemType;
    readonly [key: string]: unknown;
};

export type V2CustomToolCallOutputResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2CustomToolCallOutputResponseItemType = "custom_tool_call_output";

export type V2ToolSearchOutputResponseItem = {
    readonly "call_id"?: string | null;
    readonly "execution": string;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2ToolSearchOutputResponseItemInternalChatMessageMetadataPassthrough;
    readonly "status": string;
    readonly "tools": V2ToolSearchOutputResponseItemTools;
    readonly "type": V2ToolSearchOutputResponseItemType;
    readonly [key: string]: unknown;
};

export type V2ToolSearchOutputResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2ToolSearchOutputResponseItemTools = ReadonlyArray<unknown>;

export type V2ToolSearchOutputResponseItemType = "tool_search_output";

export type V2WebSearchCallResponseItem = {
    readonly "action"?: V2WebSearchCallResponseItemAction;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2WebSearchCallResponseItemInternalChatMessageMetadataPassthrough;
    readonly "status"?: string | null;
    readonly "type": V2WebSearchCallResponseItemType;
    readonly [key: string]: unknown;
};

export type V2WebSearchCallResponseItemAction = V2ResponsesApiWebSearchAction | null;

export type V2WebSearchCallResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2WebSearchCallResponseItemType = "web_search_call";

export type V2ImageGenerationCallResponseItem = {
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2ImageGenerationCallResponseItemInternalChatMessageMetadataPassthrough;
    readonly "result": string;
    readonly "revised_prompt"?: string | null;
    readonly "status": string;
    readonly "type": V2ImageGenerationCallResponseItemType;
    readonly [key: string]: unknown;
};

export type V2ImageGenerationCallResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2ImageGenerationCallResponseItemType = "image_generation_call";

export type V2CompactionResponseItem = {
    readonly "encrypted_content": string;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2CompactionResponseItemInternalChatMessageMetadataPassthrough;
    readonly "type": V2CompactionResponseItemType;
    readonly [key: string]: unknown;
};

export type V2CompactionResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2CompactionResponseItemType = "compaction";

export type V2CompactionTriggerResponseItem = {
    readonly "type": V2CompactionTriggerResponseItemType;
    readonly [key: string]: unknown;
};

export type V2CompactionTriggerResponseItemType = "compaction_trigger";

export type V2ContextCompactionResponseItem = {
    readonly "encrypted_content"?: string | null;
    readonly "id"?: string | null;
    readonly "internal_chat_message_metadata_passthrough"?: V2ContextCompactionResponseItemInternalChatMessageMetadataPassthrough;
    readonly "type": V2ContextCompactionResponseItemType;
    readonly [key: string]: unknown;
};

export type V2ContextCompactionResponseItemInternalChatMessageMetadataPassthrough = V2InternalChatMessageMetadataPassthrough | null;

export type V2ContextCompactionResponseItemType = "context_compaction";

export type V2OtherResponseItem = {
    readonly "type": V2OtherResponseItemType;
    readonly [key: string]: unknown;
};

export type V2OtherResponseItemType = "other";

export type V2ResponsesApiWebSearchAction = V2SearchResponsesApiWebSearchAction | V2OpenPageResponsesApiWebSearchAction | V2FindInPageResponsesApiWebSearchAction | V2OtherResponsesApiWebSearchAction;

export type V2SearchResponsesApiWebSearchAction = {
    readonly "queries"?: V2SearchResponsesApiWebSearchActionQueries;
    readonly "query"?: string | null;
    readonly "type": V2SearchResponsesApiWebSearchActionType;
    readonly [key: string]: unknown;
};

export type V2SearchResponsesApiWebSearchActionQueries = ReadonlyArray<string> | null;

export type V2SearchResponsesApiWebSearchActionType = "search";

export type V2OpenPageResponsesApiWebSearchAction = {
    readonly "type": V2OpenPageResponsesApiWebSearchActionType;
    readonly "url"?: string | null;
    readonly [key: string]: unknown;
};

export type V2OpenPageResponsesApiWebSearchActionType = "open_page";

export type V2FindInPageResponsesApiWebSearchAction = {
    readonly "pattern"?: string | null;
    readonly "type": V2FindInPageResponsesApiWebSearchActionType;
    readonly "url"?: string | null;
    readonly [key: string]: unknown;
};

export type V2FindInPageResponsesApiWebSearchActionType = "find_in_page";

export type V2OtherResponsesApiWebSearchAction = {
    readonly "type": V2OtherResponsesApiWebSearchActionType;
    readonly [key: string]: unknown;
};

export type V2OtherResponsesApiWebSearchActionType = "other";

export type V2ReviewDelivery = "inline" | "detached";

export type V2ReviewStartParams = {
    readonly "delivery"?: V2ReviewStartParamsDelivery;
    readonly "target": V2ReviewTarget;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ReviewStartParamsDelivery = V2ReviewDelivery | null;

export type V2ReviewStartResponse = {
    readonly "reviewThreadId": string;
    readonly "turn": V2Turn;
    readonly [key: string]: unknown;
};

export type V2ReviewTarget = V2UncommittedChangesReviewTarget | V2BaseBranchReviewTarget | V2CommitReviewTarget | V2CustomReviewTarget;

export type V2UncommittedChangesReviewTarget = {
    readonly "type": V2UncommittedChangesReviewTargetType;
    readonly [key: string]: unknown;
};

export type V2UncommittedChangesReviewTargetType = "uncommittedChanges";

export type V2BaseBranchReviewTarget = {
    readonly "branch": string;
    readonly "type": V2BaseBranchReviewTargetType;
    readonly [key: string]: unknown;
};

export type V2BaseBranchReviewTargetType = "baseBranch";

export type V2CommitReviewTarget = {
    readonly "sha": string;
    readonly "title"?: string | null;
    readonly "type": V2CommitReviewTargetType;
    readonly [key: string]: unknown;
};

export type V2CommitReviewTargetType = "commit";

export type V2CustomReviewTarget = {
    readonly "instructions": string;
    readonly "type": V2CustomReviewTargetType;
    readonly [key: string]: unknown;
};

export type V2CustomReviewTargetType = "custom";

export type V2SandboxMode = "read-only" | "workspace-write" | "danger-full-access";

export type V2SandboxPolicy = V2DangerFullAccessSandboxPolicy | V2ReadOnlySandboxPolicy | V2ExternalSandboxSandboxPolicy | V2WorkspaceWriteSandboxPolicy;

export type V2DangerFullAccessSandboxPolicy = {
    readonly "type": V2DangerFullAccessSandboxPolicyType;
    readonly [key: string]: unknown;
};

export type V2DangerFullAccessSandboxPolicyType = "dangerFullAccess";

export type V2ReadOnlySandboxPolicy = {
    readonly "networkAccess"?: boolean;
    readonly "type": V2ReadOnlySandboxPolicyType;
    readonly [key: string]: unknown;
};

export type V2ReadOnlySandboxPolicyType = "readOnly";

export type V2ExternalSandboxSandboxPolicy = {
    readonly "networkAccess"?: V2ExternalSandboxSandboxPolicyNetworkAccess;
    readonly "type": V2ExternalSandboxSandboxPolicyType;
    readonly [key: string]: unknown;
};

export type V2ExternalSandboxSandboxPolicyNetworkAccess = V2NetworkAccess;

export type V2ExternalSandboxSandboxPolicyType = "externalSandbox";

export type V2WorkspaceWriteSandboxPolicy = {
    readonly "excludeSlashTmp"?: boolean;
    readonly "excludeTmpdirEnvVar"?: boolean;
    readonly "networkAccess"?: boolean;
    readonly "type": V2WorkspaceWriteSandboxPolicyType;
    readonly "writableRoots"?: V2WorkspaceWriteSandboxPolicyWritableRoots;
    readonly [key: string]: unknown;
};

export type V2WorkspaceWriteSandboxPolicyType = "workspaceWrite";

export type V2WorkspaceWriteSandboxPolicyWritableRoots = ReadonlyArray<V2AbsolutePathBuf>;

export type V2SandboxWorkspaceWrite = {
    readonly "exclude_slash_tmp"?: boolean;
    readonly "exclude_tmpdir_env_var"?: boolean;
    readonly "network_access"?: boolean;
    readonly "writable_roots"?: V2SandboxWorkspaceWriteWritableRoots;
    readonly [key: string]: unknown;
};

export type V2SandboxWorkspaceWriteWritableRoots = ReadonlyArray<string>;

export type V2ScheduledTaskSchedule = V2HourlyScheduledTaskSchedule | V2DailyScheduledTaskSchedule | V2WeekdaysScheduledTaskSchedule | V2WeeklyScheduledTaskSchedule;

export type V2HourlyScheduledTaskSchedule = {
    readonly "days"?: V2HourlyScheduledTaskScheduleDays;
    readonly "intervalHours": number;
    readonly "type": V2HourlyScheduledTaskScheduleType;
    readonly [key: string]: unknown;
};

export type V2HourlyScheduledTaskScheduleDays = ReadonlyArray<V2ScheduledTaskWeekday> | null;

export type V2HourlyScheduledTaskScheduleType = "hourly";

export type V2DailyScheduledTaskSchedule = {
    readonly "time": string;
    readonly "type": V2DailyScheduledTaskScheduleType;
    readonly [key: string]: unknown;
};

export type V2DailyScheduledTaskScheduleType = "daily";

export type V2WeekdaysScheduledTaskSchedule = {
    readonly "time": string;
    readonly "type": V2WeekdaysScheduledTaskScheduleType;
    readonly [key: string]: unknown;
};

export type V2WeekdaysScheduledTaskScheduleType = "weekdays";

export type V2WeeklyScheduledTaskSchedule = {
    readonly "days": V2WeeklyScheduledTaskScheduleDays;
    readonly "time": string;
    readonly "type": V2WeeklyScheduledTaskScheduleType;
    readonly [key: string]: unknown;
};

export type V2WeeklyScheduledTaskScheduleDays = ReadonlyArray<V2ScheduledTaskWeekday>;

export type V2WeeklyScheduledTaskScheduleType = "weekly";

export type V2ScheduledTaskSummary = {
    readonly "key": string;
    readonly "name": string;
    readonly "prompt": string;
    readonly "schedule": V2ScheduledTaskSchedule;
    readonly [key: string]: unknown;
};

export type V2ScheduledTaskWeekday = "MO" | "TU" | "WE" | "TH" | "FR" | "SA" | "SU";

export type V2SelectedCapabilityRoot = {
    readonly "id": string;
    readonly "location": V2SelectedCapabilityRootLocation;
    readonly [key: string]: unknown;
};

export type V2SelectedCapabilityRootLocation = V2CapabilityRootLocation;

export type V2SendAddCreditsNudgeEmailParams = {
    readonly "creditType": V2AddCreditsNudgeCreditType;
    readonly [key: string]: unknown;
};

export type V2SendAddCreditsNudgeEmailResponse = {
    readonly "status": V2AddCreditsNudgeEmailStatus;
    readonly [key: string]: unknown;
};

export type V2ServerDiagnosticsGauge = {
    readonly "name": string;
    readonly "value": number;
    readonly [key: string]: unknown;
};

export type V2ServerDiagnosticsProcess = {
    readonly "id": number;
    readonly "physicalFootprintBytes"?: number | null;
    readonly "residentMemoryBytes"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ServerRequestResolvedNotification = {
    readonly "requestId": V2RequestId;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2SessionMigration = {
    readonly "cwd": string;
    readonly "path": string;
    readonly "title"?: string | null;
    readonly [key: string]: unknown;
};

export type V2SessionSource = V2SessionSourceOneOf1 | V2CustomSessionSource | V2SubAgentSessionSource;

export type V2SessionSourceOneOf1 = "cli" | "vscode" | "exec" | "appServer" | "unknown";

export type V2CustomSessionSource = {
    readonly "custom": string;
};

export type V2SubAgentSessionSource = {
    readonly "subAgent": V2SubAgentSource;
};

export type V2Settings = {
    readonly "developer_instructions"?: string | null;
    readonly "model": string;
    readonly "reasoning_effort"?: V2SettingsReasoningEffort;
    readonly [key: string]: unknown;
};

export type V2SettingsReasoningEffort = V2ReasoningEffort | null;

export type V2SkillDependencies = {
    readonly "tools": V2SkillDependenciesTools;
    readonly [key: string]: unknown;
};

export type V2SkillDependenciesTools = ReadonlyArray<V2SkillToolDependency>;

export type V2SkillErrorInfo = {
    readonly "message": string;
    readonly "path": string;
    readonly [key: string]: unknown;
};

export type V2SkillInterface = {
    readonly "brandColor"?: string | null;
    readonly "defaultPrompt"?: string | null;
    readonly "displayName"?: string | null;
    readonly "iconLarge"?: V2SkillInterfaceIconLarge;
    readonly "iconLargeUrl"?: string | null;
    readonly "iconSmall"?: V2SkillInterfaceIconSmall;
    readonly "iconSmallUrl"?: string | null;
    readonly "shortDescription"?: string | null;
    readonly [key: string]: unknown;
};

export type V2SkillInterfaceIconLarge = V2AbsolutePathBuf | null;

export type V2SkillInterfaceIconSmall = V2AbsolutePathBuf | null;

export type V2SkillMetadata = {
    readonly "dependencies"?: V2SkillMetadataDependencies;
    readonly "description": string;
    readonly "enabled": boolean;
    readonly "interface"?: V2SkillMetadataInterface;
    readonly "name": string;
    readonly "path": V2AbsolutePathBuf;
    readonly "scope": V2SkillScope;
    readonly "shortDescription"?: string | null;
    readonly [key: string]: unknown;
};

export type V2SkillMetadataDependencies = V2SkillDependencies | null;

export type V2SkillMetadataInterface = V2SkillInterface | null;

export type V2SkillMigration = {
    readonly "name": string;
    readonly [key: string]: unknown;
};

export type V2SkillScope = "user" | "repo" | "system" | "admin";

export type V2SkillSummary = {
    readonly "description": string;
    readonly "enabled": boolean;
    readonly "interface"?: V2SkillSummaryInterface;
    readonly "name": string;
    readonly "path"?: V2SkillSummaryPath;
    readonly "shortDescription"?: string | null;
    readonly [key: string]: unknown;
};

export type V2SkillSummaryInterface = V2SkillInterface | null;

export type V2SkillSummaryPath = V2AbsolutePathBuf | null;

export type V2SkillToolDependency = {
    readonly "command"?: string | null;
    readonly "description"?: string | null;
    readonly "transport"?: string | null;
    readonly "type": string;
    readonly "url"?: string | null;
    readonly "value": string;
    readonly [key: string]: unknown;
};

export type V2SkillsChangedNotification = {
    readonly [key: string]: unknown;
};

export type V2SkillsConfigWriteParams = {
    readonly "enabled": boolean;
    readonly "name"?: string | null;
    readonly "path"?: V2SkillsConfigWriteParamsPath;
    readonly [key: string]: unknown;
};

export type V2SkillsConfigWriteParamsPath = V2AbsolutePathBuf | null;

export type V2SkillsConfigWriteResponse = {
    readonly "effectiveEnabled": boolean;
    readonly [key: string]: unknown;
};

export type V2SkillsExtraRootsSetParams = {
    readonly "extraRoots": V2SkillsExtraRootsSetParamsExtraRoots;
    readonly [key: string]: unknown;
};

export type V2SkillsExtraRootsSetParamsExtraRoots = ReadonlyArray<V2AbsolutePathBuf>;

export type V2SkillsExtraRootsSetResponse = {
    readonly [key: string]: unknown;
};

export type V2SkillsListEntry = {
    readonly "cwd": string;
    readonly "errors": V2SkillsListEntryErrors;
    readonly "skills": V2SkillsListEntrySkills;
    readonly [key: string]: unknown;
};

export type V2SkillsListEntryErrors = ReadonlyArray<V2SkillErrorInfo>;

export type V2SkillsListEntrySkills = ReadonlyArray<V2SkillMetadata>;

export type V2SkillsListParams = {
    readonly "cwds"?: V2SkillsListParamsCwds;
    readonly "forceReload"?: boolean;
    readonly [key: string]: unknown;
};

export type V2SkillsListParamsCwds = ReadonlyArray<string>;

export type V2SkillsListResponse = {
    readonly "data": V2SkillsListResponseData;
    readonly [key: string]: unknown;
};

export type V2SkillsListResponseData = ReadonlyArray<V2SkillsListEntry>;

export type V2SortDirection = "asc" | "desc";

export type V2SpendControlLimitSnapshot = {
    readonly "limit": string;
    readonly "remainingPercent": number;
    readonly "resetsAt": number;
    readonly "used": string;
    readonly [key: string]: unknown;
};

export type V2StrictReviewRequiredNotification = {
    readonly "startedAtMs": number;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2SubAgentActivityKind = "started" | "interacted" | "interrupted";

export type V2SubAgentSource = V2SubAgentSourceOneOf1 | V2ThreadSpawnSubAgentSource | V2OtherSubAgentSource;

export type V2SubAgentSourceOneOf1 = "review" | "compact" | "memory_consolidation";

export type V2ThreadSpawnSubAgentSource = {
    readonly "thread_spawn": V2ThreadSpawnSubAgentSourceThreadSpawn;
};

export type V2ThreadSpawnSubAgentSourceThreadSpawn = {
    readonly "agent_nickname"?: string | null;
    readonly "agent_path"?: V2ThreadSpawnSubAgentSourceThreadSpawnAgentPath;
    readonly "agent_role"?: string | null;
    readonly "depth": number;
    readonly "parent_thread_id": V2ThreadId;
    readonly [key: string]: unknown;
};

export type V2ThreadSpawnSubAgentSourceThreadSpawnAgentPath = V2AgentPath | null;

export type V2OtherSubAgentSource = {
    readonly "other": string;
};

export type V2SubagentMigration = {
    readonly "name": string;
    readonly [key: string]: unknown;
};

export type V2TerminalInteractionNotification = {
    readonly "itemId": string;
    readonly "processId": string;
    readonly "stdin": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2TextElement = {
    readonly "byteRange": V2TextElementByteRange;
    readonly "placeholder"?: string | null;
    readonly [key: string]: unknown;
};

export type V2TextElementByteRange = V2ByteRange;

export type V2TextPosition = {
    readonly "column": number;
    readonly "line": number;
    readonly [key: string]: unknown;
};

export type V2TextRange = {
    readonly "end": V2TextPosition;
    readonly "start": V2TextPosition;
    readonly [key: string]: unknown;
};

export type V2Thread = {
    readonly "agentNickname"?: string | null;
    readonly "agentRole"?: string | null;
    readonly "cliVersion": string;
    readonly "createdAt": number;
    readonly "cwd": V2ThreadCwd;
    readonly "ephemeral": boolean;
    readonly "forkedFromId"?: string | null;
    readonly "gitInfo"?: V2ThreadGitInfo;
    readonly "id": string;
    readonly "modelProvider": string;
    readonly "name"?: string | null;
    readonly "parentThreadId"?: string | null;
    readonly "path"?: string | null;
    readonly "preview": string;
    readonly "projectId": string | null;
    readonly "recencyAt"?: number | null;
    readonly "section"?: V2ThreadSection2;
    readonly "sectionEnteredAt"?: number | null;
    readonly "sessionId": string;
    readonly "source": V2ThreadSource2;
    readonly "status": V2ThreadStatus2;
    readonly "threadSource"?: V2ThreadThreadSource;
    readonly "turns": V2ThreadTurns;
    readonly "updatedAt": number;
    readonly [key: string]: unknown;
};

export type V2ThreadCwd = V2AbsolutePathBuf;

export type V2ThreadGitInfo = V2GitInfo | null;

export type V2ThreadSection2 = V2ThreadSection | null;

export type V2ThreadSource2 = V2SessionSource;

export type V2ThreadStatus2 = V2ThreadStatus;

export type V2ThreadThreadSource = V2ThreadSource | null;

export type V2ThreadTurns = ReadonlyArray<V2Turn>;

export type V2ThreadActiveFlag = "waitingOnApproval" | "waitingOnUserInput";

export type V2ThreadApproveGuardianDeniedActionParams = {
    readonly "event": unknown;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadApproveGuardianDeniedActionResponse = {
    readonly [key: string]: unknown;
};

export type V2ThreadArchiveParams = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadArchiveResponse = {
    readonly [key: string]: unknown;
};

export type V2ThreadArchivedNotification = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadClosedNotification = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadCompactStartParams = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadCompactStartResponse = {
    readonly [key: string]: unknown;
};

export type V2ThreadDeleteParams = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadDeleteResponse = {
    readonly [key: string]: unknown;
};

export type V2ThreadDeletedNotification = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadExtra = {
    readonly [key: string]: unknown;
};

export type V2ThreadForkParams = {
    readonly "approvalPolicy"?: V2ThreadForkParamsApprovalPolicy;
    readonly "approvalsReviewer"?: V2ThreadForkParamsApprovalsReviewer;
    readonly "baseInstructions"?: string | null;
    readonly "config"?: V2ThreadForkParamsConfig;
    readonly "cwd"?: string | null;
    readonly "developerInstructions"?: string | null;
    readonly "ephemeral"?: boolean;
    readonly "lastTurnId"?: string | null;
    readonly "model"?: string | null;
    readonly "modelProvider"?: string | null;
    readonly "sandbox"?: V2ThreadForkParamsSandbox;
    readonly "serviceTier"?: string | null;
    readonly "threadId": string;
    readonly "threadSource"?: V2ThreadForkParamsThreadSource;
    readonly [key: string]: unknown;
};

export type V2ThreadForkParamsApprovalPolicy = V2AskForApproval | null;

export type V2ThreadForkParamsApprovalsReviewer = V2ApprovalsReviewer | null;

export type V2ThreadForkParamsConfig = {
    readonly [key: string]: unknown;
} | null;

export type V2ThreadForkParamsSandbox = V2SandboxMode | null;

export type V2ThreadForkParamsThreadSource = V2ThreadSource | null;

export type V2ThreadForkResponse = {
    readonly "approvalPolicy": V2AskForApproval;
    readonly "approvalsReviewer": V2ThreadForkResponseApprovalsReviewer;
    readonly "cwd": V2AbsolutePathBuf;
    readonly "instructionSources"?: V2ThreadForkResponseInstructionSources;
    readonly "model": string;
    readonly "modelProvider": string;
    readonly "reasoningEffort"?: V2ThreadForkResponseReasoningEffort;
    readonly "sandbox": V2ThreadForkResponseSandbox;
    readonly "serviceTier"?: string | null;
    readonly "thread": V2Thread;
    readonly [key: string]: unknown;
};

export type V2ThreadForkResponseApprovalsReviewer = V2ApprovalsReviewer;

export type V2ThreadForkResponseInstructionSources = ReadonlyArray<V2LegacyAppPathString>;

export type V2ThreadForkResponseReasoningEffort = V2ReasoningEffort | null;

export type V2ThreadForkResponseSandbox = V2SandboxPolicy;

export type V2ThreadGoal = {
    readonly "createdAt": number;
    readonly "objective": string;
    readonly "status": V2ThreadGoalStatus;
    readonly "threadId": string;
    readonly "timeUsedSeconds": number;
    readonly "tokenBudget"?: number | null;
    readonly "tokensUsed": number;
    readonly "updatedAt": number;
    readonly [key: string]: unknown;
};

export type V2ThreadGoalClearParams = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadGoalClearResponse = {
    readonly "cleared": boolean;
    readonly [key: string]: unknown;
};

export type V2ThreadGoalClearedNotification = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadGoalGetParams = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadGoalGetResponse = {
    readonly "goal"?: V2ThreadGoalGetResponseGoal;
    readonly [key: string]: unknown;
};

export type V2ThreadGoalGetResponseGoal = V2ThreadGoal | null;

export type V2ThreadGoalSetParams = {
    readonly "objective"?: string | null;
    readonly "status"?: V2ThreadGoalSetParamsStatus;
    readonly "threadId": string;
    readonly "tokenBudget"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ThreadGoalSetParamsStatus = V2ThreadGoalStatus | null;

export type V2ThreadGoalSetResponse = {
    readonly "goal": V2ThreadGoal;
    readonly [key: string]: unknown;
};

export type V2ThreadGoalStatus = "active" | "paused" | "blocked" | "usageLimited" | "budgetLimited" | "complete";

export type V2ThreadGoalUpdatedNotification = {
    readonly "goal": V2ThreadGoal;
    readonly "threadId": string;
    readonly "turnId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ThreadHistoryMode = "legacy" | "paginated";

export type V2ThreadId = string;

export type V2ThreadInjectItemsParams = {
    readonly "items": V2ThreadInjectItemsParamsItems;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadInjectItemsParamsItems = ReadonlyArray<unknown>;

export type V2ThreadInjectItemsResponse = {
    readonly [key: string]: unknown;
};

export type V2ThreadItem = V2UserMessageThreadItem | V2HookPromptThreadItem | V2AgentMessageThreadItem | V2PlanThreadItem | V2ReasoningThreadItem | V2CommandExecutionThreadItem | V2FileChangeThreadItem | V2McpToolCallThreadItem | V2DynamicToolCallThreadItem | V2CollabAgentToolCallThreadItem | V2SubAgentActivityThreadItem | V2WebSearchThreadItem | V2ImageViewThreadItem | V2SleepThreadItem | V2ImageGenerationThreadItem | V2EnteredReviewModeThreadItem | V2ExitedReviewModeThreadItem | V2ContextCompactionThreadItem;

export type V2UserMessageThreadItem = {
    readonly "clientId"?: string | null;
    readonly "content": V2UserMessageThreadItemContent;
    readonly "id": string;
    readonly "type": V2UserMessageThreadItemType;
    readonly [key: string]: unknown;
};

export type V2UserMessageThreadItemContent = ReadonlyArray<V2UserInput>;

export type V2UserMessageThreadItemType = "userMessage";

export type V2HookPromptThreadItem = {
    readonly "fragments": V2HookPromptThreadItemFragments;
    readonly "id": string;
    readonly "type": V2HookPromptThreadItemType;
    readonly [key: string]: unknown;
};

export type V2HookPromptThreadItemFragments = ReadonlyArray<V2HookPromptFragment>;

export type V2HookPromptThreadItemType = "hookPrompt";

export type V2AgentMessageThreadItem = {
    readonly "delivery"?: V2AgentMessageThreadItemDelivery;
    readonly "id": string;
    readonly "memoryCitation"?: V2AgentMessageThreadItemMemoryCitation;
    readonly "phase"?: V2AgentMessageThreadItemPhase;
    readonly "text": string;
    readonly "type": V2AgentMessageThreadItemType;
    readonly [key: string]: unknown;
};

export type V2AgentMessageThreadItemDelivery = V2AgentMessageDelivery | null;

export type V2AgentMessageThreadItemMemoryCitation = V2MemoryCitation | null;

export type V2AgentMessageThreadItemPhase = V2MessagePhase | null;

export type V2AgentMessageThreadItemType = "agentMessage";

export type V2PlanThreadItem = {
    readonly "id": string;
    readonly "text": string;
    readonly "type": V2PlanThreadItemType;
    readonly [key: string]: unknown;
};

export type V2PlanThreadItemType = "plan";

export type V2ReasoningThreadItem = {
    readonly "content"?: V2ReasoningThreadItemContent;
    readonly "id": string;
    readonly "summary"?: V2ReasoningThreadItemSummary;
    readonly "type": V2ReasoningThreadItemType;
    readonly [key: string]: unknown;
};

export type V2ReasoningThreadItemContent = ReadonlyArray<string>;

export type V2ReasoningThreadItemSummary = ReadonlyArray<string>;

export type V2ReasoningThreadItemType = "reasoning";

export type V2CommandExecutionThreadItem = {
    readonly "aggregatedOutput"?: string | null;
    readonly "command": string;
    readonly "commandActions": V2CommandExecutionThreadItemCommandActions;
    readonly "cwd": V2CommandExecutionThreadItemCwd;
    readonly "durationMs"?: number | null;
    readonly "exitCode"?: number | null;
    readonly "id": string;
    readonly "pluginId"?: string | null;
    readonly "processId"?: string | null;
    readonly "scriptPath"?: string | null;
    readonly "source"?: V2CommandExecutionThreadItemSource;
    readonly "status": V2CommandExecutionStatus;
    readonly "type": V2CommandExecutionThreadItemType;
    readonly [key: string]: unknown;
};

export type V2CommandExecutionThreadItemCommandActions = ReadonlyArray<V2CommandAction>;

export type V2CommandExecutionThreadItemCwd = V2LegacyAppPathString;

export type V2CommandExecutionThreadItemSource = V2CommandExecutionSource;

export type V2CommandExecutionThreadItemType = "commandExecution";

export type V2FileChangeThreadItem = {
    readonly "changes": V2FileChangeThreadItemChanges;
    readonly "id": string;
    readonly "status": V2PatchApplyStatus;
    readonly "type": V2FileChangeThreadItemType;
    readonly [key: string]: unknown;
};

export type V2FileChangeThreadItemChanges = ReadonlyArray<V2FileUpdateChange>;

export type V2FileChangeThreadItemType = "fileChange";

export type V2McpToolCallThreadItem = {
    readonly "appContext"?: V2McpToolCallThreadItemAppContext;
    readonly "arguments": unknown;
    readonly "durationMs"?: number | null;
    readonly "error"?: V2McpToolCallThreadItemError;
    readonly "id": string;
    readonly "mcpAppResourceUri"?: string | null;
    readonly "pluginId"?: string | null;
    readonly "readOnlyHint"?: boolean | null;
    readonly "result"?: V2McpToolCallThreadItemResult;
    readonly "server": string;
    readonly "status": V2McpToolCallStatus;
    readonly "tool": string;
    readonly "type": V2McpToolCallThreadItemType;
    readonly [key: string]: unknown;
};

export type V2McpToolCallThreadItemAppContext = V2McpToolCallAppContext | null;

export type V2McpToolCallThreadItemError = V2McpToolCallError | null;

export type V2McpToolCallThreadItemResult = V2McpToolCallResult | null;

export type V2McpToolCallThreadItemType = "mcpToolCall";

export type V2DynamicToolCallThreadItem = {
    readonly "arguments": unknown;
    readonly "contentItems"?: V2DynamicToolCallThreadItemContentItems;
    readonly "durationMs"?: number | null;
    readonly "id": string;
    readonly "namespace"?: string | null;
    readonly "status": V2DynamicToolCallStatus;
    readonly "success"?: boolean | null;
    readonly "tool": string;
    readonly "type": V2DynamicToolCallThreadItemType;
    readonly [key: string]: unknown;
};

export type V2DynamicToolCallThreadItemContentItems = ReadonlyArray<V2DynamicToolCallOutputContentItem> | null;

export type V2DynamicToolCallThreadItemType = "dynamicToolCall";

export type V2CollabAgentToolCallThreadItem = {
    readonly "agentsStates": V2CollabAgentToolCallThreadItemAgentsStates;
    readonly "id": string;
    readonly "model"?: string | null;
    readonly "prompt"?: string | null;
    readonly "reasoningEffort"?: V2CollabAgentToolCallThreadItemReasoningEffort;
    readonly "receiverThreadIds": V2CollabAgentToolCallThreadItemReceiverThreadIds;
    readonly "senderThreadId": string;
    readonly "status": V2CollabAgentToolCallThreadItemStatus;
    readonly "tool": V2CollabAgentToolCallThreadItemTool;
    readonly "type": V2CollabAgentToolCallThreadItemType;
    readonly [key: string]: unknown;
};

export type V2CollabAgentToolCallThreadItemAgentsStates = {
    readonly [key: string]: unknown;
};

export type V2CollabAgentToolCallThreadItemReasoningEffort = V2ReasoningEffort | null;

export type V2CollabAgentToolCallThreadItemReceiverThreadIds = ReadonlyArray<string>;

export type V2CollabAgentToolCallThreadItemStatus = V2CollabAgentToolCallStatus;

export type V2CollabAgentToolCallThreadItemTool = V2CollabAgentTool;

export type V2CollabAgentToolCallThreadItemType = "collabAgentToolCall";

export type V2SubAgentActivityThreadItem = {
    readonly "agentPath": string;
    readonly "agentThreadId": string;
    readonly "id": string;
    readonly "kind": V2SubAgentActivityKind;
    readonly "type": V2SubAgentActivityThreadItemType;
    readonly [key: string]: unknown;
};

export type V2SubAgentActivityThreadItemType = "subAgentActivity";

export type V2WebSearchThreadItem = {
    readonly "action"?: V2WebSearchThreadItemAction;
    readonly "id": string;
    readonly "query": string;
    readonly "results"?: V2WebSearchThreadItemResults;
    readonly "type": V2WebSearchThreadItemType;
    readonly [key: string]: unknown;
};

export type V2WebSearchThreadItemAction = V2WebSearchAction | null;

export type V2WebSearchThreadItemResults = ReadonlyArray<unknown> | null;

export type V2WebSearchThreadItemType = "webSearch";

export type V2ImageViewThreadItem = {
    readonly "id": string;
    readonly "path": V2LegacyAppPathString;
    readonly "type": V2ImageViewThreadItemType;
    readonly [key: string]: unknown;
};

export type V2ImageViewThreadItemType = "imageView";

export type V2SleepThreadItem = {
    readonly "durationMs": number;
    readonly "id": string;
    readonly "type": V2SleepThreadItemType;
    readonly [key: string]: unknown;
};

export type V2SleepThreadItemType = "sleep";

export type V2ImageGenerationThreadItem = {
    readonly "failure"?: V2ImageGenerationThreadItemFailure;
    readonly "id": string;
    readonly "result": string;
    readonly "revisedPrompt"?: string | null;
    readonly "savedPath"?: V2ImageGenerationThreadItemSavedPath;
    readonly "status": string;
    readonly "transparentBackground"?: boolean | null;
    readonly "type": V2ImageGenerationThreadItemType;
    readonly [key: string]: unknown;
};

export type V2ImageGenerationThreadItemFailure = V2ImageGenerationFailure | null;

export type V2ImageGenerationThreadItemSavedPath = V2AbsolutePathBuf | null;

export type V2ImageGenerationThreadItemType = "imageGeneration";

export type V2EnteredReviewModeThreadItem = {
    readonly "id": string;
    readonly "review": string;
    readonly "type": V2EnteredReviewModeThreadItemType;
    readonly [key: string]: unknown;
};

export type V2EnteredReviewModeThreadItemType = "enteredReviewMode";

export type V2ExitedReviewModeThreadItem = {
    readonly "id": string;
    readonly "review": string;
    readonly "type": V2ExitedReviewModeThreadItemType;
    readonly [key: string]: unknown;
};

export type V2ExitedReviewModeThreadItemType = "exitedReviewMode";

export type V2ContextCompactionThreadItem = {
    readonly "id": string;
    readonly "type": V2ContextCompactionThreadItemType;
    readonly [key: string]: unknown;
};

export type V2ContextCompactionThreadItemType = "contextCompaction";

export type V2ThreadItemEntry = {
    readonly "item": V2ThreadItem;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadListCwdFilter = string | V2ThreadListCwdFilterAnyOf2;

export type V2ThreadListCwdFilterAnyOf2 = ReadonlyArray<string>;

export type V2ThreadListParams = {
    readonly "archived"?: boolean | null;
    readonly "cursor"?: string | null;
    readonly "cwd"?: V2ThreadListParamsCwd;
    readonly "limit"?: number | null;
    readonly "modelProviders"?: V2ThreadListParamsModelProviders;
    readonly "searchTerm"?: string | null;
    readonly "sectionId"?: string | null;
    readonly "sortDirection"?: V2ThreadListParamsSortDirection;
    readonly "sortKey"?: V2ThreadListParamsSortKey;
    readonly "sourceKinds"?: V2ThreadListParamsSourceKinds;
    readonly "useStateDbOnly"?: boolean;
    readonly [key: string]: unknown;
};

export type V2ThreadListParamsCwd = V2ThreadListCwdFilter | null;

export type V2ThreadListParamsModelProviders = ReadonlyArray<string> | null;

export type V2ThreadListParamsSortDirection = V2SortDirection | null;

export type V2ThreadListParamsSortKey = V2ThreadSortKey | null;

export type V2ThreadListParamsSourceKinds = ReadonlyArray<V2ThreadSourceKind> | null;

export type V2ThreadListResponse = {
    readonly "backwardsCursor"?: string | null;
    readonly "data": V2ThreadListResponseData;
    readonly "nextCursor"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ThreadListResponseData = ReadonlyArray<V2Thread>;

export type V2ThreadLoadedListParams = {
    readonly "cursor"?: string | null;
    readonly "limit"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ThreadLoadedListResponse = {
    readonly "data": V2ThreadLoadedListResponseData;
    readonly "nextCursor"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ThreadLoadedListResponseData = ReadonlyArray<string>;

export type V2ThreadMemoryMode = "enabled" | "disabled";

export type V2ThreadMetadataGitInfoUpdateParams = {
    readonly "branch"?: string | null;
    readonly "originUrl"?: string | null;
    readonly "sha"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ThreadMetadataUpdateParams = {
    readonly "gitInfo"?: V2ThreadMetadataUpdateParamsGitInfo;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadMetadataUpdateParamsGitInfo = V2ThreadMetadataGitInfoUpdateParams | null;

export type V2ThreadMetadataUpdateResponse = {
    readonly "thread": V2Thread;
    readonly [key: string]: unknown;
};

export type V2ThreadNameUpdatedNotification = {
    readonly "threadId": string;
    readonly "threadName"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ThreadProjectUpdatedNotification = {
    readonly "projectId": string | null;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadQueueChangedNotification = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadReadParams = {
    readonly "includeTurns"?: boolean;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadReadResponse = {
    readonly "thread": V2Thread;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeAudioChunk = {
    readonly "data": string;
    readonly "itemId"?: string | null;
    readonly "numChannels": number;
    readonly "sampleRate": number;
    readonly "samplesPerChannel"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeClosedNotification = {
    readonly "reason"?: string | null;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeErrorNotification = {
    readonly "message": string;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeInitialItem = {
    readonly "role": V2ConversationTextRole;
    readonly "text": string;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeItemAddedNotification = {
    readonly "item": unknown;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeOutputAudioDeltaNotification = {
    readonly "audio": V2ThreadRealtimeAudioChunk;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeSdpNotification = {
    readonly "sdp": string;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeStartTransport = V2WebsocketThreadRealtimeStartTransport | V2WebrtcThreadRealtimeStartTransport | V2ExistingCallThreadRealtimeStartTransport;

export type V2WebsocketThreadRealtimeStartTransport = {
    readonly "type": V2WebsocketThreadRealtimeStartTransportType;
    readonly [key: string]: unknown;
};

export type V2WebsocketThreadRealtimeStartTransportType = "websocket";

export type V2WebrtcThreadRealtimeStartTransport = {
    readonly "sdp": string;
    readonly "type": V2WebrtcThreadRealtimeStartTransportType;
    readonly [key: string]: unknown;
};

export type V2WebrtcThreadRealtimeStartTransportType = "webrtc";

export type V2ExistingCallThreadRealtimeStartTransport = {
    readonly "callId": string;
    readonly "type": V2ExistingCallThreadRealtimeStartTransportType;
    readonly [key: string]: unknown;
};

export type V2ExistingCallThreadRealtimeStartTransportType = "existingCall";

export type V2ThreadRealtimeStartedNotification = {
    readonly "realtimeSessionId"?: string | null;
    readonly "threadId": string;
    readonly "version": V2RealtimeConversationVersion;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeTranscriptDeltaNotification = {
    readonly "delta": string;
    readonly "role": string;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadRealtimeTranscriptDoneNotification = {
    readonly "role": string;
    readonly "text": string;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadResumeInitialTurnsPageParams = {
    readonly "itemsView"?: V2ThreadResumeInitialTurnsPageParamsItemsView;
    readonly "limit"?: number | null;
    readonly "sortDirection"?: V2ThreadResumeInitialTurnsPageParamsSortDirection;
    readonly [key: string]: unknown;
};

export type V2ThreadResumeInitialTurnsPageParamsItemsView = V2TurnItemsView | null;

export type V2ThreadResumeInitialTurnsPageParamsSortDirection = V2SortDirection | null;

export type V2ThreadResumeParams = {
    readonly "approvalPolicy"?: V2ThreadResumeParamsApprovalPolicy;
    readonly "approvalsReviewer"?: V2ThreadResumeParamsApprovalsReviewer;
    readonly "baseInstructions"?: string | null;
    readonly "config"?: V2ThreadResumeParamsConfig;
    readonly "cwd"?: string | null;
    readonly "developerInstructions"?: string | null;
    readonly "model"?: string | null;
    readonly "modelProvider"?: string | null;
    readonly "personality"?: V2ThreadResumeParamsPersonality;
    readonly "sandbox"?: V2ThreadResumeParamsSandbox;
    readonly "serviceTier"?: string | null;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadResumeParamsApprovalPolicy = V2AskForApproval | null;

export type V2ThreadResumeParamsApprovalsReviewer = V2ApprovalsReviewer | null;

export type V2ThreadResumeParamsConfig = {
    readonly [key: string]: unknown;
} | null;

export type V2ThreadResumeParamsPersonality = V2Personality | null;

export type V2ThreadResumeParamsSandbox = V2SandboxMode | null;

export type V2ThreadResumeResponse = {
    readonly "approvalPolicy": V2AskForApproval;
    readonly "approvalsReviewer": V2ThreadResumeResponseApprovalsReviewer;
    readonly "cwd": V2AbsolutePathBuf;
    readonly "instructionSources"?: V2ThreadResumeResponseInstructionSources;
    readonly "model": string;
    readonly "modelProvider": string;
    readonly "reasoningEffort"?: V2ThreadResumeResponseReasoningEffort;
    readonly "sandbox": V2ThreadResumeResponseSandbox;
    readonly "serviceTier"?: string | null;
    readonly "thread": V2Thread;
    readonly [key: string]: unknown;
};

export type V2ThreadResumeResponseApprovalsReviewer = V2ApprovalsReviewer;

export type V2ThreadResumeResponseInstructionSources = ReadonlyArray<V2LegacyAppPathString>;

export type V2ThreadResumeResponseReasoningEffort = V2ReasoningEffort | null;

export type V2ThreadResumeResponseSandbox = V2SandboxPolicy;

export type V2ThreadRevertedNotification = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadRollbackParams = {
    readonly "numTurns": number;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadRollbackResponse = {
    readonly "thread": V2ThreadRollbackResponseThread;
    readonly [key: string]: unknown;
};

export type V2ThreadRollbackResponseThread = V2Thread;

export type V2ThreadSearchResult = {
    readonly "snippet": string;
    readonly "thread": V2Thread;
    readonly [key: string]: unknown;
};

export type V2ThreadSearchSortKey = "created_at" | "updated_at" | "recency_at";

export type V2ThreadSection = {
    readonly "appearance"?: V2ThreadSectionAppearance2;
    readonly "id": string;
    readonly "name": string;
    readonly [key: string]: unknown;
};

export type V2ThreadSectionAppearance2 = V2ThreadSectionAppearance | null;

export type V2ThreadSectionAppearance = {
    readonly "color"?: string | null;
    readonly "icon"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ThreadSectionCreateParams = {
    readonly "appearance"?: V2ThreadSectionCreateParamsAppearance;
    readonly "name": string;
    readonly [key: string]: unknown;
};

export type V2ThreadSectionCreateParamsAppearance = V2ThreadSectionAppearance | null;

export type V2ThreadSectionCreateResponse = {
    readonly "section": V2ThreadSection;
    readonly [key: string]: unknown;
};

export type V2ThreadSectionDeleteParams = {
    readonly "sectionId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadSectionDeleteResponse = {
    readonly [key: string]: unknown;
};

export type V2ThreadSectionListParams = {
    readonly "cursor"?: string | null;
    readonly "limit"?: number | null;
    readonly [key: string]: unknown;
};

export type V2ThreadSectionListResponse = {
    readonly "data": V2ThreadSectionListResponseData;
    readonly "nextCursor"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ThreadSectionListResponseData = ReadonlyArray<V2ThreadSection>;

export type V2ThreadSectionMoveParams = {
    readonly "beforeThreadId"?: string | null;
    readonly "sectionId": string | null;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadSectionMoveResponse = {
    readonly [key: string]: unknown;
};

export type V2ThreadSectionUpdateParams = {
    readonly "appearance"?: V2ThreadSectionUpdateParamsAppearance;
    readonly "name": string;
    readonly "sectionId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadSectionUpdateParamsAppearance = V2ThreadSectionAppearance | null;

export type V2ThreadSectionUpdateResponse = {
    readonly "section": V2ThreadSection;
    readonly [key: string]: unknown;
};

export type V2ThreadSetNameParams = {
    readonly "name": string;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadSetNameResponse = {
    readonly [key: string]: unknown;
};

export type V2ThreadSettings = {
    readonly "activePermissionProfile"?: V2ThreadSettingsActivePermissionProfile;
    readonly "approvalPolicy": V2AskForApproval;
    readonly "approvalsReviewer": V2ApprovalsReviewer;
    readonly "collaborationMode": V2CollaborationMode;
    readonly "cwd": V2AbsolutePathBuf;
    readonly "effort"?: V2ThreadSettingsEffort;
    readonly "model": string;
    readonly "modelProvider": string;
    readonly "personality"?: V2ThreadSettingsPersonality;
    readonly "sandboxPolicy": V2SandboxPolicy;
    readonly "serviceTier"?: string | null;
    readonly "summary"?: V2ThreadSettingsSummary;
    readonly [key: string]: unknown;
};

export type V2ThreadSettingsActivePermissionProfile = V2ActivePermissionProfile | null;

export type V2ThreadSettingsEffort = V2ReasoningEffort | null;

export type V2ThreadSettingsPersonality = V2Personality | null;

export type V2ThreadSettingsSummary = V2ReasoningSummary | null;

export type V2ThreadSettingsUpdatedNotification = {
    readonly "threadId": string;
    readonly "threadSettings": V2ThreadSettings;
    readonly [key: string]: unknown;
};

export type V2ThreadShellCommandParams = {
    readonly "command": string;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadShellCommandResponse = {
    readonly [key: string]: unknown;
};

export type V2ThreadSortKey = "created_at" | "updated_at" | "recency_at" | "section_position";

export type V2ThreadSource = string;

export type V2ThreadSourceKind = "cli" | "vscode" | "exec" | "appServer" | "subAgent" | "subAgentReview" | "subAgentCompact" | "subAgentThreadSpawn" | "subAgentOther" | "unknown";

export type V2ThreadStartParams = {
    readonly "approvalPolicy"?: V2ThreadStartParamsApprovalPolicy;
    readonly "approvalsReviewer"?: V2ThreadStartParamsApprovalsReviewer;
    readonly "baseInstructions"?: string | null;
    readonly "config"?: V2ThreadStartParamsConfig;
    readonly "cwd"?: string | null;
    readonly "developerInstructions"?: string | null;
    readonly "ephemeral"?: boolean | null;
    readonly "model"?: string | null;
    readonly "modelProvider"?: string | null;
    readonly "personality"?: V2ThreadStartParamsPersonality;
    readonly "sandbox"?: V2ThreadStartParamsSandbox;
    readonly "serviceName"?: string | null;
    readonly "serviceTier"?: string | null;
    readonly "sessionStartSource"?: V2ThreadStartParamsSessionStartSource;
    readonly "threadSource"?: V2ThreadStartParamsThreadSource;
    readonly [key: string]: unknown;
};

export type V2ThreadStartParamsApprovalPolicy = V2AskForApproval | null;

export type V2ThreadStartParamsApprovalsReviewer = V2ApprovalsReviewer | null;

export type V2ThreadStartParamsConfig = {
    readonly [key: string]: unknown;
} | null;

export type V2ThreadStartParamsPersonality = V2Personality | null;

export type V2ThreadStartParamsSandbox = V2SandboxMode | null;

export type V2ThreadStartParamsSessionStartSource = V2ThreadStartSource | null;

export type V2ThreadStartParamsThreadSource = V2ThreadSource | null;

export type V2ThreadStartResponse = {
    readonly "approvalPolicy": V2AskForApproval;
    readonly "approvalsReviewer": V2ThreadStartResponseApprovalsReviewer;
    readonly "cwd": V2AbsolutePathBuf;
    readonly "instructionSources"?: V2ThreadStartResponseInstructionSources;
    readonly "model": string;
    readonly "modelProvider": string;
    readonly "reasoningEffort"?: V2ThreadStartResponseReasoningEffort;
    readonly "sandbox": V2ThreadStartResponseSandbox;
    readonly "serviceTier"?: string | null;
    readonly "thread": V2Thread;
    readonly [key: string]: unknown;
};

export type V2ThreadStartResponseApprovalsReviewer = V2ApprovalsReviewer;

export type V2ThreadStartResponseInstructionSources = ReadonlyArray<V2LegacyAppPathString>;

export type V2ThreadStartResponseReasoningEffort = V2ReasoningEffort | null;

export type V2ThreadStartResponseSandbox = V2SandboxPolicy;

export type V2ThreadStartSource = "startup" | "clear";

export type V2ThreadStartedNotification = {
    readonly "thread": V2Thread;
    readonly [key: string]: unknown;
};

export type V2ThreadStatus = V2NotLoadedThreadStatus | V2IdleThreadStatus | V2SystemErrorThreadStatus | V2ActiveThreadStatus;

export type V2NotLoadedThreadStatus = {
    readonly "type": V2NotLoadedThreadStatusType;
    readonly [key: string]: unknown;
};

export type V2NotLoadedThreadStatusType = "notLoaded";

export type V2IdleThreadStatus = {
    readonly "type": V2IdleThreadStatusType;
    readonly [key: string]: unknown;
};

export type V2IdleThreadStatusType = "idle";

export type V2SystemErrorThreadStatus = {
    readonly "type": V2SystemErrorThreadStatusType;
    readonly [key: string]: unknown;
};

export type V2SystemErrorThreadStatusType = "systemError";

export type V2ActiveThreadStatus = {
    readonly "activeFlags": V2ActiveThreadStatusActiveFlags;
    readonly "type": V2ActiveThreadStatusType;
    readonly [key: string]: unknown;
};

export type V2ActiveThreadStatusActiveFlags = ReadonlyArray<V2ThreadActiveFlag>;

export type V2ActiveThreadStatusType = "active";

export type V2ThreadStatusChangedNotification = {
    readonly "status": V2ThreadStatus;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadTokenUsage = {
    readonly "last": V2TokenUsageBreakdown;
    readonly "modelContextWindow"?: number | null;
    readonly "total": V2TokenUsageBreakdown;
    readonly [key: string]: unknown;
};

export type V2ThreadTokenUsageUpdatedNotification = {
    readonly "threadId": string;
    readonly "tokenUsage": V2ThreadTokenUsage;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadUnarchiveParams = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadUnarchiveResponse = {
    readonly "thread": V2Thread;
    readonly [key: string]: unknown;
};

export type V2ThreadUnarchivedNotification = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadUnsubscribeParams = {
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadUnsubscribeResponse = {
    readonly "status": V2ThreadUnsubscribeStatus;
    readonly [key: string]: unknown;
};

export type V2ThreadUnsubscribeStatus = "notLoaded" | "notSubscribed" | "unsubscribed";

export type V2ThreadUsage = {
    readonly "estimatedUsageCreditsMicros": number;
    readonly "estimatedUsageUsdMicros"?: number | null;
    readonly "groups": V2ThreadUsageGroups;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2ThreadUsageGroups = ReadonlyArray<V2ThreadUsageBreakdownGroup>;

export type V2ThreadUsageBreakdownGroup = {
    readonly "cachedInputTokens"?: number | null;
    readonly "estimatedUsageCreditsMicros": number;
    readonly "inputTokens"?: number | null;
    readonly "model"?: string | null;
    readonly "netNewInputTokens"?: number | null;
    readonly "outputTokens"?: number | null;
    readonly "reasoningEffort"?: string | null;
    readonly "speed"?: string | null;
    readonly "totalTokens"?: number | null;
    readonly [key: string]: unknown;
};

export type V2TokenUsageBreakdown = {
    readonly "cacheWriteInputTokens"?: number;
    readonly "cachedInputTokens": number;
    readonly "inputTokens": number;
    readonly "outputTokens": number;
    readonly "reasoningOutputTokens": number;
    readonly "totalTokens": number;
    readonly [key: string]: unknown;
};

export type V2Tool = {
    readonly "_meta"?: unknown;
    readonly "annotations"?: unknown;
    readonly "description"?: string | null;
    readonly "icons"?: V2ToolIcons;
    readonly "inputSchema": unknown;
    readonly "name": string;
    readonly "outputSchema"?: unknown;
    readonly "title"?: string | null;
    readonly [key: string]: unknown;
};

export type V2ToolIcons = ReadonlyArray<unknown> | null;

export type V2ToolsV2 = {
    readonly "web_search"?: V2ToolsV2WebSearch;
    readonly [key: string]: unknown;
};

export type V2ToolsV2WebSearch = V2WebSearchToolConfig | null;

export type V2Turn = {
    readonly "completedAt"?: number | null;
    readonly "durationMs"?: number | null;
    readonly "error"?: V2TurnError2;
    readonly "id": string;
    readonly "items": V2TurnItems;
    readonly "itemsView"?: V2TurnItemsView2;
    readonly "startedAt"?: number | null;
    readonly "status": V2TurnStatus;
    readonly [key: string]: unknown;
};

export type V2TurnError2 = V2TurnError | null;

export type V2TurnItems = ReadonlyArray<V2ThreadItem>;

export type V2TurnItemsView2 = V2TurnItemsView;

export type V2TurnCompletedNotification = {
    readonly "threadId": string;
    readonly "turn": V2Turn;
    readonly [key: string]: unknown;
};

export type V2TurnDiffUpdatedNotification = {
    readonly "diff": string;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2TurnEnvironmentParams = {
    readonly "cwd": V2LegacyAppPathString;
    readonly "environmentId": string;
    readonly "runtimeWorkspaceRoots"?: V2TurnEnvironmentParamsRuntimeWorkspaceRoots;
    readonly [key: string]: unknown;
};

export type V2TurnEnvironmentParamsRuntimeWorkspaceRoots = ReadonlyArray<V2LegacyAppPathString> | null;

export type V2TurnError = {
    readonly "additionalDetails"?: string | null;
    readonly "codexErrorInfo"?: V2TurnErrorCodexErrorInfo;
    readonly "message": string;
    readonly [key: string]: unknown;
};

export type V2TurnErrorCodexErrorInfo = V2CodexErrorInfo | null;

export type V2TurnInterruptParams = {
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2TurnInterruptResponse = {
    readonly [key: string]: unknown;
};

export type V2TurnItemsView = V2TurnItemsViewOneOf1 | V2TurnItemsViewOneOf2 | V2TurnItemsViewOneOf3;

export type V2TurnItemsViewOneOf1 = "notLoaded";

export type V2TurnItemsViewOneOf2 = "summary";

export type V2TurnItemsViewOneOf3 = "full";

export type V2TurnModerationMetadataNotification = {
    readonly "metadata": unknown;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2TurnPlanStep = {
    readonly "status": V2TurnPlanStepStatus;
    readonly "step": string;
    readonly [key: string]: unknown;
};

export type V2TurnPlanStepStatus = "pending" | "inProgress" | "completed";

export type V2TurnPlanUpdatedNotification = {
    readonly "explanation"?: string | null;
    readonly "plan": V2TurnPlanUpdatedNotificationPlan;
    readonly "threadId": string;
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2TurnPlanUpdatedNotificationPlan = ReadonlyArray<V2TurnPlanStep>;

export type V2TurnStartParams = {
    readonly "approvalPolicy"?: V2TurnStartParamsApprovalPolicy;
    readonly "approvalsReviewer"?: V2TurnStartParamsApprovalsReviewer;
    readonly "clientUserMessageId"?: string | null;
    readonly "cwd"?: string | null;
    readonly "effort"?: V2TurnStartParamsEffort;
    readonly "input": V2TurnStartParamsInput;
    readonly "model"?: string | null;
    readonly "outputSchema"?: unknown;
    readonly "personality"?: V2TurnStartParamsPersonality;
    readonly "sandboxPolicy"?: V2TurnStartParamsSandboxPolicy;
    readonly "serviceTier"?: string | null;
    readonly "summary"?: V2TurnStartParamsSummary;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2TurnStartParamsApprovalPolicy = V2AskForApproval | null;

export type V2TurnStartParamsApprovalsReviewer = V2ApprovalsReviewer | null;

export type V2TurnStartParamsEffort = V2ReasoningEffort | null;

export type V2TurnStartParamsInput = ReadonlyArray<V2UserInput>;

export type V2TurnStartParamsPersonality = V2Personality | null;

export type V2TurnStartParamsSandboxPolicy = V2SandboxPolicy | null;

export type V2TurnStartParamsSummary = V2ReasoningSummary | null;

export type V2TurnStartResponse = {
    readonly "turn": V2Turn;
    readonly [key: string]: unknown;
};

export type V2TurnStartedNotification = {
    readonly "threadId": string;
    readonly "turn": V2Turn;
    readonly [key: string]: unknown;
};

export type V2TurnStatus = "completed" | "interrupted" | "failed" | "inProgress";

export type V2TurnSteerParams = {
    readonly "clientUserMessageId"?: string | null;
    readonly "expectedTurnId": string;
    readonly "input": V2TurnSteerParamsInput;
    readonly "threadId": string;
    readonly [key: string]: unknown;
};

export type V2TurnSteerParamsInput = ReadonlyArray<V2UserInput>;

export type V2TurnSteerResponse = {
    readonly "turnId": string;
    readonly [key: string]: unknown;
};

export type V2TurnsPage = {
    readonly "backwardsCursor"?: string | null;
    readonly "data": V2TurnsPageData;
    readonly "nextCursor"?: string | null;
    readonly [key: string]: unknown;
};

export type V2TurnsPageData = ReadonlyArray<V2Turn>;

export type V2UserInput = V2TextUserInput | V2ImageUserInput | V2LocalImageUserInput | V2AudioUserInput | V2LocalAudioUserInput | V2SkillUserInput | V2MentionUserInput;

export type V2TextUserInput = {
    readonly "text": string;
    readonly "text_elements"?: V2TextUserInputTextElements;
    readonly "type": V2TextUserInputType;
    readonly [key: string]: unknown;
};

export type V2TextUserInputTextElements = ReadonlyArray<V2TextElement>;

export type V2TextUserInputType = "text";

export type V2ImageUserInput = {
    readonly "detail"?: V2ImageUserInputDetail;
    readonly "type": V2ImageUserInputType;
    readonly "url": string;
    readonly [key: string]: unknown;
};

export type V2ImageUserInputDetail = V2ImageDetail | null;

export type V2ImageUserInputType = "image";

export type V2LocalImageUserInput = {
    readonly "detail"?: V2LocalImageUserInputDetail;
    readonly "path": string;
    readonly "type": V2LocalImageUserInputType;
    readonly [key: string]: unknown;
};

export type V2LocalImageUserInputDetail = V2ImageDetail | null;

export type V2LocalImageUserInputType = "localImage";

export type V2AudioUserInput = {
    readonly "type": V2AudioUserInputType;
    readonly "url": string;
    readonly [key: string]: unknown;
};

export type V2AudioUserInputType = "audio";

export type V2LocalAudioUserInput = {
    readonly "path": string;
    readonly "type": V2LocalAudioUserInputType;
    readonly [key: string]: unknown;
};

export type V2LocalAudioUserInputType = "localAudio";

export type V2SkillUserInput = {
    readonly "name": string;
    readonly "path": string;
    readonly "type": V2SkillUserInputType;
    readonly [key: string]: unknown;
};

export type V2SkillUserInputType = "skill";

export type V2MentionUserInput = {
    readonly "name": string;
    readonly "path": string;
    readonly "type": V2MentionUserInputType;
    readonly [key: string]: unknown;
};

export type V2MentionUserInputType = "mention";

export type V2Verbosity = "low" | "medium" | "high";

export type V2WarningNotification = {
    readonly "message": string;
    readonly "threadId"?: string | null;
    readonly [key: string]: unknown;
};

export type V2WebSearchAction = V2SearchWebSearchAction | V2OpenPageWebSearchAction | V2FindInPageWebSearchAction | V2OtherWebSearchAction;

export type V2SearchWebSearchAction = {
    readonly "queries"?: V2SearchWebSearchActionQueries;
    readonly "query"?: string | null;
    readonly "type": V2SearchWebSearchActionType;
    readonly [key: string]: unknown;
};

export type V2SearchWebSearchActionQueries = ReadonlyArray<string> | null;

export type V2SearchWebSearchActionType = "search";

export type V2OpenPageWebSearchAction = {
    readonly "type": V2OpenPageWebSearchActionType;
    readonly "url"?: string | null;
    readonly [key: string]: unknown;
};

export type V2OpenPageWebSearchActionType = "openPage";

export type V2FindInPageWebSearchAction = {
    readonly "pattern"?: string | null;
    readonly "type": V2FindInPageWebSearchActionType;
    readonly "url"?: string | null;
    readonly [key: string]: unknown;
};

export type V2FindInPageWebSearchActionType = "findInPage";

export type V2OtherWebSearchAction = {
    readonly "type": V2OtherWebSearchActionType;
    readonly [key: string]: unknown;
};

export type V2OtherWebSearchActionType = "other";

export type V2WebSearchContextSize = "low" | "medium" | "high";

export type V2WebSearchLocation = {
    readonly "city"?: string | null;
    readonly "country"?: string | null;
    readonly "region"?: string | null;
    readonly "timezone"?: string | null;
};

export type V2WebSearchMode = "disabled" | "cached" | "indexed" | "live";

export type V2WebSearchToolConfig = {
    readonly "allowed_domains"?: V2WebSearchToolConfigAllowedDomains;
    readonly "context_size"?: V2WebSearchToolConfigContextSize;
    readonly "location"?: V2WebSearchToolConfigLocation;
};

export type V2WebSearchToolConfigAllowedDomains = ReadonlyArray<string> | null;

export type V2WebSearchToolConfigContextSize = V2WebSearchContextSize | null;

export type V2WebSearchToolConfigLocation = V2WebSearchLocation | null;

export type V2WindowsSandboxReadiness = "ready" | "notConfigured" | "updateRequired";

export type V2WindowsSandboxReadinessResponse = {
    readonly "status": V2WindowsSandboxReadiness;
    readonly [key: string]: unknown;
};

export type V2WindowsSandboxSetupCompletedNotification = {
    readonly "error"?: string | null;
    readonly "mode": V2WindowsSandboxSetupMode;
    readonly "success": boolean;
    readonly [key: string]: unknown;
};

export type V2WindowsSandboxSetupMode = "elevated" | "unelevated";

export type V2WindowsSandboxSetupStartParams = {
    readonly "cwd"?: V2WindowsSandboxSetupStartParamsCwd;
    readonly "mode": V2WindowsSandboxSetupMode;
    readonly [key: string]: unknown;
};

export type V2WindowsSandboxSetupStartParamsCwd = V2AbsolutePathBuf | null;

export type V2WindowsSandboxSetupStartResponse = {
    readonly "started": boolean;
    readonly [key: string]: unknown;
};

export type V2WindowsWorldWritableWarningNotification = {
    readonly "extraCount": number;
    readonly "failedScan": boolean;
    readonly "samplePaths": V2WindowsWorldWritableWarningNotificationSamplePaths;
    readonly [key: string]: unknown;
};

export type V2WindowsWorldWritableWarningNotificationSamplePaths = ReadonlyArray<string>;

export type V2WorkspaceMessage = {
    readonly "archivedAt"?: number | null;
    readonly "createdAt"?: number | null;
    readonly "messageBody": string;
    readonly "messageId": string;
    readonly "messageType": V2WorkspaceMessageType;
    readonly [key: string]: unknown;
};

export type V2WorkspaceMessageType = "headline" | "announcement" | "unknown";

export type V2WriteStatus = "ok" | "okOverridden";

export interface ClientRequestMap {
    readonly "initialize": { readonly params: RootInitializeParams; readonly response: RootInitializeResponse; readonly paramsRequired: true; };
    readonly "thread/start": { readonly params: V2ThreadStartParams; readonly response: V2ThreadStartResponse; readonly paramsRequired: true; };
    readonly "thread/resume": { readonly params: V2ThreadResumeParams; readonly response: V2ThreadResumeResponse; readonly paramsRequired: true; };
    readonly "thread/fork": { readonly params: V2ThreadForkParams; readonly response: V2ThreadForkResponse; readonly paramsRequired: true; };
    readonly "thread/archive": { readonly params: V2ThreadArchiveParams; readonly response: V2ThreadArchiveResponse; readonly paramsRequired: true; };
    readonly "thread/delete": { readonly params: V2ThreadDeleteParams; readonly response: V2ThreadDeleteResponse; readonly paramsRequired: true; };
    readonly "thread/unsubscribe": { readonly params: V2ThreadUnsubscribeParams; readonly response: V2ThreadUnsubscribeResponse; readonly paramsRequired: true; };
    readonly "thread/name/set": { readonly params: V2ThreadSetNameParams; readonly response: V2ThreadSetNameResponse; readonly paramsRequired: true; };
    readonly "thread/goal/set": { readonly params: V2ThreadGoalSetParams; readonly response: V2ThreadGoalSetResponse; readonly paramsRequired: true; };
    readonly "thread/goal/get": { readonly params: V2ThreadGoalGetParams; readonly response: V2ThreadGoalGetResponse; readonly paramsRequired: true; };
    readonly "thread/goal/clear": { readonly params: V2ThreadGoalClearParams; readonly response: V2ThreadGoalClearResponse; readonly paramsRequired: true; };
    readonly "thread/metadata/update": { readonly params: V2ThreadMetadataUpdateParams; readonly response: V2ThreadMetadataUpdateResponse; readonly paramsRequired: true; };
    readonly "thread/section/move": { readonly params: V2ThreadSectionMoveParams; readonly response: V2ThreadSectionMoveResponse; readonly paramsRequired: true; };
    readonly "thread/unarchive": { readonly params: V2ThreadUnarchiveParams; readonly response: V2ThreadUnarchiveResponse; readonly paramsRequired: true; };
    readonly "thread/compact/start": { readonly params: V2ThreadCompactStartParams; readonly response: V2ThreadCompactStartResponse; readonly paramsRequired: true; };
    readonly "thread/shellCommand": { readonly params: V2ThreadShellCommandParams; readonly response: V2ThreadShellCommandResponse; readonly paramsRequired: true; };
    readonly "thread/approveGuardianDeniedAction": { readonly params: V2ThreadApproveGuardianDeniedActionParams; readonly response: V2ThreadApproveGuardianDeniedActionResponse; readonly paramsRequired: true; };
    readonly "thread/rollback": { readonly params: V2ThreadRollbackParams; readonly response: V2ThreadRollbackResponse; readonly paramsRequired: true; };
    readonly "thread/list": { readonly params: V2ThreadListParams; readonly response: V2ThreadListResponse; readonly paramsRequired: true; };
    readonly "threadSection/list": { readonly params: V2ThreadSectionListParams; readonly response: V2ThreadSectionListResponse; readonly paramsRequired: true; };
    readonly "threadSection/create": { readonly params: V2ThreadSectionCreateParams; readonly response: V2ThreadSectionCreateResponse; readonly paramsRequired: true; };
    readonly "threadSection/update": { readonly params: V2ThreadSectionUpdateParams; readonly response: V2ThreadSectionUpdateResponse; readonly paramsRequired: true; };
    readonly "threadSection/delete": { readonly params: V2ThreadSectionDeleteParams; readonly response: V2ThreadSectionDeleteResponse; readonly paramsRequired: true; };
    readonly "thread/loaded/list": { readonly params: V2ThreadLoadedListParams; readonly response: V2ThreadLoadedListResponse; readonly paramsRequired: true; };
    readonly "thread/read": { readonly params: V2ThreadReadParams; readonly response: V2ThreadReadResponse; readonly paramsRequired: true; };
    readonly "thread/inject_items": { readonly params: V2ThreadInjectItemsParams; readonly response: V2ThreadInjectItemsResponse; readonly paramsRequired: true; };
    readonly "skills/list": { readonly params: V2SkillsListParams; readonly response: V2SkillsListResponse; readonly paramsRequired: true; };
    readonly "skills/extraRoots/set": { readonly params: V2SkillsExtraRootsSetParams; readonly response: V2SkillsExtraRootsSetResponse; readonly paramsRequired: true; };
    readonly "hooks/list": { readonly params: V2HooksListParams; readonly response: V2HooksListResponse; readonly paramsRequired: true; };
    readonly "marketplace/add": { readonly params: V2MarketplaceAddParams; readonly response: V2MarketplaceAddResponse; readonly paramsRequired: true; };
    readonly "marketplace/remove": { readonly params: V2MarketplaceRemoveParams; readonly response: V2MarketplaceRemoveResponse; readonly paramsRequired: true; };
    readonly "marketplace/upgrade": { readonly params: V2MarketplaceUpgradeParams; readonly response: V2MarketplaceUpgradeResponse; readonly paramsRequired: true; };
    readonly "plugin/list": { readonly params: V2PluginListParams; readonly response: V2PluginListResponse; readonly paramsRequired: true; };
    readonly "plugin/installed": { readonly params: V2PluginInstalledParams; readonly response: V2PluginInstalledResponse; readonly paramsRequired: true; };
    readonly "plugin/read": { readonly params: V2PluginReadParams; readonly response: V2PluginReadResponse; readonly paramsRequired: true; };
    readonly "plugin/skill/read": { readonly params: V2PluginSkillReadParams; readonly response: V2PluginSkillReadResponse; readonly paramsRequired: true; };
    readonly "plugin/share/save": { readonly params: V2PluginShareSaveParams; readonly response: V2PluginShareSaveResponse; readonly paramsRequired: true; };
    readonly "plugin/share/updateTargets": { readonly params: V2PluginShareUpdateTargetsParams; readonly response: V2PluginShareUpdateTargetsResponse; readonly paramsRequired: true; };
    readonly "plugin/share/list": { readonly params: V2PluginShareListParams; readonly response: V2PluginShareListResponse; readonly paramsRequired: true; };
    readonly "plugin/share/checkout": { readonly params: V2PluginShareCheckoutParams; readonly response: V2PluginShareCheckoutResponse; readonly paramsRequired: true; };
    readonly "plugin/share/delete": { readonly params: V2PluginShareDeleteParams; readonly response: V2PluginShareDeleteResponse; readonly paramsRequired: true; };
    readonly "app/read": { readonly params: V2AppsReadParams; readonly response: V2AppsReadResponse; readonly paramsRequired: true; };
    readonly "app/list": { readonly params: V2AppsListParams; readonly response: V2AppsListResponse; readonly paramsRequired: true; };
    readonly "app/installed": { readonly params: V2AppsInstalledParams; readonly response: V2AppsInstalledResponse; readonly paramsRequired: true; };
    readonly "fs/readFile": { readonly params: V2FsReadFileParams; readonly response: V2FsReadFileResponse; readonly paramsRequired: true; };
    readonly "fs/writeFile": { readonly params: V2FsWriteFileParams; readonly response: V2FsWriteFileResponse; readonly paramsRequired: true; };
    readonly "fs/createDirectory": { readonly params: V2FsCreateDirectoryParams; readonly response: V2FsCreateDirectoryResponse; readonly paramsRequired: true; };
    readonly "fs/getMetadata": { readonly params: V2FsGetMetadataParams; readonly response: V2FsGetMetadataResponse; readonly paramsRequired: true; };
    readonly "fs/readDirectory": { readonly params: V2FsReadDirectoryParams; readonly response: V2FsReadDirectoryResponse; readonly paramsRequired: true; };
    readonly "fs/remove": { readonly params: V2FsRemoveParams; readonly response: V2FsRemoveResponse; readonly paramsRequired: true; };
    readonly "fs/copy": { readonly params: V2FsCopyParams; readonly response: V2FsCopyResponse; readonly paramsRequired: true; };
    readonly "fs/watch": { readonly params: V2FsWatchParams; readonly response: V2FsWatchResponse; readonly paramsRequired: true; };
    readonly "fs/unwatch": { readonly params: V2FsUnwatchParams; readonly response: V2FsUnwatchResponse; readonly paramsRequired: true; };
    readonly "skills/config/write": { readonly params: V2SkillsConfigWriteParams; readonly response: V2SkillsConfigWriteResponse; readonly paramsRequired: true; };
    readonly "plugin/install": { readonly params: V2PluginInstallParams; readonly response: V2PluginInstallResponse; readonly paramsRequired: true; };
    readonly "plugin/uninstall": { readonly params: V2PluginUninstallParams; readonly response: V2PluginUninstallResponse; readonly paramsRequired: true; };
    readonly "turn/start": { readonly params: V2TurnStartParams; readonly response: V2TurnStartResponse; readonly paramsRequired: true; };
    readonly "turn/steer": { readonly params: V2TurnSteerParams; readonly response: V2TurnSteerResponse; readonly paramsRequired: true; };
    readonly "turn/interrupt": { readonly params: V2TurnInterruptParams; readonly response: V2TurnInterruptResponse; readonly paramsRequired: true; };
    readonly "review/start": { readonly params: V2ReviewStartParams; readonly response: V2ReviewStartResponse; readonly paramsRequired: true; };
    readonly "model/list": { readonly params: V2ModelListParams; readonly response: V2ModelListResponse; readonly paramsRequired: true; };
    readonly "modelProvider/capabilities/read": { readonly params: V2ModelProviderCapabilitiesReadParams; readonly response: V2ModelProviderCapabilitiesReadResponse; readonly paramsRequired: true; };
    readonly "experimentalFeature/list": { readonly params: V2ExperimentalFeatureListParams; readonly response: V2ExperimentalFeatureListResponse; readonly paramsRequired: true; };
    readonly "permissionProfile/list": { readonly params: V2PermissionProfileListParams; readonly response: V2PermissionProfileListResponse; readonly paramsRequired: true; };
    readonly "experimentalFeature/enablement/set": { readonly params: V2ExperimentalFeatureEnablementSetParams; readonly response: V2ExperimentalFeatureEnablementSetResponse; readonly paramsRequired: true; };
    readonly "mcpServer/oauth/login": { readonly params: V2McpServerOauthLoginParams; readonly response: V2McpServerOauthLoginResponse; readonly paramsRequired: true; };
    readonly "config/mcpServer/reload": { readonly params: unknown; readonly response: V2McpServerRefreshResponse; readonly paramsRequired: false; };
    readonly "mcpServerStatus/list": { readonly params: V2ListMcpServerStatusParams; readonly response: V2ListMcpServerStatusResponse; readonly paramsRequired: true; };
    readonly "mcpServer/resource/read": { readonly params: V2McpResourceReadParams; readonly response: V2McpResourceReadResponse; readonly paramsRequired: true; };
    readonly "mcpServer/tool/call": { readonly params: V2McpServerToolCallParams; readonly response: V2McpServerToolCallResponse; readonly paramsRequired: true; };
    readonly "windowsSandbox/setupStart": { readonly params: V2WindowsSandboxSetupStartParams; readonly response: V2WindowsSandboxSetupStartResponse; readonly paramsRequired: true; };
    readonly "windowsSandbox/readiness": { readonly params: unknown; readonly response: V2WindowsSandboxReadinessResponse; readonly paramsRequired: false; };
    readonly "account/login/start": { readonly params: V2LoginAccountParams; readonly response: V2LoginAccountResponse; readonly paramsRequired: true; };
    readonly "account/login/cancel": { readonly params: V2CancelLoginAccountParams; readonly response: V2CancelLoginAccountResponse; readonly paramsRequired: true; };
    readonly "account/logout": { readonly params: unknown; readonly response: V2LogoutAccountResponse; readonly paramsRequired: false; };
    readonly "account/rateLimits/read": { readonly params: unknown; readonly response: V2GetAccountRateLimitsResponse; readonly paramsRequired: false; };
    readonly "account/rateLimitResetCredit/consume": { readonly params: V2ConsumeAccountRateLimitResetCreditParams; readonly response: V2ConsumeAccountRateLimitResetCreditResponse; readonly paramsRequired: true; };
    readonly "account/usage/read": { readonly params: unknown; readonly response: V2GetAccountTokenUsageResponse; readonly paramsRequired: false; };
    readonly "account/workspaceMessages/read": { readonly params: unknown; readonly response: V2GetWorkspaceMessagesResponse; readonly paramsRequired: false; };
    readonly "account/sendAddCreditsNudgeEmail": { readonly params: V2SendAddCreditsNudgeEmailParams; readonly response: V2SendAddCreditsNudgeEmailResponse; readonly paramsRequired: true; };
    readonly "feedback/upload": { readonly params: V2FeedbackUploadParams; readonly response: V2FeedbackUploadResponse; readonly paramsRequired: true; };
    readonly "command/exec": { readonly params: V2CommandExecParams; readonly response: V2CommandExecResponse; readonly paramsRequired: true; };
    readonly "command/exec/write": { readonly params: V2CommandExecWriteParams; readonly response: V2CommandExecWriteResponse; readonly paramsRequired: true; };
    readonly "command/exec/terminate": { readonly params: V2CommandExecTerminateParams; readonly response: V2CommandExecTerminateResponse; readonly paramsRequired: true; };
    readonly "command/exec/resize": { readonly params: V2CommandExecResizeParams; readonly response: V2CommandExecResizeResponse; readonly paramsRequired: true; };
    readonly "config/read": { readonly params: V2ConfigReadParams; readonly response: V2ConfigReadResponse; readonly paramsRequired: true; };
    readonly "externalAgentConfig/detect": { readonly params: V2ExternalAgentConfigDetectParams; readonly response: V2ExternalAgentConfigDetectResponse; readonly paramsRequired: true; };
    readonly "externalAgentConfig/import": { readonly params: V2ExternalAgentConfigImportParams; readonly response: V2ExternalAgentConfigImportResponse; readonly paramsRequired: true; };
    readonly "externalAgentConfig/import/recordHistory": { readonly params: V2ExternalAgentConfigImportHistoryRecordParams; readonly response: V2ExternalAgentConfigImportHistoryRecordResponse; readonly paramsRequired: true; };
    readonly "externalAgentConfig/import/readHistories": { readonly params: unknown; readonly response: V2ExternalAgentConfigImportHistoriesReadResponse; readonly paramsRequired: false; };
    readonly "config/value/write": { readonly params: V2ConfigValueWriteParams; readonly response: V2ConfigWriteResponse; readonly paramsRequired: true; };
    readonly "config/batchWrite": { readonly params: V2ConfigBatchWriteParams; readonly response: V2ConfigWriteResponse; readonly paramsRequired: true; };
    readonly "configRequirements/read": { readonly params: unknown; readonly response: V2ConfigRequirementsReadResponse; readonly paramsRequired: false; };
    readonly "account/read": { readonly params: V2GetAccountParams; readonly response: V2GetAccountResponse; readonly paramsRequired: true; };
    readonly "fuzzyFileSearch": { readonly params: RootFuzzyFileSearchParams; readonly response: RootFuzzyFileSearchResponse; readonly paramsRequired: true; };
}

export interface ServerRequestMap {
    readonly "item/commandExecution/requestApproval": { readonly params: RootCommandExecutionRequestApprovalParams; readonly response: RootCommandExecutionRequestApprovalResponse; readonly paramsRequired: true; };
    readonly "item/fileChange/requestApproval": { readonly params: RootFileChangeRequestApprovalParams; readonly response: RootFileChangeRequestApprovalResponse; readonly paramsRequired: true; };
    readonly "item/tool/requestUserInput": { readonly params: RootToolRequestUserInputParams; readonly response: RootToolRequestUserInputResponse; readonly paramsRequired: true; };
    readonly "mcpServer/elicitation/request": { readonly params: RootMcpServerElicitationRequestParams; readonly response: RootMcpServerElicitationRequestResponse; readonly paramsRequired: true; };
    readonly "item/permissions/requestApproval": { readonly params: RootPermissionsRequestApprovalParams; readonly response: RootPermissionsRequestApprovalResponse; readonly paramsRequired: true; };
    readonly "item/tool/call": { readonly params: RootDynamicToolCallParams; readonly response: RootDynamicToolCallResponse; readonly paramsRequired: true; };
    readonly "account/chatgptAuthTokens/refresh": { readonly params: RootChatgptAuthTokensRefreshParams; readonly response: RootChatgptAuthTokensRefreshResponse; readonly paramsRequired: true; };
    readonly "attestation/generate": { readonly params: RootAttestationGenerateParams; readonly response: RootAttestationGenerateResponse; readonly paramsRequired: true; };
    readonly "applyPatchApproval": { readonly params: RootApplyPatchApprovalParams; readonly response: RootApplyPatchApprovalResponse; readonly paramsRequired: true; };
    readonly "execCommandApproval": { readonly params: RootExecCommandApprovalParams; readonly response: RootExecCommandApprovalResponse; readonly paramsRequired: true; };
}

export interface ClientNotificationMap {
    readonly "initialized": { readonly params: unknown; readonly paramsRequired: false; };
}

export interface ServerNotificationMap {
    readonly "error": { readonly params: V2ErrorNotification; readonly paramsRequired: true; };
    readonly "thread/started": { readonly params: V2ThreadStartedNotification; readonly paramsRequired: true; };
    readonly "thread/status/changed": { readonly params: V2ThreadStatusChangedNotification; readonly paramsRequired: true; };
    readonly "thread/archived": { readonly params: V2ThreadArchivedNotification; readonly paramsRequired: true; };
    readonly "thread/deleted": { readonly params: V2ThreadDeletedNotification; readonly paramsRequired: true; };
    readonly "thread/unarchived": { readonly params: V2ThreadUnarchivedNotification; readonly paramsRequired: true; };
    readonly "thread/closed": { readonly params: V2ThreadClosedNotification; readonly paramsRequired: true; };
    readonly "thread/reverted": { readonly params: V2ThreadRevertedNotification; readonly paramsRequired: true; };
    readonly "skills/changed": { readonly params: V2SkillsChangedNotification; readonly paramsRequired: true; };
    readonly "thread/name/updated": { readonly params: V2ThreadNameUpdatedNotification; readonly paramsRequired: true; };
    readonly "thread/goal/updated": { readonly params: V2ThreadGoalUpdatedNotification; readonly paramsRequired: true; };
    readonly "thread/goal/cleared": { readonly params: V2ThreadGoalClearedNotification; readonly paramsRequired: true; };
    readonly "thread/queue/changed": { readonly params: V2ThreadQueueChangedNotification; readonly paramsRequired: true; };
    readonly "project/changed": { readonly params: V2ProjectChangedNotification; readonly paramsRequired: true; };
    readonly "thread/project/updated": { readonly params: V2ThreadProjectUpdatedNotification; readonly paramsRequired: true; };
    readonly "thread/environment/connected": { readonly params: V2EnvironmentConnectionNotification; readonly paramsRequired: true; };
    readonly "thread/environment/disconnected": { readonly params: V2EnvironmentConnectionNotification; readonly paramsRequired: true; };
    readonly "thread/settings/updated": { readonly params: V2ThreadSettingsUpdatedNotification; readonly paramsRequired: true; };
    readonly "thread/tokenUsage/updated": { readonly params: V2ThreadTokenUsageUpdatedNotification; readonly paramsRequired: true; };
    readonly "turn/started": { readonly params: V2TurnStartedNotification; readonly paramsRequired: true; };
    readonly "hook/started": { readonly params: V2HookStartedNotification; readonly paramsRequired: true; };
    readonly "turn/completed": { readonly params: V2TurnCompletedNotification; readonly paramsRequired: true; };
    readonly "hook/completed": { readonly params: V2HookCompletedNotification; readonly paramsRequired: true; };
    readonly "turn/diff/updated": { readonly params: V2TurnDiffUpdatedNotification; readonly paramsRequired: true; };
    readonly "turn/plan/updated": { readonly params: V2TurnPlanUpdatedNotification; readonly paramsRequired: true; };
    readonly "item/started": { readonly params: V2ItemStartedNotification; readonly paramsRequired: true; };
    readonly "item/autoApprovalReview/started": { readonly params: V2ItemGuardianApprovalReviewStartedNotification; readonly paramsRequired: true; };
    readonly "item/autoApprovalReview/completed": { readonly params: V2ItemGuardianApprovalReviewCompletedNotification; readonly paramsRequired: true; };
    readonly "autoApprovalReview/strictReviewRequired": { readonly params: V2StrictReviewRequiredNotification; readonly paramsRequired: true; };
    readonly "item/completed": { readonly params: V2ItemCompletedNotification; readonly paramsRequired: true; };
    readonly "item/agentMessage/delta": { readonly params: V2AgentMessageDeltaNotification; readonly paramsRequired: true; };
    readonly "item/plan/delta": { readonly params: V2PlanDeltaNotification; readonly paramsRequired: true; };
    readonly "command/exec/outputDelta": { readonly params: V2CommandExecOutputDeltaNotification; readonly paramsRequired: true; };
    readonly "process/outputDelta": { readonly params: V2ProcessOutputDeltaNotification; readonly paramsRequired: true; };
    readonly "process/exited": { readonly params: V2ProcessExitedNotification; readonly paramsRequired: true; };
    readonly "item/commandExecution/outputDelta": { readonly params: V2CommandExecutionOutputDeltaNotification; readonly paramsRequired: true; };
    readonly "item/commandExecution/terminalInteraction": { readonly params: V2TerminalInteractionNotification; readonly paramsRequired: true; };
    readonly "item/fileChange/outputDelta": { readonly params: V2FileChangeOutputDeltaNotification; readonly paramsRequired: true; };
    readonly "item/fileChange/patchUpdated": { readonly params: V2FileChangePatchUpdatedNotification; readonly paramsRequired: true; };
    readonly "serverRequest/resolved": { readonly params: V2ServerRequestResolvedNotification; readonly paramsRequired: true; };
    readonly "item/mcpToolCall/progress": { readonly params: V2McpToolCallProgressNotification; readonly paramsRequired: true; };
    readonly "mcpServer/oauthLogin/completed": { readonly params: V2McpServerOauthLoginCompletedNotification; readonly paramsRequired: true; };
    readonly "mcpServer/startupStatus/updated": { readonly params: V2McpServerStatusUpdatedNotification; readonly paramsRequired: true; };
    readonly "mcpServer/event/stream/notification": { readonly params: V2McpServerEventStreamNotification; readonly paramsRequired: true; };
    readonly "account/updated": { readonly params: V2AccountUpdatedNotification; readonly paramsRequired: true; };
    readonly "account/rateLimits/updated": { readonly params: V2AccountRateLimitsUpdatedNotification; readonly paramsRequired: true; };
    readonly "app/list/updated": { readonly params: V2AppListUpdatedNotification; readonly paramsRequired: true; };
    readonly "remoteControl/status/changed": { readonly params: V2RemoteControlStatusChangedNotification; readonly paramsRequired: true; };
    readonly "externalAgentConfig/import/progress": { readonly params: V2ExternalAgentConfigImportProgressNotification; readonly paramsRequired: true; };
    readonly "externalAgentConfig/import/completed": { readonly params: V2ExternalAgentConfigImportCompletedNotification; readonly paramsRequired: true; };
    readonly "fs/changed": { readonly params: V2FsChangedNotification; readonly paramsRequired: true; };
    readonly "item/reasoning/summaryTextDelta": { readonly params: V2ReasoningSummaryTextDeltaNotification; readonly paramsRequired: true; };
    readonly "item/reasoning/summaryPartAdded": { readonly params: V2ReasoningSummaryPartAddedNotification; readonly paramsRequired: true; };
    readonly "item/reasoning/textDelta": { readonly params: V2ReasoningTextDeltaNotification; readonly paramsRequired: true; };
    readonly "thread/compacted": { readonly params: V2ContextCompactedNotification; readonly paramsRequired: true; };
    readonly "model/rerouted": { readonly params: V2ModelReroutedNotification; readonly paramsRequired: true; };
    readonly "model/verification": { readonly params: V2ModelVerificationNotification; readonly paramsRequired: true; };
    readonly "turn/moderationMetadata": { readonly params: V2TurnModerationMetadataNotification; readonly paramsRequired: true; };
    readonly "model/safetyBuffering/updated": { readonly params: V2ModelSafetyBufferingUpdatedNotification; readonly paramsRequired: true; };
    readonly "warning": { readonly params: V2WarningNotification; readonly paramsRequired: true; };
    readonly "guardianWarning": { readonly params: V2GuardianWarningNotification; readonly paramsRequired: true; };
    readonly "deprecationNotice": { readonly params: V2DeprecationNoticeNotification; readonly paramsRequired: true; };
    readonly "configWarning": { readonly params: V2ConfigWarningNotification; readonly paramsRequired: true; };
    readonly "fuzzyFileSearch/sessionUpdated": { readonly params: RootFuzzyFileSearchSessionUpdatedNotification; readonly paramsRequired: true; };
    readonly "fuzzyFileSearch/sessionCompleted": { readonly params: RootFuzzyFileSearchSessionCompletedNotification; readonly paramsRequired: true; };
    readonly "thread/realtime/started": { readonly params: V2ThreadRealtimeStartedNotification; readonly paramsRequired: true; };
    readonly "thread/realtime/itemAdded": { readonly params: V2ThreadRealtimeItemAddedNotification; readonly paramsRequired: true; };
    readonly "thread/realtime/transcript/delta": { readonly params: V2ThreadRealtimeTranscriptDeltaNotification; readonly paramsRequired: true; };
    readonly "thread/realtime/transcript/done": { readonly params: V2ThreadRealtimeTranscriptDoneNotification; readonly paramsRequired: true; };
    readonly "thread/realtime/outputAudio/delta": { readonly params: V2ThreadRealtimeOutputAudioDeltaNotification; readonly paramsRequired: true; };
    readonly "thread/realtime/sdp": { readonly params: V2ThreadRealtimeSdpNotification; readonly paramsRequired: true; };
    readonly "thread/realtime/error": { readonly params: V2ThreadRealtimeErrorNotification; readonly paramsRequired: true; };
    readonly "thread/realtime/closed": { readonly params: V2ThreadRealtimeClosedNotification; readonly paramsRequired: true; };
    readonly "windows/worldWritableWarning": { readonly params: V2WindowsWorldWritableWarningNotification; readonly paramsRequired: true; };
    readonly "windowsSandbox/setupCompleted": { readonly params: V2WindowsSandboxSetupCompletedNotification; readonly paramsRequired: true; };
    readonly "account/login/completed": { readonly params: V2AccountLoginCompletedNotification; readonly paramsRequired: true; };
}

export const clientRequestOperations = {
    "initialize": {paramsRequired: true, paramsType: "RootInitializeParams", responseType: "RootInitializeResponse"},
    "thread/start": {paramsRequired: true, paramsType: "V2ThreadStartParams", responseType: "V2ThreadStartResponse"},
    "thread/resume": {paramsRequired: true, paramsType: "V2ThreadResumeParams", responseType: "V2ThreadResumeResponse"},
    "thread/fork": {paramsRequired: true, paramsType: "V2ThreadForkParams", responseType: "V2ThreadForkResponse"},
    "thread/archive": {paramsRequired: true, paramsType: "V2ThreadArchiveParams", responseType: "V2ThreadArchiveResponse"},
    "thread/delete": {paramsRequired: true, paramsType: "V2ThreadDeleteParams", responseType: "V2ThreadDeleteResponse"},
    "thread/unsubscribe": {paramsRequired: true, paramsType: "V2ThreadUnsubscribeParams", responseType: "V2ThreadUnsubscribeResponse"},
    "thread/name/set": {paramsRequired: true, paramsType: "V2ThreadSetNameParams", responseType: "V2ThreadSetNameResponse"},
    "thread/goal/set": {paramsRequired: true, paramsType: "V2ThreadGoalSetParams", responseType: "V2ThreadGoalSetResponse"},
    "thread/goal/get": {paramsRequired: true, paramsType: "V2ThreadGoalGetParams", responseType: "V2ThreadGoalGetResponse"},
    "thread/goal/clear": {paramsRequired: true, paramsType: "V2ThreadGoalClearParams", responseType: "V2ThreadGoalClearResponse"},
    "thread/metadata/update": {paramsRequired: true, paramsType: "V2ThreadMetadataUpdateParams", responseType: "V2ThreadMetadataUpdateResponse"},
    "thread/section/move": {paramsRequired: true, paramsType: "V2ThreadSectionMoveParams", responseType: "V2ThreadSectionMoveResponse"},
    "thread/unarchive": {paramsRequired: true, paramsType: "V2ThreadUnarchiveParams", responseType: "V2ThreadUnarchiveResponse"},
    "thread/compact/start": {paramsRequired: true, paramsType: "V2ThreadCompactStartParams", responseType: "V2ThreadCompactStartResponse"},
    "thread/shellCommand": {paramsRequired: true, paramsType: "V2ThreadShellCommandParams", responseType: "V2ThreadShellCommandResponse"},
    "thread/approveGuardianDeniedAction": {paramsRequired: true, paramsType: "V2ThreadApproveGuardianDeniedActionParams", responseType: "V2ThreadApproveGuardianDeniedActionResponse"},
    "thread/rollback": {paramsRequired: true, paramsType: "V2ThreadRollbackParams", responseType: "V2ThreadRollbackResponse"},
    "thread/list": {paramsRequired: true, paramsType: "V2ThreadListParams", responseType: "V2ThreadListResponse"},
    "threadSection/list": {paramsRequired: true, paramsType: "V2ThreadSectionListParams", responseType: "V2ThreadSectionListResponse"},
    "threadSection/create": {paramsRequired: true, paramsType: "V2ThreadSectionCreateParams", responseType: "V2ThreadSectionCreateResponse"},
    "threadSection/update": {paramsRequired: true, paramsType: "V2ThreadSectionUpdateParams", responseType: "V2ThreadSectionUpdateResponse"},
    "threadSection/delete": {paramsRequired: true, paramsType: "V2ThreadSectionDeleteParams", responseType: "V2ThreadSectionDeleteResponse"},
    "thread/loaded/list": {paramsRequired: true, paramsType: "V2ThreadLoadedListParams", responseType: "V2ThreadLoadedListResponse"},
    "thread/read": {paramsRequired: true, paramsType: "V2ThreadReadParams", responseType: "V2ThreadReadResponse"},
    "thread/inject_items": {paramsRequired: true, paramsType: "V2ThreadInjectItemsParams", responseType: "V2ThreadInjectItemsResponse"},
    "skills/list": {paramsRequired: true, paramsType: "V2SkillsListParams", responseType: "V2SkillsListResponse"},
    "skills/extraRoots/set": {paramsRequired: true, paramsType: "V2SkillsExtraRootsSetParams", responseType: "V2SkillsExtraRootsSetResponse"},
    "hooks/list": {paramsRequired: true, paramsType: "V2HooksListParams", responseType: "V2HooksListResponse"},
    "marketplace/add": {paramsRequired: true, paramsType: "V2MarketplaceAddParams", responseType: "V2MarketplaceAddResponse"},
    "marketplace/remove": {paramsRequired: true, paramsType: "V2MarketplaceRemoveParams", responseType: "V2MarketplaceRemoveResponse"},
    "marketplace/upgrade": {paramsRequired: true, paramsType: "V2MarketplaceUpgradeParams", responseType: "V2MarketplaceUpgradeResponse"},
    "plugin/list": {paramsRequired: true, paramsType: "V2PluginListParams", responseType: "V2PluginListResponse"},
    "plugin/installed": {paramsRequired: true, paramsType: "V2PluginInstalledParams", responseType: "V2PluginInstalledResponse"},
    "plugin/read": {paramsRequired: true, paramsType: "V2PluginReadParams", responseType: "V2PluginReadResponse"},
    "plugin/skill/read": {paramsRequired: true, paramsType: "V2PluginSkillReadParams", responseType: "V2PluginSkillReadResponse"},
    "plugin/share/save": {paramsRequired: true, paramsType: "V2PluginShareSaveParams", responseType: "V2PluginShareSaveResponse"},
    "plugin/share/updateTargets": {paramsRequired: true, paramsType: "V2PluginShareUpdateTargetsParams", responseType: "V2PluginShareUpdateTargetsResponse"},
    "plugin/share/list": {paramsRequired: true, paramsType: "V2PluginShareListParams", responseType: "V2PluginShareListResponse"},
    "plugin/share/checkout": {paramsRequired: true, paramsType: "V2PluginShareCheckoutParams", responseType: "V2PluginShareCheckoutResponse"},
    "plugin/share/delete": {paramsRequired: true, paramsType: "V2PluginShareDeleteParams", responseType: "V2PluginShareDeleteResponse"},
    "app/read": {paramsRequired: true, paramsType: "V2AppsReadParams", responseType: "V2AppsReadResponse"},
    "app/list": {paramsRequired: true, paramsType: "V2AppsListParams", responseType: "V2AppsListResponse"},
    "app/installed": {paramsRequired: true, paramsType: "V2AppsInstalledParams", responseType: "V2AppsInstalledResponse"},
    "fs/readFile": {paramsRequired: true, paramsType: "V2FsReadFileParams", responseType: "V2FsReadFileResponse"},
    "fs/writeFile": {paramsRequired: true, paramsType: "V2FsWriteFileParams", responseType: "V2FsWriteFileResponse"},
    "fs/createDirectory": {paramsRequired: true, paramsType: "V2FsCreateDirectoryParams", responseType: "V2FsCreateDirectoryResponse"},
    "fs/getMetadata": {paramsRequired: true, paramsType: "V2FsGetMetadataParams", responseType: "V2FsGetMetadataResponse"},
    "fs/readDirectory": {paramsRequired: true, paramsType: "V2FsReadDirectoryParams", responseType: "V2FsReadDirectoryResponse"},
    "fs/remove": {paramsRequired: true, paramsType: "V2FsRemoveParams", responseType: "V2FsRemoveResponse"},
    "fs/copy": {paramsRequired: true, paramsType: "V2FsCopyParams", responseType: "V2FsCopyResponse"},
    "fs/watch": {paramsRequired: true, paramsType: "V2FsWatchParams", responseType: "V2FsWatchResponse"},
    "fs/unwatch": {paramsRequired: true, paramsType: "V2FsUnwatchParams", responseType: "V2FsUnwatchResponse"},
    "skills/config/write": {paramsRequired: true, paramsType: "V2SkillsConfigWriteParams", responseType: "V2SkillsConfigWriteResponse"},
    "plugin/install": {paramsRequired: true, paramsType: "V2PluginInstallParams", responseType: "V2PluginInstallResponse"},
    "plugin/uninstall": {paramsRequired: true, paramsType: "V2PluginUninstallParams", responseType: "V2PluginUninstallResponse"},
    "turn/start": {paramsRequired: true, paramsType: "V2TurnStartParams", responseType: "V2TurnStartResponse"},
    "turn/steer": {paramsRequired: true, paramsType: "V2TurnSteerParams", responseType: "V2TurnSteerResponse"},
    "turn/interrupt": {paramsRequired: true, paramsType: "V2TurnInterruptParams", responseType: "V2TurnInterruptResponse"},
    "review/start": {paramsRequired: true, paramsType: "V2ReviewStartParams", responseType: "V2ReviewStartResponse"},
    "model/list": {paramsRequired: true, paramsType: "V2ModelListParams", responseType: "V2ModelListResponse"},
    "modelProvider/capabilities/read": {paramsRequired: true, paramsType: "V2ModelProviderCapabilitiesReadParams", responseType: "V2ModelProviderCapabilitiesReadResponse"},
    "experimentalFeature/list": {paramsRequired: true, paramsType: "V2ExperimentalFeatureListParams", responseType: "V2ExperimentalFeatureListResponse"},
    "permissionProfile/list": {paramsRequired: true, paramsType: "V2PermissionProfileListParams", responseType: "V2PermissionProfileListResponse"},
    "experimentalFeature/enablement/set": {paramsRequired: true, paramsType: "V2ExperimentalFeatureEnablementSetParams", responseType: "V2ExperimentalFeatureEnablementSetResponse"},
    "mcpServer/oauth/login": {paramsRequired: true, paramsType: "V2McpServerOauthLoginParams", responseType: "V2McpServerOauthLoginResponse"},
    "config/mcpServer/reload": {paramsRequired: false, paramsType: "unknown", responseType: "V2McpServerRefreshResponse"},
    "mcpServerStatus/list": {paramsRequired: true, paramsType: "V2ListMcpServerStatusParams", responseType: "V2ListMcpServerStatusResponse"},
    "mcpServer/resource/read": {paramsRequired: true, paramsType: "V2McpResourceReadParams", responseType: "V2McpResourceReadResponse"},
    "mcpServer/tool/call": {paramsRequired: true, paramsType: "V2McpServerToolCallParams", responseType: "V2McpServerToolCallResponse"},
    "windowsSandbox/setupStart": {paramsRequired: true, paramsType: "V2WindowsSandboxSetupStartParams", responseType: "V2WindowsSandboxSetupStartResponse"},
    "windowsSandbox/readiness": {paramsRequired: false, paramsType: "unknown", responseType: "V2WindowsSandboxReadinessResponse"},
    "account/login/start": {paramsRequired: true, paramsType: "V2LoginAccountParams", responseType: "V2LoginAccountResponse"},
    "account/login/cancel": {paramsRequired: true, paramsType: "V2CancelLoginAccountParams", responseType: "V2CancelLoginAccountResponse"},
    "account/logout": {paramsRequired: false, paramsType: "unknown", responseType: "V2LogoutAccountResponse"},
    "account/rateLimits/read": {paramsRequired: false, paramsType: "unknown", responseType: "V2GetAccountRateLimitsResponse"},
    "account/rateLimitResetCredit/consume": {paramsRequired: true, paramsType: "V2ConsumeAccountRateLimitResetCreditParams", responseType: "V2ConsumeAccountRateLimitResetCreditResponse"},
    "account/usage/read": {paramsRequired: false, paramsType: "unknown", responseType: "V2GetAccountTokenUsageResponse"},
    "account/workspaceMessages/read": {paramsRequired: false, paramsType: "unknown", responseType: "V2GetWorkspaceMessagesResponse"},
    "account/sendAddCreditsNudgeEmail": {paramsRequired: true, paramsType: "V2SendAddCreditsNudgeEmailParams", responseType: "V2SendAddCreditsNudgeEmailResponse"},
    "feedback/upload": {paramsRequired: true, paramsType: "V2FeedbackUploadParams", responseType: "V2FeedbackUploadResponse"},
    "command/exec": {paramsRequired: true, paramsType: "V2CommandExecParams", responseType: "V2CommandExecResponse"},
    "command/exec/write": {paramsRequired: true, paramsType: "V2CommandExecWriteParams", responseType: "V2CommandExecWriteResponse"},
    "command/exec/terminate": {paramsRequired: true, paramsType: "V2CommandExecTerminateParams", responseType: "V2CommandExecTerminateResponse"},
    "command/exec/resize": {paramsRequired: true, paramsType: "V2CommandExecResizeParams", responseType: "V2CommandExecResizeResponse"},
    "config/read": {paramsRequired: true, paramsType: "V2ConfigReadParams", responseType: "V2ConfigReadResponse"},
    "externalAgentConfig/detect": {paramsRequired: true, paramsType: "V2ExternalAgentConfigDetectParams", responseType: "V2ExternalAgentConfigDetectResponse"},
    "externalAgentConfig/import": {paramsRequired: true, paramsType: "V2ExternalAgentConfigImportParams", responseType: "V2ExternalAgentConfigImportResponse"},
    "externalAgentConfig/import/recordHistory": {paramsRequired: true, paramsType: "V2ExternalAgentConfigImportHistoryRecordParams", responseType: "V2ExternalAgentConfigImportHistoryRecordResponse"},
    "externalAgentConfig/import/readHistories": {paramsRequired: false, paramsType: "unknown", responseType: "V2ExternalAgentConfigImportHistoriesReadResponse"},
    "config/value/write": {paramsRequired: true, paramsType: "V2ConfigValueWriteParams", responseType: "V2ConfigWriteResponse"},
    "config/batchWrite": {paramsRequired: true, paramsType: "V2ConfigBatchWriteParams", responseType: "V2ConfigWriteResponse"},
    "configRequirements/read": {paramsRequired: false, paramsType: "unknown", responseType: "V2ConfigRequirementsReadResponse"},
    "account/read": {paramsRequired: true, paramsType: "V2GetAccountParams", responseType: "V2GetAccountResponse"},
    "fuzzyFileSearch": {paramsRequired: true, paramsType: "RootFuzzyFileSearchParams", responseType: "RootFuzzyFileSearchResponse"},
} as const;

export const serverRequestOperations = {
    "item/commandExecution/requestApproval": {paramsRequired: true, paramsType: "RootCommandExecutionRequestApprovalParams", responseType: "RootCommandExecutionRequestApprovalResponse"},
    "item/fileChange/requestApproval": {paramsRequired: true, paramsType: "RootFileChangeRequestApprovalParams", responseType: "RootFileChangeRequestApprovalResponse"},
    "item/tool/requestUserInput": {paramsRequired: true, paramsType: "RootToolRequestUserInputParams", responseType: "RootToolRequestUserInputResponse"},
    "mcpServer/elicitation/request": {paramsRequired: true, paramsType: "RootMcpServerElicitationRequestParams", responseType: "RootMcpServerElicitationRequestResponse"},
    "item/permissions/requestApproval": {paramsRequired: true, paramsType: "RootPermissionsRequestApprovalParams", responseType: "RootPermissionsRequestApprovalResponse"},
    "item/tool/call": {paramsRequired: true, paramsType: "RootDynamicToolCallParams", responseType: "RootDynamicToolCallResponse"},
    "account/chatgptAuthTokens/refresh": {paramsRequired: true, paramsType: "RootChatgptAuthTokensRefreshParams", responseType: "RootChatgptAuthTokensRefreshResponse"},
    "attestation/generate": {paramsRequired: true, paramsType: "RootAttestationGenerateParams", responseType: "RootAttestationGenerateResponse"},
    "applyPatchApproval": {paramsRequired: true, paramsType: "RootApplyPatchApprovalParams", responseType: "RootApplyPatchApprovalResponse"},
    "execCommandApproval": {paramsRequired: true, paramsType: "RootExecCommandApprovalParams", responseType: "RootExecCommandApprovalResponse"},
} as const;

export const clientNotificationOperations = {
    "initialized": {paramsRequired: false, paramsType: "unknown"},
} as const;

export const serverNotificationOperations = {
    "error": {paramsRequired: true, paramsType: "V2ErrorNotification"},
    "thread/started": {paramsRequired: true, paramsType: "V2ThreadStartedNotification"},
    "thread/status/changed": {paramsRequired: true, paramsType: "V2ThreadStatusChangedNotification"},
    "thread/archived": {paramsRequired: true, paramsType: "V2ThreadArchivedNotification"},
    "thread/deleted": {paramsRequired: true, paramsType: "V2ThreadDeletedNotification"},
    "thread/unarchived": {paramsRequired: true, paramsType: "V2ThreadUnarchivedNotification"},
    "thread/closed": {paramsRequired: true, paramsType: "V2ThreadClosedNotification"},
    "thread/reverted": {paramsRequired: true, paramsType: "V2ThreadRevertedNotification"},
    "skills/changed": {paramsRequired: true, paramsType: "V2SkillsChangedNotification"},
    "thread/name/updated": {paramsRequired: true, paramsType: "V2ThreadNameUpdatedNotification"},
    "thread/goal/updated": {paramsRequired: true, paramsType: "V2ThreadGoalUpdatedNotification"},
    "thread/goal/cleared": {paramsRequired: true, paramsType: "V2ThreadGoalClearedNotification"},
    "thread/queue/changed": {paramsRequired: true, paramsType: "V2ThreadQueueChangedNotification"},
    "project/changed": {paramsRequired: true, paramsType: "V2ProjectChangedNotification"},
    "thread/project/updated": {paramsRequired: true, paramsType: "V2ThreadProjectUpdatedNotification"},
    "thread/environment/connected": {paramsRequired: true, paramsType: "V2EnvironmentConnectionNotification"},
    "thread/environment/disconnected": {paramsRequired: true, paramsType: "V2EnvironmentConnectionNotification"},
    "thread/settings/updated": {paramsRequired: true, paramsType: "V2ThreadSettingsUpdatedNotification"},
    "thread/tokenUsage/updated": {paramsRequired: true, paramsType: "V2ThreadTokenUsageUpdatedNotification"},
    "turn/started": {paramsRequired: true, paramsType: "V2TurnStartedNotification"},
    "hook/started": {paramsRequired: true, paramsType: "V2HookStartedNotification"},
    "turn/completed": {paramsRequired: true, paramsType: "V2TurnCompletedNotification"},
    "hook/completed": {paramsRequired: true, paramsType: "V2HookCompletedNotification"},
    "turn/diff/updated": {paramsRequired: true, paramsType: "V2TurnDiffUpdatedNotification"},
    "turn/plan/updated": {paramsRequired: true, paramsType: "V2TurnPlanUpdatedNotification"},
    "item/started": {paramsRequired: true, paramsType: "V2ItemStartedNotification"},
    "item/autoApprovalReview/started": {paramsRequired: true, paramsType: "V2ItemGuardianApprovalReviewStartedNotification"},
    "item/autoApprovalReview/completed": {paramsRequired: true, paramsType: "V2ItemGuardianApprovalReviewCompletedNotification"},
    "autoApprovalReview/strictReviewRequired": {paramsRequired: true, paramsType: "V2StrictReviewRequiredNotification"},
    "item/completed": {paramsRequired: true, paramsType: "V2ItemCompletedNotification"},
    "item/agentMessage/delta": {paramsRequired: true, paramsType: "V2AgentMessageDeltaNotification"},
    "item/plan/delta": {paramsRequired: true, paramsType: "V2PlanDeltaNotification"},
    "command/exec/outputDelta": {paramsRequired: true, paramsType: "V2CommandExecOutputDeltaNotification"},
    "process/outputDelta": {paramsRequired: true, paramsType: "V2ProcessOutputDeltaNotification"},
    "process/exited": {paramsRequired: true, paramsType: "V2ProcessExitedNotification"},
    "item/commandExecution/outputDelta": {paramsRequired: true, paramsType: "V2CommandExecutionOutputDeltaNotification"},
    "item/commandExecution/terminalInteraction": {paramsRequired: true, paramsType: "V2TerminalInteractionNotification"},
    "item/fileChange/outputDelta": {paramsRequired: true, paramsType: "V2FileChangeOutputDeltaNotification"},
    "item/fileChange/patchUpdated": {paramsRequired: true, paramsType: "V2FileChangePatchUpdatedNotification"},
    "serverRequest/resolved": {paramsRequired: true, paramsType: "V2ServerRequestResolvedNotification"},
    "item/mcpToolCall/progress": {paramsRequired: true, paramsType: "V2McpToolCallProgressNotification"},
    "mcpServer/oauthLogin/completed": {paramsRequired: true, paramsType: "V2McpServerOauthLoginCompletedNotification"},
    "mcpServer/startupStatus/updated": {paramsRequired: true, paramsType: "V2McpServerStatusUpdatedNotification"},
    "mcpServer/event/stream/notification": {paramsRequired: true, paramsType: "V2McpServerEventStreamNotification"},
    "account/updated": {paramsRequired: true, paramsType: "V2AccountUpdatedNotification"},
    "account/rateLimits/updated": {paramsRequired: true, paramsType: "V2AccountRateLimitsUpdatedNotification"},
    "app/list/updated": {paramsRequired: true, paramsType: "V2AppListUpdatedNotification"},
    "remoteControl/status/changed": {paramsRequired: true, paramsType: "V2RemoteControlStatusChangedNotification"},
    "externalAgentConfig/import/progress": {paramsRequired: true, paramsType: "V2ExternalAgentConfigImportProgressNotification"},
    "externalAgentConfig/import/completed": {paramsRequired: true, paramsType: "V2ExternalAgentConfigImportCompletedNotification"},
    "fs/changed": {paramsRequired: true, paramsType: "V2FsChangedNotification"},
    "item/reasoning/summaryTextDelta": {paramsRequired: true, paramsType: "V2ReasoningSummaryTextDeltaNotification"},
    "item/reasoning/summaryPartAdded": {paramsRequired: true, paramsType: "V2ReasoningSummaryPartAddedNotification"},
    "item/reasoning/textDelta": {paramsRequired: true, paramsType: "V2ReasoningTextDeltaNotification"},
    "thread/compacted": {paramsRequired: true, paramsType: "V2ContextCompactedNotification"},
    "model/rerouted": {paramsRequired: true, paramsType: "V2ModelReroutedNotification"},
    "model/verification": {paramsRequired: true, paramsType: "V2ModelVerificationNotification"},
    "turn/moderationMetadata": {paramsRequired: true, paramsType: "V2TurnModerationMetadataNotification"},
    "model/safetyBuffering/updated": {paramsRequired: true, paramsType: "V2ModelSafetyBufferingUpdatedNotification"},
    "warning": {paramsRequired: true, paramsType: "V2WarningNotification"},
    "guardianWarning": {paramsRequired: true, paramsType: "V2GuardianWarningNotification"},
    "deprecationNotice": {paramsRequired: true, paramsType: "V2DeprecationNoticeNotification"},
    "configWarning": {paramsRequired: true, paramsType: "V2ConfigWarningNotification"},
    "fuzzyFileSearch/sessionUpdated": {paramsRequired: true, paramsType: "RootFuzzyFileSearchSessionUpdatedNotification"},
    "fuzzyFileSearch/sessionCompleted": {paramsRequired: true, paramsType: "RootFuzzyFileSearchSessionCompletedNotification"},
    "thread/realtime/started": {paramsRequired: true, paramsType: "V2ThreadRealtimeStartedNotification"},
    "thread/realtime/itemAdded": {paramsRequired: true, paramsType: "V2ThreadRealtimeItemAddedNotification"},
    "thread/realtime/transcript/delta": {paramsRequired: true, paramsType: "V2ThreadRealtimeTranscriptDeltaNotification"},
    "thread/realtime/transcript/done": {paramsRequired: true, paramsType: "V2ThreadRealtimeTranscriptDoneNotification"},
    "thread/realtime/outputAudio/delta": {paramsRequired: true, paramsType: "V2ThreadRealtimeOutputAudioDeltaNotification"},
    "thread/realtime/sdp": {paramsRequired: true, paramsType: "V2ThreadRealtimeSdpNotification"},
    "thread/realtime/error": {paramsRequired: true, paramsType: "V2ThreadRealtimeErrorNotification"},
    "thread/realtime/closed": {paramsRequired: true, paramsType: "V2ThreadRealtimeClosedNotification"},
    "windows/worldWritableWarning": {paramsRequired: true, paramsType: "V2WindowsWorldWritableWarningNotification"},
    "windowsSandbox/setupCompleted": {paramsRequired: true, paramsType: "V2WindowsSandboxSetupCompletedNotification"},
    "account/login/completed": {paramsRequired: true, paramsType: "V2AccountLoginCompletedNotification"},
} as const;
