/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/detail/AccountCodec.h"
#include "ai/openai/codex/detail/AppCodec.h"
#include "ai/openai/codex/detail/ApprovalCodec.h"
#include "ai/openai/codex/detail/ClientOperationCodec.h"
#include "ai/openai/codex/detail/CodexErrorInfoCodec.h"
#include "ai/openai/codex/detail/CommandCodec.h"
#include "ai/openai/codex/detail/ConfigurationCodec.h"
#include "ai/openai/codex/detail/EventDecoder.h"
#include "ai/openai/codex/detail/ExternalAgentCodec.h"
#include "ai/openai/codex/detail/FeedbackCodec.h"
#include "ai/openai/codex/detail/FilesystemCodec.h"
#include "ai/openai/codex/detail/HookCodec.h"
#include "ai/openai/codex/detail/MarketplaceCodec.h"
#include "ai/openai/codex/detail/McpCodec.h"
#include "ai/openai/codex/detail/McpReverseRequestCodec.h"
#include "ai/openai/codex/detail/ModelCodec.h"
#include "ai/openai/codex/detail/PluginCodec.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/detail/ReviewCodec.h"
#include "ai/openai/codex/detail/ServerRequestDecoder.h"
#include "ai/openai/codex/detail/SkillCodec.h"
#include "ai/openai/codex/detail/ThreadCodec.h"
#include "ai/openai/codex/detail/TurnCodec.h"
#include "ai/openai/codex/detail/WindowsSandboxCodec.h"
#include "ai/openai/codex/typed/Accounts.h"
#include "ai/openai/codex/typed/Apps.h"
#include "ai/openai/codex/typed/Commands.h"
#include "ai/openai/codex/typed/Configuration.h"
#include "ai/openai/codex/typed/Conversation.h"
#include "ai/openai/codex/typed/Events.h"
#include "ai/openai/codex/typed/ExternalAgents.h"
#include "ai/openai/codex/typed/Feedback.h"
#include "ai/openai/codex/typed/Filesystem.h"
#include "ai/openai/codex/typed/Hooks.h"
#include "ai/openai/codex/typed/Marketplace.h"
#include "ai/openai/codex/typed/Mcp.h"
#include "ai/openai/codex/typed/Models.h"
#include "ai/openai/codex/typed/PermissionProfiles.h"
#include "ai/openai/codex/typed/Plugins.h"
#include "ai/openai/codex/typed/Results.h"
#include "ai/openai/codex/typed/Reviews.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "ai/openai/codex/typed/Skills.h"
#include "ai/openai/codex/typed/Threads.h"
#include "ai/openai/codex/typed/Turns.h"
#include "ai/openai/codex/typed/Types.h"
#include "ai/openai/codex/typed/WindowsSandbox.h"

