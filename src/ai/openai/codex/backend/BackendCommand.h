/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_BACKENDCOMMAND_H
#define AI_OPENAI_CODEX_BACKEND_BACKENDCOMMAND_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/typed/Accounts.h"
#include "ai/openai/codex/typed/Apps.h"
#include "ai/openai/codex/typed/Commands.h"
#include "ai/openai/codex/typed/Configuration.h"
#include "ai/openai/codex/typed/ExternalAgents.h"
#include "ai/openai/codex/typed/Feedback.h"
#include "ai/openai/codex/typed/Filesystem.h"
#include "ai/openai/codex/typed/Hooks.h"
#include "ai/openai/codex/typed/Marketplace.h"
#include "ai/openai/codex/typed/Mcp.h"
#include "ai/openai/codex/typed/Models.h"
#include "ai/openai/codex/typed/PermissionProfiles.h"
#include "ai/openai/codex/typed/Plugins.h"
#include "ai/openai/codex/typed/Reviews.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "ai/openai/codex/typed/Skills.h"
#include "ai/openai/codex/typed/Threads.h"
#include "ai/openai/codex/typed/Turns.h"
#include "ai/openai/codex/typed/WindowsSandbox.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ai::openai::codex::backend {

    struct ControllerAcquire {};
    struct ControllerRelease {};
    struct SnapshotGet {};

    struct AccountLoginCancel {
        typed::CancelLoginAccountParams params;
    };

    struct AccountLoginStart {
        typed::LoginAccountParams params;
    };

    struct AccountLogout {};

    struct AccountRateLimitResetCreditConsume {
        typed::ConsumeAccountRateLimitResetCreditParams params;
    };

    struct AccountRateLimitsRead {};

    struct AccountRead {
        typed::GetAccountParams params;
    };

    struct AccountSendAddCreditsNudgeEmail {
        typed::SendAddCreditsNudgeEmailParams params;
    };

    struct AccountUsageRead {};
    struct AccountWorkspaceMessagesRead {};

    struct ConfigBatchWrite {
        typed::ConfigBatchWriteParams params;
    };

    struct ConfigMcpServerReload {};

    struct ConfigRead {
        typed::ConfigReadParams params;
    };

    struct ConfigValueWrite {
        typed::ConfigValueWriteParams params;
    };

    struct ConfigRequirementsRead {};

    struct ExperimentalFeatureEnablementSet {
        typed::ExperimentalFeatureEnablementSetParams params;
    };

    struct ExperimentalFeatureList {
        typed::ExperimentalFeatureListParams params;
    };

    struct ModelList {
        typed::ModelListParams params;
    };

    struct ModelProviderCapabilitiesRead {};

    struct ThreadStart {
        typed::ThreadStartParams params;
    };

    struct ThreadResume {
        typed::ThreadResumeParams params;
    };

    struct ThreadList {
        typed::ThreadListParams params;
    };

    struct ThreadRead {
        typed::ThreadReadParams params;
    };

    struct TurnStart {
        typed::TurnStartParams params;
    };

    struct TurnInterrupt {
        typed::TurnInterruptParams params;
    };

    struct ThreadArchive {
        typed::ThreadArchiveParams params;
    };

    struct ThreadCompactStart {
        typed::ThreadCompactStartParams params;
    };

    struct ThreadDelete {
        typed::ThreadDeleteParams params;
    };

    struct ThreadFork {
        typed::ThreadForkParams params;
    };

    struct ThreadGoalClear {
        typed::ThreadGoalClearParams params;
    };

    struct ThreadGoalGet {
        typed::ThreadGoalGetParams params;
    };

    struct ThreadGoalSet {
        typed::ThreadGoalSetParams params;
    };

    struct ThreadInjectItems {
        typed::ThreadInjectItemsParams params;
    };

    struct ThreadLoadedList {
        typed::ThreadLoadedListParams params;
    };

    struct ThreadMetadataUpdate {
        typed::ThreadMetadataUpdateParams params;
    };

    struct ThreadSetName {
        typed::ThreadSetNameParams params;
    };

    struct ThreadRollback {
        typed::ThreadRollbackParams params;
    };

    struct ThreadShellCommand {
        typed::ThreadShellCommandParams params;
    };

    struct ThreadUnarchive {
        typed::ThreadUnarchiveParams params;
    };

    struct ThreadUnsubscribe {
        typed::ThreadUnsubscribeParams params;
    };

    struct ThreadApproveGuardianDeniedAction {
        typed::ThreadApproveGuardianDeniedActionParams params;
    };

    struct TurnSteer {
        typed::TurnSteerParams params;
    };

    struct CommandExec {
        typed::CommandExecParams params;
    };

    struct CommandExecResize {
        typed::CommandExecResizeParams params;
    };

    struct CommandExecTerminate {
        typed::CommandExecTerminateParams params;
    };

    struct CommandExecWrite {
        typed::CommandExecWriteParams params;
    };

    struct FsCopy {
        typed::FsCopyParams params;
    };

    struct FsCreateDirectory {
        typed::FsCreateDirectoryParams params;
    };

    struct FsGetMetadata {
        typed::FsGetMetadataParams params;
    };

    struct FsReadDirectory {
        typed::FsReadDirectoryParams params;
    };

    struct FsReadFile {
        typed::FsReadFileParams params;
    };

    struct FsRemove {
        typed::FsRemoveParams params;
    };

    struct FsUnwatch {
        typed::FsUnwatchParams params;
    };

    struct FsWatch {
        typed::FsWatchParams params;
    };

    struct FsWriteFile {
        typed::FsWriteFileParams params;
    };

    struct FuzzyFileSearch {
        typed::FuzzyFileSearchParams params;
    };

    struct PermissionProfileList {
        typed::PermissionProfileListParams params;
    };

    struct ReviewStart {
        typed::ReviewStartParams params;
    };

    struct AppsList {
        typed::AppsListParams params;
    };

    struct ExternalAgentConfigDetect {
        typed::ExternalAgentConfigDetectParams params;
    };

    struct ExternalAgentConfigImport {
        typed::ExternalAgentConfigImportParams params;
    };

    struct ExternalAgentConfigImportHistoriesRead {};

    struct FeedbackUpload {
        typed::FeedbackUploadParams params;
    };

    struct HooksList {
        typed::HooksListParams params;
    };

    struct MarketplaceAdd {
        typed::MarketplaceAddParams params;
    };

    struct MarketplaceRemove {
        typed::MarketplaceRemoveParams params;
    };

    struct MarketplaceUpgrade {
        typed::MarketplaceUpgradeParams params;
    };

    struct PluginInstall {
        typed::PluginInstallParams params;
    };

    struct PluginInstalled {
        typed::PluginInstalledParams params;
    };

    struct PluginList {
        typed::PluginListParams params;
    };

    struct PluginRead {
        typed::PluginReadParams params;
    };

    struct PluginShareCheckout {
        typed::PluginShareCheckoutParams params;
    };

    struct PluginShareDelete {
        typed::PluginShareDeleteParams params;
    };

    struct PluginShareList {};

    struct PluginShareSave {
        typed::PluginShareSaveParams params;
    };

    struct PluginShareUpdateTargets {
        typed::PluginShareUpdateTargetsParams params;
    };

    struct PluginSkillRead {
        typed::PluginSkillReadParams params;
    };

    struct PluginUninstall {
        typed::PluginUninstallParams params;
    };

    struct SkillsConfigWrite {
        typed::SkillsConfigWriteParams params;
    };

    struct SkillsExtraRootsSet {
        typed::SkillsExtraRootsSetParams params;
    };

    struct SkillsList {
        typed::SkillsListParams params;
    };

    struct McpServerOauthLogin {
        typed::McpServerOauthLoginParams params;
    };

    struct McpResourceRead {
        typed::McpResourceReadParams params;
    };

    struct McpServerToolCall {
        typed::McpServerToolCallParams params;
    };

    struct McpServerStatusList {
        typed::ListMcpServerStatusParams params;
    };

    struct WindowsSandboxReadiness {};

    struct WindowsSandboxSetupStart {
        typed::WindowsSandboxSetupStartParams params;
    };

    using ApprovalResponse =
        std::variant<typed::ApprovalDecision, typed::CommandExecutionRequestApprovalResponse, typed::FileChangeRequestApprovalResponse>;

    struct ApprovalRespond {
        PendingRequestId requestId;
        ApprovalResponse response;
    };

    using UserInputResponse = std::variant<std::vector<typed::UserInputAnswer>, typed::ToolRequestUserInputResponse>;

    struct UserInputRespond {
        PendingRequestId requestId;
        UserInputResponse response;
    };

    using AuthenticationResponsePayload = std::variant<typed::AuthenticationResponse, typed::ChatgptAuthTokensRefreshResponse>;

    struct AuthenticationRespond {
        PendingRequestId requestId;
        AuthenticationResponsePayload response;
    };

    struct UnknownRequestRespondRaw {
        PendingRequestId requestId;
        Json result = nullptr;
    };

    struct UnknownRequestReject {
        PendingRequestId requestId;
        ProtocolError error;
    };

    struct ApplyPatchApprovalRespond {
        PendingRequestId requestId;
        typed::ApplyPatchApprovalResponse response;
    };

    struct ExecCommandApprovalRespond {
        PendingRequestId requestId;
        typed::ExecCommandApprovalResponse response;
    };

    struct PermissionsApprovalRespond {
        PendingRequestId requestId;
        typed::PermissionsRequestApprovalResponse response;
    };

    struct AttestationGenerateRespond {
        PendingRequestId requestId;
        typed::AttestationGenerateResponse response;
    };

    struct DynamicToolCallRespond {
        PendingRequestId requestId;
        typed::DynamicToolCallResponse response;
    };

    struct McpServerElicitationRespond {
        PendingRequestId requestId;
        typed::McpServerElicitationRequestResponse response;
    };

    struct KnownRequestReject {
        PendingRequestId requestId;
        ProtocolError error;
    };

    using BackendCommand = std::variant<ControllerAcquire,
                                        ControllerRelease,
                                        SnapshotGet,
                                        AccountLoginCancel,
                                        AccountLoginStart,
                                        AccountLogout,
                                        AccountRateLimitResetCreditConsume,
                                        AccountRateLimitsRead,
                                        AccountRead,
                                        AccountSendAddCreditsNudgeEmail,
                                        AccountUsageRead,
                                        AccountWorkspaceMessagesRead,
                                        ConfigBatchWrite,
                                        ConfigMcpServerReload,
                                        ConfigRead,
                                        ConfigValueWrite,
                                        ConfigRequirementsRead,
                                        ExperimentalFeatureEnablementSet,
                                        ExperimentalFeatureList,
                                        ModelList,
                                        ModelProviderCapabilitiesRead,
                                        ThreadArchive,
                                        ThreadCompactStart,
                                        ThreadDelete,
                                        ThreadFork,
                                        ThreadGoalClear,
                                        ThreadGoalGet,
                                        ThreadGoalSet,
                                        ThreadInjectItems,
                                        ThreadList,
                                        ThreadLoadedList,
                                        ThreadMetadataUpdate,
                                        ThreadSetName,
                                        ThreadRead,
                                        ThreadResume,
                                        ThreadRollback,
                                        ThreadShellCommand,
                                        ThreadStart,
                                        ThreadUnarchive,
                                        ThreadUnsubscribe,
                                        ThreadApproveGuardianDeniedAction,
                                        TurnStart,
                                        TurnInterrupt,
                                        TurnSteer,
                                        CommandExec,
                                        CommandExecResize,
                                        CommandExecTerminate,
                                        CommandExecWrite,
                                        FsCopy,
                                        FsCreateDirectory,
                                        FsGetMetadata,
                                        FsReadDirectory,
                                        FsReadFile,
                                        FsRemove,
                                        FsUnwatch,
                                        FsWatch,
                                        FsWriteFile,
                                        FuzzyFileSearch,
                                        PermissionProfileList,
                                        ReviewStart,
                                        AppsList,
                                        ExternalAgentConfigDetect,
                                        ExternalAgentConfigImport,
                                        ExternalAgentConfigImportHistoriesRead,
                                        FeedbackUpload,
                                        HooksList,
                                        MarketplaceAdd,
                                        MarketplaceRemove,
                                        MarketplaceUpgrade,
                                        PluginInstall,
                                        PluginInstalled,
                                        PluginList,
                                        PluginRead,
                                        PluginShareCheckout,
                                        PluginShareDelete,
                                        PluginShareList,
                                        PluginShareSave,
                                        PluginShareUpdateTargets,
                                        PluginSkillRead,
                                        PluginUninstall,
                                        SkillsConfigWrite,
                                        SkillsExtraRootsSet,
                                        SkillsList,
                                        McpServerOauthLogin,
                                        McpResourceRead,
                                        McpServerToolCall,
                                        McpServerStatusList,
                                        WindowsSandboxReadiness,
                                        WindowsSandboxSetupStart,
                                        ApprovalRespond,
                                        UserInputRespond,
                                        AuthenticationRespond,
                                        UnknownRequestRespondRaw,
                                        UnknownRequestReject,
                                        ApplyPatchApprovalRespond,
                                        ExecCommandApprovalRespond,
                                        PermissionsApprovalRespond,
                                        AttestationGenerateRespond,
                                        DynamicToolCallRespond,
                                        McpServerElicitationRespond,
                                        KnownRequestReject>;

    static_assert(std::variant_size_v<BackendCommand> == 101);

    enum class CommandAccess { Observer, Controller };

    struct CommandPolicy {
        CommandAccess access;
        bool requiresProviderReady = false;
    };

    CommandPolicy commandPolicy(const BackendCommand& command) noexcept;

    enum class CommandErrorCode {
        PermissionDenied,
        InvalidCommand,
        NotFound,
        Conflict,
        LocalSubmissionFailure,
        TypedDecodingFailure,
        RemoteAppServerError,
        Cancelled,
        BackendUnavailable
    };

    struct CommandError {
        CommandErrorCode code = CommandErrorCode::InvalidCommand;
        std::string message;
        std::optional<std::int64_t> remoteCode;
        Json details = nullptr;
    };

    struct ControllerResult {
        std::optional<SessionId> controller;
        SessionRole role = SessionRole::Observer;
    };

    namespace detail {

        template <typename Prefix, typename Suffix>
        struct VariantConcatenation;

        template <typename... Prefix, typename... Suffix>
        struct VariantConcatenation<std::variant<Prefix...>, std::variant<Suffix...>> {
            using Type = std::variant<Prefix..., Suffix...>;
        };

    } // namespace detail

    using CommandValue =
        typename detail::VariantConcatenation<std::variant<std::monostate, Snapshot, ControllerResult>, ProviderOperationValue>::Type;

    static_assert(std::variant_size_v<CommandValue> == 68);

    struct CommandResult {
        CommandValue value;
        std::optional<CommandError> error;

        explicit operator bool() const noexcept {
            return !error.has_value();
        }

        static CommandResult succeeded(CommandValue value = std::monostate{});
        static CommandResult
        failed(CommandErrorCode code, std::string message, std::optional<std::int64_t> remoteCode = std::nullopt, Json details = nullptr);
    };

    struct CommandCompletion {
        std::string requestId;
        CommandResult result;
    };

    const char* commandErrorCodeName(CommandErrorCode code) noexcept;

} // namespace ai::openai::codex::backend

#endif // AI_OPENAI_CODEX_BACKEND_BACKENDCOMMAND_H
