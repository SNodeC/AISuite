/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/Api.h"
#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
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
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace codex = ai::openai::codex;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    codex::Json requestIdJson(const codex::ServerRequestId& requestId) {
        return std::visit(
            [](const auto& value) -> codex::Json {
                return value;
            },
            requestId.value());
    }

    std::string requestMethod(const typed::TypedServerRequest& request) {
        return std::visit(
            [](const auto& value) {
                return value.raw.value("method", "");
            },
            request);
    }

    void clearRequestToken(typed::TypedServerRequest& request) {
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

    class ResolvedLifecycleRunner {
    public:
        ResolvedLifecycleRunner(tests::support::TestResult& result, std::string markerPath)
            : result(result)
            , markerPath(std::move(markerPath))
            , client(CODEX_FAKE_APP_SERVER, {"a14-server-request-resolved", this->markerPath}) {
        }

        void start() {
            const bool matrix =
                detail::serverRequestEmitsResolvedNotification(detail::ServerRequestTarget::CommandExecutionRequestApproval) &&
                detail::serverRequestEmitsResolvedNotification(detail::ServerRequestTarget::FileChangeRequestApproval) &&
                detail::serverRequestEmitsResolvedNotification(detail::ServerRequestTarget::PermissionsRequestApproval) &&
                detail::serverRequestEmitsResolvedNotification(detail::ServerRequestTarget::ToolRequestUserInput) &&
                detail::serverRequestEmitsResolvedNotification(detail::ServerRequestTarget::McpServerElicitation) &&
                !detail::serverRequestEmitsResolvedNotification(detail::ServerRequestTarget::ApplyPatchApproval) &&
                !detail::serverRequestEmitsResolvedNotification(detail::ServerRequestTarget::ExecCommandApproval) &&
                !detail::serverRequestEmitsResolvedNotification(detail::ServerRequestTarget::DynamicToolCall) &&
                !detail::serverRequestEmitsResolvedNotification(detail::ServerRequestTarget::AttestationGenerate);
            expect(matrix, "the authoritative occurrence metadata helper retains the exact five-positive/four-negative matrix");

            client.requests().setOnRequest([this](const typed::TypedServerRequest& request) {
                handleRequest(request);
            });
            client.events().setOnEvent([this](const typed::Event& event) {
                insideTypedEvent = true;
                struct Reset {
                    bool& flag;
                    ~Reset() {
                        flag = false;
                    }
                } reset{insideTypedEvent};
                handleTypedEvent(event);
            });
            client.raw().setOnNotification([this](const codex::Notification& notification) {
                handleRawNotification(notification);
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
        enum class Phase { FirstGeneration, Restarting, SecondGeneration, Complete };

        struct ExpectedResolved {
            codex::Json requestId;
            std::string threadId;
        };

        static const std::vector<ExpectedResolved>& expectedResolved() {
            static const std::vector<ExpectedResolved> Expected{
                {105, "thread-permission"},
                {103, "thread-wrong"},
                {999, "thread-unknown"},
                {103, "thread-command"},
                {103, "thread-command"},
                {"request-file", "thread-file"},
                {"request-user-input", "thread-user"},
                {109, "thread-elicitation"},
                {101, "thread-negative-apply"},
                {"request-exec", "thread-negative-exec"},
                {"request-attestation", "thread-negative-attestation"},
                {107, "thread-dynamic"},
                {103, "thread-command"},
                {103, "thread-command"},
            };
            return Expected;
        }

        void expect(bool condition, std::string message) {
            result.expectTrue(condition, std::move(message));
        }

        codex::SendResult respondSuccess(const typed::TypedServerRequest& request) {
            return std::visit(
                [this](const auto& value) -> codex::SendResult {
                    using Request = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Request, typed::ApplyPatchApprovalRequest>) {
                        return client.requests().respond(value, typed::ApplyPatchApprovalResponse{typed::DeniedReviewDecision{}});
                    } else if constexpr (std::is_same_v<Request, typed::ExecCommandApprovalRequest>) {
                        return client.requests().respond(value, typed::ExecCommandApprovalResponse{typed::TimedOutReviewDecision{}});
                    } else if constexpr (std::is_same_v<Request, typed::CommandApprovalRequest>) {
                        return client.requests().respond(
                            value, typed::CommandExecutionRequestApprovalResponse{typed::DeclineCommandExecutionApprovalDecision{}});
                    } else if constexpr (std::is_same_v<Request, typed::FileChangeApprovalRequest>) {
                        return client.requests().respond(
                            value, typed::FileChangeRequestApprovalResponse{typed::FileChangeApprovalDecision::cancel()});
                    } else if constexpr (std::is_same_v<Request, typed::PermissionsApprovalRequest>) {
                        return client.requests().respond(value, permissionResponse());
                    } else if constexpr (std::is_same_v<Request, typed::AttestationGenerateRequest>) {
                        return client.requests().respond(value, typed::AttestationGenerateResponse{"synthetic-attestation-token"});
                    } else if constexpr (std::is_same_v<Request, typed::DynamicToolCallRequest>) {
                        return client.requests().respond(value, dynamicToolResponse());
                    } else if constexpr (std::is_same_v<Request, typed::UserInputRequest>) {
                        return client.requests().respond(value, userInputResponse());
                    } else if constexpr (std::is_same_v<Request, typed::McpServerElicitationRequest>) {
                        return client.requests().respond(value, elicitationResponse());
                    } else {
                        return {false,
                                codex::Error{codex::Error::Category::Protocol, EINVAL, "unexpected request in resolved lifecycle test"}};
                    }
                },
                request);
        }

        const typed::TypedServerRequest* initialRequest(const std::string& method) const {
            const auto request = firstGenerationRequests.find(method);
            return request == firstGenerationRequests.end() ? nullptr : &request->second;
        }

        void expectLocallyRetired(const typed::TypedServerRequest* request, std::string description) {
            if (request == nullptr) {
                expect(false, std::move(description));
                return;
            }
            const auto response = respondSuccess(*request);
            const bool rejected = !response && response.error && response.error->category == codex::Error::Category::InvalidState;
            expect(rejected, std::move(description));
            externallyRetiredAttempts += rejected ? 1U : 0U;
        }

        void submitReentrantClientRequest() {
            typed::ListMcpServerStatusParams params;
            const auto submission = client.mcp().listServers(
                std::move(params), [this](const typed::OperationResult<typed::ListMcpServerStatusResponse>& operation) {
                    mcpCompletionInline = insideTypedEvent;
                    mcpCompleted = operation && operation.value->data.empty() && operation.value->nextCursor.isNull();
                });
            reentrantSubmissionAccepted = insideTypedEvent && static_cast<bool>(submission);
        }

        void handleRequest(const typed::TypedServerRequest& request) {
            if (phase == Phase::FirstGeneration) {
                firstGenerationRequests.emplace(requestMethod(request), request);
                if (firstGenerationRequests.size() != 9) {
                    return;
                }

                const auto permission = initialRequest("item/permissions/requestApproval");
                const auto command = initialRequest("item/commandExecution/requestApproval");
                if (command) {
                    oldCommand = *command;
                }
                directResponseAccepted = permission && static_cast<bool>(respondSuccess(*permission));
                expect(directResponseAccepted, "a direct response succeeds independently before serverRequest/resolved is received");
                const auto ready = client.raw().notify("test/a14-resolved-ready");
                expect(static_cast<bool>(ready), "the first real-stdio request wave is acknowledged without polling");
                return;
            }

            if (phase != Phase::SecondGeneration || requestMethod(request) != "item/commandExecution/requestApproval") {
                expect(false, "only the reused command request is delivered in the second transport generation");
                return;
            }

            currentCommand = request;
            const auto stale = oldCommand ? respondSuccess(*oldCommand) : codex::SendResult{};
            staleTokenRejected =
                !stale && stale.error && stale.error->category == codex::Error::Category::InvalidState && stale.error->code == ESTALE;
            expect(staleTokenRejected, "the old generation token cannot answer the reused JSON-RPC ID in the current generation");
            const auto ready = client.raw().notify("test/a14-resolved-ready");
            expect(static_cast<bool>(ready), "the reused-ID request is acknowledged after stale ownership is rejected");
        }

        void handleTypedEvent(const typed::Event& event) {
            const auto* resolved = std::get_if<typed::ServerRequestResolvedNotification>(&event);
            if (resolved == nullptr) {
                return;
            }
            const auto& expected = expectedResolved();
            expect(nextResolved < expected.size(), "resolved notifications remain within the deterministic test sequence");
            if (nextResolved >= expected.size()) {
                return;
            }

            const ExpectedResolved& step = expected[nextResolved];
            expect(requestIdJson(resolved->requestId) == step.requestId && resolved->threadId.value == step.threadId,
                   "typed serverRequest/resolved preserves the exact integer/string ID and thread");
            typedDeliveredForStep = true;
            ++typedEventCount;

            switch (nextResolved) {
                case 0:
                    expectLocallyRetired(initialRequest("item/permissions/requestApproval"),
                                         "a notification after a direct response is an idempotent no-op");
                    positiveMethods.insert("item/permissions/requestApproval");
                    break;
                case 1:
                    if (const auto* command = initialRequest("item/commandExecution/requestApproval")) {
                        typed::TypedServerRequest staleOwner = *command;
                        clearRequestToken(staleOwner);
                        const auto probe = respondSuccess(staleOwner);
                        wrongThreadPreserved = !probe && probe.error && probe.error->category == codex::Error::Category::InvalidState &&
                                               probe.error->code == ESTALE;
                    }
                    expect(wrongThreadPreserved,
                           "a wrong-thread notification leaves the exact command occurrence pending without consuming it");
                    break;
                case 2:
                    ++callbackExceptions;
                    throw std::runtime_error("intentional resolved-event callback exception");
                case 3:
                    expectLocallyRetired(initialRequest("item/commandExecution/requestApproval"),
                                         "a matching command occurrence retires before the typed callback");
                    positiveMethods.insert("item/commandExecution/requestApproval");
                    submitReentrantClientRequest();
                    break;
                case 4:
                    expectLocallyRetired(initialRequest("item/commandExecution/requestApproval"),
                                         "a duplicate resolved notification cannot reopen a retired occurrence");
                    duplicateObserved = true;
                    break;
                case 5:
                    expectLocallyRetired(initialRequest("item/fileChange/requestApproval"),
                                         "a matching string-ID file occurrence retires externally");
                    positiveMethods.insert("item/fileChange/requestApproval");
                    break;
                case 6:
                    expectLocallyRetired(initialRequest("item/tool/requestUserInput"),
                                         "a matching user-input occurrence retires externally");
                    positiveMethods.insert("item/tool/requestUserInput");
                    break;
                case 7:
                    expectLocallyRetired(initialRequest("mcpServer/elicitation/request"),
                                         "a matching MCP elicitation occurrence retires externally");
                    positiveMethods.insert("mcpServer/elicitation/request");
                    break;
                case 8:
                    acceptNegativeTarget("applyPatchApproval");
                    break;
                case 9:
                    acceptNegativeTarget("execCommandApproval");
                    break;
                case 10:
                    acceptNegativeTarget("attestation/generate");
                    break;
                case 11:
                    acceptNegativeTarget("item/tool/call");
                    break;
                case 12:
                    staleNotificationBeforeReuse = true;
                    break;
                case 13:
                    expectLocallyRetired(currentCommand ? &*currentCommand : nullptr,
                                         "the current-generation reused-ID occurrence retires exactly once");
                    break;
                default:
                    break;
            }
        }

        void acceptNegativeTarget(const std::string& method) {
            const typed::TypedServerRequest* request = initialRequest(method);
            const bool accepted = request && static_cast<bool>(respondSuccess(*request));
            expect(accepted, "serverRequest/resolved does not retire negative target " + method);
            if (accepted) {
                negativeMethods.insert(method);
                ++negativeResponsesAccepted;
            }
        }

        void handleRawNotification(const codex::Notification& notification) {
            if (notification.method != "serverRequest/resolved") {
                return;
            }
            expect(typedDeliveredForStep && typedEventCount == rawEventCount + 1,
                   "serverRequest/resolved retains typed-before-raw observer delivery even after callback exceptions");
            typedDeliveredForStep = false;
            ++rawEventCount;

            const auto acknowledgement = client.raw().notify("test/a14-resolved-step", {{"index", nextResolved}});
            expect(static_cast<bool>(acknowledgement),
                   "each resolved lifecycle step acknowledges through the existing non-blocking transport");
            ++nextResolved;

            if (nextResolved == expectedResolved().size()) {
                phase = Phase::Complete;
                client.stop();
            }
        }

        void handleState(const codex::StateChange& change) {
            if (change.current == codex::State::Ready) {
                ++readyCount;
                return;
            }
            if (change.current == codex::State::Failed) {
                ++failureCount;
                expect(phase == Phase::FirstGeneration && nextResolved == 12,
                       "the first fake child exits after the complete first-generation resolution matrix");
                phase = Phase::Restarting;
                client.stop();
                return;
            }
            if (change.current != codex::State::Stopped) {
                return;
            }

            ++stoppedCount;
            if (phase == Phase::Restarting) {
                phase = Phase::SecondGeneration;
                client.start();
                return;
            }
            if (phase != Phase::Complete) {
                return;
            }

            expect(positiveMethods == std::set<std::string>{"item/commandExecution/requestApproval",
                                                            "item/fileChange/requestApproval",
                                                            "item/permissions/requestApproval",
                                                            "item/tool/requestUserInput",
                                                            "mcpServer/elicitation/request"},
                   "all five positive serverRequest/resolved targets are exercised");
            expect(negativeMethods ==
                       std::set<std::string>{"applyPatchApproval", "attestation/generate", "execCommandApproval", "item/tool/call"},
                   "all four negative serverRequest/resolved targets remain directly answerable");
            expect(directResponseAccepted && externallyRetiredAttempts == 7 && negativeResponsesAccepted == 4,
                   "direct, external, duplicate, and negative terminal transitions remain one-shot");
            expect(wrongThreadPreserved && duplicateObserved && staleNotificationBeforeReuse && staleTokenRejected,
                   "wrong-thread, duplicate, stale/no-candidate, and reconnect reuse cases are nonfatal and isolated");
            expect(reentrantSubmissionAccepted && mcpCompleted && !mcpCompletionInline,
                   "a resolved-event callback can submit a typed client request with asynchronous completion");
            expect(callbackExceptions == 1 && typedEventCount == rawEventCount && rawEventCount == expectedResolved().size(),
                   "callback exceptions are contained while every typed and raw resolved event remains observable");
            expect(readyCount == 2 && failureCount == 1 && stoppedCount == 2,
                   "the real stdio lifecycle spans one disconnect and one clean restarted shutdown");
            finished = true;
            core::SNodeC::stop();
        }

        tests::support::TestResult& result;
        std::string markerPath;
        codex::stdio::Client client;
        Phase phase = Phase::FirstGeneration;
        std::map<std::string, typed::TypedServerRequest> firstGenerationRequests;
        std::optional<typed::TypedServerRequest> oldCommand;
        std::optional<typed::TypedServerRequest> currentCommand;
        std::set<std::string> positiveMethods;
        std::set<std::string> negativeMethods;
        std::size_t nextResolved = 0;
        std::size_t typedEventCount = 0;
        std::size_t rawEventCount = 0;
        std::size_t externallyRetiredAttempts = 0;
        std::size_t negativeResponsesAccepted = 0;
        std::size_t callbackExceptions = 0;
        bool typedDeliveredForStep = false;
        bool insideTypedEvent = false;
        bool directResponseAccepted = false;
        bool wrongThreadPreserved = false;
        bool duplicateObserved = false;
        bool staleNotificationBeforeReuse = false;
        bool staleTokenRejected = false;
        bool reentrantSubmissionAccepted = false;
        bool mcpCompleted = false;
        bool mcpCompletionInline = false;
        bool finished = false;
        int readyCount = 0;
        int failureCount = 0;
        int stoppedCount = 0;
    };
} // namespace

int main([[maybe_unused]] int argc, char* argv[]) {
    tests::support::TestResult result;
    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexA14ServerRequestResolvedLifecycleTest");
        return tests::support::cTestSkipReturnCode;
    }

    std::string markerTemplate = (std::filesystem::temp_directory_path() / "snodec-codex-a14-resolved-XXXXXX").string();
    const int reservedMarker = ::mkstemp(markerTemplate.data());
    result.expectTrue(reservedMarker >= 0, "resolved lifecycle test reserves a unique generation marker");
    if (reservedMarker < 0) {
        return result.processResult();
    }
    ::close(reservedMarker);
    if (::unlink(markerTemplate.c_str()) != 0) {
        result.expectTrue(false, "resolved lifecycle marker is absent before the first child launch");
        return result.processResult();
    }

    char* snodeArguments[] = {argv[0], nullptr};
    core::SNodeC::init(1, snodeArguments);
    ResolvedLifecycleRunner runner(result, markerTemplate);
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

    result.expectTrue(!timedOut && runner.isFinished(),
                      "serverRequest/resolved real-stdio lifecycle completes without polling or wall-clock races");
    result.expectEqual(0, startResult, "serverRequest/resolved lifecycle stops the event loop cleanly");
    result.expectTrue(markerRemoved, "serverRequest/resolved generation marker is removed after both child launches");
    core::SNodeC::free();
    return result.processResult();
}
