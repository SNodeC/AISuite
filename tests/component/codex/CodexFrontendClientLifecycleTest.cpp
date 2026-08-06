/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Client.h"
#include "support/TestResult.h"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace client = ai::openai::codex::frontend::client;

    struct Harness {
        std::vector<client::OutboundMessage> messages;
        std::vector<client::ConnectionState> states;
        std::size_t closes = 0;
        std::size_t diagnostics = 0;

        client::TransportCallbacks transport() {
            return {
                [this](client::OutboundMessage message) {
                    messages.push_back(std::move(message));
                    return client::SendResult{client::SendStatus::Accepted, std::nullopt};
                },
                [this](std::string) {
                    ++closes;
                },
            };
        }

        client::ClientCallbacks callbacks() {
            return {
                [this](const client::ConnectionStateChange& change) {
                    states.push_back(change.current);
                },
                {},
                {},
                {},
                {},
                [this](const client::Diagnostic&) {
                    ++diagnostics;
                },
            };
        }
    };

    client::ClientOptions options() {
        client::ClientOptions value;
        value.credentialProvider = [] {
            return client::AuthenticationContext{frontend::BearerCredential{"BEARER_SENTINEL"}, "bearer-profile:test"};
        };
        return value;
    }

    frontend::CapabilityAdvertisement advertisedCapabilities() {
        const std::vector<frontend::FrontendCapability> capabilities{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
            frontend::FrontendCapability::AuthenticatedFrontend,
        };
        return {capabilities, capabilities, capabilities, frontend::Json::object()};
    }

    void makeReady(client::Connection& connection, frontend::SequenceNumber sequence = frontend::SequenceNumber(7)) {
        (void) connection.receive(frontend::ServerMessage{frontend::Welcome{"session-1",
                                                                            frontend::SessionRole::Observer,
                                                                            sequence,
                                                                            frontend::SyncMode::Snapshot,
                                                                            frontend::Json::object(),
                                                                            advertisedCapabilities()}});
        (void) connection.receive(frontend::ServerMessage{frontend::Snapshot{sequence, frontend::Json::object()}});
        (void) connection.receive(frontend::ServerMessage{frontend::SyncComplete{sequence}});
    }

    void testHandshakeAndLifecycle(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());

        result.expectTrue(connection.isOpen() && !connection.isTransportConnected() && harness.messages.empty() &&
                              sdk.connectionState() == client::ConnectionState::Connecting,
                          "opening a logical attachment emits no Hello before the physical transport reports connected");

        connection.transportConnected();
        connection.transportConnected();
        const auto decodedHello = frontend::Codec::decodeClient(std::string_view(harness.messages.front().compactJson));
        const auto* hello = decodedHello ? std::get_if<frontend::Hello>(&decodedHello.value()) : nullptr;
        const std::vector<frontend::FrontendCapability> exactCapabilities{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };
        result.expectTrue(harness.messages.size() == 1 && harness.messages.front().kind == client::OutboundKind::Hello &&
                              harness.messages.front().sensitive && hello != nullptr && hello->capabilities == exactCapabilities &&
                              !hello->resumeAfter && sdk.connectionState() == client::ConnectionState::Authenticating,
                          "transportConnected emits exactly one sensitive Hello with exactly five representation capabilities");

        makeReady(connection);
        result.expectTrue(sdk.isReady() && sdk.session() && sdk.session()->sessionId == "session-1" &&
                              sdk.synchronizedThrough() == frontend::SequenceNumber(7),
                          "Welcome, Snapshot, and SyncComplete perform the exact initial lifecycle transition to Ready");

        client::Connection rejected = sdk.openConnection(harness.transport());
        result.expectTrue(!rejected.isOpen() && sdk.hasActiveConnection(), "one Client permits at most one active physical attachment");

        connection.transportDisconnected(client::TransportError{"peer closed", true});
        result.expectTrue(!connection.isOpen() && sdk.connectionState() == client::ConnectionState::Disconnected,
                          "an unexpected physical disconnect returns the reusable Client to Disconnected");

        client::Connection reconnect = sdk.openConnection(harness.transport());
        reconnect.transportConnected();
        const auto reconnectHelloMessage = frontend::Codec::decodeClient(std::string_view(harness.messages.back().compactJson));
        const auto* reconnectHello = reconnectHelloMessage ? std::get_if<frontend::Hello>(&reconnectHelloMessage.value()) : nullptr;
        result.expectTrue(reconnectHello != nullptr && reconnectHello->resumeAfter == frontend::SequenceNumber(7) &&
                              reconnect.generation() == 2,
                          "unchanged authenticated continuity resumes from synchronizedThrough on a new connection generation");

        sdk.close();
        sdk.close();
        result.expectTrue(sdk.connectionState() == client::ConnectionState::Closed && harness.closes == 1,
                          "Client close is terminal, idempotent, and asks the active transport to close at most once");
    }

    void testCorrelationAndFailures(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection);

        std::size_t completions = 0;
        bool succeeded = false;
        client::Submission submission = sdk.submit(
            generated::CompleteCommandParameters{
                generated::MethodParameters<generated::MethodId::ControllerAcquire>{frontend::Json::object()}},
            [&](const client::GeneratedOperationResult& operation) {
                ++completions;
                succeeded = operation.succeeded() && generated::commandMethod(*operation.value) == generated::MethodId::ControllerAcquire;
            });
        const auto encodedCommand = frontend::Codec::decodeDefinedCommand(std::string_view(harness.messages.back().compactJson));
        result.expectTrue(submission && encodedCommand && encodedCommand.value().requestId == submission.requestId->value() &&
                              generated::commandMethod(encodedCommand.value().parameters) == generated::MethodId::ControllerAcquire &&
                              sdk.pendingOperationCount() == 1,
                          "the SDK generates and correlates a bounded request ID through the generated MethodId authority");

        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(submission.requestId->value(), frontend::Json{{"role", "controller"}})});
        result.expectTrue(completions == 1 && succeeded && sdk.pendingOperationCount() == 0,
                          "a schema-valid result completes its operation exactly once");

        std::size_t commandFailures = 0;
        client::Submission failedSubmission =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                           frontend::Json::object()}},
                       [&](const client::GeneratedOperationResult& operation) {
                           commandFailures += operation.error && operation.error->origin == client::ErrorOrigin::Command ? 1U : 0U;
                       });
        (void) connection.receive(frontend::ServerMessage{frontend::Response::failure(
            failedSubmission.requestId->value(),
            frontend::CommandError{frontend::ErrorCode::Conflict, "controller is already owned", std::nullopt, frontend::Json::object()})});
        result.expectTrue(commandFailures == 1 && connection.isOpen() && sdk.isReady(),
                          "a normal command error completes once without closing the physical connection");

        (void) connection.receive(
            frontend::ServerMessage{frontend::Response::success(submission.requestId->value(), frontend::Json{{"role", "controller"}})});
        result.expectTrue(completions == 1 && !connection.isOpen() && sdk.connectionState() == client::ConnectionState::Disconnected &&
                              harness.closes == 1,
                          "a duplicate or unsolicited response is contained as a connection-local protocol failure");
    }

    void testPendingDisconnectAndCallbackContainment(tests::support::TestResult& result) {
        Harness harness;
        client::Client sdk(options(), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        makeReady(connection);

        std::size_t completions = 0;
        client::Submission pending =
            sdk.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                           frontend::Json::object()}},
                       [&](const client::GeneratedOperationResult& operation) {
                           completions += operation.error && operation.error->origin == client::ErrorOrigin::Transport ? 1U : 0U;
                       });
        const std::size_t messagesBeforeDisconnect = harness.messages.size();
        connection.transportDisconnected(client::TransportError{"connection lost", true});
        connection.transportDisconnected(client::TransportError{"duplicate disconnect", true});
        result.expectTrue(pending && completions == 1 && sdk.pendingOperationCount() == 0 &&
                              sdk.connectionState() == client::ConnectionState::Disconnected,
                          "disconnect completes every pending operation exactly once with a transport error");

        client::Connection reconnect = sdk.openConnection(harness.transport());
        reconnect.transportConnected();
        result.expectTrue(harness.messages.size() == messagesBeforeDisconnect + 1 &&
                              harness.messages.back().kind == client::OutboundKind::Hello,
                          "reconnect emits only a new Hello and never retries an unacknowledged command");
        reconnect.transportDisconnected();

        Harness throwingHarness;
        client::ClientCallbacks throwingCallbacks = throwingHarness.callbacks();
        throwingCallbacks.onConnectionStateChanged = [](const client::ConnectionStateChange&) {
            throw std::runtime_error("callback sentinel");
        };
        client::Client callbackClient(options(), std::move(throwingCallbacks));
        client::Connection callbackConnection = callbackClient.openConnection(throwingHarness.transport());
        callbackConnection.transportConnected();
        makeReady(callbackConnection);
        result.expectTrue(callbackClient.isReady() && throwingHarness.diagnostics > 0,
                          "application callback exceptions are contained and reported without corrupting lifecycle state");
    }

    void testExceptionContainmentAndCapacity(tests::support::TestResult& result) {
        Harness harness;
        client::ClientOptions failing = options();
        failing.credentialProvider = []() -> client::AuthenticationContext {
            throw std::runtime_error("credential sentinel");
        };
        client::Client sdk(std::move(failing), harness.callbacks());
        client::Connection connection = sdk.openConnection(harness.transport());
        connection.transportConnected();
        result.expectTrue(!connection.isOpen() && sdk.connectionState() == client::ConnectionState::Disconnected && harness.closes == 1 &&
                              harness.messages.empty(),
                          "credential-provider exceptions are contained without exposing or sending a partial Hello");

        bool invalidRequiredRejected = false;
        try {
            client::ClientOptions invalid = options();
            invalid.requestedCapabilities.clear();
            invalid.requiredCapabilities = {frontend::FrontendCapability::CompleteBackendDomains};
            client::Client invalidClient(std::move(invalid));
        } catch (const std::invalid_argument&) {
            invalidRequiredRejected = true;
        }
        result.expectTrue(invalidRequiredRejected,
                          "a required representation capability is rejected unless it is also in the Hello representation request");

        Harness boundedHarness;
        client::ClientOptions boundedOptions = options();
        boundedOptions.maximumPendingOperations = 0;
        client::Client bounded(std::move(boundedOptions), boundedHarness.callbacks());
        client::Connection boundedConnection = bounded.openConnection(boundedHarness.transport());
        boundedConnection.transportConnected();
        makeReady(boundedConnection);
        const client::Submission capacityRejected =
            bounded.submit(generated::CompleteCommandParameters{generated::MethodParameters<generated::MethodId::ControllerAcquire>{
                               frontend::Json::object()}},
                           {});
        result.expectTrue(!capacityRejected && capacityRejected.error &&
                              capacityRejected.error->clientCode == client::ClientErrorCode::TooManyPendingOperations,
                          "zero pending-operation capacity is finite and rejects locally rather than meaning unlimited");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testHandshakeAndLifecycle(result);
    testCorrelationAndFailures(result);
    testPendingDisconnectAndCallbackContainment(result);
    testExceptionContainmentAndCapacity(result);
    return result.processResult();
}