#include <cerrno>
#include <exception>
#include <functional>
#include <nlohmann/detail/json_ref.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ai::openai::codex::typed {
    namespace {
        Submission submissionFailure(std::string message) {
            return {std::nullopt, Error{Error::Category::Protocol, EINVAL, std::move(message)}};
        }

        std::optional<Json> encodeUnitParams(const Unit&, std::string& error) {
            error.clear();
            return std::optional<Json>{Json(nullptr)};
        }

        std::string registeredMethod(detail::ClientRequestTarget target) {
            return std::string(detail::entryFor(target).key.name);
        }

        std::string_view registeredMethod(detail::ServerRequestTarget target) {
            return detail::entryFor(target).key.name;
        }

        template <typename T, typename Handler>
        AppServerClient::RawProtocol::ResponseHandler
        adaptResponse(Handler handler,
                      detail::ClientRequestTarget target,
                      std::optional<ThreadId> contextualThreadId) {
            return [handler = std::move(handler),
                    target,
                    contextualThreadId = std::move(contextualThreadId)](
                       const Response& response) {
                OperationResult<T> result;
                result.requestId = response.id;
                result.raw = response.result;

                switch (response.kind) {
                    case Response::Kind::RemoteError:
                        result.kind = OperationResult<T>::Kind::RemoteError;
                        result.remoteError = response.remoteError;
                        if (result.remoteError) {
                            detail::decodeProtocolErrorTurnInfo(
                                *result.remoteError, result.codexErrorInfo, result.codexErrorDiagnostic);
                        }
                        break;
                    case Response::Kind::Cancelled:
                        result.kind = OperationResult<T>::Kind::Cancelled;
                        result.localError = response.localError;
                        break;
                    case Response::Kind::Result: {
                        std::string decodingError;
                        try {
                            result.value = detail::decodeClientOperationResultAs<T>(
                                target,
                                response.result,
                                contextualThreadId,
                                decodingError);
                        } catch (const std::exception& exception) {
                            decodingError = std::string("typed App Server result decoder threw: ") + exception.what();
                        } catch (...) {
                            decodingError = "typed App Server result decoder threw an unknown exception";
                        }

                        if (result.value) {
                            result.kind = OperationResult<T>::Kind::Success;
                        } else {
                            result.kind = OperationResult<T>::Kind::LocalError;
                            if (decodingError.empty()) {
                                decodingError = "typed App Server result could not be decoded";
                            }
                            result.localError = Error{Error::Category::Protocol, EINVAL, std::move(decodingError)};
                        }
                        break;
                    }
                }

                handler(result);
            };
        }

        template <typename T, typename Params, typename Handler, typename Encoder>
        Submission submitTypedRequest(AppServerClient::RawProtocol* protocol,
                                      detail::ClientRequestTarget target,
                                      const Params& params,
                                      Handler handler,
                                      Encoder encoder,
                                      std::optional<ThreadId> contextualThreadId = std::nullopt) {
            const std::string method = registeredMethod(target);
            if (!handler) {
                return submissionFailure("typed " + method + " requires a result handler");
            }

            std::string encodingError;
            std::optional<Json> encodedParams = encoder(params, encodingError);
            if (!encodedParams) {
                return submissionFailure(encodingError.empty() ? "typed " + method + " parameters could not be encoded"
                                                               : std::move(encodingError));
            }

            return protocol->request(
                method,
                std::move(*encodedParams),
                adaptResponse<T>(
                    std::move(handler), target, std::move(contextualThreadId)));
        }

        template <typename To, typename From, typename Mapper>
        OperationResult<To> mapOperationResult(const OperationResult<From>& source, Mapper mapper) {
            OperationResult<To> result;
            switch (source.kind) {
                case OperationResult<From>::Kind::Success:
                    result.kind = OperationResult<To>::Kind::Success;
                    if (source.value) {
                        result.value = mapper(*source.value);
                    }
                    break;
                case OperationResult<From>::Kind::RemoteError:
                    result.kind = OperationResult<To>::Kind::RemoteError;
                    break;
                case OperationResult<From>::Kind::Cancelled:
                    result.kind = OperationResult<To>::Kind::Cancelled;
                    break;
                case OperationResult<From>::Kind::LocalError:
                    result.kind = OperationResult<To>::Kind::LocalError;
                    break;
            }
            result.remoteError = source.remoteError;
            result.localError = source.localError;
            result.requestId = source.requestId;
            result.raw = source.raw;
            result.codexErrorInfo = source.codexErrorInfo;
            result.codexErrorDiagnostic = source.codexErrorDiagnostic;
            return result;
        }
    } // namespace

    Apps::Apps(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Apps::list(AppsListParams params, CompletionHandler<AppsListResponse> handler) {
        return submitTypedRequest<AppsListResponse>(
            protocol, detail::ClientRequestTarget::AppsList, params, std::move(handler), detail::encodeAppsListParams);
    }

    Submission Apps::list(CompletionHandler<AppsListResponse> handler) {
        return list(AppsListParams{}, std::move(handler));
    }

    ExternalAgents::ExternalAgents(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission ExternalAgents::detect(ExternalAgentConfigDetectParams params,
                                      CompletionHandler<ExternalAgentConfigDetectResponse> handler) {
        return submitTypedRequest<ExternalAgentConfigDetectResponse>(protocol,
                                                                     detail::ClientRequestTarget::ExternalAgentConfigDetect,
                                                                     params,
                                                                     std::move(handler),
                                                                     detail::encodeExternalAgentConfigDetectParams);
    }

    Submission ExternalAgents::detect(CompletionHandler<ExternalAgentConfigDetectResponse> handler) {
        return detect(ExternalAgentConfigDetectParams{}, std::move(handler));
    }

    Submission ExternalAgents::importConfiguration(ExternalAgentConfigImportParams params,
                                                   CompletionHandler<ExternalAgentConfigImportResponse> handler) {
        return submitTypedRequest<ExternalAgentConfigImportResponse>(protocol,
                                                                     detail::ClientRequestTarget::ExternalAgentConfigImport,
                                                                     params,
                                                                     std::move(handler),
                                                                     detail::encodeExternalAgentConfigImportParams);
    }

    Submission ExternalAgents::readImportHistories(CompletionHandler<ExternalAgentConfigImportHistoriesReadResponse> handler) {
        return submitTypedRequest<ExternalAgentConfigImportHistoriesReadResponse>(
            protocol, detail::ClientRequestTarget::ExternalAgentConfigImportHistoriesRead, Unit{}, std::move(handler), encodeUnitParams);
    }

    Feedback::Feedback(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Feedback::upload(FeedbackUploadParams params, CompletionHandler<FeedbackUploadResponse> handler) {
        return submitTypedRequest<FeedbackUploadResponse>(
            protocol, detail::ClientRequestTarget::FeedbackUpload, params, std::move(handler), detail::encodeFeedbackUploadParams);
    }

    Hooks::Hooks(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Hooks::list(HooksListParams params, CompletionHandler<HooksListResponse> handler) {
        return submitTypedRequest<HooksListResponse>(
            protocol, detail::ClientRequestTarget::HooksList, params, std::move(handler), detail::encodeHooksListParams);
    }

    Submission Hooks::list(CompletionHandler<HooksListResponse> handler) {
        return list(HooksListParams{}, std::move(handler));
    }

    Marketplace::Marketplace(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Marketplace::add(MarketplaceAddParams params, CompletionHandler<MarketplaceAddResponse> handler) {
        return submitTypedRequest<MarketplaceAddResponse>(
            protocol, detail::ClientRequestTarget::MarketplaceAdd, params, std::move(handler), detail::encodeMarketplaceAddParams);
    }

    Submission Marketplace::remove(MarketplaceRemoveParams params, CompletionHandler<MarketplaceRemoveResponse> handler) {
        return submitTypedRequest<MarketplaceRemoveResponse>(
            protocol, detail::ClientRequestTarget::MarketplaceRemove, params, std::move(handler), detail::encodeMarketplaceRemoveParams);
    }

    Submission Marketplace::upgrade(MarketplaceUpgradeParams params, CompletionHandler<MarketplaceUpgradeResponse> handler) {
        return submitTypedRequest<MarketplaceUpgradeResponse>(
            protocol, detail::ClientRequestTarget::MarketplaceUpgrade, params, std::move(handler), detail::encodeMarketplaceUpgradeParams);
    }

    Submission Marketplace::upgrade(CompletionHandler<MarketplaceUpgradeResponse> handler) {
        return upgrade(MarketplaceUpgradeParams{}, std::move(handler));
    }

    Mcp::Mcp(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Mcp::startOauthLogin(McpServerOauthLoginParams params, CompletionHandler<McpServerOauthLoginResponse> handler) {
        return submitTypedRequest<McpServerOauthLoginResponse>(protocol,
                                                               detail::ClientRequestTarget::McpServerOauthLogin,
                                                               params,
                                                               std::move(handler),
                                                               detail::encodeMcpServerOauthLoginParams);
    }

    Submission Mcp::readResource(McpResourceReadParams params, CompletionHandler<McpResourceReadResponse> handler) {
        return submitTypedRequest<McpResourceReadResponse>(
            protocol, detail::ClientRequestTarget::McpResourceRead, params, std::move(handler), detail::encodeMcpResourceReadParams);
    }

    Submission Mcp::callTool(McpServerToolCallParams params, CompletionHandler<McpServerToolCallResponse> handler) {
        return submitTypedRequest<McpServerToolCallResponse>(
            protocol, detail::ClientRequestTarget::McpServerToolCall, params, std::move(handler), detail::encodeMcpServerToolCallParams);
    }

    Submission Mcp::listServers(ListMcpServerStatusParams params, CompletionHandler<ListMcpServerStatusResponse> handler) {
        return submitTypedRequest<ListMcpServerStatusResponse>(protocol,
                                                               detail::ClientRequestTarget::McpServerStatusList,
                                                               params,
                                                               std::move(handler),
                                                               detail::encodeListMcpServerStatusParams);
    }

    Submission Mcp::listServers(CompletionHandler<ListMcpServerStatusResponse> handler) {
        return listServers(ListMcpServerStatusParams{}, std::move(handler));
    }

    WindowsSandbox::WindowsSandbox(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission WindowsSandbox::checkReadiness(CompletionHandler<WindowsSandboxReadinessResponse> handler) {
        return submitTypedRequest<WindowsSandboxReadinessResponse>(
            protocol, detail::ClientRequestTarget::WindowsSandboxReadiness, Unit{}, std::move(handler), encodeUnitParams);
    }

    Submission WindowsSandbox::startSetup(WindowsSandboxSetupStartParams params,
                                          CompletionHandler<WindowsSandboxSetupStartResponse> handler) {
        return submitTypedRequest<WindowsSandboxSetupStartResponse>(protocol,
                                                                    detail::ClientRequestTarget::WindowsSandboxSetupStart,
                                                                    params,
                                                                    std::move(handler),
                                                                    detail::encodeWindowsSandboxSetupStartParams);
    }

    Skills::Skills(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Skills::writeConfig(SkillsConfigWriteParams params, CompletionHandler<SkillsConfigWriteResponse> handler) {
        return submitTypedRequest<SkillsConfigWriteResponse>(
            protocol, detail::ClientRequestTarget::SkillsConfigWrite, params, std::move(handler), detail::encodeSkillsConfigWriteParams);
    }

    Submission Skills::setExtraRoots(SkillsExtraRootsSetParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(protocol,
                                        detail::ClientRequestTarget::SkillsExtraRootsSet,
                                        params,
                                        std::move(handler),
                                        detail::encodeSkillsExtraRootsSetParams);
    }

    Submission Skills::list(SkillsListParams params, CompletionHandler<SkillsListResponse> handler) {
        return submitTypedRequest<SkillsListResponse>(
            protocol, detail::ClientRequestTarget::SkillsList, params, std::move(handler), detail::encodeSkillsListParams);
    }

    Submission Skills::list(CompletionHandler<SkillsListResponse> handler) {
        return list(SkillsListParams{}, std::move(handler));
    }

    Plugins::Plugins(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Plugins::install(PluginInstallParams params, CompletionHandler<PluginInstallResponse> handler) {
        return submitTypedRequest<PluginInstallResponse>(
            protocol, detail::ClientRequestTarget::PluginInstall, params, std::move(handler), detail::encodePluginInstallParams);
    }

    Submission Plugins::installed(PluginInstalledParams params, CompletionHandler<PluginInstalledResponse> handler) {
        return submitTypedRequest<PluginInstalledResponse>(
            protocol, detail::ClientRequestTarget::PluginInstalled, params, std::move(handler), detail::encodePluginInstalledParams);
    }

    Submission Plugins::installed(CompletionHandler<PluginInstalledResponse> handler) {
        return installed(PluginInstalledParams{}, std::move(handler));
    }

    Submission Plugins::list(PluginListParams params, CompletionHandler<PluginListResponse> handler) {
        return submitTypedRequest<PluginListResponse>(
            protocol, detail::ClientRequestTarget::PluginList, params, std::move(handler), detail::encodePluginListParams);
    }

    Submission Plugins::list(CompletionHandler<PluginListResponse> handler) {
        return list(PluginListParams{}, std::move(handler));
    }

    Submission Plugins::read(PluginReadParams params, CompletionHandler<PluginReadResponse> handler) {
        return submitTypedRequest<PluginReadResponse>(
            protocol, detail::ClientRequestTarget::PluginRead, params, std::move(handler), detail::encodePluginReadParams);
    }

    Submission Plugins::shareCheckout(PluginShareCheckoutParams params, CompletionHandler<PluginShareCheckoutResponse> handler) {
        return submitTypedRequest<PluginShareCheckoutResponse>(protocol,
                                                               detail::ClientRequestTarget::PluginShareCheckout,
                                                               params,
                                                               std::move(handler),
                                                               detail::encodePluginShareCheckoutParams);
    }

    Submission Plugins::shareDelete(PluginShareDeleteParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::PluginShareDelete, params, std::move(handler), detail::encodePluginShareDeleteParams);
    }

    Submission Plugins::shareList(CompletionHandler<PluginShareListResponse> handler) {
        return submitTypedRequest<PluginShareListResponse>(protocol,
                                                           detail::ClientRequestTarget::PluginShareList,
                                                           PluginShareListParams{},
                                                           std::move(handler),
                                                           detail::encodePluginShareListParams);
    }

    Submission Plugins::shareSave(PluginShareSaveParams params, CompletionHandler<PluginShareSaveResponse> handler) {
        return submitTypedRequest<PluginShareSaveResponse>(
            protocol, detail::ClientRequestTarget::PluginShareSave, params, std::move(handler), detail::encodePluginShareSaveParams);
    }

    Submission Plugins::shareUpdateTargets(PluginShareUpdateTargetsParams params,
                                           CompletionHandler<PluginShareUpdateTargetsResponse> handler) {
        return submitTypedRequest<PluginShareUpdateTargetsResponse>(protocol,
                                                                    detail::ClientRequestTarget::PluginShareUpdateTargets,
                                                                    params,
                                                                    std::move(handler),
                                                                    detail::encodePluginShareUpdateTargetsParams);
    }

    Submission Plugins::readSkill(PluginSkillReadParams params, CompletionHandler<PluginSkillReadResponse> handler) {
        return submitTypedRequest<PluginSkillReadResponse>(
            protocol, detail::ClientRequestTarget::PluginSkillRead, params, std::move(handler), detail::encodePluginSkillReadParams);
    }

    Submission Plugins::uninstall(PluginUninstallParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::PluginUninstall, params, std::move(handler), detail::encodePluginUninstallParams);
    }

    Accounts::Accounts(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Accounts::cancelLogin(CancelLoginAccountParams params, CompletionHandler<CancelLoginAccountResponse> handler) {
        return submitTypedRequest<CancelLoginAccountResponse>(
            protocol, detail::ClientRequestTarget::AccountLoginCancel, params, std::move(handler), detail::encodeCancelLoginAccountParams);
    }

    Submission Accounts::startLogin(LoginAccountParams params, CompletionHandler<LoginAccountResponse> handler) {
        return submitTypedRequest<LoginAccountResponse>(
            protocol, detail::ClientRequestTarget::AccountLoginStart, params, std::move(handler), detail::encodeLoginAccountParams);
    }

    Submission Accounts::logout(DoneHandler handler) {
        return submitTypedRequest<Unit>(protocol, detail::ClientRequestTarget::AccountLogout, Unit{}, std::move(handler), encodeUnitParams);
    }

    Submission Accounts::consumeRateLimitResetCredit(ConsumeAccountRateLimitResetCreditParams params,
                                                     CompletionHandler<ConsumeAccountRateLimitResetCreditResponse> handler) {
        return submitTypedRequest<ConsumeAccountRateLimitResetCreditResponse>(
            protocol,
            detail::ClientRequestTarget::AccountRateLimitResetCreditConsume,
            params,
            std::move(handler),
            detail::encodeConsumeAccountRateLimitResetCreditParams);
    }

    Submission Accounts::readRateLimits(CompletionHandler<GetAccountRateLimitsResponse> handler) {
        return submitTypedRequest<GetAccountRateLimitsResponse>(
            protocol, detail::ClientRequestTarget::AccountRateLimitsRead, Unit{}, std::move(handler), encodeUnitParams);
    }

    Submission Accounts::read(GetAccountParams params, CompletionHandler<GetAccountResponse> handler) {
        return submitTypedRequest<GetAccountResponse>(
            protocol, detail::ClientRequestTarget::AccountRead, params, std::move(handler), detail::encodeGetAccountParams);
    }

    Submission Accounts::read(CompletionHandler<GetAccountResponse> handler) {
        return read(GetAccountParams{}, std::move(handler));
    }

    Submission Accounts::sendAddCreditsNudgeEmail(SendAddCreditsNudgeEmailParams params,
                                                  CompletionHandler<SendAddCreditsNudgeEmailResponse> handler) {
        return submitTypedRequest<SendAddCreditsNudgeEmailResponse>(protocol,
                                                                    detail::ClientRequestTarget::AccountSendAddCreditsNudgeEmail,
                                                                    params,
                                                                    std::move(handler),
                                                                    detail::encodeSendAddCreditsNudgeEmailParams);
    }

    Commands::Commands(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Commands::exec(CommandExecParams params, CompletionHandler<CommandExecResponse> handler) {
        return submitTypedRequest<CommandExecResponse>(
            protocol, detail::ClientRequestTarget::CommandExec, params, std::move(handler), detail::encodeCommandExecParams);
    }

    Submission Commands::resize(CommandExecResizeParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::CommandExecResize, params, std::move(handler), detail::encodeCommandExecResizeParams);
    }

    Submission Commands::terminate(CommandExecTerminateParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(protocol,
                                        detail::ClientRequestTarget::CommandExecTerminate,
                                        params,
                                        std::move(handler),
                                        detail::encodeCommandExecTerminateParams);
    }

    Submission Commands::write(CommandExecWriteParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::CommandExecWrite, params, std::move(handler), detail::encodeCommandExecWriteParams);
    }

    Filesystem::Filesystem(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Filesystem::copy(FsCopyParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::FsCopy, params, std::move(handler), detail::encodeFsCopyParams);
    }

    Submission Filesystem::createDirectory(FsCreateDirectoryParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::FsCreateDirectory, params, std::move(handler), detail::encodeFsCreateDirectoryParams);
    }

    Submission Filesystem::getMetadata(FsGetMetadataParams params, CompletionHandler<FsGetMetadataResponse> handler) {
        return submitTypedRequest<FsGetMetadataResponse>(
            protocol, detail::ClientRequestTarget::FsGetMetadata, params, std::move(handler), detail::encodeFsGetMetadataParams);
    }

    Submission Filesystem::readDirectory(FsReadDirectoryParams params, CompletionHandler<FsReadDirectoryResponse> handler) {
        return submitTypedRequest<FsReadDirectoryResponse>(
            protocol, detail::ClientRequestTarget::FsReadDirectory, params, std::move(handler), detail::encodeFsReadDirectoryParams);
    }

    Submission Filesystem::readFile(FsReadFileParams params, CompletionHandler<FsReadFileResponse> handler) {
        return submitTypedRequest<FsReadFileResponse>(
            protocol, detail::ClientRequestTarget::FsReadFile, params, std::move(handler), detail::encodeFsReadFileParams);
    }

    Submission Filesystem::remove(FsRemoveParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::FsRemove, params, std::move(handler), detail::encodeFsRemoveParams);
    }

    Submission Filesystem::watch(FsWatchParams params, CompletionHandler<FsWatchResponse> handler) {
        return submitTypedRequest<FsWatchResponse>(
            protocol, detail::ClientRequestTarget::FsWatch, params, std::move(handler), detail::encodeFsWatchParams);
    }

    Submission Filesystem::unwatch(FsUnwatchParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::FsUnwatch, params, std::move(handler), detail::encodeFsUnwatchParams);
    }

    Submission Filesystem::writeFile(FsWriteFileParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::FsWriteFile, params, std::move(handler), detail::encodeFsWriteFileParams);
    }

    Submission Filesystem::fuzzyFileSearch(FuzzyFileSearchParams params, CompletionHandler<FuzzyFileSearchResponse> handler) {
        return submitTypedRequest<FuzzyFileSearchResponse>(
            protocol, detail::ClientRequestTarget::FuzzyFileSearch, params, std::move(handler), detail::encodeFuzzyFileSearchParams);
    }

    Submission Accounts::readUsage(CompletionHandler<GetAccountTokenUsageResponse> handler) {
        return submitTypedRequest<GetAccountTokenUsageResponse>(
            protocol, detail::ClientRequestTarget::AccountUsageRead, Unit{}, std::move(handler), encodeUnitParams);
    }

    Submission Accounts::readWorkspaceMessages(CompletionHandler<GetWorkspaceMessagesResponse> handler) {
        return submitTypedRequest<GetWorkspaceMessagesResponse>(
            protocol, detail::ClientRequestTarget::AccountWorkspaceMessagesRead, Unit{}, std::move(handler), encodeUnitParams);
    }

    Configuration::Configuration(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Configuration::batchWrite(ConfigBatchWriteParams params, CompletionHandler<ConfigWriteResponse> handler) {
        return submitTypedRequest<ConfigWriteResponse>(
            protocol,
            detail::ClientRequestTarget::ConfigBatchWrite,
            params,
            std::move(handler),
            detail::encodeConfigBatchWriteParams);
    }

    Submission Configuration::reloadMcpServers(DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol, detail::ClientRequestTarget::ConfigMcpServerReload, Unit{}, std::move(handler), encodeUnitParams);
    }

    Submission Configuration::read(ConfigReadParams params, CompletionHandler<ConfigReadResponse> handler) {
        return submitTypedRequest<ConfigReadResponse>(
            protocol, detail::ClientRequestTarget::ConfigRead, params, std::move(handler), detail::encodeConfigReadParams);
    }

    Submission Configuration::read(CompletionHandler<ConfigReadResponse> handler) {
        return read(ConfigReadParams{}, std::move(handler));
    }

    Submission Configuration::readRequirements(CompletionHandler<ConfigRequirementsReadResponse> handler) {
        return submitTypedRequest<ConfigRequirementsReadResponse>(
            protocol, detail::ClientRequestTarget::ConfigRequirementsRead, Unit{}, std::move(handler), encodeUnitParams);
    }

    Submission Configuration::writeValue(ConfigValueWriteParams params, CompletionHandler<ConfigWriteResponse> handler) {
        return submitTypedRequest<ConfigWriteResponse>(
            protocol,
            detail::ClientRequestTarget::ConfigValueWrite,
            params,
            std::move(handler),
            detail::encodeConfigValueWriteParams);
    }

    Submission Configuration::setExperimentalFeatureEnablement(ExperimentalFeatureEnablementSetParams params,
                                                               CompletionHandler<ExperimentalFeatureEnablementSetResponse> handler) {
        return submitTypedRequest<ExperimentalFeatureEnablementSetResponse>(
            protocol,
            detail::ClientRequestTarget::ExperimentalFeatureEnablementSet,
            params,
            std::move(handler),
            detail::encodeExperimentalFeatureEnablementSetParams);
    }

    Submission Configuration::listExperimentalFeatures(ExperimentalFeatureListParams params,
                                                       CompletionHandler<ExperimentalFeatureListResponse> handler) {
        return submitTypedRequest<ExperimentalFeatureListResponse>(
            protocol,
            detail::ClientRequestTarget::ExperimentalFeatureList,
            params,
            std::move(handler),
            detail::encodeExperimentalFeatureListParams);
    }

    Submission Configuration::listExperimentalFeatures(CompletionHandler<ExperimentalFeatureListResponse> handler) {
        return listExperimentalFeatures(ExperimentalFeatureListParams{}, std::move(handler));
    }

    Models::Models(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Models::list(ModelListParams params, CompletionHandler<ModelListResponse> handler) {
        return submitTypedRequest<ModelListResponse>(
            protocol, detail::ClientRequestTarget::ModelList, params, std::move(handler), detail::encodeModelListParams);
    }

    Submission Models::list(CompletionHandler<ModelListResponse> handler) {
        return list(ModelListParams{}, std::move(handler));
    }

    Submission Models::readProviderCapabilities(CompletionHandler<ModelProviderCapabilitiesReadResponse> handler) {
        return submitTypedRequest<ModelProviderCapabilitiesReadResponse>(protocol,
                                                                         detail::ClientRequestTarget::ModelProviderCapabilitiesRead,
                                                                         ModelProviderCapabilitiesReadParams{},
                                                                         std::move(handler),
                                                                         detail::encodeModelProviderCapabilitiesReadParams);
    }

    PermissionProfiles::PermissionProfiles(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission PermissionProfiles::list(PermissionProfileListParams params, CompletionHandler<PermissionProfileListResponse> handler) {
        return submitTypedRequest<PermissionProfileListResponse>(protocol,
                                                                 detail::ClientRequestTarget::PermissionProfileList,
                                                                 params,
                                                                 std::move(handler),
                                                                 detail::encodePermissionProfileListParams);
    }

    Submission PermissionProfiles::list(CompletionHandler<PermissionProfileListResponse> handler) {
        return list(PermissionProfileListParams{}, std::move(handler));
    }

    Reviews::Reviews(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Reviews::start(ReviewStartParams params, CompletionHandler<ReviewStartResponse> handler) {
        return submitTypedRequest<ReviewStartResponse>(
            protocol, detail::ClientRequestTarget::ReviewStart, params, std::move(handler), detail::encodeReviewStartParams);
    }

    Threads::Threads(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Threads::archive(ThreadArchiveParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol,
            detail::ClientRequestTarget::ThreadArchive,
            params,
            std::move(handler),
            detail::encodeThreadArchiveParams);
    }

    Submission Threads::approveGuardianDeniedAction(ThreadApproveGuardianDeniedActionParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(protocol,
                                        detail::ClientRequestTarget::ThreadApproveGuardianDeniedAction,
                                        params,
                                        std::move(handler),
                                        detail::encodeThreadApproveGuardianDeniedActionParams);
    }

    Submission Threads::startCompaction(ThreadCompactStartParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol,
            detail::ClientRequestTarget::ThreadCompactStart,
            params,
            std::move(handler),
            detail::encodeThreadCompactStartParams);
    }

    Submission Threads::remove(ThreadDeleteParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol,
            detail::ClientRequestTarget::ThreadDelete,
            params,
            std::move(handler),
            detail::encodeThreadDeleteParams);
    }

    Submission Threads::fork(ThreadForkParams params, CompletionHandler<ThreadForkResponse> handler) {
        return submitTypedRequest<ThreadForkResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadFork,
            params,
            std::move(handler),
            detail::encodeThreadForkParams);
    }

    Submission Threads::clearGoal(ThreadGoalClearParams params, CompletionHandler<ThreadGoalClearResponse> handler) {
        return submitTypedRequest<ThreadGoalClearResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadGoalClear,
            params,
            std::move(handler),
            detail::encodeThreadGoalClearParams);
    }

    Submission Threads::getGoal(ThreadGoalGetParams params, CompletionHandler<ThreadGoalGetResponse> handler) {
        return submitTypedRequest<ThreadGoalGetResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadGoalGet,
            params,
            std::move(handler),
            detail::encodeThreadGoalGetParams);
    }

    Submission Threads::setGoal(ThreadGoalSetParams params, CompletionHandler<ThreadGoalSetResponse> handler) {
        return submitTypedRequest<ThreadGoalSetResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadGoalSet,
            params,
            std::move(handler),
            detail::encodeThreadGoalSetParams);
    }

    Submission Threads::injectItems(ThreadInjectItemsParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol,
            detail::ClientRequestTarget::ThreadInjectItems,
            params,
            std::move(handler),
            detail::encodeThreadInjectItemsParams);
    }

    Submission Threads::list(ThreadListParams params, CompletionHandler<ThreadListResponse> handler) {
        return submitTypedRequest<ThreadListResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadList,
            params,
            std::move(handler),
            [](const ThreadListParams& value, std::string& error) {
                return detail::encodeThreadListParams(value, error);
            });
    }

    Submission Threads::list(CompletionHandler<ThreadListResponse> handler) {
        return list(ThreadListParams{}, std::move(handler));
    }

    Submission Threads::listLoaded(ThreadLoadedListParams params, CompletionHandler<ThreadLoadedListResponse> handler) {
        return submitTypedRequest<ThreadLoadedListResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadLoadedList,
            params,
            std::move(handler),
            detail::encodeThreadLoadedListParams);
    }

    Submission Threads::listLoaded(CompletionHandler<ThreadLoadedListResponse> handler) {
        return listLoaded(ThreadLoadedListParams{}, std::move(handler));
    }

    Submission Threads::updateMetadata(ThreadMetadataUpdateParams params, CompletionHandler<ThreadMetadataUpdateResponse> handler) {
        return submitTypedRequest<ThreadMetadataUpdateResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadMetadataUpdate,
            params,
            std::move(handler),
            detail::encodeThreadMetadataUpdateParams);
    }

    Submission Threads::setName(ThreadSetNameParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol,
            detail::ClientRequestTarget::ThreadSetName,
            params,
            std::move(handler),
            detail::encodeThreadSetNameParams);
    }

    Submission Threads::read(ThreadReadParams params, CompletionHandler<ThreadReadResponse> handler) {
        return submitTypedRequest<ThreadReadResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadRead,
            params,
            std::move(handler),
            [](const ThreadReadParams& value, std::string& error) {
                return detail::encodeThreadReadParams(value, error);
            });
    }

    Submission Threads::read(ThreadId threadId, CompletionHandler<ThreadReadResponse> handler) {
        ThreadReadParams params;
        params.threadId = std::move(threadId);
        return read(std::move(params), std::move(handler));
    }

    Submission Threads::resume(ThreadResumeParams params, CompletionHandler<ThreadResumeResponse> handler) {
        return submitTypedRequest<ThreadResumeResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadResume,
            params,
            std::move(handler),
            [](const ThreadResumeParams& value, std::string& error) {
                return detail::encodeThreadResumeParams(value, error);
            });
    }

    Submission Threads::resume(ThreadId threadId, CompletionHandler<ThreadResumeResponse> handler) {
        ThreadResumeParams params;
        params.threadId = std::move(threadId);
        return resume(std::move(params), std::move(handler));
    }

    Submission Threads::rollback(ThreadRollbackParams params, CompletionHandler<ThreadRollbackResponse> handler) {
        return submitTypedRequest<ThreadRollbackResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadRollback,
            params,
            std::move(handler),
            detail::encodeThreadRollbackParams);
    }

    Submission Threads::shellCommand(ThreadShellCommandParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol,
            detail::ClientRequestTarget::ThreadShellCommand,
            params,
            std::move(handler),
            detail::encodeThreadShellCommandParams);
    }

    Submission Threads::start(ThreadStartParams params, CompletionHandler<ThreadStartResponse> handler) {
        return submitTypedRequest<ThreadStartResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadStart,
            params,
            std::move(handler),
            [](const ThreadStartParams& value, std::string& error) {
                return detail::encodeThreadStartParams(value, error);
            });
    }

    Submission Threads::start(CompletionHandler<ThreadStartResponse> handler) {
        return start(ThreadStartParams{}, std::move(handler));
    }

    Submission Threads::start(AbsolutePath cwd, CompletionHandler<ThreadStartResponse> handler) {
        ThreadStartParams params;
        params.cwd = std::move(cwd.value);
        return start(std::move(params), std::move(handler));
    }

    Submission Threads::unarchive(ThreadUnarchiveParams params, CompletionHandler<ThreadUnarchiveResponse> handler) {
        return submitTypedRequest<ThreadUnarchiveResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadUnarchive,
            params,
            std::move(handler),
            detail::encodeThreadUnarchiveParams);
    }

    Submission Threads::unsubscribe(ThreadUnsubscribeParams params, CompletionHandler<ThreadUnsubscribeResponse> handler) {
        return submitTypedRequest<ThreadUnsubscribeResponse>(
            protocol,
            detail::ClientRequestTarget::ThreadUnsubscribe,
            params,
            std::move(handler),
            detail::encodeThreadUnsubscribeParams);
    }

    Turns::Turns(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    Submission Turns::interrupt(TurnInterruptParams params, DoneHandler handler) {
        return submitTypedRequest<Unit>(
            protocol,
            detail::ClientRequestTarget::TurnInterrupt,
            params,
            std::move(handler),
            [](const TurnInterruptParams& value, std::string& error) {
                return detail::encodeTurnInterruptParams(value, error);
            });
    }

    Submission Turns::interrupt(ThreadId threadId, TurnId turnId, DoneHandler handler) {
        return interrupt(TurnInterruptParams{std::move(threadId), std::move(turnId)}, std::move(handler));
    }

    Submission Turns::start(TurnStartParams params, CompletionHandler<TurnStartResponse> handler) {
        const ThreadId threadId = params.threadId;
        return submitTypedRequest<TurnStartResponse>(
            protocol,
            detail::ClientRequestTarget::TurnStart,
            params,
            std::move(handler),
            [](const TurnStartParams& value, std::string& error) {
                return detail::encodeTurnStartParams(value, error);
            },
            threadId);
    }

    Submission Turns::start(ThreadId threadId, std::vector<TurnInput> input, CompletionHandler<TurnStartResponse> handler) {
        TurnStartParams params;
        params.threadId = std::move(threadId);
        params.input = std::move(input);
        return start(std::move(params), std::move(handler));
    }

    Submission Turns::start(ThreadId threadId, std::string text, CompletionHandler<TurnStartResponse> handler) {
        TextInput input;
        input.text = std::move(text);
        return start(std::move(threadId), std::vector<TurnInput>{std::move(input)}, std::move(handler));
    }

    Submission Turns::steer(TurnSteerParams params, CompletionHandler<TurnSteerResponse> handler) {
        return submitTypedRequest<TurnSteerResponse>(
            protocol,
            detail::ClientRequestTarget::TurnSteer,
            params,
            std::move(handler),
            detail::encodeTurnSteerParams);
    }

    Events::Events(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    void Events::setOnEvent(EventHandler handler) {
        if (!handler) {
            protocol->setTypedNotificationDispatcher({});
            return;
        }

        protocol->setTypedNotificationDispatcher([handler = std::move(handler)](const Notification& notification) {
            const Event event = detail::decodeEvent(notification);
            handler(event);
        });
    }

    Requests::Requests(AppServerClient::RawProtocol& protocol) noexcept
        : protocol(&protocol) {
    }

    void Requests::setOnRequest(RequestHandler handler) {
        if (!handler) {
            protocol->setTypedServerRequestDispatcher({});
            return;
        }

        protocol->setTypedServerRequestDispatcher([handler = std::move(handler)](const ServerRequest& request) {
            const TypedServerRequest typedRequest = detail::decodeServerRequest(request);
            handler(typedRequest);
        });
    }

    SendResult Requests::respond(const CommandApprovalRequest& request, ApprovalDecision decision) {
        if (decision.value.empty()) {
            return validationFailure("approval decision must not be empty");
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::CommandExecutionRequestApproval),
                                      Json{{"decision", std::move(decision.value)}});
    }

    SendResult Requests::respond(const CommandApprovalRequest& request, CommandExecutionRequestApprovalResponse response) {
        std::string error;
        std::optional<Json> encoded = detail::encodeCommandExecutionRequestApprovalResponse(response, error);
        if (!encoded) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::CommandExecutionRequestApproval),
                                      std::move(*encoded));
    }

    SendResult Requests::respond(const FileChangeApprovalRequest& request, ApprovalDecision decision) {
        if (decision.value.empty()) {
            return validationFailure("approval decision must not be empty");
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::FileChangeRequestApproval),
                                      Json{{"decision", std::move(decision.value)}});
    }

    SendResult Requests::respond(const FileChangeApprovalRequest& request, FileChangeRequestApprovalResponse response) {
        std::string error;
        std::optional<Json> encoded = detail::encodeFileChangeRequestApprovalResponse(response, error);
        if (!encoded) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::FileChangeRequestApproval),
                                      std::move(*encoded));
    }

    SendResult Requests::respond(const ApplyPatchApprovalRequest& request, ApplyPatchApprovalResponse response) {
        std::string error;
        std::optional<Json> encoded = detail::encodeApplyPatchApprovalResponse(response, error);
        if (!encoded) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::ApplyPatchApproval),
                                      std::move(*encoded));
    }

    SendResult Requests::respond(const ExecCommandApprovalRequest& request, ExecCommandApprovalResponse response) {
        std::string error;
        std::optional<Json> encoded = detail::encodeExecCommandApprovalResponse(response, error);
        if (!encoded) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::ExecCommandApproval),
                                      std::move(*encoded));
    }

    SendResult Requests::respond(const PermissionsApprovalRequest& request, PermissionsRequestApprovalResponse response) {
        std::string error;
        std::optional<Json> encoded = detail::encodePermissionsRequestApprovalResponse(response, error);
        if (!encoded) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::PermissionsRequestApproval),
                                      std::move(*encoded));
    }

    SendResult Requests::respond(const AttestationGenerateRequest& request, AttestationGenerateResponse response) {
        std::string error;
        std::optional<Json> encoded = detail::encodeAttestationGenerateResponse(response, error);
        if (!encoded) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::AttestationGenerate),
                                      std::move(*encoded));
    }

    SendResult Requests::respond(const DynamicToolCallRequest& request, DynamicToolCallResponse response) {
        std::string error;
        std::optional<Json> encoded = detail::encodeDynamicToolCallResponse(response, error);
        if (!encoded) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(
            request.requestId, request.requestToken, registeredMethod(detail::ServerRequestTarget::DynamicToolCall), std::move(*encoded));
    }

    SendResult Requests::respond(const UserInputRequest& request, ToolRequestUserInputResponse response) {
        std::string error;
        std::optional<Json> encoded = detail::encodeToolRequestUserInputResponse(response, error);
        if (!encoded) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::ToolRequestUserInput),
                                      std::move(*encoded));
    }

    SendResult Requests::respond(const UserInputRequest& request, std::vector<UserInputAnswer> answers) {
        std::set<std::string> questionIds;
        for (const UserInputQuestion& question : request.questions) {
            questionIds.insert(question.id);
        }

        std::set<std::string> answeredIds;
        ToolRequestUserInputResponse response;
        for (UserInputAnswer& answer : answers) {
            if (!questionIds.contains(answer.questionId)) {
                return validationFailure("user-input answer refers to an unknown question ID");
            }
            if (!answeredIds.insert(answer.questionId).second) {
                return validationFailure("duplicate user-input answer for a question ID");
            }
            response.answers.emplace(std::move(answer.questionId), ToolRequestUserInputAnswer{std::move(answer.answers), Json::object()});
        }
        return respond(request, std::move(response));
    }

    SendResult Requests::respond(const McpServerElicitationRequest& request, McpServerElicitationRequestResponse response) {
        std::string error;
        std::optional<Json> encoded = detail::encodeMcpServerElicitationRequestResponse(response, error);
        if (!encoded) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::McpServerElicitation),
                                      std::move(*encoded));
    }

    SendResult Requests::respond(const AuthenticationRequest& request, ChatgptAuthTokensRefreshResponse response) {
        std::string error;
        std::optional<Json> result = detail::encodeChatgptAuthTokensRefreshResponse(std::move(response), error);
        if (!result) {
            return validationFailure(std::move(error));
        }
        return protocol->respondOwned(request.requestId,
                                      request.requestToken,
                                      registeredMethod(detail::ServerRequestTarget::ChatgptAuthTokensRefresh),
                                      std::move(*result));
    }

    SendResult Requests::respond(const AuthenticationRequest& request, AuthenticationResponse response) {
        ChatgptAuthTokensRefreshResponse canonical{
            std::move(response.accessToken),
            AccountId{std::move(response.chatgptAccountId)},
            response.chatgptPlanType ? OptionalNullable<PlanType>::withValue(PlanType{std::move(*response.chatgptPlanType)})
                                     : OptionalNullable<PlanType>::explicitNull()};
        return respond(request, std::move(canonical));
    }

    SendResult Requests::reject(const AttestationGenerateRequest& request, ProtocolError error) {
        return protocol->rejectOwned(
            request.requestId, request.requestToken, registeredMethod(detail::ServerRequestTarget::AttestationGenerate), std::move(error));
    }

    SendResult Requests::reject(const DynamicToolCallRequest& request, ProtocolError error) {
        return protocol->rejectOwned(
            request.requestId, request.requestToken, registeredMethod(detail::ServerRequestTarget::DynamicToolCall), std::move(error));
    }

    SendResult Requests::reject(const UserInputRequest& request, ProtocolError error) {
        return protocol->rejectOwned(
            request.requestId, request.requestToken, registeredMethod(detail::ServerRequestTarget::ToolRequestUserInput), std::move(error));
    }

    SendResult Requests::reject(const McpServerElicitationRequest& request, ProtocolError error) {
        return protocol->rejectOwned(
            request.requestId, request.requestToken, registeredMethod(detail::ServerRequestTarget::McpServerElicitation), std::move(error));
    }

    SendResult Requests::respondRaw(const UnknownServerRequest& request, Json result) {
        return protocol->respondOwned(request.requestId, request.requestToken, std::move(result));
    }

    SendResult Requests::reject(const UnknownServerRequest& request, ProtocolError error) {
        return protocol->rejectOwned(request.requestId, request.requestToken, std::move(error));
    }

    SendResult Requests::validationFailure(std::string message) {
        return {false, Error{Error::Category::Protocol, EINVAL, std::move(message)}};
    }

} // namespace ai::openai::codex::typed
