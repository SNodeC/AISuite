/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "support/TestResult.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef CODEX_FRONTEND_GOLDEN_FIXTURE
#define CODEX_FRONTEND_GOLDEN_FIXTURE "tests/component/codex/fixtures/frontend-protocol-v1.generated.json"
#endif

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace core = ai::openai::codex::frontend::internal::client;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace generated = ai::openai::codex::frontend::generated;

    core::ClientOptions clientOptions() {
        core::ClientOptions options;
        options.credentialProvider = [] {
            return core::AuthenticationContext{frontend::NoCredential{}, std::nullopt};
        };
        return options;
    }

    const frontend::Json& fixtures() {
        static const frontend::Json value = [] {
            std::ifstream stream(CODEX_FRONTEND_GOLDEN_FIXTURE);
            if (!stream) {
                throw std::runtime_error("unable to open generated frontend protocol fixture");
            }
            return frontend::Json::parse(stream);
        }();
        return value;
    }

    const frontend::Json* fixtureFor(generated::MethodId method) {
        const auto found = std::find_if(fixtures().at("methods").begin(),
                                        fixtures().at("methods").end(),
                                        [method](const frontend::Json& fixture) {
                                            return generated::definedMethodFromString(fixture.at("method").get<std::string>()) == method;
                                        });
        return found == fixtures().at("methods").end() ? nullptr : &*found;
    }

    struct Harness {
        struct Sent {
            bool hello = false;
            bool command = false;
            bool sensitive = false;
            std::optional<generated::MethodId> method;
            std::optional<std::string> requestId;
        };

        std::vector<Sent> outbound;
        std::vector<std::string> closeReasons;
        core::SendStatus status = core::SendStatus::Accepted;
        bool retryable = false;
        bool throwOnCommand = false;
        std::function<void(const core::OutboundMessage&)> onCommandSend;
        core::TransportCallbacks transport() {
            return {[this](core::OutboundMessage message) {
                        Sent sent;
                        sent.hello = message.isHello();
                        sent.command = message.isCommand();
                        sent.sensitive = message.sensitive;
                        if (const auto* command = std::get_if<generated::DefinedCommand>(&message.value)) {
                            sent.method = generated::commandMethod(command->parameters);
                            sent.requestId = command->requestId;
                        }
                        if (sent.command && onCommandSend) {
                            onCommandSend(message);
                        }
                        outbound.push_back(std::move(sent));
                        if (sent.command && throwOnCommand) {
                            throw std::runtime_error("send sentinel");
                        }
                        return core::SendResult{
                            status,
                            status == core::SendStatus::Accepted
                                ? std::nullopt
                                : std::optional<core::TransportError>{core::TransportError{"send rejected", retryable}}};
                    },
                    [this](std::string_view reason) {
                        closeReasons.emplace_back(reason);
                    }};
        }
    };

    std::vector<frontend::FrontendMethod> methods() {
        std::vector<frontend::FrontendMethod> result;
        for (const auto& metadata : generated::AllMethods) {
            result.emplace_back(metadata.method);
        }
        return result;
    }

    frontend::CapabilityAdvertisement capabilities() {
        std::vector<frontend::FrontendCapability> defined;
        for (const auto& metadata : generated::AllCapabilities) {
            if (metadata.defined) {
                defined.push_back(static_cast<frontend::FrontendCapability>(metadata.id));
            }
        }
        return {defined, defined, defined, frontend::Json::object()};
    }

    frontend::Snapshot emptySnapshot() {
        model::CanonicalSnapshot state;
        const auto encoded = frontend::Codec::encodeExpandedSnapshot(model::encodeSnapshot(state).value());
        return {frontend::SequenceNumber(0), encoded.value().at("state")};
    }

    frontend::Welcome welcome(std::string sessionId = "operation-session") {
        return {std::move(sessionId),
                frontend::SessionRole::Controller,
                frontend::SequenceNumber(0),
                frontend::SyncMode::Snapshot,
                {{"permittedScopes",
                  frontend::Json::array({"observe",
                                         "control",
                                         "provider_lifecycle",
                                         "account_management",
                                         "configuration_write",
                                         "command_execution",
                                         "filesystem_read",
                                         "filesystem_write",
                                         "extension_management",
                                         "mcp_invoke",
                                         "sensitive_response",
                                         "unknown_request_response"})}},
                capabilities(),
                methods(),
                methods()};
    }

    core::PhysicalGeneration ready(core::ClientCore& client, Harness& harness) {
        const core::PhysicalGeneration generation = *client.attach(harness.transport());
        client.transportConnected(generation);
        (void) client.receive(generation, frontend::ServerMessage{welcome()});
        (void) client.receive(generation, frontend::ServerMessage{emptySnapshot()});
        (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
        return generation;
    }

    void testAllGeneratedOperations(tests::support::TestResult& result) {
        bool allAccepted = true;
        bool allCorrelated = true;
        bool allTyped = true;
        std::size_t covered = 0;
        for (const frontend::Json& fixture : fixtures().at("methods")) {
            const auto method = generated::definedMethodFromString(fixture.at("method").get<std::string>());
            if (!method.has_value()) {
                allAccepted = false;
                continue;
            }
            Harness harness;
            core::ClientCore client(clientOptions());
            const core::PhysicalGeneration generation = ready(client, harness);
            std::optional<core::OperationResult> completion;
            const core::Submission submitted = client.submit(generated::makeParameters(*method, fixture.at("minimalParams")),
                                                             [&completion](const core::OperationResult& value) {
                                                                 completion = value;
                                                             });
            const Harness::Sent* command = submitted && !harness.outbound.empty() ? &harness.outbound.back() : nullptr;
            allAccepted = allAccepted && submitted && command != nullptr && command->command && command->method == method;
            allCorrelated = allCorrelated && submitted.requestId == std::optional<std::string>{"c1-r1"} && command != nullptr &&
                            command->requestId == submitted.requestId;
            if (submitted) {
                (void) client.receive(
                    generation, frontend::ServerMessage{frontend::Response::success(*submitted.requestId, fixture.at("minimalResult"))});
                if (*method == generated::MethodId::SnapshotGet) {
                    (void) client.receive(generation, frontend::ServerMessage{emptySnapshot()});
                    (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
                } else if (*method == generated::MethodId::EventsReplay) {
                    (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
                }
            }
            allTyped =
                allTyped && completion.has_value() && completion->succeeded() && generated::commandMethod(*completion->value) == *method;
            ++covered;
        }
        result.expectTrue(allAccepted && allCorrelated && allTyped && covered == generated::MethodCount,
                          "all 105 generated method parameter/result contracts dispatch and correlate through one metadata-driven core");
    }

    void testFailureAndSensitiveReverseResponse(tests::support::TestResult& result) {
        Harness harness;
        core::ClientCore client(clientOptions());
        const core::PhysicalGeneration generation = ready(client, harness);
        std::optional<core::OperationResult> failure;
        const core::Submission ordinary = client.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                                                        [&failure](const auto& value) {
                                                            failure = value;
                                                        });
        (void) client.receive(
            generation,
            frontend::ServerMessage{frontend::Response::failure(
                *ordinary.requestId,
                frontend::CommandError{frontend::ErrorCode::NotFound, "ordinary failure", std::nullopt, frontend::Json::object()})});
        result.expectTrue(failure.has_value() && failure->error.has_value() && failure->error->origin == core::ErrorOrigin::Command &&
                              client.ready(),
                          "an ordinary remote command failure completes only that operation and leaves the client Ready");

        const frontend::Json secretParameters{
            {"pendingRequestId", "1"}, {"accessToken", "TOKEN_SENTINEL"}, {"chatgptAccountId", "account"}};
        const core::Submission reverse =
            client.submit(generated::makeParameters(generated::MethodId::AuthenticationRespond, secretParameters));
        const bool markedSensitive = reverse && harness.outbound.back().sensitive;
        const core::Submission duplicate =
            client.submit(generated::makeParameters(generated::MethodId::AuthenticationRespond, secretParameters));
        result.expectTrue(reverse.accepted(),
                          reverse.error.has_value() ? "reverse response rejected: " + reverse.error->message
                                                    : "a schema-valid reverse response is accepted for transport submission");
        result.expectTrue(markedSensitive, "secret reverse responses are marked sensitive at the transport boundary");
        result.expectTrue(duplicate.accepted(),
                          duplicate.error.has_value() ? "repeated reverse response rejected: " + duplicate.error->message
                                                      : "user-initiated repeated reverse submissions remain server-authoritative");
    }

    void testDuplicateAndUnexpectedResponses(tests::support::TestResult& result) {
        const frontend::Json* modelListFixture = fixtureFor(generated::MethodId::ModelList);
        if (modelListFixture == nullptr) {
            result.expectTrue(false, "the generated model.list fixture is available for response-correlation coverage");
            return;
        }

        auto runCase = [&](bool duplicate) {
            Harness harness;
            std::vector<core::ClientError> observedErrors;
            std::vector<core::StateChange> connectionChanges;
            core::ClientCallbacks callbacks;
            callbacks.onError = [&observedErrors](const core::ClientError& error) {
                observedErrors.push_back(error);
            };
            callbacks.onConnectionStateChanged = [&connectionChanges](const core::StateChange& change) {
                connectionChanges.push_back(change);
            };
            core::ClientCore client(clientOptions(), std::move(callbacks));
            const core::PhysicalGeneration generation = ready(client, harness);

            std::string responseId = "c1-r999";
            if (duplicate) {
                const core::Submission submitted =
                    client.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()));
                if (!submitted) {
                    return false;
                }
                responseId = *submitted.requestId;
                if (!client.receive(generation,
                                    frontend::ServerMessage{frontend::Response::success(
                                        responseId, modelListFixture->at("minimalResult"))}) ||
                    !client.ready()) {
                    return false;
                }
            }

            const bool accepted = client.receive(
                generation,
                frontend::ServerMessage{frontend::Response::success(responseId, modelListFixture->at("minimalResult"))});
            const auto closing = std::find_if(connectionChanges.begin(), connectionChanges.end(), [](const core::StateChange& change) {
                return change.current == core::ConnectionState::Closing;
            });
            return !accepted && client.connectionState() == core::ConnectionState::Disconnected && harness.closeReasons.size() == 1 &&
                   harness.closeReasons.front() == "frontend response correlation failed" && observedErrors.size() == 1 &&
                   observedErrors.front().origin == core::ErrorOrigin::Protocol &&
                   observedErrors.front().clientCode == core::ClientErrorCode::UnexpectedMessage &&
                   !observedErrors.front().protocolCode.has_value() &&
                   observedErrors.front().message == "unsolicited or duplicate frontend response" &&
                   closing != connectionChanges.end() && closing->error.has_value() &&
                   closing->error->clientCode == core::ClientErrorCode::UnexpectedMessage &&
                   !closing->error->protocolCode.has_value();
        };

        result.expectTrue(runCase(true),
                          "a duplicate response closes with the frozen UnexpectedMessage correlation diagnostic");
        result.expectTrue(runCase(false),
                          "an unsolicited response closes with the frozen UnexpectedMessage correlation diagnostic");
    }

    void testMalformedResponseParity(tests::support::TestResult& result) {
        const frontend::Json* modelListFixture = fixtureFor(generated::MethodId::ModelList);
        if (modelListFixture == nullptr) {
            result.expectTrue(false, "the generated model.list fixture is available for malformed-response coverage");
            return;
        }

        const frontend::Json remoteDetails{{"category", "fixture"}};
        Harness failedHarness;
        core::ClientCore failedClient(clientOptions());
        const core::PhysicalGeneration failedGeneration = ready(failedClient, failedHarness);
        std::optional<core::OperationResult> failedCompletion;
        const core::Submission failed =
            failedClient.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                                [&failedCompletion](const core::OperationResult& completion) {
                                    failedCompletion = completion;
                                });
        const bool failedAccepted = failed.requestId.has_value() &&
                                    failedClient.receive(
                                        failedGeneration,
                                        frontend::ServerMessage{frontend::Response::failure(
                                            *failed.requestId,
                                            frontend::CommandError{frontend::ErrorCode::NotFound,
                                                                   "remote failure",
                                                                   std::optional<frontend::Json>{remoteDetails},
                                                                   frontend::Json::object()})});
        result.expectTrue(failedAccepted && failedClient.ready() && failedHarness.closeReasons.empty() &&
                              failedClient.pendingOperationCount() == 0 && failedCompletion.has_value() &&
                              failedCompletion->error.has_value() &&
                              failedCompletion->error->origin == core::ErrorOrigin::Command &&
                              failedCompletion->error->protocolCode == frontend::ErrorCode::NotFound &&
                              failedCompletion->error->message == "remote failure" &&
                              failedCompletion->error->remoteDetails == std::optional<frontend::Json>{remoteDetails},
                          "a schema-valid ok=false response completes as a command error without closing");

        const auto runSchemaInvalid = [&](bool ok,
                                          std::optional<frontend::Json> responseResult,
                                          std::optional<frontend::CommandError> responseError) {
            Harness harness;
            core::ClientCore client(clientOptions());
            const core::PhysicalGeneration generation = ready(client, harness);
            std::vector<core::OperationResult> completions;
            const core::Submission submitted =
                client.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                              [&completions](const core::OperationResult& completion) {
                                  completions.push_back(completion);
                              });
            if (!submitted.requestId.has_value()) {
                return false;
            }
            frontend::Response malformed{
                *submitted.requestId, ok, std::move(responseResult), std::move(responseError), frontend::Json::object()};
            const bool accepted = client.receive(generation, frontend::ServerMessage{std::move(malformed)});
            return !accepted && client.connectionState() == core::ConnectionState::Disconnected &&
                   client.pendingOperationCount() == 0 && harness.closeReasons.size() == 1 &&
                   harness.closeReasons.front() == "frontend server message encoding failed" && completions.size() == 1 &&
                   completions.front().error.has_value() &&
                   completions.front().error->origin == core::ErrorOrigin::Protocol &&
                   completions.front().error->clientCode == core::ClientErrorCode::DecodeFailure &&
                   completions.front().error->message == "failed to encode frontend server message";
        };
        result.expectTrue(runSchemaInvalid(true, std::nullopt, std::nullopt),
                          "an ok=true response without a result is rejected at the frozen schema border");
        result.expectTrue(
            runSchemaInvalid(false,
                             std::optional<frontend::Json>{modelListFixture->at("minimalResult")},
                             frontend::CommandError{frontend::ErrorCode::NotFound,
                                                    "remote failure",
                                                    std::optional<frontend::Json>{remoteDetails},
                                                    frontend::Json::object()}),
            "an ok=false response containing a result is rejected at the frozen schema border");
        result.expectTrue(
            runSchemaInvalid(true,
                             std::optional<frontend::Json>{modelListFixture->at("minimalResult")},
                             frontend::CommandError{frontend::ErrorCode::InternalError,
                                                    "invalid",
                                                    std::nullopt,
                                                    frontend::Json::object()}),
            "an ok=true response containing an error is rejected at the frozen schema border");

        Harness wrongResultHarness;
        core::ClientCore wrongResultClient(clientOptions());
        const core::PhysicalGeneration wrongResultGeneration = ready(wrongResultClient, wrongResultHarness);
        std::optional<core::OperationResult> wrongResultCompletion;
        const core::Submission wrongResult =
            wrongResultClient.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                                     [&wrongResultCompletion](const core::OperationResult& completion) {
                                         wrongResultCompletion = completion;
                                     });
        const bool wrongResultAccepted = wrongResult.requestId.has_value() &&
                                         wrongResultClient.receive(
                                             wrongResultGeneration,
                                             frontend::ServerMessage{frontend::Response::success(
                                                 *wrongResult.requestId, frontend::Json{{"wrong", true}})});
        result.expectTrue(!wrongResultAccepted && wrongResultClient.connectionState() == core::ConnectionState::Disconnected &&
                              wrongResultHarness.closeReasons == std::vector<std::string>{"frontend response type mismatch"} &&
                              wrongResultCompletion.has_value() && wrongResultCompletion->error.has_value() &&
                              wrongResultCompletion->error->clientCode == core::ClientErrorCode::ResponseTypeMismatch,
                          "a schema-valid success with the wrong method result fails once and closes");
    }

    void testImmediateSendParity(tests::support::TestResult& result) {
        const auto runRejectedSend = [](bool throws, bool retryable) {
            Harness harness;
            std::vector<core::StateChange> transitions;
            core::ClientCallbacks callbacks;
            callbacks.onConnectionStateChanged = [&transitions](const core::StateChange& change) {
                transitions.push_back(change);
            };
            core::ClientCore client(clientOptions(), std::move(callbacks));
            (void) ready(client, harness);
            harness.throwOnCommand = throws;
            harness.status = throws ? core::SendStatus::Accepted : core::SendStatus::Backpressure;
            harness.retryable = retryable;
            std::size_t completions = 0;
            const core::Submission submission =
                client.submit(generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                              [&completions](const core::OperationResult&) {
                                  ++completions;
                              });
            const auto closing = std::find_if(transitions.begin(), transitions.end(), [](const core::StateChange& change) {
                return change.current == core::ConnectionState::Closing;
            });
            const std::string expectedSubmissionMessage =
                throws ? "frontend transport send failed" : "frontend transport rejected command";
            const std::string expectedCloseReason = throws ? "frontend command send failed" : "frontend command rejected";
            return !submission && submission.error.has_value() && submission.error->origin == core::ErrorOrigin::Client &&
                   submission.error->clientCode == core::ClientErrorCode::SendRejected &&
                   submission.error->message == expectedSubmissionMessage && submission.error->retryable == (throws || retryable) &&
                   completions == 0 && client.pendingOperationCount() == 0 &&
                   client.connectionState() == core::ConnectionState::Disconnected && harness.closeReasons.size() == 1 &&
                   harness.closeReasons.front() == expectedCloseReason && closing != transitions.end() && closing->error.has_value() &&
                   closing->error->origin == core::ErrorOrigin::Transport &&
                   closing->error->clientCode == core::ClientErrorCode::TransportFailure;
        };
        result.expectTrue(runRejectedSend(false, false) && runRejectedSend(false, true) && runRejectedSend(true, false),
                          "immediate send rejection or throw returns Client SendRejected, invokes no completion, then transport-closes");

        const frontend::Json* snapshotFixture = fixtureFor(generated::MethodId::SnapshotGet);
        if (snapshotFixture == nullptr) {
            result.expectTrue(false, "the generated snapshot.get fixture is available for reentrant-send coverage");
            return;
        }
        Harness harness;
        core::ClientCore client(clientOptions());
        const core::PhysicalGeneration generation = ready(client, harness);
        const std::uint64_t revisionBeforeSend = client.state()->revision;
        bool readyAtSend = false;
        bool revisionUnchangedAtSend = false;
        bool synchronizingAfterReentrantResponse = false;
        harness.onCommandSend = [&](const core::OutboundMessage& outbound) {
            const auto* command = std::get_if<generated::DefinedCommand>(&outbound.value);
            readyAtSend = client.ready();
            revisionUnchangedAtSend = client.state()->revision == revisionBeforeSend &&
                                      client.state()->freshness == core::PublishedFreshness::Current;
            if (command != nullptr) {
                (void) client.receive(
                    generation,
                    frontend::ServerMessage{frontend::Response::success(command->requestId, snapshotFixture->at("minimalResult"))});
                synchronizingAfterReentrantResponse = client.connectionState() == core::ConnectionState::Synchronizing;
            }
        };
        std::vector<core::OperationResult> completions;
        const core::Submission submission = client.requestSnapshot([&completions](const core::OperationResult& completion) {
            completions.push_back(completion);
        });
        const bool noPrematurePublication =
            submission && readyAtSend && revisionUnchangedAtSend && synchronizingAfterReentrantResponse &&
            client.state()->revision == revisionBeforeSend && client.state()->freshness == core::PublishedFreshness::Current &&
            completions.empty();
        (void) client.receive(generation, frontend::ServerMessage{emptySnapshot()});
        (void) client.receive(generation, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
        result.expectTrue(noPrematurePublication && client.ready() && completions.size() == 1 && completions.front().succeeded(),
                          "explicit synchronization sends while Ready and handles a reentrant response before publishing synchronization state");
    }

    void testCompletionLifecycleInvalidation(tests::support::TestResult& result) {
        const frontend::Json* modelListFixture = fixtureFor(generated::MethodId::ModelList);
        if (modelListFixture == nullptr) {
            result.expectTrue(false, "the generated model.list fixture is available for completion reentry coverage");
            return;
        }

        Harness harness;
        core::ClientCore client(clientOptions());
        const core::PhysicalGeneration firstGeneration = ready(client, harness);
        harness.outbound.clear();
        std::size_t primaryCompletions = 0;
        std::size_t nestedCompletions = 0;
        std::optional<core::Submission> nestedSubmission;
        const core::Submission primary = client.submit(
            generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
            [&](const core::OperationResult& completion) {
                ++primaryCompletions;
                if (!completion.succeeded()) {
                    return;
                }
                nestedSubmission = client.submit(
                    generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                    [&nestedCompletions](const core::OperationResult& nested) {
                        nestedCompletions += nested.error.has_value() ? 1U : 100U;
                    });
                client.detach(firstGeneration, "operation completion detached");
            });
        const bool responseAccepted =
            primary && client.receive(firstGeneration,
                                      frontend::ServerMessage{frontend::Response::success(
                                          *primary.requestId, modelListFixture->at("minimalResult"))});
        const std::size_t firstGenerationCommands =
            static_cast<std::size_t>(std::count_if(harness.outbound.begin(), harness.outbound.end(), [](const Harness::Sent& sent) {
                return sent.command;
            }));
        const std::optional<core::PhysicalGeneration> secondGeneration = client.attach(harness.transport());
        if (secondGeneration.has_value()) {
            client.transportConnected(*secondGeneration);
        }
        const std::size_t commandsAfterReattach =
            static_cast<std::size_t>(std::count_if(harness.outbound.begin(), harness.outbound.end(), [](const Harness::Sent& sent) {
                return sent.command;
            }));
        result.expectTrue(!responseAccepted && nestedSubmission.has_value() && nestedSubmission->accepted() && primaryCompletions == 1 &&
                              nestedCompletions == 1 && firstGenerationCommands == 1 && commandsAfterReattach == 1 &&
                              client.pendingOperationCount() == 0 && secondGeneration == std::optional<core::PhysicalGeneration>{2},
                          "completion reentry may queue then detach, but never sends the old-generation deferred operation");
    }

    void testSynchronizationCancellationInvalidation(tests::support::TestResult& result) {
        Harness firstHarness;
        Harness replacementHarness;
        core::ClientCore* clientPointer = nullptr;
        std::optional<core::PhysicalGeneration> firstGeneration;
        std::optional<core::PhysicalGeneration> replacementGeneration;
        bool invalidateReadyTransition = false;
        core::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&](const core::StateChange& change) {
            if (!invalidateReadyTransition || change.current != core::ConnectionState::Ready || clientPointer == nullptr ||
                !firstGeneration.has_value()) {
                return;
            }
            invalidateReadyTransition = false;
            clientPointer->detach(*firstGeneration, "synchronization cancellation replaced attachment");
            replacementGeneration = clientPointer->attach(replacementHarness.transport());
        };
        core::ClientCore client(clientOptions(), std::move(callbacks));
        clientPointer = &client;
        firstGeneration = ready(client, firstHarness);
        std::size_t staleCompletions = 0;
        invalidateReadyTransition = true;
        const core::Submission submission = client.requestSnapshot([&](const core::OperationResult&) {
            ++staleCompletions;
        });
        const bool responseAccepted = submission.requestId.has_value() &&
                                      client.receive(*firstGeneration,
                                                     frontend::ServerMessage{frontend::Response::failure(
                                                         *submission.requestId,
                                                         frontend::CommandError{frontend::ErrorCode::BackendUnavailable,
                                                                                "synchronization unavailable",
                                                                                std::nullopt,
                                                                                frontend::Json::object()})});
        result.expectTrue(!responseAccepted && staleCompletions == 0 && firstHarness.closeReasons.size() == 1 &&
                              replacementGeneration == std::optional<core::PhysicalGeneration>{2} &&
                              client.activeGeneration() == replacementGeneration &&
                              client.connectionState() == core::ConnectionState::Connecting && client.pendingOperationCount() == 0 &&
                              replacementHarness.outbound.empty(),
                          "a Ready transition callback that replaces the attachment suppresses the retired synchronization completion");
    }

    void testDirectSendLifecycleInvalidation(tests::support::TestResult& result) {
        Harness firstHarness;
        Harness replacementHarness;
        core::ClientCore client(clientOptions());
        const core::PhysicalGeneration firstGeneration = ready(client, firstHarness);
        std::optional<core::PhysicalGeneration> replacementGeneration;
        std::vector<core::OperationResult> completions;
        firstHarness.status = core::SendStatus::Failed;
        firstHarness.retryable = true;
        firstHarness.onCommandSend = [&](const core::OutboundMessage&) {
            client.transportDisconnected(firstGeneration, {"first physical generation disconnected", true});
            replacementGeneration = client.attach(replacementHarness.transport());
            if (!replacementGeneration) {
                return;
            }
            client.transportConnected(*replacementGeneration);
            (void) client.receive(*replacementGeneration,
                                  frontend::ServerMessage{welcome("replacement-operation-session")});
        };
        const core::Submission invalidated = client.requestSnapshot([&](const core::OperationResult& completion) {
            completions.push_back(completion);
        });
        const bool replacementSynchronizationSurvived =
            replacementGeneration.has_value() && client.activeGeneration() == replacementGeneration &&
            client.connectionState() == core::ConnectionState::Synchronizing && client.pendingOperationCount() == 0 &&
            client.receive(*replacementGeneration, frontend::ServerMessage{emptySnapshot()}) &&
            client.receive(*replacementGeneration,
                           frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}}) &&
            client.ready();
        result.expectTrue(!invalidated && invalidated.error.has_value() &&
                              invalidated.error->clientCode == core::ClientErrorCode::NotConnected &&
                              completions.size() == 1 && completions.front().error.has_value() &&
                              completions.front().error->clientCode == core::ClientErrorCode::TransportFailure &&
                              replacementSynchronizationSurvived,
                          "a direct send that replaces its physical generation cannot reset or continue through the replacement synchronization");

        Harness closingHarness;
        core::ClientCore closingClient(clientOptions());
        (void) ready(closingClient, closingHarness);
        std::size_t closingCompletions = 0;
        closingHarness.onCommandSend = [&](const core::OutboundMessage&) {
            closingClient.close("direct send callback closed client");
        };
        const core::Submission closed = closingClient.submit(
            generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
            [&](const core::OperationResult&) {
                ++closingCompletions;
            });
        result.expectTrue(!closed && closed.error.has_value() && closed.error->clientCode == core::ClientErrorCode::NotConnected &&
                              closingCompletions == 1 &&
                              closingClient.connectionState() == core::ConnectionState::Closed &&
                              closingClient.pendingOperationCount() == 0,
                          "a direct send callback that closes cannot return an accepted request ID for the failed old-generation operation");
    }

    void testDirectSendSynchronousSynchronization(tests::support::TestResult& result) {
        const frontend::Json* snapshotFixture = fixtureFor(generated::MethodId::SnapshotGet);
        if (snapshotFixture == nullptr) {
            result.expectTrue(false, "the generated snapshot.get fixture is available for synchronous-send coverage");
            return;
        }

        Harness successHarness;
        core::ClientCore successClient(clientOptions());
        const core::PhysicalGeneration successGeneration = ready(successClient, successHarness);
        std::vector<core::OperationResult> successCompletions;
        bool successExchange = false;
        successHarness.onCommandSend = [&](const core::OutboundMessage& outbound) {
            const auto* command = std::get_if<generated::DefinedCommand>(&outbound.value);
            if (command == nullptr || generated::commandMethod(command->parameters) != generated::MethodId::SnapshotGet) {
                return;
            }
            successExchange =
                successClient.receive(successGeneration,
                                      frontend::ServerMessage{frontend::Response::success(
                                          command->requestId, snapshotFixture->at("minimalResult"))}) &&
                successClient.receive(successGeneration, frontend::ServerMessage{emptySnapshot()}) &&
                successClient.receive(
                    successGeneration, frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
        };
        const core::Submission synchronousSuccess = successClient.requestSnapshot([&](const core::OperationResult& completion) {
            successCompletions.push_back(completion);
        });
        result.expectTrue(synchronousSuccess.accepted() && synchronousSuccess.requestId == std::optional<std::string>{"c1-r1"} &&
                              successExchange && successCompletions.size() == 1 && successCompletions.front().succeeded() &&
                              successClient.ready() && successClient.pendingOperationCount() == 0,
                          "an accepted direct synchronization send remains an accepted submission when its complete successful exchange "
                          "runs synchronously inside the transport callback");

        Harness errorHarness;
        core::ClientCore errorClient(clientOptions());
        const core::PhysicalGeneration errorGeneration = ready(errorClient, errorHarness);
        std::vector<core::OperationResult> errorCompletions;
        bool errorResponseAccepted = false;
        errorHarness.onCommandSend = [&](const core::OutboundMessage& outbound) {
            const auto* command = std::get_if<generated::DefinedCommand>(&outbound.value);
            if (command == nullptr || generated::commandMethod(command->parameters) != generated::MethodId::SnapshotGet) {
                return;
            }
            errorResponseAccepted = errorClient.receive(
                errorGeneration,
                frontend::ServerMessage{frontend::Response::failure(
                    command->requestId,
                    frontend::CommandError{
                        frontend::ErrorCode::BackendUnavailable, "synchronous snapshot failure", std::nullopt, frontend::Json::object()})});
        };
        const core::Submission synchronousError = errorClient.requestSnapshot([&](const core::OperationResult& completion) {
            errorCompletions.push_back(completion);
        });
        result.expectTrue(synchronousError.accepted() && synchronousError.requestId == std::optional<std::string>{"c1-r1"} &&
                              errorResponseAccepted && errorCompletions.size() == 1 && errorCompletions.front().error.has_value() &&
                              errorCompletions.front().error->origin == core::ErrorOrigin::Command && errorClient.ready() &&
                              errorClient.pendingOperationCount() == 0,
                          "an accepted direct synchronization send remains an accepted submission when its command error completes "
                          "synchronously inside the transport callback");
    }

    void testDeferredSynchronousSynchronizationDrain(tests::support::TestResult& result) {
        const frontend::Json* snapshotFixture = fixtureFor(generated::MethodId::SnapshotGet);
        const frontend::Json* modelListFixture = fixtureFor(generated::MethodId::ModelList);
        if (snapshotFixture == nullptr || modelListFixture == nullptr) {
            result.expectTrue(false, "generated fixtures are available for deferred synchronous-drain coverage");
            return;
        }

        Harness harness;
        core::ClientCore* clientPointer = nullptr;
        bool submitSnapshotFromProtocolCallback = false;
        std::optional<core::Submission> snapshotSubmission;
        std::optional<core::Submission> nestedSubmission;
        std::vector<core::OperationResult> snapshotCompletions;
        std::vector<core::OperationResult> nestedCompletions;
        core::ClientCallbacks callbacks;
        callbacks.onProtocolMessage = [&](const frontend::ServerMessage&) {
            if (!submitSnapshotFromProtocolCallback || clientPointer == nullptr) {
                return;
            }
            submitSnapshotFromProtocolCallback = false;
            snapshotSubmission = clientPointer->requestSnapshot([&](const core::OperationResult& completion) {
                snapshotCompletions.push_back(completion);
                nestedSubmission = clientPointer->submit(
                    generated::makeParameters(generated::MethodId::ModelList, frontend::Json::object()),
                    [&](const core::OperationResult& nestedCompletion) {
                        nestedCompletions.push_back(nestedCompletion);
                    });
            });
        };
        core::ClientCore client(clientOptions(), std::move(callbacks));
        clientPointer = &client;
        const core::PhysicalGeneration generation = ready(client, harness);
        harness.outbound.clear();
        harness.onCommandSend = [&](const core::OutboundMessage& outbound) {
            const auto* command = std::get_if<generated::DefinedCommand>(&outbound.value);
            if (command == nullptr) {
                return;
            }
            const generated::MethodId method = generated::commandMethod(command->parameters);
            if (method == generated::MethodId::SnapshotGet) {
                (void) client.receive(
                    generation,
                    frontend::ServerMessage{frontend::Response::success(command->requestId, snapshotFixture->at("minimalResult"))});
                (void) client.receive(generation, frontend::ServerMessage{emptySnapshot()});
                (void) client.receive(generation,
                                      frontend::ServerMessage{frontend::SyncComplete{frontend::SequenceNumber(0)}});
            } else if (method == generated::MethodId::ModelList) {
                (void) client.receive(
                    generation,
                    frontend::ServerMessage{frontend::Response::success(command->requestId, modelListFixture->at("minimalResult"))});
            }
        };

        submitSnapshotFromProtocolCallback = true;
        frontend::ProtocolErrorMessage trigger;
        trigger.code = frontend::ErrorCode::RateLimited;
        trigger.message = "deferred synchronization trigger";
        trigger.closeConnection = false;
        const bool triggerAccepted = client.receive(generation, frontend::ServerMessage{std::move(trigger)});
        const std::size_t snapshotCommands =
            static_cast<std::size_t>(std::count_if(harness.outbound.begin(), harness.outbound.end(), [](const Harness::Sent& sent) {
                return sent.method == generated::MethodId::SnapshotGet;
            }));
        const std::size_t modelListCommands =
            static_cast<std::size_t>(std::count_if(harness.outbound.begin(), harness.outbound.end(), [](const Harness::Sent& sent) {
                return sent.method == generated::MethodId::ModelList;
            }));
        result.expectTrue(triggerAccepted && snapshotSubmission.has_value() && snapshotSubmission->accepted() &&
                              nestedSubmission.has_value() && nestedSubmission->accepted() && snapshotCompletions.size() == 1 &&
                              snapshotCompletions.front().succeeded() && nestedCompletions.size() == 1 &&
                              nestedCompletions.front().succeeded() && snapshotCommands == 1 && modelListCommands == 1 && client.ready() &&
                              client.pendingOperationCount() == 0,
                          "a deferred synchronization that completes synchronously drains the ordinary command queued by its completion "
                          "before the outer receive returns");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testAllGeneratedOperations(result);
    testFailureAndSensitiveReverseResponse(result);
    testDuplicateAndUnexpectedResponses(result);
    testMalformedResponseParity(result);
    testImmediateSendParity(result);
    testCompletionLifecycleInvalidation(result);
    testSynchronizationCancellationInvalidation(result);
    testDirectSendLifecycleInvalidation(result);
    testDirectSendSynchronousSynchronization(result);
    testDeferredSynchronousSynchronizationDrain(result);
    return result.processResult();
}
