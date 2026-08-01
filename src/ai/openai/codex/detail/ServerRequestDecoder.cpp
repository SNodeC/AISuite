/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/ServerRequestDecoder.h"

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/detail/AccountCodec.h"
#include "ai/openai/codex/detail/ApprovalCodec.h"
#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/detail/McpReverseRequestCodec.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/typed/Accounts.h"
#include "ai/openai/codex/typed/Items.h"
#include "ai/openai/codex/typed/Types.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <nlohmann/detail/iterators/iter_impl.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::detail {

    namespace {
        typed::UnknownServerRequest unknownRequest(const ServerRequest& request, std::optional<std::string> decodeError = std::nullopt) {
            const bool malformed = decodeError.has_value();
            std::string fieldPath = "$.params";
            if (malformed && request.method == "account/chatgptAuthTokens/refresh") {
                const std::size_t begin = decodeError->find("'$");
                if (begin != std::string::npos) {
                    const std::size_t end = decodeError->find('\'', begin + 1);
                    if (end != std::string::npos) {
                        fieldPath = decodeError->substr(begin + 1, end - begin - 1);
                    }
                }
            }
            return {request.id,
                    request.token,
                    request.method,
                    request.params,
                    request.raw,
                    malformed ? std::optional<typed::DecodeDiagnostic>{malformedKnownDiagnostic(request.method, std::move(fieldPath))}
                              : std::optional<typed::DecodeDiagnostic>{unknownMethodDiagnostic(request.method)}};
        }

        std::optional<typed::CommandApprovalRequest> decodeCommandApproval(const ServerRequest& request, std::string& error) {
            std::optional<typed::CommandExecutionRequestApprovalParams> canonical =
                decodeCommandExecutionRequestApprovalParams(request.params, error);
            if (!canonical) {
                return std::nullopt;
            }

            std::optional<std::string> command;
            std::optional<std::string> cwd;
            std::optional<std::string> reason;
            if (canonical->command.value) {
                command = *canonical->command.value;
            }
            if (canonical->cwd.value) {
                cwd = canonical->cwd.value->value;
            }
            if (canonical->reason.value) {
                reason = *canonical->reason.value;
            }

            std::vector<typed::DecodeDiagnostic> diagnostics = canonical->diagnostics;
            return typed::CommandApprovalRequest{request.id,
                                                 request.token,
                                                 canonical->threadId,
                                                 canonical->turnId,
                                                 canonical->itemId,
                                                 canonical->startedAtMs,
                                                 std::move(command),
                                                 std::move(cwd),
                                                 std::move(reason),
                                                 request.params,
                                                 request.raw,
                                                 std::move(*canonical),
                                                 std::move(diagnostics)};
        }

        std::optional<typed::FileChangeApprovalRequest> decodeFileChangeApproval(const ServerRequest& request, std::string& error) {
            std::optional<typed::FileChangeRequestApprovalParams> canonical = decodeFileChangeRequestApprovalParams(request.params, error);
            if (!canonical) {
                return std::nullopt;
            }

            std::optional<std::string> reason;
            std::optional<std::string> grantRoot;
            if (canonical->reason.value) {
                reason = *canonical->reason.value;
            }
            if (canonical->grantRoot.value) {
                grantRoot = *canonical->grantRoot.value;
            }

            std::vector<typed::DecodeDiagnostic> diagnostics = canonical->diagnostics;
            return typed::FileChangeApprovalRequest{request.id,
                                                    request.token,
                                                    canonical->threadId,
                                                    canonical->turnId,
                                                    canonical->itemId,
                                                    canonical->startedAtMs,
                                                    std::move(reason),
                                                    std::move(grantRoot),
                                                    request.raw,
                                                    std::move(*canonical),
                                                    std::move(diagnostics)};
        }

        std::optional<typed::ApplyPatchApprovalRequest> decodeApplyPatchApproval(const ServerRequest& request, std::string& error) {
            std::optional<typed::ApplyPatchApprovalParams> params = decodeApplyPatchApprovalParams(request.params, error);
            if (!params) {
                return std::nullopt;
            }
            std::vector<typed::DecodeDiagnostic> diagnostics = params->diagnostics;
            return typed::ApplyPatchApprovalRequest{request.id, request.token, std::move(*params), request.raw, std::move(diagnostics)};
        }

        std::optional<typed::ExecCommandApprovalRequest> decodeExecCommandApproval(const ServerRequest& request, std::string& error) {
            std::optional<typed::ExecCommandApprovalParams> params = decodeExecCommandApprovalParams(request.params, error);
            if (!params) {
                return std::nullopt;
            }
            std::vector<typed::DecodeDiagnostic> diagnostics = params->diagnostics;
            return typed::ExecCommandApprovalRequest{request.id, request.token, std::move(*params), request.raw, std::move(diagnostics)};
        }

        std::optional<typed::PermissionsApprovalRequest> decodePermissionsApproval(const ServerRequest& request, std::string& error) {
            std::optional<typed::PermissionsRequestApprovalParams> params = decodePermissionsRequestApprovalParams(request.params, error);
            if (!params) {
                return std::nullopt;
            }
            std::vector<typed::DecodeDiagnostic> diagnostics = params->diagnostics;
            return typed::PermissionsApprovalRequest{request.id, request.token, std::move(*params), request.raw, std::move(diagnostics)};
        }

        std::optional<typed::AttestationGenerateRequest> decodeAttestationGenerate(const ServerRequest& request, std::string& error) {
            std::optional<typed::AttestationGenerateParams> params = decodeAttestationGenerateParams(request.params, error);
            if (!params) {
                return std::nullopt;
            }
            std::vector<typed::DecodeDiagnostic> diagnostics = params->diagnostics;
            return typed::AttestationGenerateRequest{request.id, request.token, std::move(*params), request.raw, std::move(diagnostics)};
        }

        std::optional<typed::DynamicToolCallRequest> decodeDynamicToolCall(const ServerRequest& request, std::string& error) {
            std::optional<typed::DynamicToolCallParams> params = decodeDynamicToolCallParams(request.params, error);
            if (!params) {
                return std::nullopt;
            }
            std::vector<typed::DecodeDiagnostic> diagnostics = params->diagnostics;
            return typed::DynamicToolCallRequest{request.id, request.token, std::move(*params), request.raw, std::move(diagnostics)};
        }

        std::optional<typed::UserInputRequest> decodeUserInput(const ServerRequest& request, std::string& error) {
            std::optional<typed::ToolRequestUserInputParams> canonical = decodeToolRequestUserInputParams(request.params, error);
            if (!canonical) {
                return std::nullopt;
            }

            std::vector<typed::UserInputQuestion> decodedQuestions;
            decodedQuestions.reserve(canonical->questions.size());
            for (const typed::ToolRequestUserInputQuestion& canonicalQuestion : canonical->questions) {
                std::vector<typed::UserInputOption> options;
                if (canonicalQuestion.options.value) {
                    options.reserve(canonicalQuestion.options.value->size());
                    for (const typed::ToolRequestUserInputOption& canonicalOption : *canonicalQuestion.options.value) {
                        options.push_back({canonicalOption.label, canonicalOption.description, canonicalOption.raw});
                    }
                }
                decodedQuestions.push_back({canonicalQuestion.id,
                                            canonicalQuestion.header,
                                            canonicalQuestion.question,
                                            std::move(options),
                                            canonicalQuestion.isOther.value_or(false),
                                            canonicalQuestion.isSecret.value_or(false),
                                            canonicalQuestion.raw});
            }

            std::vector<typed::DecodeDiagnostic> diagnostics = canonical->diagnostics;
            return typed::UserInputRequest{request.id,
                                           request.token,
                                           canonical->threadId,
                                           canonical->turnId,
                                           canonical->itemId,
                                           std::move(decodedQuestions),
                                           canonical->autoResolutionMs.value,
                                           request.raw,
                                           std::move(*canonical),
                                           std::move(diagnostics)};
        }

        std::optional<typed::McpServerElicitationRequest> decodeMcpServerElicitation(const ServerRequest& request, std::string& error) {
            std::optional<typed::McpServerElicitationRequestParams> params = decodeMcpServerElicitationRequestParams(request.params, error);
            if (!params) {
                return std::nullopt;
            }
            std::vector<typed::DecodeDiagnostic> diagnostics = params->diagnostics;
            return typed::McpServerElicitationRequest{request.id, request.token, std::move(*params), request.raw, std::move(diagnostics)};
        }

        std::optional<typed::AuthenticationRequest> decodeAuthentication(const ServerRequest& request, std::string& error) {
            std::optional<typed::ChatgptAuthTokensRefreshParams> canonical = decodeChatgptAuthTokensRefreshParams(request.params, error);
            if (!canonical) {
                return std::nullopt;
            }

            std::optional<std::string> previousAccountId;
            if (canonical->previousAccountId.value) {
                previousAccountId = canonical->previousAccountId.value->value;
            }

            std::vector<typed::DecodeDiagnostic> diagnostics = canonical->diagnostics;
            return typed::AuthenticationRequest{request.id,
                                                request.token,
                                                canonical->reason.value,
                                                std::move(previousAccountId),
                                                request.raw,
                                                std::move(*canonical),
                                                std::move(diagnostics)};
        }
    } // namespace

    typed::TypedServerRequest decodeServerRequest(const ServerRequest& request) noexcept {
        try {
            std::string error;
            const ProtocolSurfaceEntry* entry = findSurface(SurfaceCategory::ServerRequest, "ServerRequest", "method", request.method);
            const ServerRequestTarget* target = entry == nullptr || entry->runtimeDisposition != RuntimeDisposition::Typed
                                                    ? nullptr
                                                    : std::get_if<ServerRequestTarget>(&entry->runtimeTarget);
            if (target == nullptr) {
                return unknownRequest(request);
            }

            switch (*target) {
                case ServerRequestTarget::CommandExecutionRequestApproval: {
                    std::optional<typed::CommandApprovalRequest> decoded = decodeCommandApproval(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::FileChangeRequestApproval: {
                    std::optional<typed::FileChangeApprovalRequest> decoded = decodeFileChangeApproval(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::ToolRequestUserInput: {
                    std::optional<typed::UserInputRequest> decoded = decodeUserInput(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::ChatgptAuthTokensRefresh: {
                    std::optional<typed::AuthenticationRequest> decoded = decodeAuthentication(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::ApplyPatchApproval: {
                    std::optional<typed::ApplyPatchApprovalRequest> decoded = decodeApplyPatchApproval(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::ExecCommandApproval: {
                    std::optional<typed::ExecCommandApprovalRequest> decoded = decodeExecCommandApproval(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::PermissionsRequestApproval: {
                    std::optional<typed::PermissionsApprovalRequest> decoded = decodePermissionsApproval(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::AttestationGenerate: {
                    std::optional<typed::AttestationGenerateRequest> decoded = decodeAttestationGenerate(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::DynamicToolCall: {
                    std::optional<typed::DynamicToolCallRequest> decoded = decodeDynamicToolCall(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::McpServerElicitation: {
                    std::optional<typed::McpServerElicitationRequest> decoded = decodeMcpServerElicitation(request, error);
                    return decoded ? typed::TypedServerRequest{std::move(*decoded)}
                                   : typed::TypedServerRequest{unknownRequest(request, std::move(error))};
                }
                case ServerRequestTarget::Count:
                    break;
            }

            return unknownRequest(request);
        } catch (const Json::exception& exception) {
            return unknownRequest(request, std::string("unable to decode server request: ") + exception.what());
        } catch (const std::exception& exception) {
            return unknownRequest(request, std::string("unable to decode server request: ") + exception.what());
        } catch (...) {
            return unknownRequest(request, "unable to decode server request: unknown exception");
        }
    }

} // namespace ai::openai::codex::detail
