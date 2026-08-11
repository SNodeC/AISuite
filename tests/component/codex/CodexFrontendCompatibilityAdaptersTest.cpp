/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexFrontendCompatibilityAdapters.h"
#include "support/TestResult.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {
    namespace adapter = tests::codex::frontend_compatibility;
    namespace frontend = ai::openai::codex::frontend;
    namespace public_client = ai::openai::codex::frontend::client;
    namespace core_client = ai::openai::codex::frontend::internal::client;
    namespace core_model = ai::openai::codex::frontend::internal::model;

    void testServerOptions(tests::support::TestResult& result) {
        frontend::FrontendServiceOptions source;
        source.journal = {11, 1200, frontend::SequenceNumber{13}};
        source.batches = {17, 1800};
        source.coalescer = {19};
        source.maxOutboundMessagesPerConnection = 23;
        source.maxOutboundBytesPerConnection = 2400;
        source.maxMessagesPerDelivery = 29;
        source.maxConnections = 31;
        source.maxUnauthenticatedConnections = 37;
        source.handshakeTimeoutMs = 41;
        source.maximumInboundMessageBytes = 4300;
        source.maxInboundMessagesPerSecond = 47;
        source.maxInboundBurst = 53;
        source.maxOutstandingCommandsPerConnection = 59;
        source.maximumFailedAuthenticationsPerPeer = 61;
        source.failedAuthenticationWindowMs = 67;
        source.allowVerifiedLocalTrust = false;
        source.allowInsecureLocalTrust = true;
        source.trustedLocalUserId = 71;
        source.enableFilesystemReadMethods = true;
        source.enableFilesystemWriteMethods = true;
        source.enableCommandExecutionMethods = true;
        source.filesystemReadPolicy = [](const frontend::FrontendPrincipal&, std::string_view method, const frontend::Json&) {
            return method == "read";
        };
        source.filesystemWritePolicy = [](const frontend::FrontendPrincipal&, std::string_view method, const frontend::Json&) {
            return method == "write";
        };
        source.commandExecutionPolicy = [](const frontend::FrontendPrincipal&, std::string_view method, const frontend::Json&) {
            return method == "execute";
        };
        source.authenticator = [](const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential&) {
            return frontend::AuthenticationFailure{frontend::AuthenticationFailureCode::AuthenticationFailed};
        };
        std::size_t scheduled = 0;
        source.scheduler = [&scheduled](std::function<void()> callback) {
            ++scheduled;
            callback();
        };
        source.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {}};
        };
        source.monotonicClockMs = [] { return std::uint64_t{73}; };

        const auto core = adapter::serverOptions(source);
        const auto roundTrip = adapter::publicServerOptions(core);
        frontend::FrontendPrincipal principal;
        const bool scalarParity = roundTrip.journal == source.journal && roundTrip.batches == source.batches &&
                                  roundTrip.coalescer == source.coalescer &&
                                  roundTrip.maxOutboundMessagesPerConnection == source.maxOutboundMessagesPerConnection &&
                                  roundTrip.maxOutboundBytesPerConnection == source.maxOutboundBytesPerConnection &&
                                  roundTrip.maxMessagesPerDelivery == source.maxMessagesPerDelivery &&
                                  roundTrip.maxConnections == source.maxConnections &&
                                  roundTrip.maxUnauthenticatedConnections == source.maxUnauthenticatedConnections &&
                                  roundTrip.handshakeTimeoutMs == source.handshakeTimeoutMs &&
                                  roundTrip.maximumInboundMessageBytes == source.maximumInboundMessageBytes &&
                                  roundTrip.maxInboundMessagesPerSecond == source.maxInboundMessagesPerSecond &&
                                  roundTrip.maxInboundBurst == source.maxInboundBurst &&
                                  roundTrip.maxOutstandingCommandsPerConnection == source.maxOutstandingCommandsPerConnection &&
                                  roundTrip.maximumFailedAuthenticationsPerPeer == source.maximumFailedAuthenticationsPerPeer &&
                                  roundTrip.failedAuthenticationWindowMs == source.failedAuthenticationWindowMs &&
                                  roundTrip.allowVerifiedLocalTrust == source.allowVerifiedLocalTrust &&
                                  roundTrip.allowInsecureLocalTrust == source.allowInsecureLocalTrust &&
                                  roundTrip.trustedLocalUserId == source.trustedLocalUserId &&
                                  roundTrip.enableFilesystemReadMethods == source.enableFilesystemReadMethods &&
                                  roundTrip.enableFilesystemWriteMethods == source.enableFilesystemWriteMethods &&
                                  roundTrip.enableCommandExecutionMethods == source.enableCommandExecutionMethods;
        const bool callableParity = roundTrip.filesystemReadPolicy(principal, "read", frontend::Json::object()) &&
                                    roundTrip.filesystemWritePolicy(principal, "write", frontend::Json::object()) &&
                                    roundTrip.commandExecutionPolicy(principal, "execute", frontend::Json::object()) &&
                                    roundTrip.monotonicClockMs() == 73;
        roundTrip.scheduler([] {});
        result.expectTrue(scalarParity && callableParity && scheduled == 1 && core.maxPendingDeliveryGroups == source.coalescer.maxDirtyEntities,
                          "every frozen FrontendServiceOptions member maps to the server core without semantic loss");
    }

    void testServerCallbacks(tests::support::TestResult& result) {
        std::optional<frontend::OutboundMessage> sent;
        std::string closed;
        auto callbacks = adapter::serverCallbacks(
            {[&sent](const frontend::OutboundMessage& message) {
                 sent = message;
                 return true;
             },
             [&closed](const std::string& reason) {
                 closed = reason;
             }});
        const frontend::ServerMessage message = frontend::ProtocolErrorMessage{
            frontend::ErrorCode::InvalidCommand, "adapter probe", {}, false, std::nullopt, std::nullopt, frontend::Json::object()};
        const bool accepted = callbacks.onMessage(message);
        callbacks.onClosed({"adapter close", frontend::ErrorCode::InvalidCommand, false});
        result.expectTrue(accepted && sent && sent->message == message && sent->serializedBytes == sent->compactJson.size() &&
                              closed == "adapter close",
                          "FrontendConnectionCallbacks bind to typed server-core callbacks with exact message and close identity");
    }

    void testClientOptionsAndTransport(tests::support::TestResult& result) {
        public_client::ClientOptions source;
        source.requestedCapabilities = {frontend::FrontendCapability::CompleteBackendDomains};
        source.requiredCapabilities = {frontend::FrontendCapability::MethodDiscovery};
        source.credentialProvider = [] {
            return public_client::AuthenticationContext{frontend::NoCredential{}, std::string("continuity")};
        };
        source.maximumInboundMessageBytes = 101;
        source.maximumDecodedStateBytes = 103;
        source.maximumPendingOperations = 107;
        source.maximumRetainedDiagnostics = 109;
        source.allowLegacyV1 = false;
        const auto core = adapter::clientOptions(source);
        const auto roundTrip = adapter::publicClientOptions(core);
        const auto authentication = roundTrip.credentialProvider();
        const bool optionsParity = roundTrip.requestedCapabilities == source.requestedCapabilities &&
                                   roundTrip.requiredCapabilities == source.requiredCapabilities &&
                                   roundTrip.maximumInboundMessageBytes == source.maximumInboundMessageBytes &&
                                   roundTrip.maximumDecodedStateBytes == source.maximumDecodedStateBytes &&
                                   roundTrip.maximumPendingOperations == source.maximumPendingOperations &&
                                   roundTrip.maximumRetainedDiagnostics == source.maximumRetainedDiagnostics &&
                                   roundTrip.allowLegacyV1 == source.allowLegacyV1 &&
                                   authentication.continuityKey == std::optional<std::string>{"continuity"};

        std::optional<public_client::OutboundMessage> outbound;
        std::string closed;
        auto transport = adapter::clientTransportCallbacks(
            {[&outbound](public_client::OutboundMessage message) {
                 outbound = std::move(message);
                 return public_client::SendResult{public_client::SendStatus::Accepted, std::nullopt};
             },
             [&closed](std::string reason) {
                 closed = std::move(reason);
             }});
        const auto send = transport.send(core_client::OutboundMessage{frontend::Hello{}, true});
        transport.close("adapter transport close");
        result.expectTrue(optionsParity && send.status == core_client::SendStatus::Accepted && outbound &&
                              outbound->kind == public_client::OutboundKind::Hello && outbound->sensitive &&
                              outbound->serializedBytes == outbound->compactJson.size() && closed == "adapter transport close",
                          "every public ClientOptions member and transport callback binds to the client core");
    }

    void testPublicValuesAndState(tests::support::TestResult& result) {
        core_client::ClientError error{core_client::ErrorOrigin::Command,
                                       std::nullopt,
                                       frontend::ErrorCode::Conflict,
                                       "command conflict",
                                       std::optional<frontend::Json>{frontend::Json{{"bounded", true}}},
                                       false};
        const public_client::Error convertedError = adapter::publicError(error);
        const public_client::Submission submission = adapter::publicSubmission({std::string("c1-r1"), std::nullopt});
        core_client::OperationResult operation;
        operation.requestId = "c1-r1";
        operation.method = frontend::generated::MethodId::ControllerAcquire;
        operation.value = frontend::generated::makeResult(operation.method, frontend::Json{{"role", "observer"}});
        const public_client::GeneratedOperationResult convertedResult = adapter::publicResult(operation);

        core_model::CanonicalSnapshot snapshot;
        snapshot.sequence = core_model::FrontendSequence{5};
        snapshot.threadList.complete = true;
        snapshot.threads.emplace_back(core_model::ThreadIdentity{"thread-adapter"});
        snapshot.turns.emplace_back(core_model::TurnIdentity{"turn-adapter"}, core_model::ThreadIdentity{"thread-adapter"});
        core_model::LegacyItemCompatibility future{
            core_model::ItemData{core_model::ItemIdentity{"future-adapter-item"},
                                 core_model::ThreadIdentity{"thread-adapter"},
                                 core_model::TurnIdentity{"turn-adapter"}},
            "future_codex_item_kind",
            0,
            "/threads/0/turns/0/items/0"};
        future.value.sourceIndex = 0;
        future.value.safeDetails = *core_model::SafeDetail::fromJson(frontend::Json{{"safeSentinel", "adapter-safe"}});
        snapshot.legacyItems.push_back(std::move(future));
        core_client::SessionInfo session{core_model::SessionIdentity{"session-adapter"}};
        session.serverCurrentSequence = core_model::FrontendSequence{5};
        core_client::PublishedState published;
        published.revision = 7;
        published.freshness = core_client::PublishedFreshness::Current;
        published.representation = core_client::RepresentationMode::LegacyV1;
        published.visibleSequence = core_model::FrontendSequence{5};
        published.synchronizedThrough = core_model::FrontendSequence{5};
        published.session = session;
        published.snapshot = std::make_shared<const core_model::CanonicalSnapshot>(std::move(snapshot));
        std::string stateError;
        const std::optional<public_client::State> state = adapter::publicState(published, 1024U * 1024U, 64, true, stateError);
        const public_client::ItemState* futureItem = state ? state->item("future-adapter-item") : nullptr;
        result.expectTrue(convertedError.origin == public_client::ErrorOrigin::Command &&
                              convertedError.protocolCode == frontend::ErrorCode::Conflict && convertedError.details.has_value() &&
                              submission.accepted() && submission.requestId->value() == "c1-r1" && convertedResult.succeeded() &&
                              state && state->revision() == 7 && state->freshness() == public_client::StateFreshness::Current &&
                              state->visibleSequence() == frontend::SequenceNumber{5} &&
                              state->synchronizedThrough() == frontend::SequenceNumber{5} && futureItem &&
                              futureItem->kind.identity == "future_codex_item_kind" && !futureItem->kind.known.has_value(),
                          "client core values produce the frozen public error, submission, typed result, and immutable State types");
    }

    void testGeneratedFacadesAndCallbacks(tests::support::TestResult& result) {
        std::size_t native = 0;
        std::size_t provider = 0;
        std::size_t reverse = 0;
        bool exact = true;
        for (const frontend::generated::MethodMetadata& method : frontend::generated::AllMethods) {
            const public_client::generated::BindingMetadata* binding = public_client::generated::bindingMetadata(method.id);
            exact = exact && binding != nullptr && binding->method == method.id && !binding->facade.empty() && !binding->operation.empty() &&
                    !binding->parameterType.empty() && !binding->resultType.empty();
            if (binding) {
                native += binding->category == public_client::generated::BindingCategory::Native ? 1U : 0U;
                provider += binding->category == public_client::generated::BindingCategory::Provider ? 1U : 0U;
                reverse += binding->category == public_client::generated::BindingCategory::Reverse ? 1U : 0U;
            }
        }

        std::size_t connectionCallbacks = 0;
        std::size_t diagnostics = 0;
        public_client::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&connectionCallbacks](const public_client::ConnectionStateChange&) {
            ++connectionCallbacks;
        };
        callbacks.onDiagnostic = [&diagnostics](const public_client::Diagnostic&) {
            ++diagnostics;
        };
        auto bound = adapter::clientCallbacks(std::move(callbacks));
        bound.onConnectionStateChanged({core_client::ConnectionState::Disconnected,
                                        core_client::ConnectionState::Connecting,
                                        std::nullopt,
                                        1});
        bound.onError({core_client::ErrorOrigin::Client,
                       core_client::ClientErrorCode::NotReady,
                       std::nullopt,
                       "not ready",
                       std::nullopt,
                       false});
        result.expectTrue(exact && native == 7 && provider == 86 && reverse == 12 && connectionCallbacks == 1 && diagnostics == 1,
                          "all 105 generated public facades and frozen callback contracts bind to the generic client core");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testServerOptions(result);
    testServerCallbacks(result);
    testClientOptionsAndTransport(result);
    testPublicValuesAndState(result);
    testGeneratedFacadesAndCallbacks(result);
    return result.processResult();
}
