/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/server/ServerCore.h"
#include "support/TestResult.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace server = ai::openai::codex::frontend::internal::server;

    class Backend final : public server::BackendPort {
    public:
        [[nodiscard]] bool providerReady() const noexcept override {
            if (onProviderReady) {
                std::function<void()> callback = std::move(onProviderReady);
                try {
                    callback();
                } catch (...) {
                }
            }
            return ready;
        }

        [[nodiscard]] model::CanonicalSnapshot snapshot() const override {
            if (snapshotThrows) {
                throw std::runtime_error("injected snapshot failure");
            }
            model::CanonicalSnapshot captured = state;
            if (onSnapshot) {
                std::function<void()> callback = std::move(onSnapshot);
                callback();
            }
            return captured;
        }

        [[nodiscard]] server::BackendSubmitStatus submit(server::BackendInvocation invocation) override {
            ++submissionCount;
            tokens.push_back(invocation.token);
            if (onSubmit) {
                return onSubmit(std::move(invocation));
            }
            if (core && invocation.token.method == generated::MethodId::ControllerAcquire) {
                const bool conflict = controller && *controller != invocation.session;
                if (!conflict) {
                    controller = invocation.session;
                }
                static_cast<void>(core->complete(server::BackendCompletion{
                    invocation.token,
                    conflict ? server::BackendCompletionValue{server::BackendCommandFailure{
                                   frontend::ErrorCode::Conflict, "frontend command conflicts with current state", std::nullopt}}
                             : server::BackendCompletionValue{server::BackendCommandSuccess{generated::makeResult(
                                   invocation.token.method,
                                   frontend::Json{{"controllerSessionId", invocation.session.value()}, {"role", "controller"}})}}}));
                return server::BackendSubmitStatus::Accepted;
            }
            if (core && invocation.token.method == generated::MethodId::ControllerRelease) {
                controller.reset();
                static_cast<void>(core->complete(server::BackendCompletion{
                    invocation.token,
                    server::BackendCommandSuccess{generated::makeResult(invocation.token.method, frontend::Json{{"role", "observer"}})}}));
                return server::BackendSubmitStatus::Accepted;
            }
            return submissionStatus;
        }

        void bind(server::ServerCore& boundCore) noexcept override {
            core = &boundCore;
        }

        void unbind(server::ServerCore& boundCore) noexcept override {
            if (core == &boundCore) {
                core = nullptr;
            }
        }

        [[nodiscard]] bool performProviderLifecycleAction(server::ProviderLifecycleAction action) override {
            lifecycleActions.push_back(action);
            if (lifecycleAccepted) {
                switch (action) {
                    case server::ProviderLifecycleAction::Start:
                    case server::ProviderLifecycleAction::Restart:
                        state.provider.lifecycle = model::ProviderLifecycle::Ready;
                        state.provider.desiredRunning = true;
                        break;
                    case server::ProviderLifecycleAction::Stop:
                        state.provider.lifecycle = model::ProviderLifecycle::Stopped;
                        state.provider.desiredRunning = false;
                        break;
                }
            }
            if (onLifecycleAction) {
                std::function<void()> callback = std::move(onLifecycleAction);
                callback();
            }
            return lifecycleAccepted;
        }

        bool ready = true;
        bool lifecycleAccepted = true;
        bool snapshotThrows = false;
        server::BackendSubmitStatus submissionStatus = server::BackendSubmitStatus::Accepted;
        model::CanonicalSnapshot state;
        std::size_t submissionCount = 0;
        std::vector<server::CommandToken> tokens;
        std::vector<server::ProviderLifecycleAction> lifecycleActions;
        std::function<server::BackendSubmitStatus(server::BackendInvocation)> onSubmit;
        server::ServerCore* core = nullptr;
        std::optional<model::SessionIdentity> controller;
        mutable std::function<void()> onProviderReady;
        mutable std::function<void()> onSnapshot;
        std::function<void()> onLifecycleAction;
    };

    frontend::AuthenticationResult authenticate(const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
        frontend::FrontendPrincipal principal;
        principal.id = "command-principal";
        principal.profile = "test";
        principal.scopes.assign(frontend::LocalTrustedScopes.begin(), frontend::LocalTrustedScopes.end());
        return frontend::AuthenticationSuccess{std::move(principal)};
    }

    server::ServerCoreOptions options() {
        server::ServerCoreOptions result;
        result.authenticator = authenticate;
        result.maxInboundBurst = 1000;
        result.maxInboundMessagesPerSecond = 1000;
        result.maxOutstandingCommandsPerConnection = 256;
        result.enableFilesystemReadMethods = true;
        result.enableFilesystemWriteMethods = true;
        result.enableCommandExecutionMethods = true;
        result.filesystemReadPolicy = [](const auto&, std::string_view, const frontend::Json&) {
            return true;
        };
        result.filesystemWritePolicy = result.filesystemReadPolicy;
        result.commandExecutionPolicy = result.filesystemReadPolicy;
        return result;
    }

    std::map<std::string, frontend::Json, std::less<>> loadMinimalParameters(tests::support::TestResult& result) {
        const std::filesystem::path fixture =
            std::filesystem::path(__FILE__).parent_path() / "fixtures" / "frontend-protocol-v1.generated.json";
        std::ifstream input(fixture);
        frontend::Json document;
        if (input) {
            input >> document;
        }
        std::map<std::string, frontend::Json, std::less<>> parameters;
        if (document.is_object() && document.contains("methods") && document.at("methods").is_array()) {
            for (const frontend::Json& method : document.at("methods")) {
                parameters.emplace(method.at("method").get<std::string>(), method.at("minimalParams"));
            }
        }
        result.expectTrue(parameters.size() == generated::AllMethods.size(),
                          "the reviewed generated fixture supplies parameters for every generated method");
        return parameters;
    }

    std::optional<generated::DefinedCommand> command(std::string requestId,
                                                     const generated::MethodMetadata& metadata,
                                                     const std::map<std::string, frontend::Json, std::less<>>& parameters) {
        const auto found = parameters.find(std::string(metadata.method));
        if (found == parameters.end()) {
            return std::nullopt;
        }
        frontend::Json wire{{"protocol", frontend::ProtocolIdentity},
                            {"version", frontend::ProtocolVersion},
                            {"kind", frontend::kind::Command},
                            {"requestId", std::move(requestId)},
                            {"method", metadata.method},
                            {"params", found->second}};
        auto decoded = frontend::Codec::decodeDefinedCommand(wire);
        return decoded ? std::optional<generated::DefinedCommand>{decoded.value()} : std::nullopt;
    }

    server::ConnectionCallbacks collect(std::vector<frontend::ServerMessage>& messages, std::vector<server::ConnectionClose>& closes) {
        return {[&messages](const frontend::ServerMessage& message) {
                    messages.push_back(message);
                    return true;
                },
                [&closes](const server::ConnectionClose& close) {
                    closes.push_back(close);
                }};
    }

    void testGeneratedDispatchAndCorrelation(tests::support::TestResult& result) {
        const auto parameters = loadMinimalParameters(result);
        Backend backend;
        server::ServerCore core(backend, options());
        core.start();
        std::vector<frontend::ServerMessage> messages;
        std::vector<server::ConnectionClose> closes;
        const auto connection = core.openConnection({}, collect(messages, closes));
        result.expectTrue(connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted(),
                          "the command harness reaches synchronized authenticated state");
        messages.clear();

        std::size_t nativeCount = 0;
        std::size_t providerCount = 0;
        std::size_t reverseCount = 0;
        bool allAccepted = true;
        std::string failedMethod;
        for (std::size_t index = 0; index < generated::AllMethods.size(); ++index) {
            const generated::MethodMetadata& metadata = generated::AllMethods[index];
            nativeCount += metadata.frontendNative ? 1U : 0U;
            providerCount += metadata.category == generated::MethodCategory::ProviderOperation ? 1U : 0U;
            reverseCount += metadata.category == generated::MethodCategory::ReverseResponse ? 1U : 0U;
            if (metadata.id == generated::MethodId::ProviderStart) {
                backend.state.provider.lifecycle = model::ProviderLifecycle::Stopped;
                backend.state.provider.desiredRunning = false;
            } else if (metadata.id == generated::MethodId::ProviderStop) {
                backend.state.provider.lifecycle = model::ProviderLifecycle::Ready;
                backend.state.provider.desiredRunning = true;
            }
            const auto defined = command("generated-" + std::to_string(index), metadata, parameters);
            if (!defined || !core.receiveDefinedCommand(*connection, *defined).accepted()) {
                allAccepted = false;
                failedMethod = std::string(metadata.method);
                break;
            }
            if (metadata.category == generated::MethodCategory::ProviderLifecycle) {
                allAccepted = core.publishSnapshot(backend.state).accepted;
            }
            if (metadata.id == generated::MethodId::ControllerRelease) {
                const auto reacquire = command("controller-reacquire", generated::AllMethods.front(), parameters);
                allAccepted = reacquire && core.receiveDefinedCommand(*connection, *reacquire).accepted();
            }
            if (!allAccepted) {
                failedMethod = std::string(metadata.method);
                break;
            }
        }

        const std::vector<frontend::FrontendMethod> definedMethods = core.definedMethods();
        bool authorityOrder = definedMethods.size() == generated::AllMethods.size();
        for (std::size_t index = 0; authorityOrder && index < definedMethods.size(); ++index) {
            authorityOrder = definedMethods[index] == generated::AllMethods[index].method;
        }
        result.expectTrue(allAccepted && authorityOrder && generated::AllMethods.size() == 105 && nativeCount == 7 && providerCount == 86 &&
                              reverseCount == 12 && backend.submissionCount == 101 && backend.lifecycleActions.size() == 3,
                          "one generated authority dispatches all 105 paths with the frozen 7/86/12 split; first failure=" +
                              (failedMethod.empty() ? std::string{"none"} : failedMethod));

        const generated::MethodMetadata& threadList = generated::AllMethods[6];
        const auto duplicate = command("generated-6", threadList, parameters);
        const server::ReceiveResult duplicateResult = core.receiveDefinedCommand(*connection, *duplicate);
        const auto* duplicateResponse = !messages.empty() ? std::get_if<frontend::Response>(&messages.back()) : nullptr;
        result.expectTrue(duplicateResult.status == server::ReceiveStatus::Rejected && duplicateResponse && duplicateResponse->error &&
                              duplicateResponse->error->code == frontend::ErrorCode::DuplicateRequestId &&
                              duplicateResponse->error->message == "requestId is already pending in this frontend session" && closes.empty(),
                          "an outstanding request ID is rejected without closing the connection");

        const auto completionCandidate = std::find_if(backend.tokens.begin(), backend.tokens.end(), [](const server::CommandToken& token) {
            return token.method != generated::MethodId::ControllerAcquire && token.method != generated::MethodId::ControllerRelease;
        });
        const server::CommandToken completedToken = *completionCandidate;
        const bool completed = core.complete(server::BackendCompletion{
            completedToken,
            server::BackendCommandFailure{frontend::ErrorCode::RemoteAppServerError, "deterministic backend failure", std::nullopt}});
        const auto* completion = !messages.empty() ? std::get_if<frontend::Response>(&messages.back()) : nullptr;
        result.expectTrue(completed && completion && completion->requestId == completedToken.requestId && completion->error &&
                              completion->error->code == frontend::ErrorCode::RemoteAppServerError && core.isOpen() &&
                              !core.complete(server::BackendCompletion{
                                  completedToken, server::BackendCommandFailure{frontend::ErrorCode::Cancelled, "late", std::nullopt}}),
                          "typed completion correlates once and ordinary backend failure leaves the client connection open");
    }

    void testFrontendNativeFailureStatus(tests::support::TestResult& result) {
        Backend backend;
        backend.state.provider.lifecycle = model::ProviderLifecycle::Stopped;
        backend.state.provider.desiredRunning = false;
        server::ServerCore core(backend, options());
        core.start();
        std::vector<frontend::ServerMessage> firstMessages;
        std::vector<frontend::ServerMessage> secondMessages;
        std::vector<server::ConnectionClose> closes;
        const auto first = core.openConnection({}, collect(firstMessages, closes));
        const auto second = core.openConnection({}, collect(secondMessages, closes));
        const bool synchronized = first && second &&
                                  core.receive(*first, frontend::ClientMessage{frontend::Hello{}}).accepted() &&
                                  core.receive(*second, frontend::ClientMessage{frontend::Hello{}}).accepted();
        firstMessages.clear();
        secondMessages.clear();

        generated::DefinedCommand acquire{
            "native-acquire", generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
        generated::DefinedCommand conflict{
            "native-conflict", generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
        const bool acquired = first && core.receiveDefinedCommand(*first, acquire).accepted();
        firstMessages.clear();
        secondMessages.clear();
        const std::size_t submissionsAfterAcquire = backend.submissionCount;
        const server::ReceiveResult conflictResult = core.receiveDefinedCommand(*second, conflict);
        const auto* conflictResponse = !secondMessages.empty() ? std::get_if<frontend::Response>(&secondMessages.back()) : nullptr;
        const std::optional<frontend::CommandError> conflictError = conflictResponse ? conflictResponse->error : std::nullopt;

        firstMessages.clear();
        generated::DefinedCommand futureReplay{
            "native-future-replay",
            generated::makeParameters(generated::MethodId::EventsReplay,
                                      frontend::Json{{"after", core.currentSequence().value() + 1}})};
        const server::ReceiveResult replayResult = core.receiveDefinedCommand(*first, futureReplay);
        const auto* replayResponse = !firstMessages.empty() ? std::get_if<frontend::Response>(&firstMessages.back()) : nullptr;
        const std::optional<frontend::CommandError> replayError = replayResponse ? replayResponse->error : std::nullopt;

        firstMessages.clear();
        backend.lifecycleAccepted = false;
        generated::DefinedCommand start{
            "native-start", generated::makeParameters(generated::MethodId::ProviderStart, frontend::Json::object())};
        const server::ReceiveResult lifecycleResult = core.receiveDefinedCommand(*first, start);
        const auto* lifecycleResponse = !firstMessages.empty() ? std::get_if<frontend::Response>(&firstMessages.back()) : nullptr;
        const std::optional<frontend::CommandError> lifecycleError = lifecycleResponse ? lifecycleResponse->error : std::nullopt;

        firstMessages.clear();
        backend.lifecycleAccepted = true;
        backend.snapshotThrows = true;
        generated::DefinedCommand snapshotFailure{
            "native-snapshot-failure", generated::makeParameters(generated::MethodId::ProviderStart, frontend::Json::object())};
        const server::ReceiveResult snapshotFailureResult = core.receiveDefinedCommand(*first, snapshotFailure);
        const auto* snapshotFailureResponse =
            !firstMessages.empty() ? std::get_if<frontend::Response>(&firstMessages.back()) : nullptr;

        result.expectTrue(
            synchronized && acquired && conflictResult.status == server::ReceiveStatus::Accepted && conflictError &&
                conflictError->code == frontend::ErrorCode::Conflict &&
                conflictError->message == "frontend command conflicts with current state" &&
                backend.submissionCount == submissionsAfterAcquire && replayResult.status == server::ReceiveStatus::Rejected &&
                replayError && replayError->code == frontend::ErrorCode::InvalidCommand &&
                lifecycleResult.status == server::ReceiveStatus::Rejected && lifecycleError &&
                lifecycleError->code == frontend::ErrorCode::InternalError &&
                lifecycleError->message == "provider lifecycle action failed locally" &&
                snapshotFailureResult.status == server::ReceiveStatus::Rejected && snapshotFailureResponse &&
                snapshotFailureResponse->error && snapshotFailureResponse->error->code == frontend::ErrorCode::InternalError &&
                snapshotFailureResponse->error->message == "failed to dispatch frontend command" && closes.empty(),
            "frontend-native service failures reject locally and a known controller conflict never reaches the backend");
    }

    void testBackendSubmissionErrors(tests::support::TestResult& result) {
        const auto parameters = loadMinimalParameters(result);
        Backend backend;
        server::ServerCore core(backend, options());
        core.start();
        std::vector<frontend::ServerMessage> messages;
        std::vector<server::ConnectionClose> closes;
        const auto connection = core.openConnection({}, collect(messages, closes));
        const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        messages.clear();

        const generated::MethodMetadata& metadata =
            generated::AllMethods[static_cast<std::size_t>(generated::MethodId::ThreadList)];
        constexpr std::array statuses{server::BackendSubmitStatus::Rejected,
                                      server::BackendSubmitStatus::Unavailable,
                                      server::BackendSubmitStatus::CapacityExceeded};
        constexpr std::array codes{frontend::ErrorCode::LocalSubmissionFailure,
                                   frontend::ErrorCode::BackendUnavailable,
                                   frontend::ErrorCode::CapacityExceeded};
        bool exact = synchronized;
        for (std::size_t index = 0; index < statuses.size(); ++index) {
            backend.submissionStatus = statuses[index];
            const auto defined = command("submission-" + std::to_string(index), metadata, parameters);
            const server::ReceiveResult received = core.receiveDefinedCommand(*connection, *defined);
            const auto* response = !messages.empty() ? std::get_if<frontend::Response>(&messages.back()) : nullptr;
            exact = exact && received.status == server::ReceiveStatus::Rejected && response && response->error &&
                    response->error->code == codes[index] && response->error->message == "frontend command submission failed";
        }
        result.expectTrue(exact && closes.empty(),
                          "backend submission failures preserve the oracle code mapping and one exact bounded diagnostic");
    }

    void testOutstandingCapacity(tests::support::TestResult& result) {
        const auto parameters = loadMinimalParameters(result);
        Backend backend;
        server::ServerCoreOptions bounded = options();
        bounded.maxOutstandingCommandsPerConnection = 1;
        server::ServerCore core(backend, std::move(bounded));
        core.start();
        std::vector<frontend::ServerMessage> messages;
        std::vector<server::ConnectionClose> closes;
        const auto connection = core.openConnection({}, collect(messages, closes));
        (void) core.receive(*connection, frontend::ClientMessage{frontend::Hello{}});
        messages.clear();

        const auto first = command("capacity-1", generated::AllMethods[6], parameters);
        const auto second = command("capacity-2", generated::AllMethods[19], parameters);
        const bool firstAccepted = core.receiveDefinedCommand(*connection, *first).accepted();
        const server::ReceiveResult secondResult = core.receiveDefinedCommand(*connection, *second);
        const auto* response = !messages.empty() ? std::get_if<frontend::Response>(&messages.back()) : nullptr;
        result.expectTrue(firstAccepted && secondResult.status == server::ReceiveStatus::Rejected && response && response->error &&
                              response->error->code == frontend::ErrorCode::CapacityExceeded &&
                              core.outstandingCommands(*connection) == 1 && closes.empty(),
                          "the per-connection outstanding-command bound rejects only the excess operation");
    }

    void testSynchronousCompletionAndStaleGeneration(tests::support::TestResult& result) {
        const auto parameters = loadMinimalParameters(result);
        Backend backend;
        server::ServerCore core(backend, options());
        core.start();
        std::vector<frontend::ServerMessage> messages;
        std::vector<server::ConnectionClose> closes;
        const auto connection = core.openConnection({}, collect(messages, closes));
        const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        messages.clear();

        bool completedInline = false;
        std::optional<server::CommandToken> inlineToken;
        backend.onSubmit = [&](server::BackendInvocation invocation) {
            inlineToken = invocation.token;
            completedInline = core.complete(server::BackendCompletion{
                invocation.token,
                server::BackendCommandFailure{frontend::ErrorCode::RemoteAppServerError, "synchronous completion", std::nullopt}});
            return server::BackendSubmitStatus::Accepted;
        };
        const auto inlineCommand = command("synchronous-completion", generated::AllMethods[6], parameters);
        const server::ReceiveResult inlineResult = connection && inlineCommand
                                                       ? core.receiveDefinedCommand(*connection, *inlineCommand)
                                                       : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
        const std::size_t matchingResponses = static_cast<std::size_t>(std::count_if(
            messages.begin(), messages.end(), [](const frontend::ServerMessage& message) {
                const auto* response = std::get_if<frontend::Response>(&message);
                return response && response->requestId == "synchronous-completion";
            }));
        const bool duplicateCompletionRejected =
            inlineToken && !core.complete(server::BackendCompletion{
                               *inlineToken,
                               server::BackendCommandFailure{frontend::ErrorCode::Cancelled, "duplicate completion", std::nullopt}});
        result.expectTrue(synchronized && inlineCommand && inlineResult.accepted() && completedInline && matchingResponses == 1 &&
                              duplicateCompletionRejected && core.outstandingCommands(*connection) == 0 && closes.empty(),
                          "backend submit may complete synchronously exactly once without leaving correlation pending");

        messages.clear();
        std::optional<server::CommandToken> staleToken;
        backend.onSubmit = [&](server::BackendInvocation invocation) {
            staleToken = invocation.token;
            return server::BackendSubmitStatus::Accepted;
        };
        const auto staleCommand = command("stale-completion", generated::AllMethods[19], parameters);
        const bool staleSubmitted = connection && staleCommand && core.receiveDefinedCommand(*connection, *staleCommand).accepted();
        if (connection) {
            core.closeConnection(*connection, "close before completion");
        }
        const bool staleRejected = staleToken && !core.complete(server::BackendCompletion{
                                                     *staleToken,
                                                     server::BackendCommandFailure{
                                                         frontend::ErrorCode::Cancelled, "stale completion", std::nullopt}});
        result.expectTrue(staleSubmitted && staleRejected && messages.empty() && closes.size() == 1 &&
                              !core.connectionOpen(*connection),
                          "a backend completion after connection close is ignored and cannot emit a stale response");
    }

    void testUnknownRequestResponseCompatibility(tests::support::TestResult& result) {
        const auto parameters = loadMinimalParameters(result);
        Backend backend;
        server::ServerCore core(backend, options());
        core.start();
        std::vector<frontend::ServerMessage> messages;
        std::vector<server::ConnectionClose> closes;
        const auto connection = core.openConnection({}, collect(messages, closes));
        const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        const auto acquire = command("unknown-controller", generated::AllMethods.front(), parameters);
        const bool controlled = acquire && core.receiveDefinedCommand(*connection, *acquire).accepted();
        messages.clear();

        std::vector<frontend::Json> submitted;
        backend.onSubmit = [&](server::BackendInvocation invocation) {
            const auto encoded = frontend::Codec::encodeDefinedCommand(invocation.command);
            if (encoded) {
                submitted.push_back(encoded.value());
            }
            (void) core.complete(server::BackendCompletion{
                invocation.token,
                server::BackendCommandFailure{frontend::ErrorCode::RemoteAppServerError, "terminal proof", std::nullopt}});
            return server::BackendSubmitStatus::Accepted;
        };
        const auto decode = [](std::string requestId, std::string_view method, frontend::Json params) {
            return frontend::Codec::decodeDefinedCommand(frontend::Json{{"protocol", frontend::ProtocolIdentity},
                                                                        {"version", frontend::ProtocolVersion},
                                                                        {"kind", frontend::kind::Command},
                                                                        {"requestId", std::move(requestId)},
                                                                        {"method", method},
                                                                        {"params", std::move(params)}});
        };
        const auto respond = decode("unknown-respond",
                                    "request.unknown.respond",
                                    frontend::Json{{"pendingRequestId", "172"}, {"result", {{"accepted", true}}}});
        const auto reject = decode("unknown-reject",
                                   "request.unknown.reject",
                                   frontend::Json{{"pendingRequestId", "172"},
                                                  {"code", -9},
                                                  {"message", "rejected"},
                                                  {"data", {{"safe", true}}}});
        const bool accepted = respond && reject && core.receiveDefinedCommand(*connection, respond.value()).accepted() &&
                              core.receiveDefinedCommand(*connection, reject.value()).accepted();
        const auto responseCount = [&](std::string_view requestId) {
            return std::count_if(messages.begin(), messages.end(), [&](const frontend::ServerMessage& message) {
                const auto* response = std::get_if<frontend::Response>(&message);
                return response && response->requestId == requestId;
            });
        };
        result.expectTrue(synchronized && controlled && accepted && submitted.size() == 2 &&
                              submitted[0].at("params").at("pendingRequestId") == "172" &&
                              submitted[1].at("params").at("pendingRequestId") == "172" && responseCount("unknown-respond") == 1 &&
                              responseCount("unknown-reject") == 1 && closes.empty(),
                          "request.unknown.respond and request.unknown.reject preserve the exact pendingRequestId and one terminal response");
    }

    void testControllerRevalidationAfterProviderReady(tests::support::TestResult& result) {
        const auto parameters = loadMinimalParameters(result);
        const auto candidate = std::find_if(generated::AllMethods.begin(), generated::AllMethods.end(), [](const auto& metadata) {
            return metadata.currentlyImplemented && metadata.providerReadyRequired && metadata.controllerRequired &&
                   !metadata.frontendNative;
        });
        if (candidate == generated::AllMethods.end()) {
            result.expectTrue(false, "the generated authority contains a provider-ready controller-required command");
            return;
        }

        Backend backend;
        server::ServerCore core(backend, options());
        core.start();
        std::vector<frontend::ServerMessage> messages;
        std::vector<server::ConnectionClose> closes;
        const auto connection = core.openConnection({}, collect(messages, closes));
        const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
        generated::DefinedCommand acquire{
            "provider-ready-acquire",
            generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
        const bool acquired = connection && core.receiveDefinedCommand(*connection, acquire).accepted();
        messages.clear();

        server::ReceiveResult nestedRelease;
        backend.onProviderReady = [&] {
            generated::DefinedCommand release{
                "provider-ready-release",
                generated::makeParameters(generated::MethodId::ControllerRelease, frontend::Json::object())};
            nestedRelease = core.receiveDefinedCommand(*connection, release);
        };
        const auto outer = command("provider-ready-outer", *candidate, parameters);
        const std::size_t submissionsBefore = backend.submissionCount;
        const server::ReceiveResult outerResult =
            connection && outer ? core.receiveDefinedCommand(*connection, *outer)
                                : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
        const auto* outerResponse = !messages.empty() ? std::get_if<frontend::Response>(&messages.back()) : nullptr;
        result.expectTrue(synchronized && acquired && outer && nestedRelease.accepted() &&
                              outerResult.status == server::ReceiveStatus::Rejected && outerResponse && outerResponse->error &&
                              outerResponse->error->code == frontend::ErrorCode::PermissionDenied &&
                              backend.submissionCount == submissionsBefore + 1 && !core.currentController() && closes.empty(),
                          "controller ownership lost inside providerReady is revalidated before backend submission");
    }

    void testProviderLifecycleReentrancyGeneration(tests::support::TestResult& result) {
        const auto responseFor = [](const std::vector<frontend::ServerMessage>& messages,
                                    std::string_view requestId) -> const frontend::Response* {
            const auto found = std::find_if(messages.begin(), messages.end(), [&](const frontend::ServerMessage& message) {
                const auto* response = std::get_if<frontend::Response>(&message);
                return response && response->requestId == requestId;
            });
            return found == messages.end() ? nullptr : std::get_if<frontend::Response>(&*found);
        };

        {
            Backend backend;
            backend.state.provider.lifecycle = model::ProviderLifecycle::Ready;
            backend.state.provider.desiredRunning = true;
            server::ServerCore core(backend, options());
            core.start();
            std::vector<frontend::ServerMessage> messages;
            std::vector<server::ConnectionClose> closes;
            const auto connection = core.openConnection({}, collect(messages, closes));
            const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
            generated::DefinedCommand acquire{
                "snapshot-reentry-acquire",
                generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
            const bool acquired = connection && core.receiveDefinedCommand(*connection, acquire).accepted();
            messages.clear();

            server::ReceiveResult nestedRestart;
            backend.onSnapshot = [&] {
                generated::DefinedCommand restart{
                    "nested-snapshot-restart",
                    generated::makeParameters(generated::MethodId::ProviderRestart, frontend::Json::object())};
                nestedRestart = core.receiveDefinedCommand(*connection, restart);
            };
            generated::DefinedCommand stop{
                "outer-stale-stop", generated::makeParameters(generated::MethodId::ProviderStop, frontend::Json::object())};
            const server::ReceiveResult outerStop =
                connection ? core.receiveDefinedCommand(*connection, stop)
                           : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
            const frontend::Response* outerResponse = responseFor(messages, "outer-stale-stop");
            result.expectTrue(synchronized && acquired && nestedRestart.accepted() &&
                                  outerStop.status == server::ReceiveStatus::Rejected && outerResponse && outerResponse->error &&
                                  outerResponse->error->code == frontend::ErrorCode::Conflict &&
                                  backend.lifecycleActions == std::vector{server::ProviderLifecycleAction::Restart} && closes.empty(),
                              "a lifecycle action installed during backend snapshot reentry prevents the stale outer action");
        }

        {
            Backend backend;
            backend.state.provider.lifecycle = model::ProviderLifecycle::Ready;
            backend.state.provider.desiredRunning = true;
            server::ServerCore core(backend, options());
            core.start();
            std::vector<frontend::ServerMessage> messages;
            std::vector<server::ConnectionClose> closes;
            const auto connection = core.openConnection({}, collect(messages, closes));
            const bool synchronized = connection && core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted();
            generated::DefinedCommand acquire{
                "dispatch-reentry-acquire",
                generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
            const bool acquired = connection && core.receiveDefinedCommand(*connection, acquire).accepted();
            messages.clear();

            server::ReceiveResult nestedRestart;
            backend.onLifecycleAction = [&] {
                generated::DefinedCommand restart{
                    "nested-dispatch-restart",
                    generated::makeParameters(generated::MethodId::ProviderRestart, frontend::Json::object())};
                nestedRestart = core.receiveDefinedCommand(*connection, restart);
                backend.state.provider.lifecycle = model::ProviderLifecycle::Starting;
            };
            generated::DefinedCommand restart{
                "outer-dispatch-restart",
                generated::makeParameters(generated::MethodId::ProviderRestart, frontend::Json::object())};
            const server::ReceiveResult outerRestart =
                connection ? core.receiveDefinedCommand(*connection, restart)
                           : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};

            generated::DefinedCommand laterStop{
                "later-stop", generated::makeParameters(generated::MethodId::ProviderStop, frontend::Json::object())};
            const server::ReceiveResult later =
                connection ? core.receiveDefinedCommand(*connection, laterStop)
                           : server::ReceiveResult{server::ReceiveStatus::UnknownConnection, std::nullopt};
            const frontend::Response* outerResponse = responseFor(messages, "outer-dispatch-restart");
            const frontend::Response* laterResponse = responseFor(messages, "later-stop");
            result.expectTrue(synchronized && acquired && nestedRestart.accepted() &&
                                  outerRestart.status == server::ReceiveStatus::Rejected && outerResponse && outerResponse->error &&
                                  outerResponse->error->code == frontend::ErrorCode::Conflict &&
                                  later.status == server::ReceiveStatus::Rejected && laterResponse && laterResponse->error &&
                                  laterResponse->error->code == frontend::ErrorCode::Conflict &&
                                  backend.lifecycleActions ==
                                      std::vector{server::ProviderLifecycleAction::Restart,
                                                  server::ProviderLifecycleAction::Restart} &&
                                  closes.empty(),
                              "a same-kind lifecycle action replacement has a distinct generation and survives the stale outer continuation");
        }
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testGeneratedDispatchAndCorrelation(result);
    testFrontendNativeFailureStatus(result);
    testBackendSubmissionErrors(result);
    testOutstandingCapacity(result);
    testSynchronousCompletionAndStaleGeneration(result);
    testUnknownRequestResponseCompatibility(result);
    testControllerRevalidationAfterProviderReady(result);
    testProviderLifecycleReentrancyGeneration(result);
    return result.processResult();
}
