/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/Api.h"
#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/stdio/Client.h"
#include "ai/openai/codex/typed/Mcp.h"
#include "ai/openai/codex/typed/PermissionProfiles.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace codex = ai::openai::codex;
    namespace typed = ai::openai::codex::typed;

    constexpr std::size_t RequestCount = 9;

    codex::Json requestIdJson(const typed::TypedServerRequest& request) {
        return std::visit(
            [](const auto& value) -> codex::Json {
                return std::visit(
                    [](const auto& id) -> codex::Json {
                        return id;
                    },
                    value.requestId.value());
            },
            request);
    }

    codex::ServerRequestToken requestToken(const typed::TypedServerRequest& request) {
        return std::visit(
            [](const auto& value) {
                return value.requestToken;
            },
            request);
    }

    std::string requestMethod(const typed::TypedServerRequest& request) {
        return std::visit(
            [](const auto& value) {
                return value.raw.value("method", "");
            },
            request);
    }

    void replaceOwnership(typed::TypedServerRequest& target, const typed::TypedServerRequest& owner) {
        const codex::ServerRequestId id = std::visit(
            [](const auto& value) {
                return value.requestId;
            },
            owner);
        const codex::ServerRequestToken token = requestToken(owner);
        std::visit(
            [&](auto& value) {
                value.requestId = id;
                value.requestToken = token;
            },
            target);
    }

    void clearToken(typed::TypedServerRequest& request) {
        std::visit(
            [](auto& value) {
                value.requestToken = codex::ServerRequestToken{};
            },
            request);
    }

    typed::PermissionsRequestApprovalResponse permissionResponse() {
        typed::GrantedPermissionProfile permissions;
        typed::AdditionalFileSystemPermissions fileSystem;
        fileSystem.entries = typed::OptionalNullable<std::vector<typed::FileSystemSandboxEntry>>::withValue(
            {{.access = typed::FileSystemAccessMode::write(),
              .path = typed::SpecialFileSystemPath{.value = typed::SlashTmpFileSystemSpecialPath{},
                                                   .raw = codex::Json::object(),
                                                   .diagnostics = {}},
              .raw = codex::Json::object(),
              .diagnostics = {}}});
        fileSystem.globScanMaxDepth = typed::OptionalNullable<std::uint64_t>::withValue(1);
        fileSystem.read = typed::OptionalNullable<std::vector<std::string>>::explicitNull();
        fileSystem.write = typed::OptionalNullable<std::vector<std::string>>::withValue({"/synthetic/write"});
        permissions.fileSystem = typed::OptionalNullable<typed::AdditionalFileSystemPermissions>::withValue(std::move(fileSystem));
        permissions.network = typed::OptionalNullable<typed::AdditionalNetworkPermissions>::withValue(
            {.enabled = typed::OptionalNullable<bool>::withValue(true), .raw = codex::Json::object(), .diagnostics = {}});
        return {std::move(permissions), typed::PermissionGrantScope::session(), typed::OptionalNullable<bool>::withValue(false)};
    }

    typed::AttestationGenerateResponse attestationResponse() {
        typed::AttestationGenerateResponse response;
        response.token = "synthetic-attestation-token";
        return response;
    }

    typed::DynamicToolCallResponse dynamicToolResponse() {
        typed::InputTextDynamicToolCallOutputContentItem text;
        text.text = "synthetic tool output";
        typed::InputImageDynamicToolCallOutputContentItem image;
        image.imageUrl = "data:image/png;base64,AA==";

        typed::DynamicToolCallResponse response;
        response.contentItems = {std::move(text), std::move(image)};
        response.success = true;
        return response;
    }

    typed::ToolRequestUserInputResponse userInputResponse() {
        typed::ToolRequestUserInputResponse response;
        response.answers["choice"].answers = {"alpha"};
        response.answers["secret"].answers = {};
        return response;
    }

    typed::McpServerElicitationRequestResponse elicitationResponse() {
        typed::McpServerElicitationRequestResponse response;
        response.action = typed::McpServerElicitationAction::decline();
        return response;
    }

    class NineRequestRunner {
    public:
        NineRequestRunner(tests::support::TestResult& result, std::string markerPath)
            : result(result)
            , markerPath(std::move(markerPath))
            , client(CODEX_FAKE_APP_SERVER, {"a14-nine-requests", this->markerPath}) {
        }

        void start() {
            static_assert(std::variant_size_v<typed::TypedServerRequest> == 11);
            static_assert(std::is_same_v<std::variant_alternative_t<2, typed::TypedServerRequest>, typed::UserInputRequest>);
            static_assert(std::is_same_v<std::variant_alternative_t<8, typed::TypedServerRequest>, typed::AttestationGenerateRequest>);
            static_assert(std::is_same_v<std::variant_alternative_t<9, typed::TypedServerRequest>, typed::DynamicToolCallRequest>);
            static_assert(std::is_same_v<std::variant_alternative_t<10, typed::TypedServerRequest>, typed::McpServerElicitationRequest>);

            client.requests().setOnRequest([this](const typed::TypedServerRequest& request) {
                insideRequestCallback = true;
                struct ResetFlag {
                    bool& value;
                    ~ResetFlag() {
                        value = false;
                    }
                } reset{insideRequestCallback};
                handleRequest(request);
            });
            client.setOnStateChanged([this](const codex::StateChange& change) {
                handleState(change);
            });
            client.start();
        }

        [[nodiscard]] bool isFinished() const noexcept {
            return finished;
        }

    private:
        enum class Phase { Initial, DisconnectPending, Reconnected, ShutdownPending };

        void expect(bool condition, std::string message) {
            result.expectTrue(condition, std::move(message));
        }

        codex::SendResult respondSuccess(const typed::TypedServerRequest& request) {
            return std::visit(
                [this](const auto& typedRequest) -> codex::SendResult {
                    using Request = std::decay_t<decltype(typedRequest)>;
                    if constexpr (std::is_same_v<Request, typed::ApplyPatchApprovalRequest>) {
                        return client.requests().respond(typedRequest, typed::ApplyPatchApprovalResponse{typed::DeniedReviewDecision{}});
                    } else if constexpr (std::is_same_v<Request, typed::ExecCommandApprovalRequest>) {
                        return client.requests().respond(typedRequest, typed::ExecCommandApprovalResponse{typed::TimedOutReviewDecision{}});
                    } else if constexpr (std::is_same_v<Request, typed::CommandApprovalRequest>) {
                        return client.requests().respond(
                            typedRequest, typed::CommandExecutionRequestApprovalResponse{typed::DeclineCommandExecutionApprovalDecision{}});
                    } else if constexpr (std::is_same_v<Request, typed::FileChangeApprovalRequest>) {
                        return client.requests().respond(
                            typedRequest, typed::FileChangeRequestApprovalResponse{typed::FileChangeApprovalDecision::cancel()});
                    } else if constexpr (std::is_same_v<Request, typed::PermissionsApprovalRequest>) {
                        return client.requests().respond(typedRequest, permissionResponse());
                    } else if constexpr (std::is_same_v<Request, typed::AttestationGenerateRequest>) {
                        return client.requests().respond(typedRequest, attestationResponse());
                    } else if constexpr (std::is_same_v<Request, typed::DynamicToolCallRequest>) {
                        return client.requests().respond(typedRequest, dynamicToolResponse());
                    } else if constexpr (std::is_same_v<Request, typed::UserInputRequest>) {
                        return client.requests().respond(typedRequest, userInputResponse());
                    } else if constexpr (std::is_same_v<Request, typed::McpServerElicitationRequest>) {
                        return client.requests().respond(typedRequest, elicitationResponse());
                    } else {
                        return {false,
                                codex::Error{codex::Error::Category::Protocol,
                                             EINVAL,
                                             "unexpected server-request alternative in nine-request test"}};
                    }
                },
                request);
        }

        codex::SendResult rejectNew(const typed::TypedServerRequest& request) {
            return std::visit(
                [this](const auto& typedRequest) -> codex::SendResult {
                    using Request = std::decay_t<decltype(typedRequest)>;
                    if constexpr (std::is_same_v<Request, typed::AttestationGenerateRequest> ||
                                  std::is_same_v<Request, typed::DynamicToolCallRequest> ||
                                  std::is_same_v<Request, typed::UserInputRequest> ||
                                  std::is_same_v<Request, typed::McpServerElicitationRequest>) {
                        return client.requests().reject(typedRequest,
                                                        codex::ProtocolError{-32'140, "synthetic reverse request rejection", std::nullopt});
                    } else {
                        return respondSuccess(typed::TypedServerRequest{typedRequest});
                    }
                },
                request);
        }

        bool validateRequest(const typed::TypedServerRequest& request, std::size_t index) {
            static const std::vector<codex::Json> Ids{
                101, "request-exec", 103, "request-file", 105, "request-attestation", 107, "request-user-input", 109};
            static const std::vector<std::string> Methods{"applyPatchApproval",
                                                          "execCommandApproval",
                                                          "item/commandExecution/requestApproval",
                                                          "item/fileChange/requestApproval",
                                                          "item/permissions/requestApproval",
                                                          "attestation/generate",
                                                          "item/tool/call",
                                                          "item/tool/requestUserInput",
                                                          "mcpServer/elicitation/request"};
            static const std::vector<std::size_t> Indices{5, 6, 0, 1, 7, 8, 9, 2, 10};
            if (index >= RequestCount || requestIdJson(request) != Ids[index] || requestMethod(request) != Methods[index] ||
                request.index() != Indices[index]) {
                return false;
            }

            return std::visit(
                [](const auto& typedRequest) {
                    using Request = std::decay_t<decltype(typedRequest)>;
                    if constexpr (std::is_same_v<Request, typed::ApplyPatchApprovalRequest>) {
                        return typedRequest.params.callId.value == "call-patch";
                    } else if constexpr (std::is_same_v<Request, typed::ExecCommandApprovalRequest>) {
                        return typedRequest.params.callId.value == "call-exec";
                    } else if constexpr (std::is_same_v<Request, typed::CommandApprovalRequest>) {
                        return typedRequest.canonicalParams.itemId.value == "item-command";
                    } else if constexpr (std::is_same_v<Request, typed::FileChangeApprovalRequest>) {
                        return typedRequest.canonicalParams.itemId.value == "item-file";
                    } else if constexpr (std::is_same_v<Request, typed::PermissionsApprovalRequest>) {
                        return typedRequest.params.itemId.value == "item-permission";
                    } else if constexpr (std::is_same_v<Request, typed::AttestationGenerateRequest>) {
                        return typedRequest.params.raw.contains("futureAttestationField");
                    } else if constexpr (std::is_same_v<Request, typed::DynamicToolCallRequest>) {
                        return typedRequest.params.callId.value == "call-dynamic" && typedRequest.params.nameSpace.isNull() &&
                               typedRequest.params.arguments.value("count", 0) == 2;
                    } else if constexpr (std::is_same_v<Request, typed::UserInputRequest>) {
                        return typedRequest.itemId.value == "item-user" && typedRequest.questions.size() == 2 &&
                               typedRequest.canonicalParams.autoResolutionMs.isNull();
                    } else if constexpr (std::is_same_v<Request, typed::McpServerElicitationRequest>) {
                        return typedRequest.params.raw.value("mode", "") == "form" &&
                               typedRequest.params.raw.value("serverName", "") == "synthetic-mcp";
                    }
                    return false;
                },
                request);
        }

        void validateConcurrentOccurrences(const std::vector<typed::TypedServerRequest>& requests, std::string_view phaseName) {
            std::set<std::uint64_t> tokens;
            std::size_t exact = 0;
            for (std::size_t index = 0; index < requests.size(); ++index) {
                exact += validateRequest(requests[index], index) ? 1U : 0U;
                tokens.insert(requestToken(requests[index]).value());
            }
            expect(exact == RequestCount, std::string(phaseName) + " decodes all nine exact typed alternatives and mixed IDs");
            expect(tokens.size() == RequestCount && !tokens.contains(0),
                   std::string(phaseName) + " assigns nine distinct nonzero occurrence tokens");
        }

        void submitReentrantMcpRequest() {
            typed::ListMcpServerStatusParams params;
            const auto submission = client.mcp().listServers(
                std::move(params), [this](const typed::OperationResult<typed::ListMcpServerStatusResponse>& operation) {
                    mcpCompletionInline = insideRequestCallback;
                    mcpCompletion = operation && operation.value->data.empty() && operation.value->nextCursor.isNull();
                });
            reentrantSubmission = insideRequestCallback && submission && submission.id && !submission.error;
        }

        void rejectMalformedNewResponses() {
            std::size_t rejected = 0;
            for (const typed::TypedServerRequest& request : initialRequests) {
                const codex::SendResult invalid = std::visit(
                    [this](const auto& typedRequest) -> codex::SendResult {
                        using Request = std::decay_t<decltype(typedRequest)>;
                        if constexpr (std::is_same_v<Request, typed::AttestationGenerateRequest>) {
                            typed::AttestationGenerateResponse response = attestationResponse();
                            response.raw = nullptr;
                            return client.requests().respond(typedRequest, std::move(response));
                        } else if constexpr (std::is_same_v<Request, typed::DynamicToolCallRequest>) {
                            typed::DynamicToolCallResponse response = dynamicToolResponse();
                            response.raw = nullptr;
                            return client.requests().respond(typedRequest, std::move(response));
                        } else if constexpr (std::is_same_v<Request, typed::UserInputRequest>) {
                            typed::ToolRequestUserInputResponse response = userInputResponse();
                            response.raw = nullptr;
                            return client.requests().respond(typedRequest, std::move(response));
                        } else if constexpr (std::is_same_v<Request, typed::McpServerElicitationRequest>) {
                            typed::McpServerElicitationRequestResponse response = elicitationResponse();
                            response.action.value = "future-action";
                            return client.requests().respond(typedRequest, std::move(response));
                        } else {
                            return {true, std::nullopt};
                        }
                    },
                    request);
                if (!invalid && invalid.error && invalid.error->category == codex::Error::Category::Protocol) {
                    ++rejected;
                }
            }
            expect(rejected == 4, "all four malformed successful reverse-request responses are rejected locally without retirement");
        }

        void respondInitial() {
            validateConcurrentOccurrences(initialRequests, "initial stdio wave");
            submitReentrantMcpRequest();

            std::size_t staleRejected = 0;
            std::size_t wrongOwnerRejected = 0;
            for (std::size_t index = 0; index < RequestCount; ++index) {
                typed::TypedServerRequest stale = initialRequests[index];
                clearToken(stale);
                const auto staleResult = respondSuccess(stale);
                staleRejected +=
                    !staleResult && staleResult.error && staleResult.error->category == codex::Error::Category::InvalidState ? 1U : 0U;

                typed::TypedServerRequest wrongOwner = initialRequests[index];
                replaceOwnership(wrongOwner, initialRequests[(index + 1) % RequestCount]);
                const auto wrongResult = respondSuccess(wrongOwner);
                wrongOwnerRejected +=
                    !wrongResult && wrongResult.error && wrongResult.error->category == codex::Error::Category::InvalidState ? 1U : 0U;
            }
            expect(staleRejected == RequestCount, "all nine zero/stale occurrence tokens are rejected");
            expect(wrongOwnerRejected == RequestCount, "no typed response can target a different pending request occurrence");

            rejectMalformedNewResponses();

            std::size_t accepted = 0;
            for (auto iterator = initialRequests.rbegin(); iterator != initialRequests.rend(); ++iterator) {
                accepted += respondSuccess(*iterator) ? 1U : 0U;
            }
            responsesInsideCallback = insideRequestCallback;
            expect(accepted == RequestCount, "all nine exact successful responses enqueue out of order from the typed callback");

            std::size_t duplicateRejected = 0;
            for (const typed::TypedServerRequest& request : initialRequests) {
                const auto duplicate = respondSuccess(request);
                duplicateRejected +=
                    !duplicate && duplicate.error && duplicate.error->category == codex::Error::Category::InvalidState ? 1U : 0U;
            }
            expect(duplicateRejected == RequestCount, "every duplicate response is rejected after exactly one terminal response");
            phase = Phase::DisconnectPending;
        }

        void respondReconnected() {
            validateConcurrentOccurrences(currentRequests, "reconnected stdio wave");
            expect(disconnectRequests.size() == RequestCount, "the disconnected generation retained all nine application values");
            if (disconnectRequests.size() != RequestCount) {
                client.stop();
                return;
            }

            std::size_t changedTokens = 0;
            std::size_t staleGenerationRejected = 0;
            for (std::size_t index = 0; index < RequestCount; ++index) {
                changedTokens += requestToken(disconnectRequests[index]) != requestToken(currentRequests[index]) ? 1U : 0U;
                const auto stale = respondSuccess(disconnectRequests[index]);
                staleGenerationRejected +=
                    !stale && stale.error && stale.error->category == codex::Error::Category::InvalidState && stale.error->code == ESTALE
                        ? 1U
                        : 0U;
            }
            expect(changedTokens == RequestCount, "reconnect assigns new tokens to all nine reused JSON-RPC IDs");
            expect(staleGenerationRejected == RequestCount, "all nine stale-generation response attempts are rejected with ESTALE");

            std::size_t accepted = 0;
            for (auto iterator = currentRequests.rbegin(); iterator != currentRequests.rend(); ++iterator) {
                accepted += rejectNew(*iterator) ? 1U : 0U;
            }
            expect(accepted == RequestCount,
                   "the reconnected wave emits exact errors for four new requests and exact results for five approvals");
            phase = Phase::ShutdownPending;
        }

        void handleRequest(const typed::TypedServerRequest& request) {
            switch (phase) {
                case Phase::Initial:
                    initialRequests.push_back(request);
                    if (initialRequests.size() == RequestCount) {
                        respondInitial();
                    }
                    if (initialRequests.size() == 1 || initialRequests.size() >= 6) {
                        ++callbackExceptionsInjected;
                        throw std::runtime_error("intentional nine-request callback exception");
                    }
                    return;
                case Phase::DisconnectPending:
                    disconnectRequests.push_back(request);
                    if (disconnectRequests.size() == RequestCount) {
                        validateConcurrentOccurrences(disconnectRequests, "same-generation ID-reuse wave");
                        const auto acknowledgement = client.raw().notify("test/a14-nine-ready-to-disconnect");
                        disconnectAcknowledged = static_cast<bool>(acknowledgement);
                    }
                    return;
                case Phase::Reconnected:
                    currentRequests.push_back(request);
                    if (currentRequests.size() == RequestCount) {
                        respondReconnected();
                    }
                    return;
                case Phase::ShutdownPending:
                    shutdownRequests.push_back(request);
                    if (shutdownRequests.size() == RequestCount) {
                        validateConcurrentOccurrences(shutdownRequests, "explicit-shutdown stdio wave");
                        client.stop();
                    }
                    return;
            }
        }

        void handleState(const codex::StateChange& change) {
            if (change.current == codex::State::Ready) {
                ++readyCount;
                return;
            }
            if (change.current == codex::State::Failed) {
                ++failureCount;
                expect(failureCount == 1 && phase == Phase::DisconnectPending && disconnectRequests.size() == RequestCount,
                       "the first child exits only after all nine disconnect occurrences are pending");
                client.stop();
                return;
            }
            if (change.current != codex::State::Stopped) {
                return;
            }

            ++stoppedCount;
            if (stoppedCount == 1) {
                std::size_t cleaned = 0;
                for (const typed::TypedServerRequest& request : disconnectRequests) {
                    const auto response = respondSuccess(request);
                    cleaned += !response && response.error && response.error->category == codex::Error::Category::InvalidState ? 1U : 0U;
                }
                disconnectCleanup = cleaned == RequestCount;
                phase = Phase::Reconnected;
                client.start();
                return;
            }

            std::size_t cleaned = 0;
            for (const typed::TypedServerRequest& request : shutdownRequests) {
                const auto response = respondSuccess(request);
                cleaned += !response && response.error && response.error->category == codex::Error::Category::InvalidState ? 1U : 0U;
            }
            shutdownCleanup = cleaned == RequestCount;
            expect(readyCount == 2 && failureCount == 1 && stoppedCount == 2,
                   "one unexpected disconnect and one explicit shutdown span exactly two stdio generations");
            expect(callbackExceptionsInjected == 5 && responsesInsideCallback,
                   "callback exceptions from every new request kind are contained and later callbacks answer reentrantly");
            expect(reentrantSubmission && mcpCompletion && !mcpCompletionInline,
                   "a typed MCP client request submitted by a reverse-request callback completes asynchronously");
            expect(disconnectAcknowledged && disconnectCleanup,
                   "all nine pending occurrences are observed before disconnect and retired during disconnect cleanup");
            expect(shutdownCleanup, "explicit shutdown retires all nine pending occurrences exactly once");
            finished = true;
            core::SNodeC::stop();
        }

        tests::support::TestResult& result;
        std::string markerPath;
        codex::stdio::Client client;
        Phase phase = Phase::Initial;
        std::vector<typed::TypedServerRequest> initialRequests;
        std::vector<typed::TypedServerRequest> disconnectRequests;
        std::vector<typed::TypedServerRequest> currentRequests;
        std::vector<typed::TypedServerRequest> shutdownRequests;
        bool insideRequestCallback = false;
        std::size_t callbackExceptionsInjected = 0;
        bool reentrantSubmission = false;
        bool mcpCompletion = false;
        bool mcpCompletionInline = false;
        bool responsesInsideCallback = false;
        bool disconnectAcknowledged = false;
        bool disconnectCleanup = false;
        bool shutdownCleanup = false;
        bool finished = false;
        int readyCount = 0;
        int failureCount = 0;
        int stoppedCount = 0;
    };
} // namespace

int main([[maybe_unused]] int argc, char* argv[]) {
    tests::support::TestResult result;
    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexA14NineRequestStdioTest");
        return tests::support::cTestSkipReturnCode;
    }

    std::string markerTemplate = (std::filesystem::temp_directory_path() / "snodec-codex-a14-nine-XXXXXX").string();
    const int reservedMarker = ::mkstemp(markerTemplate.data());
    result.expectTrue(reservedMarker >= 0, "nine-request stdio test reserves a unique generation marker");
    if (reservedMarker < 0) {
        return result.processResult();
    }
    ::close(reservedMarker);
    if (::unlink(markerTemplate.c_str()) != 0) {
        result.expectTrue(false, "nine-request generation marker is absent before the first child launch");
        return result.processResult();
    }

    char* snodeArguments[] = {argv[0], nullptr};
    core::SNodeC::init(1, snodeArguments);
    NineRequestRunner runner(result, markerTemplate);
    runner.start();

    bool timedOut = false;
    [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
        [&]() {
            timedOut = true;
            core::SNodeC::stop();
        },
        utils::Timeval({15, 0}));
    const int startResult = core::SNodeC::start(utils::Timeval({16, 0}));
    const bool markerRemoved = ::unlink(markerTemplate.c_str()) == 0;

    result.expectTrue(!timedOut && runner.isFinished(), "nine-request production stdio lifecycle completes before the watchdog");
    result.expectEqual(0, startResult, "nine-request production stdio lifecycle stops the event loop cleanly");
    result.expectTrue(markerRemoved, "nine-request generation marker is removed after both child launches");
    core::SNodeC::free();
    return result.processResult();
}
