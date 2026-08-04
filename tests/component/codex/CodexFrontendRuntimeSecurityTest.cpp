/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/frontend/detail/BackendCommandMapper.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = frontend::generated;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;
    using Json = ai::openai::codex::Json;
    using ai::openai::codex::detail::TransportCallbacks;

    constexpr std::uint64_t TrustedUserId = 7107;
    constexpr std::string_view SecretSentinel = "A17B_RUNTIME_SECURITY_SECRET_SENTINEL";

    class ManualScheduler {
    public:
        void schedule(std::function<void()> callback) {
            callbacks.push_back(std::move(callback));
        }

        void drain(std::size_t limit = 100'000) {
            std::size_t executed = 0;
            while (!callbacks.empty()) {
                std::function<void()> callback = std::move(callbacks.front());
                callbacks.pop_front();
                callback();
                if (++executed > limit) {
                    throw std::runtime_error("runtime security scheduler drain limit exceeded");
                }
            }
        }

    private:
        std::deque<std::function<void()>> callbacks;
    };

    struct Observations {
        std::vector<frontend::ServerMessage> messages;
        std::vector<std::string> compactJson;
        std::vector<std::string> closeReasons;
    };

    frontend::FrontendConnectionCallbacks callbacksFor(Observations& observations) {
        return {[&observations](const frontend::OutboundMessage& message) {
                    observations.messages.push_back(message.message);
                    observations.compactJson.push_back(message.compactJson);
                    return true;
                },
                [&observations](const std::string& reason) {
                    observations.closeReasons.push_back(reason);
                }};
    }

    frontend::FrontendPeerContext verifiedLocalPeer() {
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Unix;
        peer.loopback = true;
        peer.localPeer = true;
        peer.unixUserId = TrustedUserId;
        return peer;
    }

    frontend::FrontendPeerContext remotePeer(std::string address = "127.0.0.71") {
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Ipv4;
        peer.loopback = true;
        peer.remoteAddress = std::move(address);
        return peer;
    }

    frontend::ClientMessage localHello() {
        return frontend::Hello{std::nullopt, Json::object()};
    }

    frontend::ClientMessage bearerHello() {
        return frontend::Hello{std::nullopt,
                               Json::object(),
                               std::vector{frontend::FrontendCapability::MethodDiscovery, frontend::FrontendCapability::SecurityScopes},
                               frontend::AuthenticationCredential{frontend::BearerCredential{"runtime-security-test-token"}}};
    }

    Json command(std::string requestId, std::string method, Json parameters = Json::object()) {
        return Json{{"protocol", frontend::ProtocolIdentity},
                    {"version", frontend::ProtocolVersion},
                    {"kind", "command"},
                    {"requestId", std::move(requestId)},
                    {"method", std::move(method)},
                    {"params", std::move(parameters)}};
    }

    const frontend::Response* response(const Observations& observations, std::string_view requestId) {
        for (auto iterator = observations.messages.rbegin(); iterator != observations.messages.rend(); ++iterator) {
            if (const auto* value = std::get_if<frontend::Response>(&*iterator); value && value->requestId == requestId) {
                return value;
            }
        }
        return nullptr;
    }

    const frontend::ProtocolErrorMessage* protocolError(const Observations& observations, const std::optional<std::string>& requestId) {
        for (auto iterator = observations.messages.rbegin(); iterator != observations.messages.rend(); ++iterator) {
            if (const auto* value = std::get_if<frontend::ProtocolErrorMessage>(&*iterator); value && value->requestId == requestId) {
                return value;
            }
        }
        return nullptr;
    }

    bool failed(const Observations& observations, std::string_view requestId, frontend::ErrorCode code) {
        const frontend::Response* value = response(observations, requestId);
        return value != nullptr && !value->ok && value->error.has_value() && value->error->code == code;
    }

    bool contains(const Observations& observations, std::string_view value) {
        return std::any_of(observations.compactJson.begin(),
                           observations.compactJson.end(),
                           [value](const std::string& encoded) {
                               return encoded.find(value) != std::string::npos;
                           }) ||
               std::any_of(observations.closeReasons.begin(), observations.closeReasons.end(), [value](const std::string& reason) {
                   return reason.find(value) != std::string::npos;
               });
    }

    class RuntimeHarness {
    public:
        RuntimeHarness()
            : transport(std::make_shared<tests::codex::FakeTransportState>()) {
            tests::codex::installInitializingFake(transport, [this](const Json& message, const TransportCallbacks& callbacks) {
                handleProviderRequest(message, callbacks);
            });

            backend::BackendCoreOptions options;
            options.initialThreadListLimit = 1;
            options.scheduler = [this](std::function<void()> callback) {
                scheduler.schedule(std::move(callback));
            };
            core = std::make_unique<FakeBackendCore>(std::move(options), transport);
        }

        frontend::FrontendServiceOptions serviceOptions() {
            frontend::FrontendServiceOptions options;
            options.scheduler = [this](std::function<void()> callback) {
                scheduler.schedule(std::move(callback));
            };
            options.timerScheduler = [](std::uint64_t, std::function<void()>) {
                return frontend::FrontendTimerCancellation{[] {
                }};
            };
            options.trustedLocalUserId = TrustedUserId;
            return options;
        }

        frontend::FrontendServiceOptions remoteServiceOptions() {
            frontend::FrontendServiceOptions options = serviceOptions();
            options.authenticator = [](const frontend::FrontendPeerContext&,
                                       const frontend::AuthenticationCredential& credential) -> frontend::AuthenticationResult {
                const auto* bearer = std::get_if<frontend::BearerCredential>(&credential);
                if (bearer == nullptr || bearer->token != "runtime-security-test-token") {
                    return frontend::AuthenticationFailure{frontend::AuthenticationFailureCode::AuthenticationFailed};
                }
                return frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
                    "runtime-security-remote",
                    std::vector<frontend::FrontendScope>{frontend::DefaultRemoteScopes.begin(), frontend::DefaultRemoteScopes.end()},
                    "default_remote",
                    false}};
            };
            return options;
        }

        ManualScheduler scheduler;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        std::unique_ptr<FakeBackendCore> core;
        bool modelListRemoteError = false;

    private:
        void handleProviderRequest(const Json& message, const TransportCallbacks& callbacks) {
            const auto method = message.find("method");
            const auto id = message.find("id");
            if (method == message.end() || !method->is_string() || id == message.end()) {
                return;
            }
            if (*method == "thread/list") {
                tests::codex::inject(
                    callbacks,
                    Json{{"id", *id}, {"result", {{"data", Json::array()}, {"nextCursor", nullptr}, {"backwardsCursor", nullptr}}}});
            } else if (*method == "model/list") {
                if (std::exchange(modelListRemoteError, false)) {
                    tests::codex::inject(
                        callbacks,
                        Json{{"id", *id},
                             {"error", {{"code", -32'071}, {"message", SecretSentinel}, {"data", {{"diagnostic", SecretSentinel}}}}}});
                } else {
                    tests::codex::inject(callbacks, Json{{"id", *id}, {"result", {{"data", Json::array()}}}});
                }
            }
        }
    };

    void authenticateLocal(RuntimeHarness& harness,
                           frontend::FrontendConnection& connection,
                           tests::support::TestResult& result,
                           std::string_view description) {
        result.expectTrue(connection.receive(localHello()).accepted(), std::string{description});
        harness.scheduler.drain();
    }

    void testPreAuthenticationAndTypedFrameLimits(tests::support::TestResult& result) {
        RuntimeHarness harness;
        frontend::FrontendService service(*harness.core, harness.serviceOptions());

        Observations unauthenticated;
        frontend::FrontendConnection unauthenticatedConnection = service.openConnection(remotePeer(), callbacksFor(unauthenticated));
        const frontend::ConnectionReceiveResult unauthenticatedResult = unauthenticatedConnection.receive(
            command("secret-request-id", "command.exec", Json{{"command", Json::array({SecretSentinel})}}));
        harness.scheduler.drain();
        const frontend::ProtocolErrorMessage* authenticationError = protocolError(unauthenticated, std::nullopt);
        result.expectTrue(unauthenticatedResult.status == frontend::ConnectionReceiveStatus::Closing && authenticationError != nullptr &&
                              authenticationError->code == frontend::ErrorCode::AuthenticationRequired &&
                              !unauthenticatedConnection.isOpen() && !contains(unauthenticated, "command.exec") &&
                              !contains(unauthenticated, "secret-request-id") && !contains(unauthenticated, SecretSentinel),
                          "a command before authentication closes generically without disclosing method, correlation, or parameters");

        frontend::FrontendServiceOptions boundedOptions = harness.serviceOptions();
        boundedOptions.maximumInboundMessageBytes = 128;
        frontend::FrontendService boundedService(*harness.core, boundedOptions);

        Observations legacyFrame;
        frontend::FrontendConnection legacyConnection = boundedService.openConnection(remotePeer("127.0.0.72"), callbacksFor(legacyFrame));
        frontend::ThreadStart oversizedThreadStart;
        oversizedThreadStart.cwd = "/" + std::string(512, 'x');
        const frontend::ClientMessage legacyCommand =
            frontend::Command{"oversized-legacy", std::move(oversizedThreadStart), Json::object(), Json::object()};
        const frontend::ConnectionReceiveResult legacyResult = legacyConnection.receive(legacyCommand);
        harness.scheduler.drain();
        result.expectTrue(legacyResult.status == frontend::ConnectionReceiveStatus::Closing &&
                              protocolError(legacyFrame, std::nullopt) != nullptr &&
                              protocolError(legacyFrame, std::nullopt)->code == frontend::ErrorCode::FrameTooLarge,
                          "the typed legacy ClientMessage path enforces maximumInboundMessageBytes before authentication");

        Observations definedFrame;
        frontend::FrontendConnection definedConnection =
            boundedService.openConnection(remotePeer("127.0.0.73"), callbacksFor(definedFrame));
        const frontend::ConnectionReceiveResult definedResult =
            definedConnection.receive(command("oversized-defined", "fs.readFile", Json{{"path", "/" + std::string(512, 'y')}}));
        harness.scheduler.drain();
        result.expectTrue(definedResult.status == frontend::ConnectionReceiveStatus::Closing &&
                              protocolError(definedFrame, std::nullopt) != nullptr &&
                              protocolError(definedFrame, std::nullopt)->code == frontend::ErrorCode::FrameTooLarge,
                          "the complete 105-method JSON path enforces the same transport-neutral frame bound");

        boundedService.close();
        service.close();
        harness.scheduler.drain();
    }

    void testDeploymentBeforeSchemaAndConfiguredPolicy(tests::support::TestResult& result) {
        RuntimeHarness harness;

        frontend::FrontendService disabledService(*harness.core, harness.serviceOptions());
        Observations disabledObservations;
        frontend::FrontendConnection disabled = disabledService.openConnection(verifiedLocalPeer(), callbacksFor(disabledObservations));
        authenticateLocal(harness, disabled, result, "the disabled-method precedence test authenticates through verified local trust");
        const frontend::ConnectionReceiveResult disabledResult =
            disabled.receive(command("disabled-malformed", "fs.readFile", Json{{"path", 42}, {"traceNote", SecretSentinel}}));
        harness.scheduler.drain();
        const frontend::ProtocolErrorMessage* disabledError =
            protocolError(disabledObservations, std::optional<std::string>{"disabled-malformed"});
        result.expectTrue(disabledResult.status == frontend::ConnectionReceiveStatus::Rejected && disabledError != nullptr &&
                              disabledError->code == frontend::ErrorCode::UnknownMethod &&
                              response(disabledObservations, "disabled-malformed") == nullptr &&
                              !contains(disabledObservations, SecretSentinel),
                          "a raw defined but deployment-disabled method returns protocol.error unknown_method before malformed parameter "
                          "diagnostics");
        disabledService.close();
        harness.scheduler.drain();

        frontend::FrontendServiceOptions missingPolicyOptions = harness.serviceOptions();
        missingPolicyOptions.enableFilesystemReadMethods = true;
        frontend::FrontendService missingPolicyService(*harness.core, missingPolicyOptions);
        Observations missingPolicyObservations;
        frontend::FrontendConnection missingPolicy =
            missingPolicyService.openConnection(verifiedLocalPeer(), callbacksFor(missingPolicyObservations));
        authenticateLocal(harness, missingPolicy, result, "the missing-policy deployment test authenticates normally");
        const frontend::ConnectionReceiveResult missingPolicyResult =
            missingPolicy.receive(command("missing-policy", "fs.readFile", Json{{"path", "/tmp/a17b"}}));
        harness.scheduler.drain();
        const frontend::ProtocolErrorMessage* missingPolicyError =
            protocolError(missingPolicyObservations, std::optional<std::string>{"missing-policy"});
        result.expectTrue(missingPolicyResult.status == frontend::ConnectionReceiveStatus::Rejected && missingPolicyError != nullptr &&
                              missingPolicyError->code == frontend::ErrorCode::UnknownMethod &&
                              response(missingPolicyObservations, "missing-policy") == nullptr,
                          "a conditional deployment boolean produces protocol.error unknown_method without its configured policy callback");
        missingPolicyService.close();
        harness.scheduler.drain();

        std::size_t readPolicyCalls = 0;
        std::size_t writePolicyCalls = 0;
        bool readParametersValidated = false;
        bool throwFromReadPolicy = false;
        frontend::FrontendServiceOptions enabledOptions = harness.remoteServiceOptions();
        enabledOptions.enableFilesystemReadMethods = true;
        enabledOptions.enableFilesystemWriteMethods = true;
        enabledOptions.filesystemReadPolicy = [&](const frontend::FrontendPrincipal&, std::string_view method, const Json& parameters) {
            ++readPolicyCalls;
            readParametersValidated = method == "fs.readFile" && parameters.at("path").is_string() &&
                                      parameters.value("traceNote", "") == "validated-safe-extension";
            if (throwFromReadPolicy) {
                throw std::runtime_error("synthetic policy failure");
            }
            return true;
        };
        enabledOptions.filesystemWritePolicy = [&](const frontend::FrontendPrincipal&, std::string_view method, const Json& parameters) {
            ++writePolicyCalls;
            return method == "fs.writeFile" && parameters.at("path").is_string() && parameters.at("dataBase64").is_string();
        };
        frontend::FrontendService enabledService(*harness.core, enabledOptions);

        Observations remoteObservations;
        frontend::FrontendConnection remote = enabledService.openConnection(remotePeer(), callbacksFor(remoteObservations));
        result.expectTrue(remote.receive(bearerHello()).accepted(), "the scope-order test authenticates a default_remote principal");
        Observations localObservations;
        frontend::FrontendConnection local = enabledService.openConnection(verifiedLocalPeer(), callbacksFor(localObservations));
        authenticateLocal(harness, local, result, "the conditional-policy test authenticates a local_trusted principal");
        harness.scheduler.drain();

        const frontend::ConnectionReceiveResult missingScope =
            remote.receive(command("missing-filesystem-scope", "fs.readFile", Json{{"path", "/tmp/a17b"}}));
        harness.scheduler.drain();
        result.expectTrue(missingScope.status == frontend::ConnectionReceiveStatus::Rejected &&
                              failed(remoteObservations, "missing-filesystem-scope", frontend::ErrorCode::PermissionDenied) &&
                              readPolicyCalls == 1,
                          "validated parameter-sensitive deployment policy is checked before required scopes");

        const frontend::ConnectionReceiveResult wrongType = local.receive(command("wrong-path-type", "fs.readFile", Json{{"path", 42}}));
        harness.scheduler.drain();
        const frontend::ProtocolErrorMessage* wrongTypeError =
            protocolError(localObservations, std::optional<std::string>{"wrong-path-type"});
        result.expectTrue(wrongType.status == frontend::ConnectionReceiveStatus::Rejected && wrongTypeError != nullptr &&
                              wrongTypeError->code == frontend::ErrorCode::InvalidField && readPolicyCalls == 1,
                          "schema validation rejects a known wrong type before invoking deployment policy");

        const frontend::ConnectionReceiveResult validatedRead =
            local.receive(command("validated-read", "fs.readFile", Json{{"path", "/tmp/a17b"}, {"traceNote", "validated-safe-extension"}}));
        harness.scheduler.drain();
        result.expectTrue(validatedRead.status == frontend::ConnectionReceiveStatus::Rejected &&
                              failed(localObservations, "validated-read", frontend::ErrorCode::BackendUnavailable) &&
                              readPolicyCalls == 2 && readParametersValidated,
                          "the conditional callback receives only schema-validated parameters and precedes provider readiness");

        throwFromReadPolicy = true;
        const frontend::ConnectionReceiveResult throwingRead = local.receive(
            command("throwing-policy", "fs.readFile", Json{{"path", "/tmp/a17b"}, {"traceNote", "validated-safe-extension"}}));
        harness.scheduler.drain();
        result.expectTrue(throwingRead.status == frontend::ConnectionReceiveStatus::Rejected &&
                              failed(localObservations, "throwing-policy", frontend::ErrorCode::PermissionDenied) && readPolicyCalls == 3,
                          "a conditional policy exception is contained and deterministically denies the invocation");

        const Json writeParameters{{"path", "/tmp/a17b"}, {"dataBase64", "eA=="}};
        const frontend::ConnectionReceiveResult missingController =
            local.receive(command("write-without-controller", "fs.writeFile", writeParameters));
        harness.scheduler.drain();
        result.expectTrue(missingController.status == frontend::ConnectionReceiveStatus::Rejected &&
                              failed(localObservations, "write-without-controller", frontend::ErrorCode::PermissionDenied) &&
                              writePolicyCalls == 1,
                          "validated filesystem mutation policy is checked before the generated controller requirement");

        result.expectTrue(local.receive(command("controller", "controller.acquire")).accepted(),
                          "the fully scoped local principal explicitly acquires the controller");
        harness.scheduler.drain();
        const frontend::ConnectionReceiveResult permittedWrite =
            local.receive(command("write-after-controller", "fs.writeFile", writeParameters));
        harness.scheduler.drain();
        result.expectTrue(permittedWrite.status == frontend::ConnectionReceiveStatus::Rejected &&
                              failed(localObservations, "write-after-controller", frontend::ErrorCode::BackendUnavailable) &&
                              writePolicyCalls == 2,
                          "conditional filesystem mutation requires deployment enablement, scope, controller, and its policy callback");

        enabledService.close();
        harness.scheduler.drain();
    }

    void testPrivateMappingFailureRedaction(tests::support::TestResult& result) {
        generated::DefinedCommand invalidMapping{"mapping-error",
                                                 generated::makeParameters(generated::MethodId::FsReadFile, Json::object()),
                                                 Json::object(),
                                                 Json{{"traceNote", SecretSentinel}}};
        const frontend::detail::DefinedCommandMapping mapped = frontend::detail::mapDefinedCommand(invalidMapping);
        const auto* mappingError = std::get_if<frontend::detail::BackendCommandMappingError>(&mapped);
        result.expectTrue(mappingError != nullptr && mappingError->message.find(SecretSentinel) == std::string::npos,
                          "the private mapping failure is bounded and never incorporates extension values");
    }

    class ProviderSecurityRunner {
    public:
        explicit ProviderSecurityRunner(tests::support::TestResult& result)
            : result(result) {
        }

        void start() {
            transport = std::make_shared<tests::codex::FakeTransportState>();
            tests::codex::installInitializingFake(transport, [this](const Json& message, const TransportCallbacks& callbacks) {
                handleProviderRequest(message, callbacks);
            });

            backend::BackendCoreOptions backendOptions;
            backendOptions.initialThreadListLimit = 1;
            core = std::make_unique<FakeBackendCore>(std::move(backendOptions), transport);

            frontend::FrontendServiceOptions serviceOptions;
            serviceOptions.trustedLocalUserId = TrustedUserId;
            service = std::make_unique<frontend::FrontendService>(*core, std::move(serviceOptions));
            first.emplace(service->openConnection(verifiedLocalPeer(), callbacksFor(firstObservations)));
            expect(first->receive(localHello()).accepted(),
                   "the provider security runner authenticates its first connection through verified local trust");
            waitUntil(
                "the provider security runner completes its first authentication",
                [this]() {
                    return first->principal().has_value() && first->helloComplete();
                },
                [this]() {
                    core->start();
                    waitUntil(
                        "the failure-redaction provider reaches Ready",
                        [this]() {
                            return core->isReady();
                        },
                        [this]() {
                            testRemoteFailureRedaction();
                        });
                });
        }

        bool isFinished() const noexcept {
            return finished;
        }

        const std::string& waitingStage() const noexcept {
            return waitingDescription;
        }

    private:
        static void defer(std::function<void()> callback) {
            core::EventReceiver::atNextTick(std::move(callback));
        }

        void expect(bool condition, const std::string& message) {
            result.expectTrue(condition, message);
        }

        void
        waitUntil(std::string description, std::function<bool()> predicate, std::function<void()> next, std::size_t remaining = 8'000) {
            waitingDescription = description;
            defer([this,
                   description = std::move(description),
                   predicate = std::move(predicate),
                   next = std::move(next),
                   remaining]() mutable {
                if (finished || finishing) {
                    return;
                }
                try {
                    if (predicate()) {
                        waitingDescription = "advancing after: " + description;
                        next();
                        return;
                    }
                    if (remaining == 0) {
                        expect(false, description);
                        beginShutdown();
                        return;
                    }
                    waitUntil(std::move(description), std::move(predicate), std::move(next), remaining - 1);
                } catch (...) {
                    expect(false, "provider security runner contains all callback exceptions at stage: " + description);
                    beginShutdown();
                }
            });
        }

        void testRemoteFailureRedaction() {
            modelListRemoteError = true;
            const frontend::ConnectionReceiveResult remoteError =
                first->receive(command("remote-error", "model.list", Json{{"traceNote", SecretSentinel}}));
            expect(remoteError.accepted(), "the remote-error provider operation is accepted asynchronously");
            waitUntil(
                "the remote provider error reaches FrontendService",
                [this]() {
                    return response(firstObservations, "remote-error") != nullptr;
                },
                [this]() {
                    expect(failed(firstObservations, "remote-error", frontend::ErrorCode::RemoteAppServerError) &&
                               !contains(firstObservations, SecretSentinel),
                           "a backend remote error never exposes its provider message, data, or safe unknown input extension");
                    testSubmissionFailureRedaction();
                });
        }

        void testSubmissionFailureRedaction() {
            transport->rejectNextSend = true;
            const frontend::ConnectionReceiveResult submissionError =
                first->receive(command("submission-error", "model.list", Json{{"traceNote", SecretSentinel}}));
            expect(submissionError.accepted(), "the provider enqueue failure remains an asynchronously correlated operation");
            waitUntil(
                "the provider enqueue failure reaches FrontendService",
                [this]() {
                    return response(firstObservations, "submission-error") != nullptr;
                },
                [this]() {
                    expect(failed(firstObservations, "submission-error", frontend::ErrorCode::LocalSubmissionFailure) &&
                               !contains(firstObservations, SecretSentinel),
                           "a provider enqueue failure returns one bounded category without echoing command parameters");
                    core->stop();
                    waitUntil(
                        "the provider returns to Stopped before lifecycle conflict coverage",
                        [this]() {
                            return core->snapshot().provider.lifecycle == backend::ProviderLifecycle::Stopped;
                        },
                        [this]() {
                            acquireFirstController();
                        });
                });
        }

        void acquireFirstController() {
            expect(first->receive(command("controller-one", "controller.acquire")).accepted(),
                   "the lifecycle test explicitly acquires the global controller");
            waitUntil(
                "the first controller acquisition completes",
                [this]() {
                    const frontend::Response* value = response(firstObservations, "controller-one");
                    return value != nullptr && value->ok;
                },
                [this]() {
                    testInvalidStop();
                });
        }

        void testInvalidStop() {
            const frontend::ConnectionReceiveResult invalidStop = first->receive(command("invalid-stop", "provider.stop"));
            expect(invalidStop.status == frontend::ConnectionReceiveStatus::Rejected,
                   "provider.stop rejects synchronously while the provider is already Stopped");
            waitUntil(
                "the invalid provider.stop conflict is delivered",
                [this]() {
                    return response(firstObservations, "invalid-stop") != nullptr;
                },
                [this]() {
                    expect(failed(firstObservations, "invalid-stop", frontend::ErrorCode::Conflict) && transport->stopCount == 1,
                           "provider.stop in the already-stopped state is a deterministic conflict with no additional BackendCore action");
                    testAcceptedStartSurvivesClose();
                });
        }

        void testAcceptedStartSurvivesClose() {
            const std::size_t startsBefore = transport->startCount;
            const frontend::ConnectionReceiveResult acceptedStart = first->receive(command("accepted-start", "provider.start"));
            expect(acceptedStart.accepted(), "provider.start is accepted synchronously into the one BackendCore lifecycle");
            first->close("close before lifecycle response");
            waitUntil(
                "the accepted provider.start reaches Ready and releases its closed frontend controller",
                [this]() {
                    return core->isReady() && !service->currentController().has_value();
                },
                [this, startsBefore]() {
                    expect(transport->startCount == startsBefore + 1 && response(firstObservations, "accepted-start") == nullptr,
                           "closing the originating connection suppresses only its response while the accepted provider start completes");
                    authenticateReplacement();
                });
        }

        void authenticateReplacement() {
            second.emplace(service->openConnection(verifiedLocalPeer(), callbacksFor(secondObservations)));
            expect(second->receive(localHello()).accepted(), "a new connection reauthenticates after the accepted lifecycle action");
            waitUntil(
                "the replacement connection completes authentication",
                [this]() {
                    return second->principal().has_value() && second->helloComplete();
                },
                [this]() {
                    expect(second->receive(command("controller-two", "controller.acquire")).accepted(),
                           "the replacement connection acquires control explicitly rather than inheriting it");
                    waitUntil(
                        "the replacement controller acquisition completes",
                        [this]() {
                            const frontend::Response* value = response(secondObservations, "controller-two");
                            return value != nullptr && value->ok;
                        },
                        [this]() {
                            testInvalidStart();
                        });
                });
        }

        void testInvalidStart() {
            const std::size_t startsBefore = transport->startCount;
            const frontend::ConnectionReceiveResult invalidStart = second->receive(command("invalid-start", "provider.start"));
            expect(invalidStart.status == frontend::ConnectionReceiveStatus::Rejected,
                   "provider.start rejects synchronously while the provider is Ready");
            waitUntil(
                "the invalid provider.start conflict is delivered",
                [this]() {
                    return response(secondObservations, "invalid-start") != nullptr;
                },
                [this, startsBefore]() {
                    expect(failed(secondObservations, "invalid-start", frontend::ErrorCode::Conflict) &&
                               transport->startCount == startsBefore && service->isOpen(),
                           "provider.start while Ready conflicts without restarting the provider or closing FrontendService");
                    testOverlappingStop();
                });
        }

        void testOverlappingStop() {
            const std::size_t stopsBefore = transport->stopCount;
            const frontend::ConnectionReceiveResult accepted = second->receive(command("stop-first", "provider.stop"));
            const frontend::ConnectionReceiveResult overlapping = second->receive(command("stop-overlap", "provider.stop"));
            expect(accepted.accepted() && overlapping.status == frontend::ConnectionReceiveStatus::Rejected,
                   "one provider.stop is accepted while an overlapping lifecycle action is rejected synchronously");
            waitUntil(
                "the accepted and overlapping provider.stop responses complete deterministically",
                [this]() {
                    return response(secondObservations, "stop-first") != nullptr &&
                           response(secondObservations, "stop-overlap") != nullptr &&
                           core->snapshot().provider.lifecycle == backend::ProviderLifecycle::Stopped;
                },
                [this, stopsBefore]() {
                    expect(response(secondObservations, "stop-first")->ok &&
                               failed(secondObservations, "stop-overlap", frontend::ErrorCode::Conflict) &&
                               transport->stopCount == stopsBefore + 1,
                           "overlapping lifecycle commands produce one BackendCore action and one terminal conflict");
                    beginShutdown();
                });
        }

        void handleProviderRequest(const Json& message, const TransportCallbacks& callbacks) {
            const auto method = message.find("method");
            const auto id = message.find("id");
            if (method == message.end() || !method->is_string() || id == message.end()) {
                return;
            }
            if (*method == "thread/list") {
                tests::codex::inject(
                    callbacks,
                    Json{{"id", *id}, {"result", {{"data", Json::array()}, {"nextCursor", nullptr}, {"backwardsCursor", nullptr}}}});
            } else if (*method == "model/list") {
                if (std::exchange(modelListRemoteError, false)) {
                    tests::codex::inject(
                        callbacks,
                        Json{{"id", *id},
                             {"error", {{"code", -32'071}, {"message", SecretSentinel}, {"data", {{"diagnostic", SecretSentinel}}}}}});
                } else {
                    tests::codex::inject(callbacks, Json{{"id", *id}, {"result", {{"data", Json::array()}}}});
                }
            }
        }

        void beginShutdown() {
            if (finishing || finished) {
                return;
            }
            finishing = true;
            waitingDescription = "waiting for clean provider shutdown";
            if (service) {
                service->close("provider security runner complete");
            }
            if (core) {
                core->stop();
            }
            waitForShutdown(8'000);
        }

        void waitForShutdown(std::size_t remaining) {
            defer([this, remaining]() {
                try {
                    if (!core || core->snapshot().provider.lifecycle == backend::ProviderLifecycle::Stopped || remaining == 0) {
                        expect(remaining != 0, "the provider security runner stops BackendCore within its deterministic cleanup bound");
                        first.reset();
                        second.reset();
                        service.reset();
                        core.reset();
                        finished = true;
                        core::SNodeC::stop();
                        return;
                    }
                    waitForShutdown(remaining - 1);
                } catch (...) {
                    first.reset();
                    second.reset();
                    service.reset();
                    core.reset();
                    finished = true;
                    core::SNodeC::stop();
                }
            });
        }

        tests::support::TestResult& result;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        std::unique_ptr<FakeBackendCore> core;
        std::unique_ptr<frontend::FrontendService> service;
        Observations firstObservations;
        Observations secondObservations;
        std::optional<frontend::FrontendConnection> first;
        std::optional<frontend::FrontendConnection> second;
        bool modelListRemoteError = false;
        bool finishing = false;
        bool finished = false;
        std::string waitingDescription = "not started";
    };
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    int returnCode = tests::support::cTestSkipReturnCode;

    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexFrontendRuntimeSecurityTest");
    } else {
        core::SNodeC::init(argc, argv);
        testPreAuthenticationAndTypedFrameLimits(result);
        testDeploymentBeforeSchemaAndConfiguredPolicy(result);
        testPrivateMappingFailureRedaction(result);

        bool timedOut = false;
        ProviderSecurityRunner runner(result);
        [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
            [&timedOut]() {
                timedOut = true;
                core::SNodeC::stop();
            },
            utils::Timeval({10, 0}));
        runner.start();
        const int eventLoopResult = core::SNodeC::start(utils::Timeval({12, 0}));
        result.expectTrue(!timedOut, "frontend runtime security completes before the watchdog (last stage: " + runner.waitingStage() + ")");
        result.expectTrue(runner.isFinished(), "frontend runtime security reaches a clean terminal state");
        result.expectEqual(0, eventLoopResult, "frontend runtime security event loop exits cleanly");

        core::SNodeC::free();
        returnCode = result.processResult();
    }
    return returnCode;
}
